#pragma once

#include "win_headers.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace vcmic {

// What the icon's colour says from across the desktop (spec 4.12). The tooltip
// carries the detail; this is the part that has to be readable at 16 pixels.
enum class TrayState {
    Starting,  // waiting for the configured endpoints to turn up
    Running,   // all three streams up
    Degraded,  // a stream is being rebuilt, or something is muted
    Failed,    // stopped, and not because anybody asked it to
};

// The menu raises these; the owner does the work. Keeping the interface here
// means the tray never learns what an endpoint is, and the engine never learns
// what a window is.
class TrayHost {
public:
    virtual ~TrayHost() = default;

    virtual void OnToggleChatMute() = 0;
    virtual void OnToggleMicMute() = 0;
    virtual void OnReloadConfig() = 0;
    virtual void OnWriteStatus() = 0;
    virtual void OnExit() = 0;

    // Read just before the menu is built, so the check marks are never stale.
    virtual bool ChatMuted() const = 0;
    virtual bool MicMuted() const = 0;
};

// A hidden top-level window and one shell notification icon.
//
// Top-level rather than message-only for two reasons: a message-only window
// does not receive the TaskbarCreated broadcast that explorer sends when it
// restarts, and SetForegroundWindow - which a popup menu needs to dismiss
// itself properly - is unreliable on one.
//
// Everything here runs on the thread that called Create, which must be the
// thread pumping messages.
class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // `log_file` and `config_file` are what the two "open" menu entries hand to
    // the shell; either may be empty, and then its entry is left out.
    bool Create(TrayHost& host, const std::filesystem::path& log_file,
                const std::filesystem::path& config_file, std::wstring& error);
    void Destroy();

    // Repaints the icon if the state changed and refreshes the tooltip. Cheap
    // enough to call on every housekeeping tick.
    void SetState(TrayState state, std::wstring_view detail);

    // A balloon, for the few things worth interrupting somebody's game over.
    void Notify(std::wstring_view title, std::wstring_view text, bool error);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

    bool AddIcon();
    void UpdateIcon();
    void ShowMenu();

    TrayHost* host_ = nullptr;
    HWND hwnd_ = nullptr;
    HICON icon_ = nullptr;
    UINT taskbar_created_ = 0;  // re-add the icon when explorer restarts
    bool icon_added_ = false;

    TrayState state_ = TrayState::Starting;
    std::wstring detail_;
    std::filesystem::path log_file_;
    std::filesystem::path config_file_;
};

}  // namespace vcmic
