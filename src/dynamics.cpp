#include "dynamics.h"

#include "config.h"

#include <cmath>

namespace vcmic {
namespace {

// exp(-decay / samples) leaves `decay` as the number of time constants covered
// by the requested time: 1 reaches 63 %, 3 reaches 95 %.
float OnePole(double ms, std::uint32_t rate, double decay) noexcept {
    if (ms <= 0.0 || rate == 0) {
        return 1.0f;
    }
    const double samples = ms * static_cast<double>(rate) / 1000.0;
    if (samples < 1.0) {
        return 1.0f;
    }
    return static_cast<float>(1.0 - std::exp(-decay / samples));
}

}  // namespace

float GainFromDb(double db) noexcept { return static_cast<float>(std::pow(10.0, db / 20.0)); }

double DbFromGain(double gain) noexcept {
    return gain <= 1.0e-6 ? -120.0 : 20.0 * std::log10(gain);
}

float SmoothingCoefficient(double ms, std::uint32_t rate) noexcept { return OnePole(ms, rate, 1.0); }

float RampCoefficient(double ms, std::uint32_t rate) noexcept { return OnePole(ms, rate, 3.0); }

// ------------------------------------------------------------------ PeakLimiter

void PeakLimiter::Configure(const MixConfig& mix, std::uint32_t rate) {
    enabled_ = mix.limiter_enabled;
    threshold_ = GainFromDb(mix.limiter_threshold_db);
    release_ = RampCoefficient(mix.limiter_release_ms, rate);
    Reset();
}

DynamicsBlock PeakLimiter::Process(float* left, float* right, std::size_t frames) noexcept {
    DynamicsBlock result;
    if (!enabled_) {
        return result;
    }

    const float threshold = threshold_;
    const float release = release_;
    float gain = gain_;
    float min_gain = 1.0f;
    std::size_t active = 0;

    for (std::size_t i = 0; i < frames; ++i) {
        const float l = left[i];
        const float r = right[i];
        const float peak = (std::fabs)(l) > (std::fabs)(r) ? (std::fabs)(l) : (std::fabs)(r);

        // The most gain this frame may have without crossing the threshold.
        const float allowed = peak > threshold ? threshold / peak : 1.0f;

        // Attack is immediate and release only ever climbs towards `allowed`,
        // so `gain` stays at or below it and the output cannot overshoot.
        if (allowed < gain) {
            gain = allowed;
        } else {
            gain += (allowed - gain) * release;
        }

        left[i] = l * gain;
        right[i] = r * gain;

        if (gain < 0.999f) {
            ++active;
            if (gain < min_gain) {
                min_gain = gain;
            }
        }
    }

    gain_ = gain;
    result.active_frames = active;
    result.min_gain = min_gain;
    return result;
}

// -------------------------------------------------------------------- NoiseGate

void NoiseGate::Configure(const GateConfig& gate, std::uint32_t rate) {
    enabled_ = gate.enabled;
    open_threshold_ = GainFromDb(gate.threshold_db);
    close_threshold_ = GainFromDb(gate.threshold_db - kHysteresisDb);
    attack_ = RampCoefficient(gate.attack_ms, rate);
    release_ = RampCoefficient(gate.release_ms, rate);
    detector_ = RampCoefficient(kDetectorDecayMs, rate);
    hold_frames_ = static_cast<std::uint32_t>(gate.hold_ms * rate / 1000.0);
    Reset();
}

void NoiseGate::Reset() noexcept {
    open_ = false;
    hold_left_ = 0;
    envelope_ = 0.0f;
    // Start closed but not muted: a gate that has never seen audio should not
    // swallow the first word while it works out that someone is talking.
    gain_ = enabled_ ? 0.0f : 1.0f;
}

DynamicsBlock NoiseGate::Process(float* left, float* right, std::size_t frames) noexcept {
    DynamicsBlock result;
    if (!enabled_) {
        return result;
    }

    const float attack = attack_;
    const float release = release_;
    const float detector = detector_;
    float envelope = envelope_;
    float gain = gain_;
    bool open = open_;
    std::uint32_t hold_left = hold_left_;
    float min_gain = 1.0f;
    std::size_t active = 0;

    for (std::size_t i = 0; i < frames; ++i) {
        const float l = left[i];
        const float r = right[i];
        const float peak = (std::fabs)(l) > (std::fabs)(r) ? (std::fabs)(l) : (std::fabs)(r);

        if (peak > envelope) {
            envelope = peak;  // instant attack on the detector
        } else {
            envelope += (peak - envelope) * detector;
        }

        if (envelope >= (open ? close_threshold_ : open_threshold_)) {
            open = true;
            hold_left = hold_frames_;
        } else if (open) {
            if (hold_left > 0) {
                --hold_left;
            } else {
                open = false;
            }
        }

        const float target = open ? 1.0f : 0.0f;
        gain += (target - gain) * (target > gain ? attack : release);

        left[i] = l * gain;
        right[i] = r * gain;

        if (gain < 0.999f) {
            ++active;
            if (gain < min_gain) {
                min_gain = gain;
            }
        }
    }

    envelope_ = envelope;
    gain_ = gain;
    open_ = open;
    hold_left_ = hold_left;
    result.active_frames = active;
    result.min_gain = min_gain;
    return result;
}

}  // namespace vcmic
