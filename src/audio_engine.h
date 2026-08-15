#pragma once

#include "audio_stats.h"
#include "com.h"
#include "config.h"
#include "device_registry.h"
#include "device_watcher.h"
#include "drift.h"
#include "dynamics.h"
#include "ring_buffer.h"
#include "sample_convert.h"
#include "win_headers.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace vcmic {

// Spec 4.9, the optional half: a render endpoint nobody is playing into stops
// its audio engine, and WASAPI loopback on a stopped engine delivers no packets
// at all - not even silent ones. Holding a started render client of our own on
// that endpoint keeps its clock running, so the loopback keeps handing over
// silence, the ring stays fed and the drift loop never has to reconverge.
//
// It is a real stream on somebody else's output device, which is why it is off
// by default. What it renders is digital silence, so nothing is audible and the
// hardware GameVoice Mix is untouched.
class ClockKeeper {
public:
    ClockKeeper() = default;
    ~ClockKeeper();

    ClockKeeper(const ClockKeeper&) = delete;
    ClockKeeper& operator=(const ClockKeeper&) = delete;

    // `device` is the same endpoint the loopback capture reads.
    HRESULT Open(IMMDevice* device, std::wstring& error);
    void Close();

    // False until the stream is started, so a half-open one is never pumped.
    bool active() const noexcept { return started_; }

    // Audio path, called from the capture poll tick: tops the endpoint buffer
    // up with silence. Returns the failure that took the stream down.
    HRESULT Pump() noexcept;

private:
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    std::uint32_t buffer_frames_ = 0;
    bool started_ = false;
};

// One WASAPI capture stream: either a loopback of a render endpoint (the chat
// side) or an ordinary shared-mode capture (the microphone). Spec 4.4 has both
// polled from their own thread with a high-resolution waitable timer, because
// shared-mode loopback is unreliable with an event callback. Everything the
// thread produces goes into an SPSC ring that the render thread drains.
//
// The thread also owns the stream's recovery (spec 4.8): it is the only thread
// that touches these WASAPI objects, so it can drop and rebuild them without a
// lock anywhere near the audio path.
class CaptureSource {
public:
    CaptureSource(const wchar_t* label, EDataFlow flow, bool loopback);
    ~CaptureSource();

    CaptureSource(const CaptureSource&) = delete;
    CaptureSource& operator=(const CaptureSource&) = delete;

    // Called once, before the thread starts. Sizes the ring and remembers
    // everything the thread will need to rebuild the stream on its own.
    HRESULT Open(IMMDeviceEnumerator* enumerator, const DeviceSelector& selector,
                 const Config& config, std::wstring& error);
    void Close();

    bool StartThread(HANDLE stop_event);
    void JoinThread();

    const wchar_t* label() const { return label_; }
    const EndpointInfo& info() const { return info_; }
    const FormatInfo& format() const { return format_; }
    std::wstring FormatDescription() const { return DescribeFormat(AsWaveFormat(format_blob_)); }
    MatchKind matched() const { return matched_; }
    SampleFormat sample_format() const { return sample_format_; }
    const DownmixMap& downmix() const { return downmix_; }
    std::uint32_t buffer_frames() const { return buffer_frames_; }
    double poll_interval_ms() const { return poll_ms_; }
    bool clock_keeper_enabled() const { return keep_clock_alive_; }

    // Pulsed by the device watcher to cut short a rebuild backoff (spec 4.8).
    HANDLE wake_event() const { return wake_event_; }

    StereoRing& ring() { return ring_; }
    const StereoRing& ring() const { return ring_; }
    SourceStats& stats() { return stats_; }
    HRESULT fault() const { return fault_.load(std::memory_order_relaxed); }

private:
    // Everything WASAPI, and nothing else: the ring and the scratch buffers are
    // left alone so that the capture thread may call this while the render
    // thread is still reading what the ring already holds. When `reopening`,
    // the endpoint has to come back with the format the engine was built for.
    HRESULT OpenStream(IMMDeviceEnumerator* enumerator, bool reopening, std::wstring& error);
    void CloseStream();
    void OpenClockKeeper();

    void ThreadMain(HANDLE stop_event);

    // Is the endpoint this stream was opened on still there? Asked only after a
    // long silence, so it costs at most one pair of COM calls every few seconds
    // and only while nothing whatsoever is arriving.
    bool EndpointStillActive(IMMDeviceEnumerator* enumerator) const;

    // Runs until the stop event, a WASAPI failure, or the silence watchdog;
    // S_OK means "asked to stop", and `reason` describes anything else.
    HRESULT Drain(IMMDeviceEnumerator* enumerator, HANDLE stop_event, HANDLE timer,
                  std::wstring& reason);
    HRESULT DrainAvailable() noexcept;  // audio path

    const wchar_t* label_;
    EDataFlow flow_;
    bool loopback_;

    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    EndpointInfo info_;
    MatchKind matched_ = MatchKind::None;

    // Kept for the rebuild, which happens on the audio thread with nobody left
    // to hand it the configuration.
    DeviceSelector selector_;
    AudioConfig audio_;
    std::uint32_t backoff_min_ms_ = 100;
    std::uint32_t backoff_max_ms_ = 5000;
    bool keep_clock_alive_ = false;
    ClockKeeper keeper_;
    HANDLE wake_event_ = nullptr;

    std::vector<std::uint8_t> format_blob_;
    FormatInfo format_;
    SampleFormat sample_format_ = SampleFormat::Unsupported;
    DownmixMap downmix_;
    std::uint32_t block_align_ = 0;
    std::uint32_t buffer_frames_ = 0;
    double poll_ms_ = 5.0;
    // Poll ticks of unbroken silence after which the stream's health is
    // questioned; for a loopback source that means asking the enumerator rather
    // than concluding anything, because silence there is normal.
    std::uint32_t silence_limit_ticks_ = 1;
    const bool silence_is_normal_;

