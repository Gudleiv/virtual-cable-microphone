#include "session.h"

#include "audio_engine.h"
#include "hresult.h"
#include "logging.h"
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

    bool ChatMuted() const override { return engine_.chat_muted(); }
    bool MicMuted() const override { return engine_.mic_muted(); }

private:
    // Waits for the configured endpoints to turn up. Returns S_OK once the
    // engine is open, S_FALSE if we were asked to stop while waiting.
    HRESULT StartWithRetries(IMMDeviceEnumerator* enumerator, std::wstring& error);
    void Refresh();

    SessionOptions options_;
    Config config_;
    HANDLE stop_event_ = nullptr;
    AudioEngine engine_;
    TrayIcon tray_;
    bool tray_up_ = false;
    // The menu is live during the startup wait too, and a reload means
    // something different on either side of this.
    bool engine_running_ = false;
};

HRESULT MixerSession::StartWithRetries(IMMDeviceEnumerator* enumerator, std::wstring& error) {
    const ULONGLONG started = ::GetTickCount64();
    DWORD backoff = config_.resilience.backoff_min_ms;
    unsigned attempts = 0;

    for (;;) {
        error.clear();
        const HRESULT hr = engine_.Start(enumerator, config_, error);
        if (SUCCEEDED(hr)) {
            if (attempts > 0) {
                LogInfo(L"devices ready after {} attempt(s)", attempts + 1);
            }
            return hr;
        }
        ++attempts;

        // Read from config_ every time round rather than worked out once: the
        // tray can reload the config while this is still waiting, and a longer
        // window is very often exactly what that reload was for.
        const ULONGLONG deadline =
            started + static_cast<ULONGLONG>(config_.resilience.startup_wait_s) * 1000;

        // Nothing to wait for: this is the plain "your config is wrong" case,
        // and it is the caller's job to say so.
        if (config_.resilience.startup_wait_s == 0 || ::GetTickCount64() >= deadline) {
            return hr;
        }

        if (attempts == 1) {
            // Said once. At logon the USB stack is very often still
            // enumerating, and a wall of identical lines helps nobody.
            LogWarn(L"{}", error);
            LogInfo(L"waiting up to {} s for the devices to appear (resilience.startup_wait_s)",
                    config_.resilience.startup_wait_s);
        }
        if (tray_up_) {
            tray_.SetState(TrayState::Starting, L"waiting for the audio devices");
        }

        if (WaitPumping(stop_event_, backoff) == WaitOutcome::Stopped) {
            return S_FALSE;
        }
        backoff = (std::min)(backoff * 2, static_cast<DWORD>(config_.resilience.backoff_max_ms));
    }
}

void MixerSession::Refresh() {
    if (!tray_up_) {
        return;
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
            tray_.Notify(kAppName, L"config not reloaded - it has errors, see the log", true);
        }
        return;
    }

    std::vector<std::wstring> invalid;
    ValidateForEngine(loaded.config, invalid);
    if (!invalid.empty()) {
        for (const std::wstring& message : invalid) {
            LogError(L"config: {}", message);
        }
        if (tray_up_) {
            tray_.Notify(kAppName, L"config not reloaded - it would not start, see the log", true);
        }
        return;
    }

    if (!engine_running_) {
        // Nothing is open yet, so nothing is off limits - including the device
        // ids, which is exactly what somebody reloading during the startup
        // window has most likely just corrected. The retry loop reads config_
        // on every attempt, so the next one uses this.
        config_ = loaded.config;
        Logger::SetLevel(config_.log.level);
        LogInfo(L"config reloaded before the engine started; the next attempt will use all of it");
        if (tray_up_) {
            tray_.Notify(kAppName, L"config reloaded; retrying with it", false);
        }
        return;
    }

    std::vector<std::wstring> blocked;
    engine_.ApplyLiveConfig(loaded.config, blocked);
    config_ = engine_.config();

    if (tray_up_) {
        std::wstring message = L"config reloaded";
        if (!blocked.empty()) {
            message += L"; still running the old ";
            for (std::size_t i = 0; i < blocked.size(); ++i) {
                if (i != 0) {
                    message += L", ";
                }
                message += blocked[i];
            }
            message += L" - those need a restart";
        }
        tray_.Notify(kAppName, message, false);
    }
    Refresh();
}

int MixerSession::Run(IMMDeviceEnumerator* enumerator) {
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

    std::wstring error;
    const HRESULT hr = StartWithRetries(enumerator, error);
    if (hr == S_FALSE) {
        LogInfo(L"stop requested before the devices appeared");
        return kExitOk;
    }
    if (FAILED(hr)) {
        LogError(L"{}", error);
        if (tray_up_) {
            tray_.SetState(TrayState::Failed, L"could not open the audio devices");
            tray_.Notify(kAppName, L"could not open the audio devices - see the log", true);
            // Leave the icon up long enough to be read, then go.
            WaitPumping(stop_event_, 10000);
        }
        return kExitFailure;
    }

    if (!engine_.StartThreads(stop_event_)) {
        engine_.Stop();
        return kExitFailure;
    }
    engine_running_ = true;
    LogInfo(L"running; {}", options_.tray ? L"use the tray icon to stop" : L"press Ctrl+C to stop");
    Refresh();

    bool faulted = false;
    for (;;) {
        const WaitOutcome outcome = WaitPumping(stop_event_, kTickMs);
        if (!engine_.Tick(kTickMs, config_.log.stats_interval_s)) {
            faulted = true;
            break;
        }
        if (outcome == WaitOutcome::Stopped) {
            LogInfo(L"stop requested");
            break;
        }
        Refresh();
    }

    if (faulted && tray_up_) {
        tray_.SetState(TrayState::Failed, L"stopped after an unrecoverable failure");
        tray_.Notify(kAppName, L"the mixer stopped after an unrecoverable failure - see the log",
                     true);
        WaitPumping(stop_event_, 10000);
    }

    engine_.Stop();
    engine_running_ = false;
    engine_.LogSummary();
    tray_.Destroy();
    tray_up_ = false;
    return faulted ? kExitFailure : kExitOk;
}

}  // namespace

int RunMixerSession(IMMDeviceEnumerator* enumerator, const Config& config,
                    const SessionOptions& options, HANDLE stop_event) {
    MixerSession session(config, options, stop_event);
    return session.Run(enumerator);
}

}  // namespace vcmic
