#pragma once

#include <atomic>
#include <cstdint>

namespace vcmic {

// Counters the audio threads may touch: plain relaxed atomics, so the audio
// path never logs, locks or allocates (spec 4.10). A low-priority thread reads
// them for the periodic report and the exit summary (spec 4.11).
struct SourceStats {
    std::atomic<std::uint64_t> packets{0};
    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint64_t> silent_packets{0};
    std::atomic<std::uint64_t> discontinuities{0};
    std::atomic<std::uint64_t> overruns{0};        // ring was full, capture data dropped
    std::atomic<std::uint64_t> overrun_frames{0};
    std::atomic<std::uint64_t> underruns{0};       // render wanted frames the ring did not have
    std::atomic<std::uint64_t> underrun_frames{0};
    std::atomic<std::uint64_t> resyncs{0};         // consumer dropped a backlog to restore latency
    std::atomic<std::uint64_t> resync_frames{0};
    std::atomic<std::uint64_t> primings{0};        // waits for the ring to reach the target fill
    std::atomic<std::uint32_t> fill_frames{0};     // most recent fill level seen by the render side
};

struct RenderStats {
    std::atomic<std::uint64_t> callbacks{0};
    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint64_t> clipped_samples{0};
    std::atomic<std::uint64_t> timeouts{0};  // no render event within the wait window
};

}  // namespace vcmic
