#pragma once

#include <cstddef>
#include <cstdint>

namespace vcmic {

struct GateConfig;
struct MixConfig;

float GainFromDb(double db) noexcept;
double DbFromGain(double gain) noexcept;

// One-pole coefficient covering ~63 % of a change in `ms`. Used for the mix
// gains, where the specification asks for a ramp rather than a step (4.7).
float SmoothingCoefficient(double ms, std::uint32_t rate) noexcept;

// One-pole coefficient covering ~95 % of a change in `ms`, which is closer to
// what an attack or release time is normally understood to mean.
float RampCoefficient(double ms, std::uint32_t rate) noexcept;

// What a dynamics stage did to one block, for the counters. Returned rather
// than stored, so nothing in the audio path shares state with the reporter.
struct DynamicsBlock {
    std::size_t active_frames = 0;  // frames where the stage was reducing gain
    float min_gain = 1.0f;
};

// Spec 4.7: peak limiter on the sum, no lookahead.
//
// The gain is never allowed above what the current frame permits, so the output
// cannot exceed the threshold - there is no overshoot to hide with a lookahead
// delay. Recovery is a one-pole release, which is what keeps a caught peak from
// modulating everything behind it.
class PeakLimiter {
public:
    // Everything a reload changes, and nothing the stage is currently doing.
    // Computing these costs an exp() and a pow(), which is why the tray thread
    // works them out and the render thread only copies them in (spec 4.10).
    struct Settings {
        bool enabled = false;
        float threshold = 1.0f;
        float release = 1.0f;
    };

    static Settings SettingsFrom(const MixConfig& mix, std::uint32_t rate);

    void Configure(const MixConfig& mix, std::uint32_t rate);
    void Reset() noexcept { gain_ = 1.0f; }

    // Audio path. Deliberately leaves `gain_` alone: a reload that arrives
    // while the limiter is riding a peak must not drop the gain reduction and
    // let that peak through.
    void Adopt(const Settings& settings) noexcept;

    bool enabled() const noexcept { return enabled_; }
    float threshold() const noexcept { return threshold_; }

    // In place on a deinterleaved stereo pair. Audio path.
    DynamicsBlock Process(float* left, float* right, std::size_t frames) noexcept;

private:
    bool enabled_ = false;
    float threshold_ = 1.0f;
    float release_ = 1.0f;
    float gain_ = 1.0f;
};

// Spec 4.7: optional noise gate, microphone only, off by default.
//
// The detector is a peak follower with an instant attack, so a consonant opens
// the gate on its first sample. Closing waits out the hold time and then rides
// the release down, and the level that keeps the gate open is 6 dB below the
// one that opens it, so a voice sitting on the threshold does not chatter.
class NoiseGate {
public:
    // As above: the coefficients a reload replaces, kept apart from the
    // detector and the envelope so that adopting them mid-word is inaudible.
    struct Settings {
        bool enabled = false;
        float open_threshold = 0.0f;
        float close_threshold = 0.0f;
        float attack = 1.0f;
        float release = 1.0f;
        float detector = 1.0f;
        std::uint32_t hold_frames = 0;
    };

    static Settings SettingsFrom(const GateConfig& gate, std::uint32_t rate);

    void Configure(const GateConfig& gate, std::uint32_t rate);
    void Reset() noexcept;

    // Audio path. Keeps the envelope and the current gain, so a gate that is
    // open on somebody's voice stays open across a reload.
    void Adopt(const Settings& settings) noexcept;

    bool enabled() const noexcept { return enabled_; }

    // In place on a deinterleaved stereo pair. Audio path.
    DynamicsBlock Process(float* left, float* right, std::size_t frames) noexcept;

private:
    static constexpr double kHysteresisDb = 6.0;
    static constexpr double kDetectorDecayMs = 15.0;

    bool enabled_ = false;
    float open_threshold_ = 0.0f;
    float close_threshold_ = 0.0f;
    float attack_ = 1.0f;
    float release_ = 1.0f;
    float detector_ = 1.0f;
    std::uint32_t hold_frames_ = 0;

    bool open_ = false;
    std::uint32_t hold_left_ = 0;
    float envelope_ = 0.0f;
    float gain_ = 0.0f;
};

}  // namespace vcmic
