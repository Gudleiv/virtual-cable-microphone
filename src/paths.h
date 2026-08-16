#pragma once

#include <filesystem>

namespace vcmic {

// Never the current working directory: the Task Scheduler sets that to
// something of its own, so a relative path means nothing by the time the
// autostart task runs (spec 4.11).
std::filesystem::path ExecutablePath();
std::filesystem::path ExecutableDirectory();

// %APPDATA%\vcmic - where settings and the log live unless told otherwise.
// Returns an empty path if the shell cannot say where roaming data goes, which
// is the caller's cue to fall back to the executable's directory.
//
// Roaming rather than local, because that is the folder people know how to
// find. The one thing in here that is genuinely machine-specific is the
// endpoint ids, and a config that roams onto a machine without those devices
// lands in the tray's setup state rather than doing damage.
std::filesystem::path AppDataDirectory();

}  // namespace vcmic
