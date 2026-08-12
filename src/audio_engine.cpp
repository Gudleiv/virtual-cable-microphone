#include "audio_engine.h"

#include <avrt.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include "hresult.h"
#include "logging.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace vcmic {
namespace {

// Capture buffers only need to be large enough that a scheduling stall does not
// lose data; polled capture drains everything each tick, so their size costs no
// latency. The render buffer is a different matter and is left to WASAPI, which
// picks the device period configured in VBCABLE_ControlPanel.exe.
constexpr REFERENCE_TIME kCaptureBufferDuration = 100 * 10000;  // 100 ms in 100 ns units
constexpr DWORD kRenderWaitMs = 2000;
constexpr double kMinPollMs = 1.0;
constexpr double kMaxPollMs = 5.0;

// MMCSS registration for an audio thread (spec 4.10), released on the way out.
class MmcssTask {
public:
    explicit MmcssTask(const wchar_t* name) {
        DWORD index = 0;
        handle_ = ::AvSetMmThreadCharacteristicsW(name, &index);
    }

    ~MmcssTask() {
        if (handle_ != nullptr) {
            ::AvRevertMmThreadCharacteristics(handle_);
        }
    }

    MmcssTask(const MmcssTask&) = delete;
    MmcssTask& operator=(const MmcssTask&) = delete;

    bool ok() const { return handle_ != nullptr; }

private:
    HANDLE handle_ = nullptr;
};

HANDLE CreatePollTimer() {
    HANDLE timer = ::CreateWaitableTimerExW(nullptr, nullptr,
                                            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == nullptr) {
        // Before Windows 10 1803 the high-resolution flag does not exist.
        timer = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    return timer;
}

double FramesToMs(std::uint64_t frames, std::uint32_t rate) {
    return rate == 0 ? 0.0 : (static_cast<double>(frames) * 1000.0 / static_cast<double>(rate));
}

std::size_t MsToFrames(double ms, std::uint32_t rate) {
    const double frames = ms * static_cast<double>(rate) / 1000.0;
    return frames <= 0.0 ? 0 : static_cast<std::size_t>(frames);
}

float GainFromDb(double db) { return static_cast<float>(std::pow(10.0, db / 20.0)); }

// One-pole coefficient reaching ~63 % of a gain change in `ms` (spec 4.7).
float SmoothingCoefficient(double ms, std::uint32_t rate) {
    if (ms <= 0.0 || rate == 0) {
        return 1.0f;
    }
    const double samples = ms * static_cast<double>(rate) / 1000.0;
    return static_cast<float>(1.0 - std::exp(-1.0 / samples));
}

std::wstring DescribeEndpoint(const EndpointInfo& info) {
    return std::format(L"{} [{}]", info.friendly_name, info.id);
}

}  // namespace

// ---------------------------------------------------------------- CaptureSource

CaptureSource::CaptureSource(const wchar_t* label, EDataFlow flow, bool loopback)
    : label_(label), flow_(flow), loopback_(loopback) {}

CaptureSource::~CaptureSource() {
    JoinThread();
    Close();
}

HRESULT CaptureSource::Open(IMMDeviceEnumerator* enumerator, const DeviceSelector& selector,
                            const AudioConfig& audio, std::wstring& error) {
    ResolvedDevice resolved;
    HRESULT hr = ResolveDevice(enumerator, flow_, selector, false, resolved, error);
    if (FAILED(hr)) {
        return hr;
    }
    device_ = resolved.device;
    info_ = resolved.info;
    matched_ = resolved.matched;

    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client_.PutVoid());
    if (FAILED(hr)) {
        error = std::format(L"Activate(IAudioClient) failed: {}", FormatHresult(hr));
        return hr;
    }

    WAVEFORMATEX* mix = nullptr;
    hr = client_->GetMixFormat(&mix);
    if (FAILED(hr) || mix == nullptr) {
        error = std::format(L"GetMixFormat failed: {}", FormatHresult(hr));
        return FAILED(hr) ? hr : E_FAIL;
    }
    CoTaskMemPtr<WAVEFORMATEX> owned_format(mix);

    format_blob_ = CloneFormat(mix);
    format_ = InspectFormat(mix);
    sample_format_ = ResolveSampleFormat(format_);
    block_align_ = format_.block_align;

