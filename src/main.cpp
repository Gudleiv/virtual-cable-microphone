#include "audio_format.h"
#include "autostart.h"
#include "com.h"
#include "config.h"
#include "console.h"
#include "device_registry.h"
#include "hresult.h"
#include "logging.h"
#include "paths.h"
#include "session.h"
#include "strings.h"
#include "version.h"
#include "win_headers.h"

#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <optional>
#include <string>
#include <vector>

namespace vcmic {
namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitFailure = 2;

// Spec 4.2 again: the autostart task lives in the user's session, so the guard
// against a second copy is per-session too. Two mixers on one cable would each
// render half the frames and neither would sound right.
constexpr const wchar_t* kSingletonName = L"Local\\vcmic.mixer";

constexpr std::uint32_t kDefaultAutostartDelayS = 30;

struct Options {
    bool list_devices = false;
    bool check_config = false;
    bool show_help = false;
    bool show_version = false;
    bool active_only = false;
    bool tray = false;
    bool install_autostart = false;
    bool uninstall_autostart = false;
    bool autostart_status = false;
    std::uint32_t autostart_delay_s = kDefaultAutostartDelayS;
    std::optional<LogLevel> log_level;
    std::filesystem::path config_path;
};

void PrintUsage() {
    PrintLine(L"{} {} - {}", kAppName, kAppVersion, kAppSummary);
    PrintLine();
    PrintLine(L"Usage: vcmic [options]");
    PrintLine();
    PrintLine(L"With no mode option, vcmic runs the mixer until Ctrl+C.");
    PrintLine();
    PrintLine(L"  -l, --list-devices     list every audio endpoint with id, roles and mix format");
    PrintLine(L"      --active-only      with --list-devices: hide disabled/unplugged endpoints");
    PrintLine(L"      --check-config     resolve the configured devices and validate their formats");
    PrintLine(L"  -c, --config <path>    config file (default: config.toml next to the exe)");
    PrintLine(L"      --log-level <lvl>  trace|debug|info|warn|error|off, overrides the config");
    PrintLine(L"  -t, --tray             run with an icon in the notification area");
    PrintLine(L"  -v, --version          print the version and exit");
    PrintLine(L"  -h, --help             print this help and exit");
    PrintLine();
    PrintLine(L"Autostart (a per-user Task Scheduler job; no administrator rights needed):");
    PrintLine(L"      --install-autostart    start vcmic --tray when this user logs on");
    PrintLine(L"      --logon-delay <s>      with --install-autostart: wait this many seconds");
    PrintLine(L"                             after logon, so USB enumeration finishes first");
    PrintLine(L"                             (default {})", kDefaultAutostartDelayS);
    PrintLine(L"      --uninstall-autostart  remove that job");
    PrintLine(L"      --autostart-status     print what is currently registered");
    PrintLine();
    PrintLine(L"Fill config.toml from the ids printed by --list-devices, then run --check-config.");
}

bool ParseArguments(int argc, wchar_t** argv, Options& options, std::wstring& error) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const auto next = [&](const wchar_t* name, std::wstring& value) {
            if (i + 1 >= argc) {
                error = std::format(L"option {} needs a value", name);
                return false;
            }
            value = argv[++i];
            return true;
        };

