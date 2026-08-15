#include "tray.h"

#include "logging.h"
#include "version.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <vector>

namespace vcmic {
namespace {

constexpr const wchar_t* kWindowClass = L"vcmic.tray.window";
constexpr UINT kIconId = 1;
constexpr UINT kCallbackMessage = WM_APP + 1;

enum MenuId : UINT {
    kIdMuteChat = 0x100,
    kIdMuteMic,
    kIdReload,
    kIdStatus,
    kIdOpenLog,
    kIdOpenConfig,
    kIdExit,
};

COLORREF StateColour(TrayState state) {
    switch (state) {
        case TrayState::Running:
            return RGB(76, 189, 106);
        case TrayState::Degraded:
            return RGB(233, 168, 48);
        case TrayState::Failed:
            return RGB(214, 73, 62);
        case TrayState::Starting:
        default:
            return RGB(126, 156, 196);
    }
}

const wchar_t* StateWord(TrayState state) {
    switch (state) {
        case TrayState::Running:
            return L"running";
        case TrayState::Degraded:
            return L"degraded";
        case TrayState::Failed:
            return L"stopped";
        case TrayState::Starting:
        default:
            return L"starting";
    }
}

// A microphone silhouette in normalized coordinates. Sampled rather than drawn
// with GDI, because GDI leaves the alpha channel alone and the shell wants a
// 32-bit icon that has one.
//
// The cradle - the open arc under the capsule - is what makes this read as a
// microphone rather than a lollipop at 16 pixels. It is worth its 1.2 px.
bool InsideMic(double x, double y) {
    constexpr double kHalfWidth = 0.135;
    constexpr double kTop = 0.09;
    constexpr double kBottom = 0.50;

    const double dx = x - 0.5;
    const double adx = std::fabs(dx);

    // Capsule: the straight section between the two end caps, plus the caps.
    if (y >= kTop + kHalfWidth && y <= kBottom - kHalfWidth) {
        if (adx <= kHalfWidth) {
            return true;
        }
    } else if (y >= kTop && y < kTop + kHalfWidth) {
        const double dy = y - (kTop + kHalfWidth);
        if (dx * dx + dy * dy <= kHalfWidth * kHalfWidth) {
            return true;
        }
    } else if (y > kBottom - kHalfWidth && y <= kBottom) {
        const double dy = y - (kBottom - kHalfWidth);
        if (dx * dx + dy * dy <= kHalfWidth * kHalfWidth) {
            return true;
        }
    }

    // Cradle: the lower half of an annulus, so it wraps the capsule and leaves
    // a gap either side.
    constexpr double kCradleY = 0.44;
    const double cradle_dy = y - kCradleY;
    if (cradle_dy >= 0.0) {
        const double radius = std::sqrt(dx * dx + cradle_dy * cradle_dy);
        if (radius <= 0.30 && radius >= 0.235) {
            return true;
        }
    }

    if (y > 0.74 && y <= 0.87 && adx <= 0.05) {
        return true;  // stem
    }
    if (y > 0.86 && y <= 0.94 && adx <= 0.21) {
        return true;  // base
    }
    return false;
}

// 4x4 coverage sampling, so the shape has an edge rather than a staircase at
// the 16 pixels the notification area actually gives us.
HICON MakeIcon(int size, COLORREF colour) {
    if (size <= 0) {
        return nullptr;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;  // top-down, so row 0 is the top row
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* raw = nullptr;
    const HBITMAP colours = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &raw, nullptr, 0);
    if (colours == nullptr || raw == nullptr) {
        if (colours != nullptr) {
            ::DeleteObject(colours);
        }
        return nullptr;
    }

    constexpr int kSamples = 4;
    auto* pixels = static_cast<std::uint8_t*>(raw);
    for (int py = 0; py < size; ++py) {
        for (int px = 0; px < size; ++px) {
            int covered = 0;
            for (int sy = 0; sy < kSamples; ++sy) {
                for (int sx = 0; sx < kSamples; ++sx) {
                    const double x = (px + (sx + 0.5) / kSamples) / size;
                    const double y = (py + (sy + 0.5) / kSamples) / size;
                    if (InsideMic(x, y)) {
                        ++covered;
                    }
                }
            }

            const int alpha = covered * 255 / (kSamples * kSamples);
            std::uint8_t* pixel = pixels + (static_cast<std::size_t>(py) * size + px) * 4;
            // Premultiplied BGRA, which is what the notification area composites.
            pixel[0] = static_cast<std::uint8_t>(GetBValue(colour) * alpha / 255);
            pixel[1] = static_cast<std::uint8_t>(GetGValue(colour) * alpha / 255);
            pixel[2] = static_cast<std::uint8_t>(GetRValue(colour) * alpha / 255);
            pixel[3] = static_cast<std::uint8_t>(alpha);
        }
    }

    // Ignored because the colour bitmap carries alpha, but CreateIconIndirect
    // still insists on being handed one.
    const int mask_stride = ((size + 15) / 16) * 2;
    const std::vector<std::uint8_t> mask_bits(static_cast<std::size_t>(mask_stride) * size, 0);
    const HBITMAP mask = ::CreateBitmap(size, size, 1, 1, mask_bits.data());

    ICONINFO icon_info{};
    icon_info.fIcon = TRUE;
    icon_info.hbmMask = mask;
    icon_info.hbmColor = colours;
    const HICON icon = ::CreateIconIndirect(&icon_info);

    if (mask != nullptr) {
        ::DeleteObject(mask);
    }
    ::DeleteObject(colours);
    return icon;
}

void CopyTruncated(wchar_t* destination, std::size_t capacity, std::wstring_view text) {
    const std::size_t copied = (std::min)(text.size(), capacity - 1);
    std::memcpy(destination, text.data(), copied * sizeof(wchar_t));
    destination[copied] = L'\0';
}

// Hands a path to whatever is registered for it, and falls back to showing it
// in explorer when nothing is - which is the normal case for a .log file.
void OpenInShell(const std::filesystem::path& path) {
    if (path.empty()) {
        return;
    }
    const std::wstring text = path.wstring();
    const auto result = reinterpret_cast<INT_PTR>(
        ::ShellExecuteW(nullptr, L"open", text.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result > 32) {
        return;
    }
    const std::wstring select = std::format(L"/select,\"{}\"", text);
    ::ShellExecuteW(nullptr, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);
}

}  // namespace

TrayIcon::~TrayIcon() { Destroy(); }

bool TrayIcon::Create(TrayHost& host, const std::filesystem::path& log_file,
                      const std::filesystem::path& config_file, std::wstring& error) {
    host_ = &host;
    log_file_ = log_file;
    config_file_ = config_file;

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &TrayIcon::WindowProc;
    window_class.hInstance = ::GetModuleHandleW(nullptr);
    window_class.lpszClassName = kWindowClass;
    if (::RegisterClassExW(&window_class) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        error = L"RegisterClassEx failed";
        return false;
    }

    // Never shown. It exists to own the icon, to receive the icon's callbacks
    // and to be around when explorer broadcasts TaskbarCreated.
    hwnd_ = ::CreateWindowExW(0, kWindowClass, kAppName, WS_OVERLAPPED, 0, 0, 0, 0, nullptr,
                              nullptr, window_class.hInstance, this);
    if (hwnd_ == nullptr) {
        error = L"CreateWindowEx failed";
        return false;
    }

    taskbar_created_ = ::RegisterWindowMessageW(L"TaskbarCreated");

    icon_ = MakeIcon(::GetSystemMetrics(SM_CXSMICON), StateColour(state_));
    if (!AddIcon()) {
        error = L"Shell_NotifyIcon failed to add the notification icon";
        Destroy();
        return false;
    }
    return true;
}

void TrayIcon::Destroy() {
    if (icon_added_ && hwnd_ != nullptr) {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = hwnd_;
        data.uID = kIconId;
        ::Shell_NotifyIconW(NIM_DELETE, &data);
        icon_added_ = false;
    }
    if (hwnd_ != nullptr) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (icon_ != nullptr) {
        ::DestroyIcon(icon_);
        icon_ = nullptr;
    }
    host_ = nullptr;
}

bool TrayIcon::AddIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kIconId;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kCallbackMessage;
    data.hIcon = icon_;
    CopyTruncated(data.szTip, std::size(data.szTip), std::format(L"{} {}", kAppName, kAppVersion));

