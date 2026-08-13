#pragma once

#include "logging.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vcmic {

// How a configured endpoint is found. `id` is authoritative (spec 4.3);
// `name_contains` is a fallback that logs a warning when it is used, because
// friendly names are localized and change across firmware/port changes.
struct DeviceSelector {
    std::wstring id;
    std::wstring name_contains;

    bool IsEmpty() const { return id.empty() && name_contains.empty(); }
};

struct DevicesConfig {
    DeviceSelector chat_render;    // loopback source: Discord's output, e.g. Headset (GC7)
    DeviceSelector mic_capture;    // hardware microphone, shared mode
    DeviceSelector output_render;  // VB-CABLE A input side
};

struct AudioConfig {
    std::uint32_t sample_rate = 48000;
    bool require_sample_rate = true;  // spec 4.5: fail loudly instead of resampling
    double target_buffer_ms = 25.0;   // steady-state fill level of each ring
    double ring_capacity_ms = 250.0;  // headroom before an overrun is unavoidable
};

struct MixConfig {
    double chat_gain_db = 0.0;
    double mic_gain_db = 0.0;
    double gain_smoothing_ms = 10.0;
    bool limiter_enabled = true;
    double limiter_threshold_db = -1.0;
    double limiter_release_ms = 80.0;
};

struct GateConfig {
    bool enabled = false;
    double threshold_db = -45.0;
    double attack_ms = 5.0;
    double hold_ms = 120.0;
    double release_ms = 150.0;
};

struct DriftConfig {
    bool enabled = true;
    double measure_window_s = 1.0;        // spec 4.6: average the fill before acting on it
    double response_s = 30.0;             // time constant of the correction loop
    double max_rate_correction = 0.001;   // +-0.1 %
};

struct ResilienceConfig {
    std::uint32_t backoff_min_ms = 100;
    std::uint32_t backoff_max_ms = 5000;
    bool keep_chat_clock_alive = false;  // spec 4.9: silent render client on the chat endpoint
};

struct LoggingConfig {
    LogLevel level = LogLevel::Info;
    std::wstring file = L"vcmic.log";  // relative paths resolve next to the executable
    std::uint64_t max_bytes = 2u * 1024u * 1024u;
    int keep_files = 3;
    bool console = true;
    std::uint32_t stats_interval_s = 30;
};

struct Config {
    DevicesConfig devices;
    AudioConfig audio;
    MixConfig mix;
    GateConfig gate;
    DriftConfig drift;
    ResilienceConfig resilience;
    LoggingConfig log;
};

struct ConfigLoad {
    Config config;
    bool file_exists = false;
    bool ok = false;  // false only on syntax or range errors
    std::vector<std::wstring> errors;
    std::vector<std::wstring> warnings;
};

// Missing file is not an error: defaults are returned with file_exists = false.
ConfigLoad LoadConfigFile(const std::filesystem::path& path);

// Checks the fields the audio engine cannot start without.
void ValidateForEngine(const Config& config, std::vector<std::wstring>& errors);

std::filesystem::path DefaultConfigPath();

// Resolves a possibly relative path from the config against the executable dir.
std::filesystem::path ResolveRelativeToExe(const std::wstring& path);

}  // namespace vcmic
