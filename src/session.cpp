#include "session.h"

#include "audio_engine.h"
#include "device_registry.h"
#include "hresult.h"
#include "logging.h"
#include "strings.h"
#include "tray.h"
#include "version.h"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

namespace vcmic {
namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailure = 2;
constexpr DWORD kTickMs = 500;

enum class WaitOutcome { Stopped, Elapsed };

// Waits without going deaf. The tray's window belongs to this thread, so a
// plain WaitForSingleObject here would freeze its menu for the whole interval;
// with no tray there is simply never anything in the queue.
WaitOutcome WaitPumping(HANDLE stop_event, DWORD timeout_ms) {
    const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
    for (;;) {
        const ULONGLONG now = ::GetTickCount64();
        if (now >= deadline) {
            return WaitOutcome::Elapsed;
        }
        const DWORD remaining = static_cast<DWORD>(deadline - now);

        const DWORD result =
            ::MsgWaitForMultipleObjects(1, &stop_event, FALSE, remaining, QS_ALLINPUT);
        if (result == WAIT_OBJECT_0) {
            return WaitOutcome::Stopped;
        }
        if (result != WAIT_OBJECT_0 + 1) {
            return WaitOutcome::Elapsed;  // timed out, or the wait itself failed
        }

        MSG message;
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
            if (message.message == WM_QUIT) {
                return WaitOutcome::Stopped;
            }
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }
}

// A console application started by the scheduler is handed a console window it
// never asked for. Hide it - but only when we are the sole process attached to
// it, because a console we were launched from belongs to the shell, not to us.
void HideOwnConsole() {
    const HWND console = ::GetConsoleWindow();
    if (console == nullptr) {
        return;
    }
    DWORD owners[2] = {};
    if (::GetConsoleProcessList(owners, 2) == 1) {
        ::ShowWindow(console, SW_HIDE);
    }
}

TrayState StateFor(const EngineStatus& status) {
    if (status.faulted) {
        return TrayState::Failed;
    }
    if (!status.render_live || !status.chat_live || !status.mic_live) {
        return TrayState::Degraded;
    }
    // Muted is amber on purpose. A forgotten mute produces a recording with
    // half the audio missing, and the icon is the only place that would have
    // said so before the clip was already made.
    if (status.chat_muted || status.mic_muted) {
        return TrayState::Degraded;
    }
    return TrayState::Running;
}

std::wstring DescribeStatus(const EngineStatus& status) {
    if (status.faulted) {
        return L"stopped after a failure no rebuild could fix - see the log";
    }

    std::wstring text;
    const auto add = [&text](std::wstring_view part) {
        if (!text.empty()) {
            text += L", ";
        }
        text += part;
    };

    if (!status.render_live) add(L"cable rebuilding");
    if (!status.chat_live) add(L"chat rebuilding");
    if (!status.mic_live) add(L"microphone rebuilding");
    if (status.chat_muted) add(L"chat muted");
    if (status.mic_muted) add(L"microphone muted");

    if (text.empty()) {
        // Not an error: WASAPI loopback delivers nothing at all on a render
        // endpoint nobody is playing into, so "no packets yet" only means
        // Discord has been quiet since we started.
        add(status.chat_started ? L"running" : L"running, chat endpoint idle so far");
    }
    if (status.restarts > 0) {
        text += std::format(L"; {} rebuild(s)", status.restarts);
    }
    if (status.dropouts > 0) {
        text += std::format(L"; {} dropout(s)", status.dropouts);
    }
    return text;
}

const wchar_t* RoleName(DeviceRole role) {
    switch (role) {
        case DeviceRole::Chat:
            return L"chat source";
        case DeviceRole::Microphone:
            return L"microphone";
        case DeviceRole::Output:
        default:
            return L"cable output";
    }
}

// Turns an endpoint list into menu entries, marking the configured one. A
// configured device that is not in the list gets an entry of its own, so the
// menu can still answer "what is this set to" while the thing is unplugged.
std::vector<DeviceChoice> ChoicesFor(const std::vector<EndpointInfo>& endpoints,
                                     const DeviceSelector& selector) {
    std::vector<DeviceChoice> choices;
    choices.reserve(endpoints.size() + 1);

    bool matched = false;
    for (const EndpointInfo& info : endpoints) {
        DeviceChoice choice;
        choice.id = info.id;
        choice.name = info.friendly_name.empty() ? info.id : info.friendly_name;
        choice.current = !selector.id.empty() && EqualsNoCase(info.id, selector.id);
        matched = matched || choice.current;
        choices.push_back(std::move(choice));
    }

    if (!matched && !selector.IsEmpty()) {
        DeviceChoice missing;
        missing.id = selector.id;
        missing.name = selector.name_contains.empty() ? selector.id : selector.name_contains;
        missing.current = true;
        missing.present = false;
        choices.push_back(std::move(missing));
    }
    return choices;
}

// Holds the engine and answers the tray. Everything here runs on the thread
// that pumps the messages, never on an audio thread.
class MixerSession : public TrayHost {
public:
    MixerSession(const Config& config, const SessionOptions& options, HANDLE stop_event)
        : options_(options), config_(config), stop_event_(stop_event) {}