    if (::Shell_NotifyIconW(NIM_ADD, &data) == FALSE) {
        return false;
    }
    icon_added_ = true;

    // Version 4 folds the mouse coordinates into wParam, which is what makes a
    // popup menu land in the right place on a multi-monitor desktop.
    data.uVersion = NOTIFYICON_VERSION_4;
    ::Shell_NotifyIconW(NIM_SETVERSION, &data);

    UpdateIcon();
    return true;
}

void TrayIcon::UpdateIcon() {
    if (!icon_added_) {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kIconId;
    data.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.hIcon = icon_;
    CopyTruncated(data.szTip, std::size(data.szTip),
                  detail_.empty()
                      ? std::format(L"{} {} - {}", kAppName, kAppVersion, StateWord(state_))
                      : std::format(L"{} - {}", kAppName, detail_));
    ::Shell_NotifyIconW(NIM_MODIFY, &data);
}

void TrayIcon::SetState(TrayState state, std::wstring_view detail) {
    const bool repaint = state != state_ || icon_ == nullptr;
    if (!repaint && detail_ == detail) {
        return;
    }

    state_ = state;
    detail_.assign(detail);

    if (repaint) {
        const HICON replacement = MakeIcon(::GetSystemMetrics(SM_CXSMICON), StateColour(state_));
        if (replacement != nullptr) {
            const HICON previous = icon_;
            icon_ = replacement;
            if (previous != nullptr) {
                ::DestroyIcon(previous);
            }
        }
    }
    UpdateIcon();
}

void TrayIcon::Notify(std::wstring_view title, std::wstring_view text, bool error) {
    if (!icon_added_) {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kIconId;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = error ? NIIF_ERROR : NIIF_INFO;
    CopyTruncated(data.szInfoTitle, std::size(data.szInfoTitle), title);
    CopyTruncated(data.szInfo, std::size(data.szInfo), text);
    ::Shell_NotifyIconW(NIM_MODIFY, &data);
}

void TrayIcon::ShowMenu() {
    if (host_ == nullptr) {
        return;
    }

    const HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    const std::wstring header = std::format(L"{} {} - {}", kAppName, kAppVersion, StateWord(state_));
    ::AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, header.c_str());
    if (!detail_.empty()) {
        ::AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, detail_.c_str());
    }
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (host_->ChatMuted() ? MF_CHECKED : MF_UNCHECKED), kIdMuteChat,
                  L"Mute &chat");
    ::AppendMenuW(menu, MF_STRING | (host_->MicMuted() ? MF_CHECKED : MF_UNCHECKED), kIdMuteMic,
                  L"Mute &microphone");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kIdReload, L"&Reload config");
    ::AppendMenuW(menu, MF_STRING, kIdStatus, L"Write &status to log");
    if (!log_file_.empty()) {
        ::AppendMenuW(menu, MF_STRING, kIdOpenLog, L"Open &log");
    }
    if (!config_file_.empty()) {
        ::AppendMenuW(menu, MF_STRING, kIdOpenConfig, L"Open co&nfig");
    }
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kIdExit, L"E&xit");

    POINT cursor{};
    ::GetCursorPos(&cursor);

    // The window has to be foreground for the menu to dismiss itself when the
    // user clicks elsewhere, and the posted WM_NULL afterwards is what lets it
    // give the foreground back. Both halves of a very old shell contract.
    ::SetForegroundWindow(hwnd_);
    const int command =
        ::TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y,
                         0, hwnd_, nullptr);
    ::DestroyMenu(menu);
    ::PostMessageW(hwnd_, WM_NULL, 0, 0);

    // The menu runs a modal loop of its own, so nothing else on this thread
    // moves while it is open. The audio threads are elsewhere and do not care.
    switch (command) {
        case kIdMuteChat:
            host_->OnToggleChatMute();
            break;
        case kIdMuteMic:
            host_->OnToggleMicMute();
            break;
        case kIdReload:
            host_->OnReloadConfig();
            break;
        case kIdStatus:
            host_->OnWriteStatus();
            break;
        case kIdOpenLog:
            OpenInShell(log_file_);
            break;
        case kIdOpenConfig:
            OpenInShell(config_file_);
            break;
        case kIdExit:
            host_->OnExit();
            break;
        default:
            break;
    }
}

