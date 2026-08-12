#include "paths.h"

#include "win_headers.h"

#include <string>
#include <vector>

namespace vcmic {

std::filesystem::path ExecutablePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD copied =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return std::filesystem::path();
        }
        if (copied < buffer.size()) {
            return std::filesystem::path(std::wstring(buffer.data(), copied));
        }
        if (buffer.size() >= 32768) {
            return std::filesystem::path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path ExecutableDirectory() {
    const std::filesystem::path exe = ExecutablePath();
    return exe.empty() ? std::filesystem::path() : exe.parent_path();
}

}  // namespace vcmic