        if (arg == L"-l" || arg == L"--list-devices") {
            options.list_devices = true;
        } else if (arg == L"--active-only") {
            options.active_only = true;
        } else if (arg == L"--check-config") {
            options.check_config = true;
        } else if (arg == L"-t" || arg == L"--tray") {
            options.tray = true;
        } else if (arg == L"--install-autostart") {
            options.install_autostart = true;
        } else if (arg == L"--uninstall-autostart") {
            options.uninstall_autostart = true;
        } else if (arg == L"--autostart-status") {
            options.autostart_status = true;
        } else if (arg == L"--logon-delay") {
            std::wstring value;
            if (!next(L"--logon-delay", value)) {
                return false;
            }
            wchar_t* end = nullptr;
            const unsigned long seconds = std::wcstoul(value.c_str(), &end, 10);
            if (end == value.c_str() || *end != L'\0' || seconds > 3600) {
                error = std::format(L"--logon-delay wants 0..3600 seconds, not '{}'", value);
                return false;
            }
            options.autostart_delay_s = static_cast<std::uint32_t>(seconds);
        } else if (arg == L"-c" || arg == L"--config") {
            std::wstring value;
            if (!next(L"--config", value)) {
                return false;
            }
            options.config_path = value;
        } else if (arg == L"--log-level") {
            std::wstring value;
            if (!next(L"--log-level", value)) {
                return false;
            }
            LogLevel level = LogLevel::Info;
            if (!ParseLogLevel(value, level)) {
                error = std::format(L"unknown log level '{}'", value);
                return false;
            }
            options.log_level = level;
        } else if (arg == L"-v" || arg == L"--version") {
            options.show_version = true;
        } else if (arg == L"-h" || arg == L"--help" || arg == L"-?" || arg == L"/?") {
            options.show_help = true;
        } else {
            error = std::format(L"unknown option '{}'", arg);
            return false;
        }
    }
    return true;
}

std::wstring RolesOf(const EndpointInfo& info) {
    std::wstring roles;
    const auto add = [&roles](const wchar_t* name) {
        if (!roles.empty()) {
            roles += L", ";
        }
        roles += name;
    };
    if (info.default_console) add(L"Console");
    if (info.default_multimedia) add(L"Multimedia");
    if (info.default_communications) add(L"Communications");
    return roles.empty() ? std::wstring(L"-") : roles;
}

double PeriodMs(REFERENCE_TIME period) { return static_cast<double>(period) / 10000.0; }

void PrintEndpoint(std::size_t index, const EndpointInfo& info) {
    PrintLine(L"[{}] {}", index, info.friendly_name.empty() ? L"<no name>" : info.friendly_name);
    PrintLine(L"      id          : {}", info.id);
    PrintLine(L"      state       : {}{}", DeviceStateName(info.state),
              info.IsActive() ? L"" : L"   (cannot be opened while it is not ACTIVE)");
    PrintLine(L"      roles       : {}", RolesOf(info));
    PrintLine(L"      adapter     : {}", info.adapter_name.empty() ? L"-" : info.adapter_name);
    PrintLine(L"      description : {}   [form factor: {}]",
              info.description.empty() ? L"-" : info.description, FormFactorName(info.form_factor));

    if (const WAVEFORMATEX* format = info.Format()) {
        PrintLine(L"      mix format  : {}", DescribeFormat(format));
    } else if (!info.probe_error.empty()) {
        PrintLine(L"      mix format  : <unavailable> ({})", info.probe_error);
    } else {
        PrintLine(L"      mix format  : <not probed>");
    }

    if (info.default_period != 0 || info.min_period != 0) {
        PrintLine(L"      period      : default {:.3f} ms, minimum {:.3f} ms",
                  PeriodMs(info.default_period), PeriodMs(info.min_period));
    }
    PrintLine();
}

const EndpointInfo* FindByName(const std::vector<EndpointInfo>& endpoints,
                               std::wstring_view fragment) {
    for (const EndpointInfo& info : endpoints) {
        if (info.IsActive() && ContainsNoCase(info.friendly_name, fragment)) {
            return &info;
        }
    }
    return nullptr;
}

const EndpointInfo* FindDefaultCommunications(const std::vector<EndpointInfo>& endpoints) {
    for (const EndpointInfo& info : endpoints) {
        if (info.IsActive() && info.default_communications) {
            return &info;
        }
    }
    return nullptr;
}

void PrintConfigLine(const wchar_t* key, const EndpointInfo* info) {
    if (info == nullptr) {
        PrintLine(L"{} = \"\"   # not detected - fill this in by hand", key);
        return;
    }
    PrintLine(L"{} = \"{}\"   # {}", key, info->id, info->friendly_name);
}

