#include "paths.h"

#include "win_headers.h"

#include <shlobj.h>

#include <string>
#include <vector>

namespace vcmic {
namespace {

// Defined here rather than pulled from uuid.lib, for the same reason the
// scheduler interfaces are: it keeps the link line to the libraries this
// project actually needs.
const GUID kFolderRoamingAppData = {
    0x3eb685db, 0x65f9, 0x4cf6, {0xa0, 0x3a, 0xe3, 0xef, 0x65, 0x72, 0x9f, 0x3d}};

}  // namespace

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

std::filesystem::path AppDataDirectory() {
    PWSTR folder = nullptr;
    const HRESULT hr =
        ::SHGetKnownFolderPath(kFolderRoamingAppData, KF_FLAG_DEFAULT, nullptr, &folder);
    if (FAILED(hr) || folder == nullptr) {
        if (folder != nullptr) {
            ::CoTaskMemFree(folder);
        }
        return std::filesystem::path();
    }
    std::filesystem::path path = std::filesystem::path(folder) / L"vcmic";
    ::CoTaskMemFree(folder);
    return path;
}

}  // namespace vcmic
