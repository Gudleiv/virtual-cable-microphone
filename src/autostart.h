#pragma once

#include "win_headers.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vcmic {

// Spec 4.2: this is emphatically not a Windows service. Audio endpoints belong
// to the interactive session, and a service in Session 0 would enumerate none
// of them. Autostart is therefore a per-user Task Scheduler job with a logon
// trigger, running in the session that owns the sound card.
//
// Everything here needs a COM apartment on the calling thread and none of it
// needs administrator rights: a task registered under the current user with an
// interactive token is the user's own to create.

inline constexpr const wchar_t* kAutostartTaskName = L"vcmic";

struct AutostartInfo {
    bool installed = false;
    bool enabled = false;
    std::wstring state;      // Ready, Running, Disabled...
    std::wstring command;    // executable as registered
    std::wstring arguments;
    std::wstring user;
    std::uint32_t delay_s = 0;
    std::wstring last_run;      // empty when it has never run
    std::wstring last_result;   // exit code of the last run, as text
};

// Registers, or updates in place, the logon task for the current user.
//
// `delay_s` is what stands between a working autostart and a broken one. At
// logon the USB stack is still enumerating, and a mixer that starts before the
// sound card exists has nothing to open; the delay, together with
// resilience.startup_wait_s, covers that window from both ends.
//
// `notes` collects the settings the scheduler declined. None of them stops the
// task registering, and every one of them changes how it behaves later, so the
// caller is expected to print them rather than drop them.
HRESULT InstallAutostart(std::uint32_t delay_s, const std::wstring& arguments,
                         std::vector<std::wstring>& notes, std::wstring& error);

HRESULT RemoveAutostart(std::wstring& error);

// info.installed is false, with S_OK, when there is no such task.
HRESULT QueryAutostart(AutostartInfo& info, std::wstring& error);

}  // namespace vcmic