    if (sample_format_ == SampleFormat::Unsupported) {
        error = std::format(L"unsupported mix format: {}", DescribeFormat(mix));
        return E_FAIL;
    }
    if (audio.require_sample_rate && format_.sample_rate != audio.sample_rate) {
        error = std::format(
            L"runs at {} Hz but audio.sample_rate is {} Hz; there is no resampler yet",
            format_.sample_rate, audio.sample_rate);
        return E_FAIL;
    }
    if (format_.channels == 0 || block_align_ == 0) {
        error = L"mix format reports no channels";
        return E_FAIL;
    }

    downmix_ = BuildDownmix(format_);

    const DWORD flags = loopback_ ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kCaptureBufferDuration, 0,
                             AsWaveFormat(format_blob_), nullptr);
    if (FAILED(hr)) {
        error = std::format(L"IAudioClient::Initialize failed: {}", FormatHresult(hr));
        return hr;
    }

    hr = client_->GetBufferSize(&buffer_frames_);
    if (FAILED(hr)) {
        error = std::format(L"GetBufferSize failed: {}", FormatHresult(hr));
        return hr;
    }

    hr = client_->GetService(__uuidof(IAudioCaptureClient), capture_.PutVoid());
    if (FAILED(hr)) {
        error = std::format(L"GetService(IAudioCaptureClient) failed: {}", FormatHresult(hr));
        return hr;
    }

    ring_.Reset(MsToFrames(audio.ring_capacity_ms, format_.sample_rate));
    scratch_left_.assign(buffer_frames_, 0.0f);
    scratch_right_.assign(buffer_frames_, 0.0f);

    // Poll at roughly half the device period, clamped to something sane.
    REFERENCE_TIME default_period = 0;
    REFERENCE_TIME min_period = 0;
    double period_ms = 0.0;
    if (SUCCEEDED(client_->GetDevicePeriod(&default_period, &min_period))) {
        period_ms = static_cast<double>(default_period) / 10000.0;
    }
    poll_ms_ = period_ms > 0.0 ? period_ms / 2.0 : kMaxPollMs;
    poll_ms_ = (std::max)(kMinPollMs, (std::min)(kMaxPollMs, poll_ms_));

    return S_OK;
}

void CaptureSource::Close() {
    capture_.Reset();
    client_.Reset();
    device_.Reset();
}

bool CaptureSource::StartThread(HANDLE stop_event) {
    if (!client_ || !capture_ || thread_.joinable()) {
        return false;
    }
    thread_ = std::thread(&CaptureSource::ThreadMain, this, stop_event);
    return true;
}

void CaptureSource::JoinThread() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

void CaptureSource::DrainAvailable() noexcept {
    const std::size_t scratch_frames = scratch_left_.size();

    UINT32 packet_frames = 0;
    HRESULT hr = capture_->GetNextPacketSize(&packet_frames);
    if (FAILED(hr)) {
        fault_.store(hr, std::memory_order_relaxed);
        return;
    }

    // Zero packets is normal and means nobody is playing into the endpoint;
    // the render side mixes silence for us (spec 4.9).
    while (packet_frames > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) {
            return;
        }
        if (FAILED(hr)) {
            fault_.store(hr, std::memory_order_relaxed);
            return;
        }

        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr;
        stats_.packets.fetch_add(1, std::memory_order_relaxed);
        if (silent) {
            stats_.silent_packets.fetch_add(1, std::memory_order_relaxed);
        }
        if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
            stats_.discontinuities.fetch_add(1, std::memory_order_relaxed);
        }

        std::size_t remaining = frames;
        std::size_t offset = 0;
        while (remaining > 0 && scratch_frames > 0) {
            const std::size_t chunk = (std::min)(remaining, scratch_frames);
            if (silent) {
                std::memset(scratch_left_.data(), 0, chunk * sizeof(float));
                std::memset(scratch_right_.data(), 0, chunk * sizeof(float));
            } else {
                CaptureToStereo(sample_format_, downmix_, block_align_,
                                data + offset * block_align_, chunk, scratch_left_.data(),
                                scratch_right_.data());
            }

            const std::size_t written =
                ring_.Write(scratch_left_.data(), scratch_right_.data(), chunk);
            if (written < chunk) {
                stats_.overruns.fetch_add(1, std::memory_order_relaxed);
                stats_.overrun_frames.fetch_add(chunk - written, std::memory_order_relaxed);
            }

            remaining -= chunk;
            offset += chunk;
        }
        stats_.frames.fetch_add(frames, std::memory_order_relaxed);

        hr = capture_->ReleaseBuffer(frames);
        if (FAILED(hr)) {
            fault_.store(hr, std::memory_order_relaxed);
            return;
        }

        hr = capture_->GetNextPacketSize(&packet_frames);
        if (FAILED(hr)) {
            fault_.store(hr, std::memory_order_relaxed);
            return;
        }
    }
}

