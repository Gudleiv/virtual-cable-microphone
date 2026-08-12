#pragma once

#include <atomic>
#include <cstdint>

namespace vcmic {

// Gain counters are kept as unsigned Q16 (gain * 65536) so that a "smallest
// gain seen" is a plain integer minimum; 1.0 is the neutral value.
inline constexpr std::uint32_t kGainQ16One = 65536;

inline std::uint32_t GainToQ16(float gain) noexcept {
    if (gain <= 0.0f) {
        return 0;
    }
    const float scaled = gain * static_cast<float>(kGainQ16One);
    return scaled >= static_cast<float>(kGainQ16One) ? kGainQ16One
                                                     : static_cast<std::uint32_t>(scaled);
}

inline double GainFromQ16(std::uint32_t value) noexcept {
    return static_cast<double>(value) / static_cast<double>(kGainQ16One);
}

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

    // Drift compensation (spec 4.6). `drift_frames` is the accumulated
    // correction: input frames consumed minus output frames produced, so a
    // positive value means this source's clock runs fast and the extra frames
    // were resampled away rather than dropped.
    std::atomic<std::int64_t> drift_frames{0};
    std::atomic<std::int32_t> drift_ppm{0};
    std::atomic<std::uint32_t> average_fill_frames{0};
};

struct RenderStats {
    std::atomic<std::uint64_t> callbacks{0};
    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint64_t> clipped_samples{0};
    std::atomic<std::uint64_t> timeouts{0};  // no render event within the wait window

    // Dynamics (spec 4.7). The "min gain" pair is written by the render thread
    // and reset by the reporter; the two can race, and the only consequence is
    // that one report may carry a peak from the interval before it.
    std::atomic<std::uint64_t> limiter_frames{0};
    std::atomic<std::uint32_t> limiter_min_gain{kGainQ16One};
    std::atomic<std::uint64_t> gate_frames{0};
    std::atomic<std::uint32_t> gate_min_gain{kGainQ16One};
};

// Render-thread side of the racy minimum above: only ever lowers the value.
inline void PublishMinGain(std::atomic<std::uint32_t>& slot, float gain) noexcept {
    const std::uint32_t candidate = GainToQ16(gain);
    if (candidate < slot.load(std::memory_order_relaxed)) {
        slot.store(candidate, std::memory_order_relaxed);
    }
}

}  // namespace vcmic
