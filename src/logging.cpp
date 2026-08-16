#include "logging.h"

#include "console.h"
#include "strings.h"
#include "win_headers.h"

#include <fstream>
#include <mutex>
#include <system_error>

namespace vcmic {
namespace {

struct LoggerState {
    std::mutex mutex;
    LogSettings settings;
    std::ofstream file;
    std::uint64_t bytes_written = 0;
    bool initialized = false;
};

LoggerState& State() {
    static LoggerState state;
    return state;
}

std::filesystem::path RotatedName(const std::filesystem::path& base, int index) {
    std::filesystem::path rotated = base;
    rotated += std::filesystem::path(L"." + std::to_wstring(index));
    return rotated;
}

// Caller holds the lock and has closed the stream.
void RotateFiles(const LogSettings& settings) {
    std::error_code ec;
    if (settings.keep_files <= 0) {
        std::filesystem::remove(settings.file, ec);
        return;
    }

    std::filesystem::remove(RotatedName(settings.file, settings.keep_files), ec);
    for (int i = settings.keep_files - 1; i >= 1; --i) {
        const std::filesystem::path from = RotatedName(settings.file, i);
        if (std::filesystem::exists(from, ec)) {
            std::filesystem::rename(from, RotatedName(settings.file, i + 1), ec);
        }
    }
    std::filesystem::rename(settings.file, RotatedName(settings.file, 1), ec);
}

void OpenFile(LoggerState& state) {
    if (state.settings.file.empty()) {
        return;
    }

    std::error_code ec;
    const std::filesystem::path parent = state.settings.file.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    const std::uintmax_t existing = std::filesystem::file_size(state.settings.file, ec);
    state.bytes_written = ec ? 0 : static_cast<std::uint64_t>(existing);

    state.file.open(state.settings.file, std::ios::binary | std::ios::app);
    if (!state.file.is_open()) {
        PrintErrLine(L"warning: cannot open log file '{}', logging to console only",
                     state.settings.file.wstring());
    }
}

std::wstring Timestamp() {
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    return std::format(L"{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}", now.wYear, now.wMonth,
                       now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
}

}  // namespace

const wchar_t* LogLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return L"TRACE";
        case LogLevel::Debug: return L"DEBUG";
        case LogLevel::Info:  return L"INFO";
        case LogLevel::Warn:  return L"WARN";
        case LogLevel::Error: return L"ERROR";
        case LogLevel::Off:   return L"OFF";
    }
    return L"?";
}

bool ParseLogLevel(std::wstring_view text, LogLevel& out) {
    if (EqualsNoCase(text, L"trace")) { out = LogLevel::Trace; return true; }
    if (EqualsNoCase(text, L"debug")) { out = LogLevel::Debug; return true; }
    if (EqualsNoCase(text, L"info"))  { out = LogLevel::Info;  return true; }
    if (EqualsNoCase(text, L"warn") || EqualsNoCase(text, L"warning")) {
        out = LogLevel::Warn;
        return true;
    }
    if (EqualsNoCase(text, L"error")) { out = LogLevel::Error; return true; }
    if (EqualsNoCase(text, L"off") || EqualsNoCase(text, L"none")) {
        out = LogLevel::Off;
        return true;
    }
    return false;
}

void Logger::Init(const LogSettings& settings) {
    LoggerState& state = State();
    std::scoped_lock lock(state.mutex);

    if (state.file.is_open()) {
        state.file.close();
    }
    state.settings = settings;
    state.initialized = true;
    OpenFile(state);
}

void Logger::Shutdown() {
    LoggerState& state = State();
    std::scoped_lock lock(state.mutex);
    if (state.file.is_open()) {
        state.file.flush();
        state.file.close();
    }
    state.initialized = false;
}

LogLevel Logger::Level() {
    LoggerState& state = State();
    std::scoped_lock lock(state.mutex);
    return state.initialized ? state.settings.level : LogLevel::Info;
}

void Logger::SetLevel(LogLevel level) {
    LoggerState& state = State();
    std::scoped_lock lock(state.mutex);
    state.settings.level = level;
}

void Logger::Write(LogLevel level, std::wstring_view message) {
    LoggerState& state = State();
    std::scoped_lock lock(state.mutex);

    if (state.initialized && static_cast<int>(level) < static_cast<int>(state.settings.level)) {
        return;
    }

    const std::wstring line = std::format(L"{} [{:<5}] [t={}] {}\n", Timestamp(),
                                          LogLevelName(level), ::GetCurrentThreadId(), message);

    if (state.file.is_open()) {
        const std::string utf8 = Utf8FromWide(line);
        state.file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
        state.file.flush();
        state.bytes_written += utf8.size();

        if (state.settings.max_bytes > 0 && state.bytes_written >= state.settings.max_bytes) {
            state.file.close();
            RotateFiles(state.settings);
            state.bytes_written = 0;
            state.file.open(state.settings.file, std::ios::binary | std::ios::app);
        }
    }

    if (!state.initialized || state.settings.console) {
        if (level >= LogLevel::Warn) {
            ConsoleErr(line);
        } else {
            ConsoleOut(line);
        }
    }
}

}  // namespace vcmic