void CaptureSource::ThreadMain(HANDLE stop_event) {
    const ComApartment apartment;
    const MmcssTask mmcss(L"Pro Audio");

    HANDLE timer = CreatePollTimer();
    if (timer == nullptr) {
        fault_.store(HRESULT_FROM_WIN32(::GetLastError()), std::memory_order_relaxed);
        return;
    }

    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(poll_ms_ * 10000.0);
    const LONG period_ms = (std::max)(1L, static_cast<LONG>(poll_ms_));
    if (!::SetWaitableTimer(timer, &due, period_ms, nullptr, nullptr, FALSE)) {
        fault_.store(HRESULT_FROM_WIN32(::GetLastError()), std::memory_order_relaxed);
        ::CloseHandle(timer);
        return;
    }

    HRESULT hr = client_->Start();
    if (FAILED(hr)) {
        fault_.store(hr, std::memory_order_relaxed);
        ::CancelWaitableTimer(timer);
        ::CloseHandle(timer);
        return;
    }

    HANDLE waits[2] = {stop_event, timer};
    for (;;) {
        const DWORD result = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (result != WAIT_OBJECT_0 + 1) {
            break;  // stop requested, or the wait itself failed
        }
        DrainAvailable();
        if (FAILED(fault_.load(std::memory_order_relaxed))) {
            break;
        }
    }

    client_->Stop();
    ::CancelWaitableTimer(timer);
    ::CloseHandle(timer);
}

// ------------------------------------------------------------------ RenderSink

RenderSink::RenderSink() = default;

RenderSink::~RenderSink() {
    JoinThread();
    Close();
}

double RenderSink::buffer_ms() const {
    return FramesToMs(buffer_frames_, format_.sample_rate);
}

HRESULT RenderSink::Open(IMMDeviceEnumerator* enumerator, const DeviceSelector& selector,
                         const AudioConfig& audio, std::wstring& error) {
    ResolvedDevice resolved;
    HRESULT hr = ResolveDevice(enumerator, eRender, selector, false, resolved, error);
    if (FAILED(hr)) {
        return hr;
    }
    device_ = resolved.device;
    info_ = resolved.info;
    matched_ = resolved.matched;

    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client_.PutVoid());
    if (FAILED(hr)) {
        error = std::format(L"Activate(IAudioClient) failed: {}", FormatHresult(hr));
        return hr;
    }

    WAVEFORMATEX* mix = nullptr;
    hr = client_->GetMixFormat(&mix);
    if (FAILED(hr) || mix == nullptr) {
        error = std::format(L"GetMixFormat failed: {}", FormatHresult(hr));
        return FAILED(hr) ? hr : E_FAIL;
    }
    CoTaskMemPtr<WAVEFORMATEX> owned_format(mix);

    format_blob_ = CloneFormat(mix);
    format_ = InspectFormat(mix);
    sample_format_ = ResolveSampleFormat(format_);
    block_align_ = format_.block_align;

    if (sample_format_ == SampleFormat::Unsupported) {
        error = std::format(L"unsupported mix format: {}", DescribeFormat(mix));
        return E_FAIL;
    }
    if (audio.require_sample_rate && format_.sample_rate != audio.sample_rate) {
        error = std::format(
            L"runs at {} Hz but audio.sample_rate is {} Hz; set Internal Sample Rate in "
            L"VBCABLE_ControlPanel.exe",
            format_.sample_rate, audio.sample_rate);
        return E_FAIL;
    }
    if (format_.channels == 0 || block_align_ == 0) {
        error = L"mix format reports no channels";
        return E_FAIL;
    }

    upmix_ = BuildUpmix(format_);

    event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event_ == nullptr) {
        error = L"CreateEvent for the render callback failed";
        return HRESULT_FROM_WIN32(::GetLastError());
    }

    // Duration 0 lets WASAPI use the endpoint's own period, which is what the
    // Max Latency setting of the cable controls.
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0,
                             AsWaveFormat(format_blob_), nullptr);
    if (FAILED(hr)) {
        error = std::format(L"IAudioClient::Initialize failed: {}", FormatHresult(hr));
        return hr;
    }

    hr = client_->SetEventHandle(event_);
    if (FAILED(hr)) {
        error = std::format(L"SetEventHandle failed: {}", FormatHresult(hr));
        return hr;
    }

    hr = client_->GetBufferSize(&buffer_frames_);
    if (FAILED(hr)) {
        error = std::format(L"GetBufferSize failed: {}", FormatHresult(hr));
        return hr;
    }

    hr = client_->GetService(__uuidof(IAudioRenderClient), render_.PutVoid());
    if (FAILED(hr)) {
        error = std::format(L"GetService(IAudioRenderClient) failed: {}", FormatHresult(hr));
        return hr;
    }

    chat_left_.assign(buffer_frames_, 0.0f);
    chat_right_.assign(buffer_frames_, 0.0f);
    mic_left_.assign(buffer_frames_, 0.0f);
    mic_right_.assign(buffer_frames_, 0.0f);
    out_left_.assign(buffer_frames_, 0.0f);
    out_right_.assign(buffer_frames_, 0.0f);

    return S_OK;
}

