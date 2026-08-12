#pragma once

#include "audio_stats.h"
#include "com.h"
#include "config.h"
#include "device_registry.h"
#include "ring_buffer.h"
#include "sample_convert.h"
#include "win_headers.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace vcmic {

// One WASAPI capture stream: either a loopback of a render endpoint (the chat
// side) or an ordinary shared-mode capture (the microphone). Spec 4.4 has both
// polled from their own thread with a high-resolution waitable timer, because
// shared-mode loopback is unreliable with an event callback. Everything the
// thread produces goes into an SPSC ring that the render thread drains.
class CaptureSource {
public:
    CaptureSource(const wchar_t* label, EDataFlow flow, bool loopback);
    ~CaptureSource();

    CaptureSource(const CaptureSource&) = delete;
    CaptureSource& operator=(const CaptureSource&) = delete;

    HRESULT Open(IMMDeviceEnumerator* enumerator, const DeviceSelector& selector,
                 const AudioConfig& audio, std::wstring& error);
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

    StereoRing& ring() { return ring_; }
    const StereoRing& ring() const { return ring_; }
    SourceStats& stats() { return stats_; }
    HRESULT fault() const { return fault_.load(std::memory_order_relaxed); }

private:
    void ThreadMain(HANDLE stop_event);
    void DrainAvailable() noexcept;  // audio path

    const wchar_t* label_;
    EDataFlow flow_;
    bool loopback_;

    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    EndpointInfo info_;
    MatchKind matched_ = MatchKind::None;

    std::vector<std::uint8_t> format_blob_;
    FormatInfo format_;
    SampleFormat sample_format_ = SampleFormat::Unsupported;
    DownmixMap downmix_;
    std::uint32_t block_align_ = 0;
    std::uint32_t buffer_frames_ = 0;
    double poll_ms_ = 5.0;

    StereoRing ring_;
    SourceStats stats_;
    std::vector<float> scratch_left_;
    std::vector<float> scratch_right_;

    std::thread thread_;
    std::atomic<HRESULT> fault_{S_OK};
};

// The render stream on the cable, and the clock master of the whole thing
// (spec 4.4): it is event-driven, and every event pulls whatever both rings
// have, mixes it and hands it to WASAPI.
class RenderSink {
public:
    RenderSink();
    ~RenderSink();

    RenderSink(const RenderSink&) = delete;
    RenderSink& operator=(const RenderSink&) = delete;

    HRESULT Open(IMMDeviceEnumerator* enumerator, const DeviceSelector& selector,
                 const AudioConfig& audio, std::wstring& error);
    void Close();

    // Must be called before StartThread; `chat` and `mic` have to outlive the thread.
    void Bind(CaptureSource& chat, CaptureSource& mic, const AudioConfig& audio,
              const MixConfig& mix);

    bool StartThread(HANDLE stop_event);
    void JoinThread();

    const EndpointInfo& info() const { return info_; }
    const FormatInfo& format() const { return format_; }
    std::wstring FormatDescription() const { return DescribeFormat(AsWaveFormat(format_blob_)); }
    MatchKind matched() const { return matched_; }
    SampleFormat sample_format() const { return sample_format_; }
    std::uint32_t buffer_frames() const { return buffer_frames_; }
    double buffer_ms() const;

    RenderStats& stats() { return stats_; }
    HRESULT fault() const { return fault_.load(std::memory_order_relaxed); }

private:
    // Consumer-side view of one capture ring. The fill level is managed here
    // rather than in CaptureSource because only the render thread may move the
    // read index.
    struct SourceState {
        StereoRing* ring = nullptr;
        SourceStats* stats = nullptr;
        bool primed = false;
        std::size_t target_frames = 0;
        std::size_t max_frames = 0;
        float gain_current = 1.0f;
        float gain_target = 1.0f;
    };

    void ThreadMain(HANDLE stop_event);
    void MixInto(std::uint8_t* destination, std::size_t frames) noexcept;
    void PullSource(SourceState& state, std::size_t frames, float* left, float* right) noexcept;

    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    EndpointInfo info_;
    MatchKind matched_ = MatchKind::None;
    HANDLE event_ = nullptr;

    std::vector<std::uint8_t> format_blob_;
    FormatInfo format_;
    SampleFormat sample_format_ = SampleFormat::Unsupported;
    UpmixMap upmix_;
    std::uint32_t block_align_ = 0;
    std::uint32_t buffer_frames_ = 0;

    SourceState chat_;
    SourceState mic_;
    float gain_coeff_ = 1.0f;

    std::vector<float> chat_left_;
    std::vector<float> chat_right_;
    std::vector<float> mic_left_;
    std::vector<float> mic_right_;
    std::vector<float> out_left_;
    std::vector<float> out_right_;

    RenderStats stats_;
    std::thread thread_;
    std::atomic<HRESULT> fault_{S_OK};
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    HRESULT Start(IMMDeviceEnumerator* enumerator, const Config& config, std::wstring& error);

    // Blocks until stop_event is signalled or the render stream dies, logging
    // the periodic counter report in between.
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
    HANDLE stop_event_ = nullptr;  // owned by the caller, signalled here to unblock the threads
    std::uint32_t sample_rate_ = 48000;
    bool running_ = false;
    bool chat_fault_reported_ = false;
    bool mic_fault_reported_ = false;
};

}  // namespace vcmic
