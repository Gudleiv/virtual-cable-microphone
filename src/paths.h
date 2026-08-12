#pragma once

#include <filesystem>

namespace vcmic {

// Config and log files live next to the executable (spec 4.11), not in the
// current working directory, which the Task Scheduler sets to something else.
std::filesystem::path ExecutablePath();
std::filesystem::path ExecutableDirectory();

}  // namespace vcmic