void RenderSink::Close() {
    render_.Reset();
    client_.Reset();
    device_.Reset();
    if (event_ != nullptr) {
        ::CloseHandle(event_);
        event_ = nullptr;
    }
}

void RenderSink::Bind(CaptureSource& chat, CaptureSource& mic, const AudioConfig& audio,
                      const MixConfig& mix) {
    const std::uint32_t rate = format_.sample_rate;
    const std::size_t target = MsToFrames(audio.target_buffer_ms, rate);

    const auto bind_one = [&](SourceState& state, CaptureSource& source, double gain_db) {
        state.ring = &source.ring();
        state.stats = &source.stats();
        state.primed = false;
        state.target_frames = target == 0 ? std::size_t{1} : target;
        // Four times the target, but never more than half the ring: past that
        // the backlog is latency nobody asked for, so it gets dropped.
        state.max_frames = (std::min)(state.target_frames * 4, source.ring().capacity() / 2);
        state.max_frames = (std::max)(state.max_frames, state.target_frames + 1);
        state.gain_target = GainFromDb(gain_db);
        state.gain_current = state.gain_target;
    };

    bind_one(chat_, chat, mix.chat_gain_db);
    bind_one(mic_, mic, mix.mic_gain_db);
    gain_coeff_ = SmoothingCoefficient(mix.gain_smoothing_ms, rate);
}

bool RenderSink::StartThread(HANDLE stop_event) {
    if (!client_ || !render_ || thread_.joinable()) {
        return false;
    }
    thread_ = std::thread(&RenderSink::ThreadMain, this, stop_event);
    return true;
}

void RenderSink::JoinThread() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

void RenderSink::PullSource(SourceState& state, std::size_t frames, float* left,
                            float* right) noexcept {
    const std::size_t available = state.ring->Readable();
    state.stats->fill_frames.store(static_cast<std::uint32_t>(available),
                                   std::memory_order_relaxed);

    // Hold the source silent until it has built up the target fill, so that
    // the steady-state latency is what the config asks for instead of whatever
    // the startup race happens to produce.
    if (!state.primed) {
        if (available < state.target_frames) {
            std::memset(left, 0, frames * sizeof(float));
            std::memset(right, 0, frames * sizeof(float));
            return;
        }
        state.primed = true;
        state.stats->primings.fetch_add(1, std::memory_order_relaxed);
    }

    // Emergency path of spec 4.6: a backlog this large is a stall that already
    // happened, and keeping it would just add permanent latency.
    if (available > state.max_frames) {
        const std::size_t dropped = state.ring->Discard(available - state.target_frames);
        if (dropped > 0) {
            state.stats->resyncs.fetch_add(1, std::memory_order_relaxed);
            state.stats->resync_frames.fetch_add(dropped, std::memory_order_relaxed);
        }
    }

    const std::size_t taken = state.ring->Read(left, right, frames);
    if (taken < frames) {
        const std::size_t missing = frames - taken;
        std::memset(left + taken, 0, missing * sizeof(float));
        std::memset(right + taken, 0, missing * sizeof(float));
        state.stats->underruns.fetch_add(1, std::memory_order_relaxed);
        state.stats->underrun_frames.fetch_add(missing, std::memory_order_relaxed);
        state.primed = false;  // refill to the target before consuming again
    }
}

