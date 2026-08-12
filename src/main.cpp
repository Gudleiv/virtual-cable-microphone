#include "audio_format.h"
#include "com.h"
#include "config.h"
#include "console.h"
#include "device_registry.h"
#include "hresult.h"
#include "logging.h"
#include "paths.h"
#include "strings.h"
#include "version.h"
#include "win_headers.h"

#include <optional>
#include <string>
#include <vector>

namespace vcmic {
namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitFailure = 2;

struct Options {
    bool list_devices = false;
    bool check_config = false;
    bool show_help = false;
    bool show_version = false;
    bool active_only = false;
    std::optional<LogLevel> log_level;
    std::filesystem::path config_path;
};

void PrintUsage() {
    PrintLine(L"{} {} - {}", kAppName, kAppVersion, kAppSummary);
    PrintLine();
    PrintLine(L"Usage: vcmic [options]");
    PrintLine();
    PrintLine(L"  -l, --list-devices     list every audio endpoint with id, roles and mix format");
    PrintLine(L"      --active-only      with --list-devices: hide disabled/unplugged endpoints");
    PrintLine(L"      --check-config     resolve the configured devices and validate their formats");
    PrintLine(L"  -c, --config <path>    config file (default: config.toml next to the exe)");
    PrintLine(L"      --log-level <lvl>  trace|debug|info|warn|error|off, overrides the config");
    PrintLine(L"  -v, --version          print the version and exit");
    PrintLine(L"  -h, --help             print this help and exit");
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
    log_settings.console = false;  // the console output below is the report
    log_settings.max_bytes = loaded.config.log.max_bytes;
    log_settings.keep_files = loaded.config.log.keep_files;
    Logger::Init(log_settings);
    LogInfo(L"{} {} starting ({})", kAppName, kAppVersion,
            options.check_config ? L"--check-config" : L"no mode selected");

    int exit_code = kExitOk;
    if (options.check_config) {
        exit_code = RunCheckConfig(loaded.config, config_path, loaded.file_exists);
        PrintLine(L"log file    : {}", log_settings.file.wstring());
    } else {
        PrintUsage();
        PrintLine();
        PrintLine(L"The mixing engine is not implemented yet (stage 2).");
        PrintLine(L"Start with --list-devices, then fill config.toml and run --check-config.");
    }

    LogInfo(L"exiting with code {}", exit_code);
    Logger::Shutdown();
    return exit_code;
}

}  // namespace
}  // namespace vcmic

int wmain(int argc, wchar_t** argv) { return vcmic::Run(argc, argv); }