LRESULT CALLBACK TrayIcon::WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    TrayIcon* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<TrayIcon*>(create->lpCreateParams);
        if (self != nullptr) {
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
    } else {
        self = reinterpret_cast<TrayIcon*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self != nullptr) {
        const LRESULT handled = self->HandleMessage(message, wparam, lparam);
        if (handled != 0) {
            return 0;
        }
    }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT TrayIcon::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kCallbackMessage) {
        // Version 4 packs the event into the low word of lParam.
        switch (LOWORD(lparam)) {
            case WM_CONTEXTMENU:
            case NIN_SELECT:
            case NIN_KEYSELECT:
                ShowMenu();
                return 1;
            default:
                return 1;
        }
    }

    if (taskbar_created_ != 0 && message == taskbar_created_) {
        // Explorer restarted and forgot every icon it had. Ours included.
        LogInfo(L"explorer restarted; re-adding the tray icon");
        icon_added_ = false;
        AddIcon();
        return 1;
    }

    switch (message) {
        case WM_QUERYENDSESSION:
            // Windows is going down. Say yes, and start stopping now rather
            // than waiting to be killed halfway through a WASAPI call.
            if (host_ != nullptr) {
                host_->OnExit();
            }
            return 0;  // let DefWindowProc answer TRUE
        case WM_ENDSESSION:
            if (wparam != 0 && host_ != nullptr) {
                host_->OnExit();
            }
            return 1;
        case WM_CLOSE:
            if (host_ != nullptr) {
                host_->OnExit();
            }
            return 1;
        default:
            break;
    }
    (void)wparam;
    return 0;
}

}  // namespace vcmic