    int Run(IMMDeviceEnumerator* enumerator);

    void OnToggleChatMute() override {
        engine_.SetChatMuted(!engine_.chat_muted());
        Refresh();
    }
    void OnToggleMicMute() override {
        engine_.SetMicMuted(!engine_.mic_muted());
        Refresh();
    }
    void OnReloadConfig() override;
    void OnWriteStatus() override { engine_.LogStatus(); }
    void OnExit() override {
        LogInfo(L"exit requested from the tray");
        ::SetEvent(stop_event_);
    }

    DeviceMenus Devices() override;
    void OnSelectDevice(DeviceRole role, const std::wstring& id, const std::wstring& name) override;
    void OnSetGain(DeviceRole role, double gain_db) override;
    void OnToggleLimiter() override;
    void OnToggleGate() override;
    void OnSetGateThreshold(double threshold_db) override;
    void OnToggleDrift() override;

    bool ChatMuted() const override { return engine_.chat_muted(); }
    bool MicMuted() const override { return engine_.mic_muted(); }
    bool Mixing() const override { return engine_running_; }
    TraySettings Settings() const override;

private:
    enum class RunOutcome { Stopped, Faulted, Restart };

    // Waits for the configured endpoints to turn up. Returns S_OK once the
    // engine is open, S_FALSE if we were asked to stop while waiting.
    HRESULT StartWithRetries(std::wstring& error);
    RunOutcome Pump();

    bool DevicesConfigured() const;
    std::wstring DescribeMissing() const;
    // Sits in the message loop until something asks for the engine to be
    // (re)started. False means stop.
    bool WaitForRestart();
    void RequestRestart(std::wstring_view reason);

    void FirstRun(IMMDeviceEnumerator* enumerator);
    void SaveSettings();
    void ApplyLive();
    void Refresh();