// A guess, printed only to save typing. The user still has to confirm that the
// chat endpoint really is the one Discord plays into.
void PrintSuggestedConfig(const std::vector<EndpointInfo>& render,
                          const std::vector<EndpointInfo>& capture) {
    const EndpointInfo* cable_in = FindByName(render, L"cable-a input");
    if (cable_in == nullptr) {
        cable_in = FindByName(render, L"cable input");
    }
    const EndpointInfo* chat = FindDefaultCommunications(render);
    if (chat == nullptr) {
        chat = FindByName(render, L"headset");
    }
    const EndpointInfo* mic = FindDefaultCommunications(capture);
    if (mic == nullptr) {
        mic = FindByName(capture, L"microphone");
    }

    PrintLine(L"=== Suggested [devices] block (verify before trusting it) ===");
    PrintLine();
    PrintLine(L"[devices]");
    PrintConfigLine(L"chat_render_id  ", chat);
    PrintConfigLine(L"mic_capture_id  ", mic);
    PrintConfigLine(L"output_render_id", cable_in);
    PrintLine();
    PrintLine(L"chat_render_id must be the endpoint Discord plays into (the GC7 chat side),");
    PrintLine(L"output_render_id must be the CABLE-A *Input* (render) endpoint.");
    PrintLine(L"In ShadowPlay pick the matching CABLE-A *Output* (capture) endpoint as microphone.");
    PrintLine();
}

int RunListDevices(const Options& options) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CreateDeviceEnumerator(enumerator);
    if (FAILED(hr)) {
        PrintErrLine(L"error: cannot create the device enumerator: {}", FormatHresult(hr));
        return kExitFailure;
    }

    const DWORD state_mask = options.active_only ? DEVICE_STATE_ACTIVE : DEVICE_STATEMASK_ALL;

    std::vector<EndpointInfo> render;
    std::vector<EndpointInfo> capture;
    hr = CollectEndpoints(enumerator.Get(), eRender, state_mask, true, render);
    if (FAILED(hr)) {
        PrintErrLine(L"error: enumerating render endpoints failed: {}", FormatHresult(hr));
        return kExitFailure;
    }
    hr = CollectEndpoints(enumerator.Get(), eCapture, state_mask, true, capture);
    if (FAILED(hr)) {
        PrintErrLine(L"error: enumerating capture endpoints failed: {}", FormatHresult(hr));
        return kExitFailure;
    }

    PrintLine(L"{} {} - audio endpoints", kAppName, kAppVersion);
    PrintLine(L"{}", options.active_only ? L"(active endpoints only)"
                                         : L"(all states; disabled and unplugged included)");
    PrintLine();

    PrintLine(L"=== RENDER endpoints ({}) ===", render.size());
    PrintLine();
    for (std::size_t i = 0; i < render.size(); ++i) {
        PrintEndpoint(i + 1, render[i]);
    }

    PrintLine(L"=== CAPTURE endpoints ({}) ===", capture.size());
    PrintLine();
    for (std::size_t i = 0; i < capture.size(); ++i) {
        PrintEndpoint(i + 1, capture[i]);
    }

    PrintSuggestedConfig(render, capture);
    return kExitOk;
}

struct EngineDevice {
    const wchar_t* label;
    const wchar_t* purpose;
    EDataFlow flow;
    const DeviceSelector* selector;
};

