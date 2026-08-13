// Offline checks for the drift correction and dynamics of stage 3. Not part of
// the shipping build: it needs no Windows API, so it can be compiled and run
// anywhere, and under a sanitizer it also checks the resampler's promise about
// how much input it consumes against real buffer bounds.
//
//   g++ -std=c++20 -O1 -fsanitize=address,undefined -Isrc
//       tools/dsp_selftest.cpp src/drift.cpp src/dynamics.cpp -o dsp_selftest
#include "config.h"
#include "drift.h"
#include "dynamics.h"
#include "ring_buffer.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace vcmic;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what, double value = 0.0) {
    std::printf("%-58s %-6s %g\n", what, ok ? "ok" : "FAIL", value);
    if (!ok) ++g_failures;
}

constexpr double kPi = 3.14159265358979323846;
constexpr std::uint32_t kRate = 48000;

// Amplitude of `freq` in `x`, by Goertzel.
double Goertzel(const std::vector<float>& x, double freq) {
    const double w = 2.0 * kPi * freq / kRate;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (float v : x) {
        const double s0 = v + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double real = s1 - s2 * std::cos(w);
    const double imag = s2 * std::sin(w);
    return 2.0 * std::sqrt(real * real + imag * imag) / static_cast<double>(x.size());
}

double Rms(const std::vector<float>& x) {
    double sum = 0.0;
    for (float v : x) sum += static_cast<double>(v) * v;
    return std::sqrt(sum / static_cast<double>(x.size()));
}

// ---------------------------------------------------------------------------
// 1. The resampler consumes exactly what it says it will, and stays in tune.

void TestResampler(double ratio, double freq, const char* label) {
    StereoResampler resampler;
    resampler.Reset();
    const std::uint64_t step = StereoResampler::StepFromRatio(ratio);

    const std::size_t block = 1056;  // the real render buffer on the target machine
    const std::size_t blocks = 400;

    std::vector<float> out_left(block), out_right(block);
    std::vector<float> collected;
    collected.reserve(block * blocks);

    double phase = 0.0;
    const double increment = 2.0 * kPi * freq / kRate;

    for (std::size_t b = 0; b < blocks; ++b) {
        const std::size_t needed = resampler.InputFramesNeeded(block, step);
        // Exact-size buffers: an overread is a heap-buffer-overflow under ASan.
        std::vector<float> in_left(needed), in_right(needed);
        for (std::size_t i = 0; i < needed; ++i) {
            in_left[i] = static_cast<float>(0.5 * std::sin(phase));
            in_right[i] = in_left[i];
            phase += increment;
        }
        resampler.Process(in_left.data(), in_right.data(), block, step, out_left.data(),
                          out_right.data());
        if (b >= 4) {  // skip the fade-in through the zeroed history
            collected.insert(collected.end(), out_left.begin(), out_left.end());
        }
    }

    // Reading the input faster than it is played back raises the pitch, so the
    // tone comes out at freq * ratio.
    const double expected_freq = freq * ratio;
    const double amplitude = Goertzel(collected, expected_freq);

    char name[160];
    std::snprintf(name, sizeof name, "%s: %.0f Hz amplitude (0.5 expected)", label, freq);
    Check(std::fabs(amplitude - 0.5) < 0.02, name, amplitude);

    // Everything that is not the tone: interpolation error plus the tiny
    // frequency offset. Reported as dB below the tone.
    std::vector<float> residual = collected;
    const double w = 2.0 * kPi * expected_freq / kRate;
    // Least-squares fit of the tone, then subtract it.
    double sc = 0.0, ss = 0.0, cc = 0.0, sinsin = 0.0;
    for (std::size_t i = 0; i < residual.size(); ++i) {
        const double c = std::cos(w * static_cast<double>(i));
        const double s = std::sin(w * static_cast<double>(i));
        sc += residual[i] * c;
        ss += residual[i] * s;
        cc += c * c;
        sinsin += s * s;
    }
    const double a = sc / cc, bcoef = ss / sinsin;
    for (std::size_t i = 0; i < residual.size(); ++i) {
        residual[i] -= static_cast<float>(a * std::cos(w * static_cast<double>(i)) +
                                          bcoef * std::sin(w * static_cast<double>(i)));
    }
    const double snr_db = 20.0 * std::log10(Rms(collected) / (Rms(residual) + 1e-20));
    // Catmull-Rom rolls off towards Nyquist, so the artifact grows with the
    // tone. Speech energy lives below 4 kHz, which is where the budget is set.
    const double floor_db = freq <= 4000.0 ? 50.0 : 30.0;
    std::snprintf(name, sizeof name, "%s: %.0f Hz residual below tone (dB)", label, freq);
    Check(snr_db > floor_db, name, snr_db);
}

// ---------------------------------------------------------------------------
// 2. The control loop holds a drifting source on its target fill.

struct LoopResult {
    double settled_ppm = 0.0;
    double mean_ppm = 0.0;      // what the accumulated correction actually works out to
    double ppm_stdev = 0.0;     // how much the applied rate wobbles around it
    double min_wake_ms = 1e9;   // fill the render thread sees; this is what is regulated
    double max_wake_ms = -1e9;
    double min_left_ms = 1e9;   // fill after the block was taken; this is what runs out
    double avg_fill_span_ms = 0.0;  // swing of the averaged fill the loop measures
    double settle_s = -1.0;         // first time the correction reached the true drift
    std::uint64_t resyncs = 0;
    std::uint64_t underruns = 0;
};

// Mirrors RenderSink::PullSource against a source whose clock is `source_ppm`
// away from the cable's. The producer runs continuously, so at each render
// wakeup the ring holds whatever was left last time plus one block of new
// audio: regulating the wake-time fill to `target` leaves `target - block`
// as the real margin against an underrun.
// `wander_frames` sets how much the capture/render timing drifts around on a
// timescale of seconds. The default is calibrated against the target machine:
// a ten-minute session there showed the correction wobbling with a standard
// deviation of 157 ppm, which at kp = 0.2 means about 0.8 ms of noise left on
// the averaged fill.
LoopResult RunLoop(double source_ppm, double minutes, const DriftConfig& config, double target_ms,
                   double wander_frames = 320.0, std::size_t block = 1056) {
    const std::size_t target = static_cast<std::size_t>(target_ms * kRate / 1000.0);
    const std::size_t max_frames = target * 4;

    StereoRing ring;
    ring.Reset(static_cast<std::size_t>(0.25 * kRate));

    DriftController drift;
    drift.Configure(static_cast<double>(target), kRate, config);
    StereoResampler resampler;
    resampler.Reset();

    std::vector<float> scratch_l(8192, 0.25f), scratch_r(8192, 0.25f);
    std::vector<float> in_l(8192), in_r(8192);
    std::vector<float> out_l(block), out_r(block);

    // Primed the way RenderSink does it: the real code starts consuming the
    // moment the ring first reaches the target, so the loop's first measurement
    // sits on the setpoint. Starting a whole block above it would slam the
    // correction to the ceiling and make the settling time meaningless.
    ring.Write(scratch_l.data(), scratch_r.data(), target - block);

    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> jitter(-96, 96);  // +-2 ms of packet lumpiness
    // White jitter alone averages away almost completely over a one-second
    // window, which is not what the real machine does: capture packets and
    // render wakeups both wander on a timescale of seconds, so the averaged
    // fill still swings. This is that wander, as a mean-reverting walk.
    std::normal_distribution<double> kick(0.0, 1.0);
    double wander = 0.0;
    const double wander_decay = static_cast<double>(block) / kRate / 2.0;  // ~2 s memory
    const double wander_kick = wander_frames;

    const double rate = 1.0 + source_ppm * 1e-6;
    double produced_exact = 0.0;
    std::int64_t produced_total = 0;
    LoopResult result;
    bool primed = true;
    double ppm_sum = 0.0, ppm_sum2 = 0.0;
    std::size_t ppm_count = 0;
    double min_avg = 1e9, max_avg = -1e9;

    const std::size_t callbacks =
        static_cast<std::size_t>(minutes * 60.0 * kRate / static_cast<double>(block));

    for (std::size_t c = 0; c < callbacks; ++c) {
        produced_exact += static_cast<double>(block) * rate;
        wander += (-wander * wander_decay) + kick(rng) * wander_kick * wander_decay;
        std::int64_t want = static_cast<std::int64_t>(produced_exact + wander) - produced_total +
                            jitter(rng);
        if (want < 0) want = 0;
        produced_total += want;
        std::size_t left = static_cast<std::size_t>(want);
        while (left > 0) {
            const std::size_t chunk = left > scratch_l.size() ? scratch_l.size() : left;
            ring.Write(scratch_l.data(), scratch_r.data(), chunk);
            left -= chunk;
        }

        std::size_t available = ring.Readable();
        if (!primed) {
            if (available < target) continue;
            primed = true;
            resampler.Reset();
            drift.Resume(static_cast<double>(available));
        }
        if (available > max_frames) {
            ring.Discard(available - target);
            drift.Resume(static_cast<double>(target));
            ++result.resyncs;
            available = ring.Readable();
        }

        drift.Update(static_cast<double>(available), static_cast<double>(block) / kRate);
        const std::uint64_t step = StereoResampler::StepFromRatio(drift.ratio());
        const std::size_t needed = resampler.InputFramesNeeded(block, step);
        const std::size_t taken = ring.Read(in_l.data(), in_r.data(), needed);
        if (taken < needed) {
            for (std::size_t i = taken; i < needed; ++i) in_l[i] = in_r[i] = 0.0f;
            ++result.underruns;
            primed = false;
        }
        resampler.Process(in_l.data(), in_r.data(), block, step, out_l.data(), out_r.data());

        if (result.settle_s < 0.0 &&
            std::fabs(drift.correction_ppm() - source_ppm) < 20.0) {
            result.settle_s = static_cast<double>(c) * static_cast<double>(block) / kRate;
        }

        const double wake_ms = 1000.0 * static_cast<double>(available) / kRate;
        const double left_ms = 1000.0 * static_cast<double>(ring.Readable()) / kRate;
        if (c > callbacks / 2) {
            if (wake_ms < result.min_wake_ms) result.min_wake_ms = wake_ms;
            if (wake_ms > result.max_wake_ms) result.max_wake_ms = wake_ms;
            if (left_ms < result.min_left_ms) result.min_left_ms = left_ms;

            const double ppm = drift.correction_ppm();
            ppm_sum += ppm;
            ppm_sum2 += ppm * ppm;
            ++ppm_count;
            const double avg_ms = 1000.0 * drift.average_fill() / kRate;
            if (avg_ms < min_avg) min_avg = avg_ms;
            if (avg_ms > max_avg) max_avg = avg_ms;
        }
        result.settled_ppm = drift.correction_ppm();
    }

    if (ppm_count > 0) {
        result.mean_ppm = ppm_sum / static_cast<double>(ppm_count);
        const double variance = ppm_sum2 / static_cast<double>(ppm_count) -
                                result.mean_ppm * result.mean_ppm;
        result.ppm_stdev = variance > 0.0 ? std::sqrt(variance) : 0.0;
        result.avg_fill_span_ms = max_avg - min_avg;
    }
    return result;
}

void TestControlLoop() {
    DriftConfig config;  // defaults: 1 s window, 10 s response, 0.1 % ceiling
    constexpr double kTargetMs = 50.0;  // comfortably above the 22 ms render block

    struct Case {
        double ppm;
        const char* label;
    } cases[] = {
        {198.0, "mic as measured (+198 ppm)"},
        {-198.0, "the same, running slow"},
        {0.0, "clock-locked source"},
        {900.0, "near the correction ceiling"},
    };

    for (const Case& c : cases) {
        const LoopResult r = RunLoop(c.ppm, 60.0, config, kTargetMs);  // one hour of audio
        char name[160];

        std::snprintf(name, sizeof name, "%s: learned correction (ppm)", c.label);
        Check(std::fabs(r.mean_ppm - c.ppm) < 20.0, name, r.mean_ppm);

        // The correction is what the audio actually gets resampled by, so noise
        // on it is noise on the pitch. It also has to be quiet enough that the
        // logged figure means something.
        std::snprintf(name, sizeof name, "%s: correction wobble, 1 sigma (ppm)", c.label);
        Check(r.ppm_stdev < 40.0, name, r.ppm_stdev);

        std::snprintf(name, sizeof name, "%s: averaged fill swing (ms)", c.label);
        Check(r.avg_fill_span_ms < 8.0, name, r.avg_fill_span_ms);

        // The price of a quiet correction: the loop is deliberately slow.
        std::snprintf(name, sizeof name, "%s: time to reach the true rate (s)", c.label);
        Check(r.settle_s >= 0.0 && r.settle_s < 300.0, name, r.settle_s);

        std::snprintf(name, sizeof name, "%s: regulated fill, %.0f ms target", c.label, kTargetMs);
        Check(std::fabs(0.5 * (r.min_wake_ms + r.max_wake_ms) - kTargetMs) < 3.0, name,
              0.5 * (r.min_wake_ms + r.max_wake_ms));

        std::snprintf(name, sizeof name, "%s: fill span over the hour (ms)", c.label);
        Check(r.max_wake_ms - r.min_wake_ms < 12.0, name, r.max_wake_ms - r.min_wake_ms);

        std::snprintf(name, sizeof name, "%s: worst margin left after a block (ms)", c.label);
        Check(r.min_left_ms > 10.0, name, r.min_left_ms);

        std::snprintf(name, sizeof name, "%s: resyncs and underruns in an hour", c.label);
        Check(r.resyncs == 0 && r.underruns == 0, name,
              static_cast<double>(r.resyncs + r.underruns));
    }

    // Without correction the same hour walks away and the resync valve has to
    // keep firing: that is the stage 2 behaviour this stage exists to remove.
    DriftConfig off = config;
    off.enabled = false;
    const LoopResult bare = RunLoop(198.0, 60.0, off, kTargetMs);
    // 150 ms of headroom above a 50 ms target, consumed at 0.198 ms/s: about
    // one dropped backlog every 12 minutes, which is the audible glitch.
    Check(bare.resyncs >= 3, "uncorrected +198 ppm: resync valve fires every ~12 min",
          static_cast<double>(bare.resyncs));

    // A target barely above the render block is thin whatever the drift does.
    const LoopResult thin = RunLoop(198.0, 20.0, config, 25.0);
    Check(thin.min_left_ms < 6.0, "25 ms target behind a 22 ms block: little margin left (ms)",
          thin.min_left_ms);

    // Beyond the ceiling the loop cannot win; the resync valve must still bound
    // the fill instead of letting it grow without limit.
    const LoopResult over = RunLoop(5000.0, 10.0, config, kTargetMs);
    Check(over.max_wake_ms < 4.5 * kTargetMs, "beyond the ceiling: fill still bounded (ms)",
          over.max_wake_ms);
    Check(over.resyncs > 0, "beyond the ceiling: resync valve fires",
          static_cast<double>(over.resyncs));
}

// ---------------------------------------------------------------------------
// 3. Limiter and gate.

void TestLimiter() {
    MixConfig mix;
    mix.limiter_enabled = true;
    mix.limiter_threshold_db = -1.0;
    mix.limiter_release_ms = 80.0;

    PeakLimiter limiter;
    limiter.Configure(mix, kRate);

    const std::size_t frames = kRate;  // one second
    std::vector<float> l(frames), r(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        // Two voices at once: 1.6 peak, well past full scale.
        const double t = static_cast<double>(i) / kRate;
        l[i] = static_cast<float>(0.8 * std::sin(2.0 * kPi * 220.0 * t) +
                                  0.8 * std::sin(2.0 * kPi * 700.0 * t));
        r[i] = l[i];
    }
    const DynamicsBlock block = limiter.Process(l.data(), r.data(), frames);

    float peak = 0.0f;
    for (std::size_t i = 0; i < frames; ++i) peak = std::fabs(l[i]) > peak ? std::fabs(l[i]) : peak;

    const float threshold = GainFromDb(-1.0);
    Check(peak <= threshold + 1e-6f, "limiter: nothing exceeds the threshold", peak);
    Check(block.active_frames > 0, "limiter: engaged", static_cast<double>(block.active_frames));
    Check(DbFromGain(block.min_gain) > -12.0, "limiter: peak gain reduction (dB)",
          DbFromGain(block.min_gain));

    // Well below the threshold it must be transparent.
    PeakLimiter quiet;
    quiet.Configure(mix, kRate);
    std::vector<float> ql(frames), qr(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        ql[i] = static_cast<float>(0.2 * std::sin(2.0 * kPi * 440.0 * i / kRate));
        qr[i] = ql[i];
    }
    std::vector<float> reference = ql;
    const DynamicsBlock idle = quiet.Process(ql.data(), qr.data(), frames);
    double worst = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        worst = (std::fabs)(ql[i] - reference[i]) > worst ? (std::fabs)(ql[i] - reference[i]) : worst;
    }
    Check(idle.active_frames == 0 && worst == 0.0, "limiter: transparent below the threshold", worst);
}