    StereoRing ring_;
    SourceStats stats_;
    std::vector<float> scratch_left_;
    std::vector<float> scratch_right_;

    std::thread thread_;
    std::atomic<HRESULT> fault_{S_OK};  // only what a rebuild cannot fix
};

// The render stream on the cable, and the clock master of the whole thing
// (spec 4.4): it is event-driven, and every event pulls whatever both rings
// have, resamples each of them to the cable's clock, mixes and hands the
// result to WASAPI.
class RenderSink {
public:
    RenderSink();
    ~RenderSink();

    RenderSink(const RenderSink&) = delete;
    RenderSink& operator=(const RenderSink&) = delete;

    HRESULT Open(IMMDeviceEnumerator* enumerator, const DeviceSelector& selector,
                 const Config& config, std::wstring& error);
    void Close();

    // Must be called before StartThread; `chat` and `mic` have to outlive the
    // thread. This is where every audio-path buffer is sized, so that the
    // thread itself never allocates.
    void Bind(CaptureSource& chat, CaptureSource& mic, const Config& config);

    bool StartThread(HANDLE stop_event);
    void JoinThread();

    const EndpointInfo& info() const { return info_; }
    const FormatInfo& format() const { return format_; }
    std::wstring FormatDescription() const { return DescribeFormat(AsWaveFormat(format_blob_)); }
    MatchKind matched() const { return matched_; }
    SampleFormat sample_format() const { return sample_format_; }
    std::uint32_t buffer_frames() const { return buffer_frames_; }
    double buffer_ms() const;
    HANDLE wake_event() const { return wake_event_; }

    const PeakLimiter& limiter() const { return limiter_; }
    const NoiseGate& gate() const { return gate_; }

    RenderStats& stats() { return stats_; }
    HRESULT fault() const { return fault_.load(std::memory_order_relaxed); }

private:
    // Consumer-side view of one capture ring. The fill level and the drift
    // correction live here rather than in CaptureSource because only the render
    // thread may move the read index.
    struct SourceState {
        StereoRing* ring = nullptr;
        SourceStats* stats = nullptr;
        bool primed = false;
        std::size_t target_frames = 0;
        std::size_t max_frames = 0;
        float gain_current = 1.0f;
        float gain_target = 1.0f;

        bool drift_enabled = false;
        DriftController drift;
        StereoResampler resampler;

        std::vector<float> in_left;   // ring reads, one block plus correction headroom
        std::vector<float> in_right;
        std::vector<float> left;      // this source's contribution, on the cable's clock
        std::vector<float> right;
    };

    HRESULT OpenStream(IMMDeviceEnumerator* enumerator, bool reopening, std::wstring& error);
    void CloseStream();

    // Sizes every audio-path buffer from the current block length. Called by
    // Bind, and again after a rebuild if the cable came back with a different
    // block - which is exactly what changing Max Latency in the VB-CABLE
    // control panel does to us.
    void SizeBuffers();

    // Takes over from a stream that has just been rebuilt: re-sizes if the
    // cable came back with a different block, and makes both sources re-prime.
    void AdoptRebuiltStream();

    void ThreadMain(HANDLE stop_event);

    // Runs until the stop event or a failure; S_OK means "asked to stop", and
    // `reason` describes anything else for the log.
    HRESULT Serve(HANDLE stop_event, std::wstring& reason);
    void MixInto(std::uint8_t* destination, std::size_t frames) noexcept;
    void PullSource(SourceState& state, std::size_t frames) noexcept;

    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    EndpointInfo info_;
    MatchKind matched_ = MatchKind::None;
    HANDLE event_ = nullptr;
    HANDLE wake_event_ = nullptr;

    DeviceSelector selector_;
    AudioConfig audio_;
    std::uint32_t backoff_min_ms_ = 100;
    std::uint32_t backoff_max_ms_ = 5000;

    std::vector<std::uint8_t> format_blob_;
    FormatInfo format_;
    SampleFormat sample_format_ = SampleFormat::Unsupported;
    UpmixMap upmix_;
    std::uint32_t block_align_ = 0;
    std::uint32_t buffer_frames_ = 0;

    SourceState chat_;
    SourceState mic_;
    float gain_coeff_ = 1.0f;
    double inv_rate_ = 0.0;  // seconds per frame, so the audio path never divides

    NoiseGate gate_;
    PeakLimiter limiter_;

    std::vector<float> out_left_;
    std::vector<float> out_right_;

    RenderStats stats_;
    std::thread thread_;
    std::atomic<HRESULT> fault_{S_OK};  // only what a rebuild cannot fix
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    HRESULT Start(IMMDeviceEnumerator* enumerator, const Config& config, std::wstring& error);

    // Blocks until stop_event is signalled or a thread hits something a rebuild
    // cannot fix, logging the periodic counter report in between.
    void Run(HANDLE stop_event, std::uint32_t stats_interval_s);

    void Stop();
    void LogSummary();

private:
    void LogStartupSummary(const Config& config);
    void LogCounters(const wchar_t* prefix);
    bool CheckFaults();

    CaptureSource chat_{L"chat", eRender, true};
    CaptureSource mic_{L"mic", eCapture, false};
    RenderSink cable_;
    ComPtr<DeviceWatcher> watcher_;
    HANDLE stop_event_ = nullptr;  // owned by the caller, signalled here to unblock the threads
    std::uint32_t sample_rate_ = 48000;
    bool running_ = false;
    bool chat_fault_reported_ = false;
    bool mic_fault_reported_ = false;
};

}  // namespace vcmic