void RenderSink::MixInto(std::uint8_t* destination, std::size_t frames) noexcept {
    PullSource(chat_, frames, chat_left_.data(), chat_right_.data());
    PullSource(mic_, frames, mic_left_.data(), mic_right_.data());

    const float* chat_left = chat_left_.data();
    const float* chat_right = chat_right_.data();
    const float* mic_left = mic_left_.data();
    const float* mic_right = mic_right_.data();
    float* out_left = out_left_.data();
    float* out_right = out_right_.data();

    float chat_gain = chat_.gain_current;
    float mic_gain = mic_.gain_current;
    const float chat_goal = chat_.gain_target;
    const float mic_goal = mic_.gain_target;
    const float coeff = gain_coeff_;

    std::uint64_t clipped = 0;
    for (std::size_t i = 0; i < frames; ++i) {
        chat_gain += (chat_goal - chat_gain) * coeff;
        mic_gain += (mic_goal - mic_gain) * coeff;

        float left = chat_left[i] * chat_gain + mic_left[i] * mic_gain;
        float right = chat_right[i] * chat_gain + mic_right[i] * mic_gain;

        // Stage 3 replaces this with a proper limiter; until then a hard clamp
        // at least keeps the cable from receiving out-of-range samples.
        if (left > 1.0f) {
            left = 1.0f;
            ++clipped;
        } else if (left < -1.0f) {
            left = -1.0f;
            ++clipped;
        }
        if (right > 1.0f) {
            right = 1.0f;
            ++clipped;
        } else if (right < -1.0f) {
            right = -1.0f;
            ++clipped;
        }

        out_left[i] = left;
        out_right[i] = right;
    }

    chat_.gain_current = chat_gain;
    mic_.gain_current = mic_gain;
    if (clipped > 0) {
        stats_.clipped_samples.fetch_add(clipped, std::memory_order_relaxed);
    }

    StereoToRender(sample_format_, upmix_, block_align_, out_left, out_right, frames, destination);

    stats_.callbacks.fetch_add(1, std::memory_order_relaxed);
    stats_.frames.fetch_add(frames, std::memory_order_relaxed);
}

