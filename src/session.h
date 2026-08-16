#pragma once

#include "config.h"
#include "win_headers.h"

#include <filesystem>

struct IMMDeviceEnumerator;

namespace vcmic {

struct SessionOptions {
    bool tray = false;  // notification-area icon instead of a bare console
    std::filesystem::path config_path;
    std::filesystem::path log_file;
};

// Runs the mixer until the stop event is signalled - by Ctrl+C, by the tray's
// Exit, or by a stream failing in a way no rebuild can fix - and returns the
// process exit code.
//
// `stop_event` is a manual-reset event owned by the caller. The tray, when
// there is one, lives on the calling thread: this is where its messages are
// pumped, so the caller must not be holding anything the audio path needs.
int RunMixerSession(IMMDeviceEnumerator* enumerator, const Config& config,
                    const SessionOptions& options, HANDLE stop_event);

}  // namespace vcmic
