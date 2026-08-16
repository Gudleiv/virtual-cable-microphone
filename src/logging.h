#pragma once

#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace vcmic {

enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Off = 5,
};

const wchar_t* LogLevelName(LogLevel level);
bool ParseLogLevel(std::wstring_view text, LogLevel& out);

struct LogSettings {
    std::filesystem::path file;         // empty disables the file sink
    LogLevel level = LogLevel::Info;
    bool console = true;                // mirror to stdout/stderr
    std::uint64_t max_bytes = 2u * 1024u * 1024u;
    int keep_files = 3;
};

// Process-wide logger. Thread-safe.
//
// IMPORTANT (spec 4.10): never call this from the audio path. Capture/render
// threads accumulate counters instead, and a low-priority thread reports them.
class Logger {
public:
    static void Init(const LogSettings& settings);
    static void Shutdown();

    static LogLevel Level();
    // The one setting a config reload can change without reopening the file.
    static void SetLevel(LogLevel level);
    static bool Enabled(LogLevel level) { return static_cast<int>(level) >= static_cast<int>(Level()); }

    static void Write(LogLevel level, std::wstring_view message);
};

template <class... Args>
void LogTrace(std::wformat_string<Args...> fmt, Args&&... args) {
    if (!Logger::Enabled(LogLevel::Trace)) return;
    Logger::Write(LogLevel::Trace, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void LogDebug(std::wformat_string<Args...> fmt, Args&&... args) {
    if (!Logger::Enabled(LogLevel::Debug)) return;
    Logger::Write(LogLevel::Debug, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void LogInfo(std::wformat_string<Args...> fmt, Args&&... args) {
    if (!Logger::Enabled(LogLevel::Info)) return;
    Logger::Write(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void LogWarn(std::wformat_string<Args...> fmt, Args&&... args) {
    if (!Logger::Enabled(LogLevel::Warn)) return;
    Logger::Write(LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void LogError(std::wformat_string<Args...> fmt, Args&&... args) {
    if (!Logger::Enabled(LogLevel::Error)) return;
    Logger::Write(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace vcmic