void RenderSink::ThreadMain(HANDLE stop_event) {
    const ComApartment apartment;
    const MmcssTask mmcss(L"Pro Audio");

    // Prime the whole buffer with silence so the first event is not missed.
    BYTE* prefill = nullptr;
    if (SUCCEEDED(render_->GetBuffer(buffer_frames_, &prefill))) {
        render_->ReleaseBuffer(buffer_frames_, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    HRESULT hr = client_->Start();
    if (FAILED(hr)) {
        fault_.store(hr, std::memory_order_relaxed);
        return;
    }

    HANDLE waits[2] = {stop_event, event_};
    for (;;) {
        const DWORD result = ::WaitForMultipleObjects(2, waits, FALSE, kRenderWaitMs);
        if (result == WAIT_TIMEOUT) {
            stats_.timeouts.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (result != WAIT_OBJECT_0 + 1) {
            break;  // stop requested, or the wait failed
        }

        UINT32 padding = 0;
        hr = client_->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            fault_.store(hr, std::memory_order_relaxed);
            break;
        }
        if (padding > buffer_frames_) {
            continue;
        }

        const UINT32 frames = buffer_frames_ - padding;
        if (frames == 0) {
            continue;
        }

        BYTE* buffer = nullptr;
        hr = render_->GetBuffer(frames, &buffer);
        if (FAILED(hr)) {
            fault_.store(hr, std::memory_order_relaxed);
            break;
        }

        MixInto(buffer, frames);

        hr = render_->ReleaseBuffer(frames, 0);
        if (FAILED(hr)) {
            fault_.store(hr, std::memory_order_relaxed);
            break;
        }
    }

    client_->Stop();
}

// ----------------------------------------------------------------- AudioEngine

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() { Stop(); }

HRESULT AudioEngine::Start(IMMDeviceEnumerator* enumerator, const Config& config,
                           std::wstring& error) {
    HRESULT hr = chat_.Open(enumerator, config.devices.chat_render, config.audio, error);
    if (FAILED(hr)) {
        error = std::format(L"chat loopback source: {}", error);
        return hr;
    }
    hr = mic_.Open(enumerator, config.devices.mic_capture, config.audio, error);
    if (FAILED(hr)) {
        error = std::format(L"microphone: {}", error);
        return hr;
    }
    hr = cable_.Open(enumerator, config.devices.output_render, config.audio, error);
    if (FAILED(hr)) {
        error = std::format(L"cable output: {}", error);
        return hr;
    }

    sample_rate_ = cable_.format().sample_rate;
    if (chat_.format().sample_rate != sample_rate_ || mic_.format().sample_rate != sample_rate_) {
        error = std::format(
            L"sample rates differ: chat {} Hz, mic {} Hz, cable {} Hz; align them in the Windows "
            L"device properties and in VBCABLE_ControlPanel.exe",
            chat_.format().sample_rate, mic_.format().sample_rate, sample_rate_);
        return E_FAIL;
    }

    cable_.Bind(chat_, mic_, config.audio, config.mix);
    LogStartupSummary(config);
    return S_OK;
}

void AudioEngine::LogStartupSummary(const Config& config) {
    const auto describe_source = [](const CaptureSource& source, const wchar_t* role) {
        LogInfo(L"{}: {}", role, DescribeEndpoint(source.info()));
        LogInfo(L"  format {} -> stereo float32 ({}, {} ch, {})", source.FormatDescription(),
                SampleFormatName(source.sample_format()), source.format().channels,
                source.downmix().from_channel_mask ? L"channel mask" : L"assumed layout");
        LogInfo(L"  capture buffer {} frames, poll every {:.2f} ms, ring {} frames",
                source.buffer_frames(), source.poll_interval_ms(), source.ring().capacity());
        if (source.matched() == MatchKind::ByName) {
            LogWarn(L"  matched by name fragment, not by endpoint id - copy the id into the config");
        }
    };

    LogInfo(L"--- audio engine starting ---");
    describe_source(chat_, L"chat loopback source");
    describe_source(mic_, L"microphone");

    LogInfo(L"cable output: {}", DescribeEndpoint(cable_.info()));
    LogInfo(L"  format {} ({}, {} ch)", cable_.FormatDescription(),
            SampleFormatName(cable_.sample_format()), cable_.format().channels);
    LogInfo(L"  render buffer {} frames ({:.2f} ms), event-driven, clock master",
            cable_.buffer_frames(), cable_.buffer_ms());
    if (cable_.matched() == MatchKind::ByName) {
        LogWarn(L"  matched by name fragment, not by endpoint id - copy the id into the config");
    }

    LogInfo(L"mix: chat {:+.1f} dB, mic {:+.1f} dB, smoothing {:.1f} ms, hard clamp on the sum "
            L"(the limiter lands in stage 3)",
            config.mix.chat_gain_db, config.mix.mic_gain_db, config.mix.gain_smoothing_ms);
    LogInfo(L"target ring fill {:.1f} ms; estimated added latency ~{:.1f} ms "
            L"(render buffer {:.1f} ms + ring {:.1f} ms)",
            config.audio.target_buffer_ms, cable_.buffer_ms() + config.audio.target_buffer_ms,
            cable_.buffer_ms(), config.audio.target_buffer_ms);
    LogInfo(L"drift compensation is not implemented yet (stage 3): expect the offset to grow "
            L"slowly over a long session");
}

void AudioEngine::Run(HANDLE stop_event, std::uint32_t stats_interval_s) {
    constexpr DWORD kPollMs = 500;

    stop_event_ = stop_event;
    // Captures first, so the rings are already filling when the clock master
    // starts asking for frames.
    if (!chat_.StartThread(stop_event) || !mic_.StartThread(stop_event) ||
        !cable_.StartThread(stop_event)) {
        LogError(L"could not start the audio threads");
        return;
    }
    running_ = true;
    LogInfo(L"running; press Ctrl+C to stop");

    DWORD since_report_ms = 0;
    for (;;) {
        const DWORD result = ::WaitForSingleObject(stop_event, kPollMs);
        if (CheckFaults()) {
            break;
        }
        if (result == WAIT_OBJECT_0) {
            LogInfo(L"stop requested");
            break;
        }
        if (result != WAIT_TIMEOUT) {
            break;
        }
        since_report_ms += kPollMs;
        if (stats_interval_s != 0 && since_report_ms >= stats_interval_s * 1000) {
            LogCounters(L"stats");
            since_report_ms = 0;
        }
    }
}

bool AudioEngine::CheckFaults() {
    const HRESULT chat_fault = chat_.fault();
    if (FAILED(chat_fault) && !chat_fault_reported_) {
        chat_fault_reported_ = true;
        // The render side keeps running on silence for this source (spec 4.8);
        // stage 4 turns this into a rebuild instead of a one-way failure.
        LogError(L"chat loopback capture stopped: {}", FormatHresult(chat_fault));
    }

    const HRESULT mic_fault = mic_.fault();
    if (FAILED(mic_fault) && !mic_fault_reported_) {
        mic_fault_reported_ = true;
        LogError(L"microphone capture stopped: {}", FormatHresult(mic_fault));
    }

    const HRESULT render_fault = cable_.fault();
    if (FAILED(render_fault)) {
        LogError(L"cable render stopped: {}", FormatHresult(render_fault));
        return true;
    }
    return false;
}

void AudioEngine::Stop() {
    // The threads block on the stop event, so signalling it is what actually
    // ends them; a render fault returns from Run() without anyone else having
    // set it, and the captures would otherwise never wake up.
    if (stop_event_ != nullptr) {
        ::SetEvent(stop_event_);
    }

    chat_.JoinThread();
    mic_.JoinThread();
    cable_.JoinThread();

    if (running_) {
        running_ = false;
        LogInfo(L"audio threads stopped");
    }
}

void AudioEngine::LogCounters(const wchar_t* prefix) {
    const auto report = [&](const wchar_t* role, CaptureSource& source) {
        const SourceStats& s = source.stats();
        LogInfo(L"{} {}: fill {:.1f} ms, frames {}, silent packets {}, discontinuities {}, "
                L"underruns {} ({:.1f} ms), overruns {} ({:.1f} ms), resyncs {} ({:.1f} ms)",
                prefix, role,
                FramesToMs(s.fill_frames.load(std::memory_order_relaxed), sample_rate_),
                s.frames.load(std::memory_order_relaxed),
                s.silent_packets.load(std::memory_order_relaxed),
                s.discontinuities.load(std::memory_order_relaxed),
                s.underruns.load(std::memory_order_relaxed),
                FramesToMs(s.underrun_frames.load(std::memory_order_relaxed), sample_rate_),
                s.overruns.load(std::memory_order_relaxed),
                FramesToMs(s.overrun_frames.load(std::memory_order_relaxed), sample_rate_),
                s.resyncs.load(std::memory_order_relaxed),
                FramesToMs(s.resync_frames.load(std::memory_order_relaxed), sample_rate_));
    };

    report(L"chat", chat_);
    report(L"mic", mic_);

    const RenderStats& r = cable_.stats();
    LogInfo(L"{} render: callbacks {}, frames {} ({:.1f} s), clipped samples {}, timeouts {}",
            prefix, r.callbacks.load(std::memory_order_relaxed),
            r.frames.load(std::memory_order_relaxed),
            FramesToMs(r.frames.load(std::memory_order_relaxed), sample_rate_) / 1000.0,
            r.clipped_samples.load(std::memory_order_relaxed),
            r.timeouts.load(std::memory_order_relaxed));
}

void AudioEngine::LogSummary() { LogCounters(L"session"); }

}  // namespace vcmic