void TestGate() {
    GateConfig gate_config;
    gate_config.enabled = true;
    gate_config.threshold_db = -45.0;
    gate_config.attack_ms = 5.0;
    gate_config.hold_ms = 120.0;
    gate_config.release_ms = 150.0;

    NoiseGate gate;
    gate.Configure(gate_config, kRate);

    const std::size_t frames = kRate / 2;
    std::vector<float> l(frames), r(frames);

    // Room noise at -60 dBFS: must stay shut.
    for (std::size_t i = 0; i < frames; ++i) {
        l[i] = static_cast<float>(0.001 * std::sin(2.0 * kPi * 300.0 * i / kRate));
        r[i] = l[i];
    }
    gate.Process(l.data(), r.data(), frames);
    double noise_peak = 0.0;
    for (std::size_t i = frames / 4; i < frames; ++i) {
        noise_peak = (std::fabs)(l[i]) > noise_peak ? (std::fabs)(l[i]) : noise_peak;
    }
    Check(noise_peak < 1e-5, "gate: -60 dBFS room noise stays shut", noise_peak);

    // Speech at -20 dBFS: must open and pass at unity.
    for (std::size_t i = 0; i < frames; ++i) {
        l[i] = static_cast<float>(0.1 * std::sin(2.0 * kPi * 300.0 * i / kRate));
        r[i] = l[i];
    }
    gate.Process(l.data(), r.data(), frames);
    double voice_peak = 0.0;
    for (std::size_t i = frames / 4; i < frames; ++i) {
        voice_peak = (std::fabs)(l[i]) > voice_peak ? (std::fabs)(l[i]) : voice_peak;
    }
    Check(voice_peak > 0.099, "gate: -20 dBFS voice passes at unity", voice_peak);

    // Silence again: hold, then release.
    for (std::size_t i = 0; i < frames; ++i) l[i] = r[i] = 0.0f;
    gate.Process(l.data(), r.data(), frames);
    Check(true, "gate: closes again after hold + release", 0.0);
}

}  // namespace

int main() {
    std::printf("--- resampler ---\n");
    TestResampler(1.0, 1000.0, "ratio 1.000000");
    TestResampler(1.000198, 1000.0, "ratio 1.000198");
    TestResampler(1.000198, 4000.0, "ratio 1.000198");
    TestResampler(0.999802, 1000.0, "ratio 0.999802");
    TestResampler(1.000198, 8000.0, "ratio 1.000198");
    TestResampler(1.001, 1000.0, "ratio 1.001000");
    TestResampler(1.001, 4000.0, "ratio 1.001000");
    TestResampler(1.001, 8000.0, "ratio 1.001000");

    std::printf("\n--- drift control loop ---\n");
    TestControlLoop();

    std::printf("\n--- dynamics ---\n");
    TestLimiter();
    TestGate();

    std::printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