int RunCheckConfig(const Config& config, const std::filesystem::path& config_path,
                   bool config_exists) {
    PrintLine(L"config file : {}{}", config_path.wstring(),
              config_exists ? L"" : L"   (missing - defaults in use)");
    PrintLine();

    std::vector<std::wstring> errors;
    ValidateForEngine(config, errors);
    for (const std::wstring& message : errors) {
        PrintErrLine(L"error: {}", message);
    }
    if (!errors.empty()) {
        return kExitFailure;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CreateDeviceEnumerator(enumerator);
    if (FAILED(hr)) {
        PrintErrLine(L"error: cannot create the device enumerator: {}", FormatHresult(hr));
        return kExitFailure;
    }

    const EngineDevice devices[] = {
        {L"chat (loopback source)", L"WASAPI loopback capture", eRender, &config.devices.chat_render},
        {L"microphone", L"shared-mode capture", eCapture, &config.devices.mic_capture},
        {L"cable output", L"shared-mode render, clock master", eRender,
         &config.devices.output_render},
    };

    bool ok = true;
    for (const EngineDevice& device : devices) {
        PrintLine(L"--- {} ({}) ---", device.label, device.purpose);

        ResolvedDevice resolved;
        std::wstring error;
        hr = ResolveDevice(enumerator.Get(), device.flow, *device.selector, true, resolved, error);
        if (FAILED(hr)) {
            PrintErrLine(L"  NOT RESOLVED: {}", error);
            PrintLine();
            ok = false;
            continue;
        }

        PrintLine(L"  name   : {}", resolved.info.friendly_name);
        PrintLine(L"  id     : {}", resolved.info.id);
        PrintLine(L"  matched: {}", resolved.matched == MatchKind::ById ? L"by endpoint id"
                                                                        : L"by NAME FRAGMENT");
        if (resolved.matched == MatchKind::ByName) {
            PrintLine(L"  warning: name matching is fragile; copy the id above into the config");
            LogWarn(L"device '{}' was matched by name fragment, not by id", resolved.info.friendly_name);
        }

        const WAVEFORMATEX* format = resolved.info.Format();
        if (format == nullptr) {
            PrintErrLine(L"  format : <unavailable> ({})", resolved.info.probe_error);
            ok = false;
        } else {
            const FormatInfo info = InspectFormat(format);
            PrintLine(L"  format : {}", DescribeFormat(format));

            if (info.sample_rate != config.audio.sample_rate) {
                const wchar_t* fix =
                    device.flow == eRender && device.selector == &config.devices.output_render
                        ? L"set Internal Sample Rate in VBCABLE_ControlPanel.exe"
                        : L"Sound control panel -> device -> Advanced -> Default Format";
                if (config.audio.require_sample_rate) {
                    PrintErrLine(L"  ERROR  : {} Hz, but audio.sample_rate is {} Hz - {}",
                                 info.sample_rate, config.audio.sample_rate, fix);
                    ok = false;
                } else {
                    PrintLine(L"  warning: {} Hz differs from audio.sample_rate {} Hz - {}",
                              info.sample_rate, config.audio.sample_rate, fix);
                }
            }
            if (info.sample_type == SampleType::Unknown) {
                PrintErrLine(L"  ERROR  : unsupported sample type in the mix format");
                ok = false;
            }
            if (device.selector == &config.devices.output_render && info.channels != 2) {
                PrintLine(L"  note   : the cable endpoint is {} ch; a stereo mix will be mapped onto it",
                          info.channels);
            }
            if (device.selector == &config.devices.chat_render && info.channels > 2) {
                PrintLine(L"  note   : {} ch loopback (GC7 SURR mode?) will be downmixed to stereo",
                          info.channels);
            }
        }
        PrintLine();
    }

    PrintLine(L"target ring fill : {:.1f} ms", config.audio.target_buffer_ms);
    PrintLine(L"ring capacity    : {:.1f} ms", config.audio.ring_capacity_ms);
    PrintLine(L"drift correction : {}", config.drift.enabled ? L"enabled" : L"disabled");
    PrintLine(L"limiter          : {}", config.mix.limiter_enabled ? L"enabled" : L"disabled");
    PrintLine(L"noise gate       : {}", config.gate.enabled ? L"enabled" : L"disabled");
    PrintLine();
    PrintLine(L"{}", ok ? L"config check PASSED" : L"config check FAILED");
    return ok ? kExitOk : kExitFailure;
}

void PrintAutostartInfo(const AutostartInfo& info) {
    if (!info.installed) {
        PrintLine(L"autostart: not registered");
        PrintLine(L"  run 'vcmic --install-autostart' to start the mixer at logon");
        return;
    }

    PrintLine(L"autostart: registered as the task '\\{}'", kAutostartTaskName);
    PrintLine(L"  state       : {}{}", info.state.empty() ? L"?" : info.state,
              info.enabled ? L"" : L"   (disabled)");
    PrintLine(L"  runs as     : {}", info.user.empty() ? L"?" : info.user);
    PrintLine(L"  command     : {} {}", info.command, info.arguments);
    PrintLine(L"  logon delay : {} s", info.delay_s);
    if (!info.last_run.empty()) {
        PrintLine(L"  last run    : {}   (exit code {})", info.last_run, info.last_result);
    }
    PrintLine(L"  edit or remove it in taskschd.msc, or with 'vcmic --uninstall-autostart'");
}

int RunAutostart(const Options& options) {
    std::wstring error;

    if (options.uninstall_autostart) {
        const HRESULT hr = RemoveAutostart(error);
        if (FAILED(hr)) {
            PrintErrLine(L"error: {}", error);
            return kExitFailure;
        }
        PrintLine(L"{}", hr == S_FALSE ? L"autostart was not registered; nothing to remove"
                                       : L"autostart removed");
        return kExitOk;
    }

    if (options.install_autostart) {
        // The task runs the same executable with --tray, plus whatever config
        // this invocation was pointed at: installing with -c and then starting
        // without it would silently mix a different pair of devices.
        std::wstring arguments = L"--tray";
        if (!options.config_path.empty()) {
            arguments += std::format(L" --config \"{}\"", options.config_path.wstring());
        }

        std::vector<std::wstring> notes;
        const HRESULT hr = InstallAutostart(options.autostart_delay_s, arguments, notes, error);
        if (FAILED(hr)) {
            PrintErrLine(L"error: {}", error);
            return kExitFailure;
        }
        PrintLine(L"autostart registered.");
        for (const std::wstring& note : notes) {
            PrintErrLine(L"warning: the scheduler did not accept {}", note);
        }
        PrintLine();
    }

    AutostartInfo info;
    const HRESULT hr = QueryAutostart(info, error);
    if (FAILED(hr)) {
        PrintErrLine(L"error: {}", error);
        return kExitFailure;
    }
    PrintAutostartInfo(info);

    if (options.install_autostart) {
        PrintLine();
        PrintLine(L"The delay matters: at logon the USB stack is often still enumerating, and the");
        PrintLine(L"mixer cannot open a sound card that is not there yet. resilience.startup_wait_s");
        PrintLine(L"in the config keeps it retrying after that, so the two cover the same gap from");
        PrintLine(L"either end.");
    }
    return kExitOk;
}

// Signalled by the console control handler, by the tray's Exit and by a
// Windows shutdown.
HANDLE g_stop_event = nullptr;

BOOL WINAPI ConsoleControlHandler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            if (g_stop_event != nullptr) {
                ::SetEvent(g_stop_event);
            }
            return TRUE;
        default:
            return FALSE;
    }
}

