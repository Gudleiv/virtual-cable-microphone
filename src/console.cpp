#include "console.h"

#include "strings.h"
#include "win_headers.h"

#include <algorithm>
#include <string>

namespace vcmic {
namespace {

struct Stream {
    HANDLE handle = nullptr;
    bool is_console = false;
    bool resolved = false;
};

Stream& StreamFor(DWORD std_handle) {
    static Stream out;
    static Stream err;
    Stream& stream = (std_handle == STD_OUTPUT_HANDLE) ? out : err;
    if (!stream.resolved) {
        stream.handle = ::GetStdHandle(std_handle);
        DWORD mode = 0;
        stream.is_console = stream.handle != nullptr && stream.handle != INVALID_HANDLE_VALUE &&
                            ::GetConsoleMode(stream.handle, &mode) != FALSE;
        stream.resolved = true;
    }
    return stream;
}

void Write(Stream& stream, std::wstring_view text) {
    if (text.empty() || stream.handle == nullptr || stream.handle == INVALID_HANDLE_VALUE) {
        return;
    }

    if (stream.is_console) {
        // WriteConsoleW is happier with modest chunks than with one huge write.
        constexpr std::size_t kChunk = 8192;
        std::size_t offset = 0;
        while (offset < text.size()) {
            const std::size_t count = (std::min)(kChunk, text.size() - offset);
            DWORD written = 0;
            if (!::WriteConsoleW(stream.handle, text.data() + offset,
                                 static_cast<DWORD>(count), &written, nullptr)) {
                return;
            }
            if (written == 0) {
                return;
            }
            offset += written;
        }
        return;
    }

    const std::string utf8 = Utf8FromWide(text);
    std::size_t offset = 0;
    while (offset < utf8.size()) {
        DWORD written = 0;
        const DWORD count = static_cast<DWORD>((std::min)(std::size_t{1} << 20, utf8.size() - offset));
        if (!::WriteFile(stream.handle, utf8.data() + offset, count, &written, nullptr) ||
            written == 0) {
            return;
        }
        offset += written;
    }
}

}  // namespace

void ConsoleInit() {
    // Only affects the narrow-byte path and anything else that prints via the CRT.
    ::SetConsoleOutputCP(CP_UTF8);
}

void ConsoleOut(std::wstring_view text) { Write(StreamFor(STD_OUTPUT_HANDLE), text); }

void ConsoleErr(std::wstring_view text) { Write(StreamFor(STD_ERROR_HANDLE), text); }

}  // namespace vcmic
