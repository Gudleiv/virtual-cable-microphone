#include "drift.h"

#include "config.h"

#include <algorithm>

namespace vcmic {
namespace {

double Clamp(double value, double low, double high) noexcept {
    return value < low ? low : (value > high ? high : value);
}

}  // namespace

void DriftController::Configure(double target_frames, std::uint32_t sample_rate,
                                const DriftConfig& config) {
    target_ = target_frames;
    rate_ = sample_rate == 0 ? 48000.0 : static_cast<double>(sample_rate);

    const double window = config.measure_window_s > 0.0 ? config.measure_window_s : 1.0;
    average_coeff_ = 1.0 / window;

    // Critically damped: no overshoot, so the fill level approaches the target
    // from one side instead of bouncing across it.
    const double omega = 1.0 / (config.response_s > 0.0 ? config.response_s : 10.0);
    kp_ = 2.0 * omega;
    ki_ = omega * omega;

    max_correction_ = config.enabled ? Clamp(config.max_rate_correction, 0.0, 0.05) : 0.0;
    integral_limit_ = ki_ > 0.0 ? max_correction_ / ki_ : 0.0;

    Reset();
}

void DriftController::Reset() noexcept {
    average_ = target_;
    integral_ = 0.0;
    correction_ = 0.0;
}

void DriftController::Resume(double fill_frames) noexcept {
    // Keep the integral: it is the measured clock ratio, and the clocks did not
    // change while the source was quiet. The average is a measurement of a
    // moment that has passed, so it starts again from what is there now.
    average_ = fill_frames;
}

void DriftController::Update(double fill_frames, double elapsed_s) noexcept {
    if (elapsed_s <= 0.0) {
        return;
    }

    const double alpha = (std::min)(1.0, elapsed_s * average_coeff_);
    average_ += (fill_frames - average_) * alpha;

    const double error_s = (average_ - target_) / rate_;
    integral_ = Clamp(integral_ + error_s * elapsed_s, -integral_limit_, integral_limit_);
    correction_ = Clamp(kp_ * error_s + ki_ * integral_, -max_correction_, max_correction_);
}

}  // namespace vcmic