// Refuses to be the second mixer in this session. Held for the lifetime of the
// process; the kernel drops it if we die without cleaning up.
class SingleInstance {
public:
    SingleInstance() = default;
    ~SingleInstance() {
        if (handle_ != nullptr) {
            ::CloseHandle(handle_);
        }
    }

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    bool Acquire(const wchar_t* name) {
        handle_ = ::CreateMutexW(nullptr, TRUE, name);
        if (handle_ == nullptr) {
            return true;  // cannot tell; better to run than to refuse
        }
        return ::GetLastError() != ERROR_ALREADY_EXISTS;
    }

private:
    HANDLE handle_ = nullptr;
};

int RunEngine(const Config& config, const Options& options,
              const std::filesystem::path& config_path, const std::filesystem::path& log_file) {
    SingleInstance singleton;
    if (!singleton.Acquire(kSingletonName)) {
        LogError(L"another vcmic is already mixing in this session; this one is exiting");
        PrintErrLine(L"error: vcmic is already running in this session");
        return kExitFailure;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CreateDeviceEnumerator(enumerator);
    if (FAILED(hr)) {
        LogError(L"cannot create the device enumerator: {}", FormatHresult(hr));
        return kExitFailure;
    }

    g_stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stop_event == nullptr) {
        LogError(L"CreateEvent failed: {}", FormatHresult(HRESULT_FROM_WIN32(::GetLastError())));
        return kExitFailure;
    }
    ::SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);

    SessionOptions session;
    session.tray = options.tray;
    session.config_path = config_path;
    session.log_file = log_file;
    const int exit_code = RunMixerSession(enumerator.Get(), config, session, g_stop_event);

    ::SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
    ::CloseHandle(g_stop_event);
    g_stop_event = nullptr;
    return exit_code;
}

