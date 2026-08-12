#pragma once

#include <cstddef>
#include <cstdint>

namespace vcmic {

struct DriftConfig;

// Spec 4.6, the reading half: a fractional-rate reader over a stereo stream.
//
// The render side consumes each capture ring at a rate slightly different from
// one input frame per output frame, which is how a clock mismatch is absorbed
// without ever dropping or inserting anything. The phase is 32.32 fixed point
// rather than a double so that "how many input frames will this call eat" is an
// exact integer answer that cannot disagree with the loop that eats them.
//
// Interpolation is Catmull-Rom over four points. The specification allows plain
// linear interpolation; four points cost a handful of multiplies more and are
// worth it, because at the worst phase linear loses 1.2 dB at 8 kHz and 6 dB at
// 16 kHz where Catmull-Rom loses 0.2 dB and 3.3 dB.
class StereoResampler {
public:
    static constexpr std::uint64_t kOne = 1ull << 32;

    void Reset() noexcept {
        phase_ = 0;
        for (int i = 0; i < 4; ++i) {
            left_[i] = 0.0f;
            right_[i] = 0.0f;
        }
    }

    // Step in 32.32 fixed point. Ratio > 1 consumes input faster than it
    // produces output, which drains a ring that has grown past its target.
    static std::uint64_t StepFromRatio(double ratio) noexcept {
        if (ratio < 0.5) {
            ratio = 0.5;
        } else if (ratio > 2.0) {
            ratio = 2.0;
        }
        return static_cast<std::uint64_t>(ratio * static_cast<double>(kOne) + 0.5);
    }

    // Exactly how many input frames Process() will consume. Integer arithmetic,
    // so the answer is the same one the loop arrives at.
    std::size_t InputFramesNeeded(std::size_t frames, std::uint64_t step) const noexcept {
        return static_cast<std::size_t>((phase_ + static_cast<std::uint64_t>(frames) * step) >> 32);
    }

    // Consumes exactly InputFramesNeeded(frames, step) input frames and writes
    // `frames` output frames. Audio path: no allocation, no branching on state.
    void Process(const float* in_left, const float* in_right, std::size_t frames,
                 std::uint64_t step, float* out_left, float* out_right) noexcept {
        constexpr float kScale = 1.0f / 4294967296.0f;

        std::uint64_t phase = phase_;
        std::size_t index = 0;
        float l0 = left_[0], l1 = left_[1], l2 = left_[2], l3 = left_[3];
        float r0 = right_[0], r1 = right_[1], r2 = right_[2], r3 = right_[3];

        for (std::size_t i = 0; i < frames; ++i) {
            const float t = static_cast<float>(phase & 0xFFFFFFFFull) * kScale;
            out_left[i] = CatmullRom(l0, l1, l2, l3, t);
            out_right[i] = CatmullRom(r0, r1, r2, r3, t);

            phase += step;
            while (phase >= kOne) {
                phase -= kOne;
                l0 = l1; l1 = l2; l2 = l3; l3 = in_left[index];
                r0 = r1; r1 = r2; r2 = r3; r3 = in_right[index];
                ++index;
            }
        }

        phase_ = phase;
        left_[0] = l0; left_[1] = l1; left_[2] = l2; left_[3] = l3;
        right_[0] = r0; right_[1] = r1; right_[2] = r2; right_[3] = r3;
    }

private:
    // Interpolates between h1 and h2; h0 and h3 only shape the slope.
    static float CatmullRom(float h0, float h1, float h2, float h3, float t) noexcept {
        const float c1 = 0.5f * (h2 - h0);
        const float c2 = h0 - 2.5f * h1 + 2.0f * h2 - 0.5f * h3;
        const float c3 = 0.5f * (h3 - h0) + 1.5f * (h1 - h2);
        return ((c3 * t + c2) * t + c1) * t + h1;
    }

    std::uint64_t phase_ = 0;
    float left_[4]{};
    float right_[4]{};
};

// Spec 4.6, the measuring half: holds one capture ring at its target fill by
// nudging the rate the render side reads it at.
//
// The fill level is averaged over ~1 s first, because a single reading jumps by
// a whole capture packet. A PI loop then drives the averaged error to zero: the
// integral term converges on the true clock ratio and holds it, so in steady
// state the ring sits on target rather than at the offset a proportional-only
// loop would leave behind.
class DriftController {
public:
    void Configure(double target_frames, std::uint32_t sample_rate, const DriftConfig& config);

    // Full reset, including the learned clock ratio.
    void Reset() noexcept;

    // The source went silent and came back. The clock did not change, so the
    // integral is worth keeping; only the averaged fill has to be re-seated.
    void Resume(double fill_frames) noexcept;

    // One render callback. `elapsed_s` is the block length. Audio path.
    void Update(double fill_frames, double elapsed_s) noexcept;

    double ratio() const noexcept { return 1.0 + correction_; }
    double correction_ppm() const noexcept { return correction_ * 1.0e6; }
    double average_fill() const noexcept { return average_; }

    // The clamped value actually in force, which is what the caller has to size
    // its input buffer against.
    double max_correction() const noexcept { return max_correction_; }

private:
    double target_ = 0.0;
    double rate_ = 48000.0;
    double average_ = 0.0;
    double integral_ = 0.0;
    double correction_ = 0.0;
    double average_coeff_ = 0.0;  // per-second rate of the fill average
    double kp_ = 0.0;
    double ki_ = 0.0;
    double max_correction_ = 0.0;
    double integral_limit_ = 0.0;
};

}  // namespace vcmic