    SessionOptions options_;
    Config config_;
    HANDLE stop_event_ = nullptr;
    IMMDeviceEnumerator* enumerator_ = nullptr;  // owned by the caller, outlives this
    AudioEngine engine_;
    TrayIcon tray_;
    bool tray_up_ = false;
    // The menu is live during the startup wait and the setup state too, and a
    // reload means something different on either side of this.
    bool engine_running_ = false;
    // Set by anything the running engine cannot absorb - a different endpoint,
    // a drift setting - and noticed by the run loop.
    bool restart_requested_ = false;
    // What the setup state last complained about, so filling in one of three
    // missing devices does not raise the same balloon again.
    std::wstring setup_announced_;
};

bool MixerSession::DevicesConfigured() const {
    return !config_.devices.chat_render.IsEmpty() && !config_.devices.mic_capture.IsEmpty() &&
           !config_.devices.output_render.IsEmpty();
}

std::wstring MixerSession::DescribeMissing() const {
    std::wstring text;
    const auto add = [&text](std::wstring_view part) {
        if (!text.empty()) {
            text += L", ";
        }
        text += part;
    };
    if (config_.devices.chat_render.IsEmpty()) add(L"chat source");
    if (config_.devices.mic_capture.IsEmpty()) add(L"microphone");
    if (config_.devices.output_render.IsEmpty()) add(L"cable output");
    return text;
}

TraySettings MixerSession::Settings() const {
    TraySettings settings;
    settings.chat_gain_db = config_.mix.chat_gain_db;
    settings.mic_gain_db = config_.mix.mic_gain_db;
    settings.limiter = config_.mix.limiter_enabled;
    settings.gate = config_.gate.enabled;
    settings.gate_threshold_db = config_.gate.threshold_db;
    settings.drift = config_.drift.enabled;
    return settings;
}

DeviceMenus MixerSession::Devices() {
    DeviceMenus menus;
    if (enumerator_ == nullptr) {
        return menus;
    }

    // Not probed: opening every endpoint to read its mix format would turn
    // opening a menu into a visible pause, and nothing on the menu shows one.
    std::vector<EndpointInfo> render;
    std::vector<EndpointInfo> capture;
    HRESULT hr = CollectEndpoints(enumerator_, eRender, DEVICE_STATE_ACTIVE, false, render);
    if (FAILED(hr)) {
        LogWarn(L"cannot list render endpoints for the menu: {}", FormatHresult(hr));
    }
    hr = CollectEndpoints(enumerator_, eCapture, DEVICE_STATE_ACTIVE, false, capture);
    if (FAILED(hr)) {
        LogWarn(L"cannot list capture endpoints for the menu: {}", FormatHresult(hr));
    }

    menus.chat = ChoicesFor(render, config_.devices.chat_render);
    menus.microphone = ChoicesFor(capture, config_.devices.mic_capture);
    menus.output = ChoicesFor(render, config_.devices.output_render);
    return menus;
}

void MixerSession::OnSelectDevice(DeviceRole role, const std::wstring& id,
                                  const std::wstring& name) {
    DeviceSelector* target = nullptr;
    switch (role) {
        case DeviceRole::Chat:
            target = &config_.devices.chat_render;
            break;
        case DeviceRole::Microphone:
            target = &config_.devices.mic_capture;
            break;
        case DeviceRole::Output:
            target = &config_.devices.output_render;
            break;
    }
    if (target == nullptr || EqualsNoCase(target->id, id)) {
        return;  // picking what is already picked is not a reason to stop the audio
    }

    target->id = id;
    // The name is written too, as the fallback for the day the id changes: a
    // driver update or a different USB port is enough to invalidate it.
    target->name_contains = name;
    LogInfo(L"{} set to {}", RoleName(role), name);
    SaveSettings();
    RequestRestart(std::format(L"{} changed", RoleName(role)));
}

void MixerSession::OnSetGain(DeviceRole role, double gain_db) {
    double& target =
        role == DeviceRole::Chat ? config_.mix.chat_gain_db : config_.mix.mic_gain_db;
    if (target == gain_db) {
        return;
    }
    target = gain_db;
    LogInfo(L"{} gain set to {:+.1f} dB", RoleName(role), gain_db);
    ApplyLive();
    SaveSettings();
}

void MixerSession::OnToggleLimiter() {
    config_.mix.limiter_enabled = !config_.mix.limiter_enabled;
    LogInfo(L"limiter {}", config_.mix.limiter_enabled ? L"on" : L"off");
    ApplyLive();
    SaveSettings();
}

void MixerSession::OnToggleGate() {
    config_.gate.enabled = !config_.gate.enabled;
    LogInfo(L"noise gate {}", config_.gate.enabled ? L"on" : L"off");
    ApplyLive();
    SaveSettings();
}

void MixerSession::OnSetGateThreshold(double threshold_db) {
    if (config_.gate.threshold_db == threshold_db) {
        return;
    }
    config_.gate.threshold_db = threshold_db;
    LogInfo(L"gate threshold set to {:.0f} dB", threshold_db);
    ApplyLive();
    SaveSettings();
}

void MixerSession::OnToggleDrift() {
    config_.drift.enabled = !config_.drift.enabled;
    LogInfo(L"drift compensation {}", config_.drift.enabled ? L"on" : L"off");
    SaveSettings();
    // Each stream configures its drift controller as it opens, so unlike the
    // mix settings this one cannot be slipped in underneath a running stream.
    RequestRestart(L"drift setting changed");
}

void MixerSession::ApplyLive() {
    if (!engine_running_) {
        return;
    }
    std::vector<std::wstring> blocked;
    engine_.ApplyLiveConfig(config_, blocked);
    for (const std::wstring& section : blocked) {
        // Would be a bug: the menu only changes things the engine takes live,
        // and sends everything else through RequestRestart.
        LogWarn(L"internal: {} cannot be applied live", section);
    }
    Refresh();
}

void MixerSession::SaveSettings() {
    if (options_.config_path.empty()) {
        return;
    }
    std::wstring error;
    if (!SaveConfigFile(options_.config_path, config_, error)) {
        LogError(L"cannot save settings: {}", error);
        if (tray_up_) {
            tray_.Notify(kAppName, std::format(L"settings not saved: {}", error), true);
        }
        return;
    }
    LogDebug(L"settings saved to {}", options_.config_path.wstring());
}

void MixerSession::RequestRestart(std::wstring_view reason) {
    restart_requested_ = true;
    if (engine_running_ && tray_up_) {
        tray_.SetState(TrayState::Starting, std::format(L"restarting: {}", reason));
    }
}

bool MixerSession::WaitForRestart() {
    while (!restart_requested_) {
        if (WaitPumping(stop_event_, kTickMs) == WaitOutcome::Stopped) {
            return false;
        }
    }
    return true;
}

// Nothing configured and nobody to ask: guess, so that the very first run has
// something to show rather than an error about a file the user has never seen.
void MixerSession::FirstRun(IMMDeviceEnumerator* enumerator) {
    DeviceSuggestion suggestion;
    if (FAILED(SuggestDevices(enumerator, suggestion))) {
        return;
    }

    const auto adopt = [](DeviceSelector& target, const DeviceSelector& guess, bool found) {
        if (found && target.IsEmpty()) {
            target = guess;
        }
    };
    adopt(config_.devices.chat_render, suggestion.devices.chat_render, suggestion.chat);
    adopt(config_.devices.mic_capture, suggestion.devices.mic_capture, suggestion.mic);
    adopt(config_.devices.output_render, suggestion.devices.output_render, suggestion.output);

    if (suggestion.chat) {
        LogInfo(L"first run: guessed the chat source is {}",
                config_.devices.chat_render.name_contains);
    }
    if (suggestion.mic) {
        LogInfo(L"first run: guessed the microphone is {}",
                config_.devices.mic_capture.name_contains);
    }
    if (suggestion.output) {
        LogInfo(L"first run: guessed the cable output is {}",
                config_.devices.output_render.name_contains);
    }

    if (!DevicesConfigured()) {
        return;  // the setup state will say what is still missing
    }

    LogWarn(L"these are guesses - check them against the tray menu, in particular the chat "
            L"source, which has to be the endpoint Discord actually plays into");
    SaveSettings();
    if (tray_up_) {
        tray_.Notify(kAppName,
                     L"First run: picked the devices automatically. Check the chat source in the "
                     L"tray menu - it has to be what Discord plays into.",
                     false);
    }
}

HRESULT MixerSession::StartWithRetries(std::wstring& error) {
    const ULONGLONG started = ::GetTickCount64();
    DWORD backoff = config_.resilience.backoff_min_ms;
    unsigned attempts = 0;

    for (;;) {
        error.clear();
        restart_requested_ = false;
        const HRESULT hr = engine_.Start(enumerator_, config_, error);
        if (SUCCEEDED(hr)) {
            if (attempts > 0) {
                LogInfo(L"devices ready after {} attempt(s)", attempts + 1);
            }
            return hr;
        }
        ++attempts;

        // With an icon on screen there is somebody who can fix this, and an
        // endpoint that shows up in ten minutes is still worth picking up - so
        // the wait never expires. Without one, the old contract holds: report
        // the failure and let the exit code say so.
        const bool wait_forever = tray_up_;

        // Read from config_ every time round rather than worked out once: the
        // menu and a reload can both change it while this is still waiting, and
        // a longer window is very often exactly what that change was for.
        const ULONGLONG deadline =
            started + static_cast<ULONGLONG>(config_.resilience.startup_wait_s) * 1000;

        // Nothing to wait for: this is the plain "your config is wrong" case,
        // and it is the caller's job to say so.
        if (!wait_forever &&
            (config_.resilience.startup_wait_s == 0 || ::GetTickCount64() >= deadline)) {
            return hr;
        }

        if (attempts == 1) {
            // Said once. At logon the USB stack is very often still
            // enumerating, and a wall of identical lines helps nobody.
            LogWarn(L"{}", error);
            if (wait_forever) {
                LogInfo(L"waiting for the devices; pick different ones from the tray menu if "
                        L"these are wrong");
            } else {
                LogInfo(L"waiting up to {} s for the devices to appear "
                        L"(resilience.startup_wait_s)",
                        config_.resilience.startup_wait_s);
            }
        }
        if (tray_up_) {
            const bool past_window =
                config_.resilience.startup_wait_s == 0 || ::GetTickCount64() >= deadline;
            tray_.SetState(past_window ? TrayState::Failed : TrayState::Starting,
                           L"waiting for the audio devices - see the log");
        }

        if (WaitPumping(stop_event_, backoff) == WaitOutcome::Stopped) {
            return S_FALSE;
        }
        if (restart_requested_) {
            // Somebody picked a different device. Start from that immediately
            // rather than sleeping out the rest of the backoff.
            backoff = config_.resilience.backoff_min_ms;
            continue;
        }
        backoff = (std::min)(backoff * 2, static_cast<DWORD>(config_.resilience.backoff_max_ms));
    }
}

MixerSession::RunOutcome MixerSession::Pump() {
    for (;;) {
        const WaitOutcome outcome = WaitPumping(stop_event_, kTickMs);
        if (!engine_.Tick(kTickMs, config_.log.stats_interval_s)) {
            return RunOutcome::Faulted;
        }
        if (outcome == WaitOutcome::Stopped) {
            LogInfo(L"stop requested");
            return RunOutcome::Stopped;
        }
        if (restart_requested_) {
            return RunOutcome::Restart;
        }
        Refresh();
    }
}

void MixerSession::Refresh() {
    if (!tray_up_) {
        return;
    }
    if (!engine_running_) {
        return;  // the state here is owned by whichever loop is not the run loop
    }
    const EngineStatus status = engine_.Status();
    tray_.SetState(StateFor(status), DescribeStatus(status));
}

void MixerSession::OnReloadConfig() {
    ConfigLoad loaded = LoadConfigFile(options_.config_path);
    for (const std::wstring& warning : loaded.warnings) {
        LogWarn(L"config: {}", warning);
    }
    if (!loaded.ok) {
        for (const std::wstring& message : loaded.errors) {
            LogError(L"config: {}", message);
        }
        if (tray_up_) {
            tray_.Notify(kAppName, L"settings not reloaded - the file has errors, see the log",
                         true);
        }
        return;
    }
    if (!loaded.file_exists) {
        if (tray_up_) {
            tray_.Notify(kAppName, L"there is no settings file to reload yet", true);
        }
        return;
    }

    if (!engine_running_) {
        // Nothing is open, so nothing is off limits - including the endpoints,
        // which is what somebody reloading from the setup state has most likely
        // just filled in.
        config_ = loaded.config;
        Logger::SetLevel(config_.log.level);
        LogInfo(L"settings reloaded");
        if (tray_up_) {
            tray_.Notify(kAppName, L"settings reloaded", false);
        }
        restart_requested_ = true;
        return;
    }

    std::vector<std::wstring> blocked;
    engine_.ApplyLiveConfig(loaded.config, blocked);
    config_ = loaded.config;

    if (blocked.empty()) {
        if (tray_up_) {
            tray_.Notify(kAppName, L"settings reloaded", false);
        }
        Refresh();
        return;
    }

    // The rest needs the streams reopened. Restarting takes about a second and
    // costs nothing but that second, which beats telling somebody their edit
    // was ignored and leaving them to work out what to do about it.
    std::wstring sections;
    for (const std::wstring& section : blocked) {
        if (!sections.empty()) {
            sections += L", ";
        }
        sections += section;
    }
    LogInfo(L"reload changed {} - restarting the mixer to pick it up", sections);
    if (tray_up_) {
        tray_.Notify(kAppName, L"settings reloaded; restarting the mixer to apply them", false);
    }
    RequestRestart(L"settings reloaded");
}

int MixerSession::Run(IMMDeviceEnumerator* enumerator) {
    enumerator_ = enumerator;

    if (options_.tray) {
        HideOwnConsole();
        std::wstring tray_error;
        tray_up_ = tray_.Create(*this, options_.log_file, options_.config_path, tray_error);
        if (!tray_up_) {
            // Not fatal. A mixer with no icon still mixes, and saying so beats
            // refusing to start over a shell that is having a bad day.
            LogWarn(L"no tray icon: {}", tray_error);
        } else {
            tray_.SetState(TrayState::Starting, L"starting");
        }
    }

    if (!DevicesConfigured() && tray_up_) {
        FirstRun(enumerator);
    }

    int exit_code = kExitOk;
    for (;;) {
        if (!DevicesConfigured()) {
            const std::wstring missing = DescribeMissing();
            if (!tray_up_) {
                LogError(L"no device configured for the {}; run 'vcmic --tray' and pick them from "
                         L"the menu, or fill in the config file",
                         missing);
                exit_code = kExitFailure;
                break;
            }
            tray_.SetState(TrayState::Setup, std::format(L"pick the {} from the menu", missing));
            // Filling in one of three brings us straight back here, so say it
            // again only when what is missing has actually changed.
            if (missing != setup_announced_) {
                setup_announced_ = missing;
                LogWarn(L"waiting to be told which device to use for the {}", missing);
                tray_.Notify(
                    kAppName,
                    std::format(L"Nothing to mix yet: pick the {} from the tray menu.", missing),
                    false);
            }
            restart_requested_ = false;
            if (!WaitForRestart()) {
                break;
            }
            continue;
        }

        std::wstring error;
        const HRESULT hr = StartWithRetries(error);
        if (hr == S_FALSE) {
            LogInfo(L"stop requested before the devices appeared");
            break;
        }
        if (FAILED(hr)) {
            // Only reachable without a tray; with one, StartWithRetries waits
            // until the devices turn up or the user picks different ones.
            LogError(L"{}", error);
            exit_code = kExitFailure;
            break;
        }

        if (!engine_.StartThreads(stop_event_)) {
            engine_.Stop();
            exit_code = kExitFailure;
            break;
        }
        engine_running_ = true;
        restart_requested_ = false;
        LogInfo(L"running; {}",
                options_.tray ? L"use the tray icon to stop" : L"press Ctrl+C to stop");
        Refresh();

        const RunOutcome outcome = Pump();
        engine_.Stop();
        engine_running_ = false;
        engine_.LogSummary();

        if (outcome == RunOutcome::Restart) {
            continue;
        }
        if (outcome == RunOutcome::Faulted) {
            exit_code = kExitFailure;
            if (tray_up_) {
                tray_.SetState(TrayState::Failed, L"stopped after an unrecoverable failure");
                tray_.Notify(kAppName,
                             L"the mixer stopped after an unrecoverable failure - see the log",
                             true);
                // Leave the icon up long enough to be read, then go.
                WaitPumping(stop_event_, 10000);
            }
        }
        break;
    }

    tray_.Destroy();
    tray_up_ = false;
    return exit_code;
}

}  // namespace

int RunMixerSession(IMMDeviceEnumerator* enumerator, const Config& config,
                    const SessionOptions& options, HANDLE stop_event) {
    MixerSession session(config, options, stop_event);
    return session.Run(enumerator);
}

}  // namespace vcmic