int Run(int argc, wchar_t** argv) {
    ConsoleInit();

    Options options;
    std::wstring parse_error;
    if (!ParseArguments(argc, argv, options, parse_error)) {
        PrintErrLine(L"error: {}", parse_error);
        PrintErrLine(L"run 'vcmic --help' for usage");
        return kExitUsage;
    }

    if (options.show_version) {
        PrintLine(L"{} {}", kAppName, kAppVersion);
        return kExitOk;
    }
    if (options.show_help) {
        PrintUsage();
        return kExitOk;
    }

    const ComApartment apartment;
    if (!apartment.ok()) {
        PrintErrLine(L"error: CoInitializeEx failed: {}", FormatHresult(apartment.hr()));
        return kExitFailure;
    }

    if (options.list_devices) {
        // No config and no log file needed just to print the endpoint table.
        return RunListDevices(options);
    }
    if (options.install_autostart || options.uninstall_autostart || options.autostart_status) {
        // Same: registering a scheduled task says nothing about whether the
        // devices in the config exist today.
        return RunAutostart(options);
    }

    const std::filesystem::path config_path =
        options.config_path.empty() ? DefaultConfigPath() : options.config_path;
    ConfigLoad loaded = LoadConfigFile(config_path);
    if (options.log_level.has_value()) {
        loaded.config.log.level = *options.log_level;
    }

    for (const std::wstring& warning : loaded.warnings) {
        PrintErrLine(L"config warning: {}", warning);
    }
    for (const std::wstring& error : loaded.errors) {
        PrintErrLine(L"config error: {}", error);
    }
    if (!loaded.ok) {
        return kExitFailure;
    }

    LogSettings log_settings;
    log_settings.file = ResolveRelativeToExe(loaded.config.log.file);
    log_settings.level = loaded.config.log.level;
    // --check-config prints its own report, so the log stays file-only there.
    log_settings.console = options.check_config ? false : loaded.config.log.console;
    log_settings.max_bytes = loaded.config.log.max_bytes;
    log_settings.keep_files = loaded.config.log.keep_files;
    Logger::Init(log_settings);
    LogInfo(L"{} {} starting ({})", kAppName, kAppVersion,
            options.check_config ? L"--check-config" : options.tray ? L"mixer, tray" : L"mixer");

    int exit_code = kExitOk;
    if (options.check_config) {
        exit_code = RunCheckConfig(loaded.config, config_path, loaded.file_exists);
        PrintLine(L"log file    : {}", log_settings.file.wstring());
    } else {
        std::vector<std::wstring> engine_errors;
        ValidateForEngine(loaded.config, engine_errors);
        if (!engine_errors.empty()) {
            for (const std::wstring& message : engine_errors) {
                LogError(L"{}", message);
            }
            exit_code = kExitFailure;
        } else {
            LogInfo(L"config: {}", config_path.wstring());
            LogInfo(L"log   : {}", log_settings.file.wstring());
            exit_code = RunEngine(loaded.config, options, config_path, log_settings.file);
        }
    }

    LogInfo(L"exiting with code {}", exit_code);
    Logger::Shutdown();
    return exit_code;
}

}  // namespace
}  // namespace vcmic

int wmain(int argc, wchar_t** argv) { return vcmic::Run(argc, argv); }
