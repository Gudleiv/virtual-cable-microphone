#pragma once

#include <format>
#include <string_view>
#include <utility>

namespace vcmic {

// Device friendly names are localized, so console output goes out as UTF-16 via
// WriteConsoleW when a console is attached, and as UTF-8 bytes when stdout is
// redirected to a file or a pipe. That way `vcmic --list-devices > devices.txt`
// produces a file that survives copy/paste.
void ConsoleInit();

void ConsoleOut(std::wstring_view text);
void ConsoleErr(std::wstring_view text);

template <class... Args>
void Print(std::wformat_string<Args...> fmt, Args&&... args) {
    ConsoleOut(std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void PrintLine(std::wformat_string<Args...> fmt, Args&&... args) {
    ConsoleOut(std::format(fmt, std::forward<Args>(args)...));
    ConsoleOut(L"\n");
}

inline void PrintLine() { ConsoleOut(L"\n"); }

template <class... Args>
void PrintErrLine(std::wformat_string<Args...> fmt, Args&&... args) {
    ConsoleErr(std::format(fmt, std::forward<Args>(args)...));
    ConsoleErr(L"\n");
}

}  // namespace vcmic
