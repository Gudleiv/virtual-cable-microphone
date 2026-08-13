#include "audio_engine.h"

#include <avrt.h>

#include <algorithm>
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

void RenderSink::Bind(CaptureSource& chat, CaptureSource& mic, const Config& config) {
    const std::uint32_t rate = format_.sample_rate;
    const std::size_t target = MsToFrames(config.audio.target_buffer_ms, rate);

    inv_rate_ = rate == 0 ? 0.0 : 1.0 / static_cast<double>(rate);

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

        state.drift_enabled = config.drift.enabled && config.drift.max_rate_correction > 0.0;
        state.drift.Configure(static_cast<double>(state.target_frames), rate, config.drift);

        // The resampler may ask for more input frames than it produces output
        // frames. Size its input for the largest block WASAPI can hand us at
        // the fastest read rate the controller is allowed to reach, using the
        // same integer arithmetic the resampler itself uses.
        const std::uint64_t fastest =
            StereoResampler::StepFromRatio(1.0 + state.drift.max_correction());
        const std::uint64_t worst =
            ((StereoResampler::kOne - 1) + static_cast<std::uint64_t>(buffer_frames_) * fastest) >>
            32;
        state.in_left.assign(static_cast<std::size_t>(worst) + 1, 0.0f);
        state.in_right.assign(static_cast<std::size_t>(worst) + 1, 0.0f);
        state.left.assign(buffer_frames_, 0.0f);
        state.right.assign(buffer_frames_, 0.0f);
        state.resampler.Reset();
    };

    bind_one(chat_, chat, config.mix.chat_gain_db);
    bind_one(mic_, mic, config.mix.mic_gain_db);
    gain_coeff_ = SmoothingCoefficient(config.mix.gain_smoothing_ms, rate);

    gate_.Configure(config.gate, rate);
    limiter_.Configure(config.mix, rate);
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

void RenderSink::PullSource(SourceState& state, std::size_t frames) noexcept {
    float* left = state.left.data();
    float* right = state.right.data();

    const std::size_t available = state.ring->Readable();
    state.stats->fill_frames.store(static_cast<std::uint32_t>(available),
                                   std::memory_order_relaxed);

    // Hold the source silent until it has built up the target fill, so that
    // the steady-state latency is what the config asks for instead of whatever
    // the startup race happens to produce.
    if (!state.primed) {
        if (available < state.target_frames) {
            state.stats->primed.store(false, std::memory_order_relaxed);
            std::memset(left, 0, frames * sizeof(float));
            std::memset(right, 0, frames * sizeof(float));
            return;
        }
        state.primed = true;
        state.resampler.Reset();
        // The clock ratio the controller learned is still valid; only the
        // averaged fill describes a moment that has passed.
        state.drift.Resume(static_cast<double>(available));
        state.stats->primings.fetch_add(1, std::memory_order_relaxed);
    }
    state.stats->primed.store(true, std::memory_order_relaxed);

    // Emergency path of spec 4.6: a backlog this large is a stall that already
    // happened, and keeping it would just add permanent latency. Drift
    // correction is a micro-adjustment and cannot dig out of it.
    if (available > state.max_frames) {
        const std::size_t dropped = state.ring->Discard(available - state.target_frames);
        if (dropped > 0) {
            state.stats->resyncs.fetch_add(1, std::memory_order_relaxed);
            state.stats->resync_frames.fetch_add(dropped, std::memory_order_relaxed);
            state.drift.Resume(static_cast<double>(state.target_frames));
        }
    }

    if (!state.drift_enabled) {
        const std::size_t taken = state.ring->Read(left, right, frames);
        if (taken < frames) {
            const std::size_t missing = frames - taken;
            std::memset(left + taken, 0, missing * sizeof(float));
            std::memset(right + taken, 0, missing * sizeof(float));
            state.stats->underruns.fetch_add(1, std::memory_order_relaxed);
            state.stats->underrun_frames.fetch_add(missing, std::memory_order_relaxed);
            state.primed = false;  // refill to the target before consuming again
        }
        return;
    }

    // Spec 4.6: hold the ring on its target by reading it slightly faster or
    // slower than the cable plays, rather than by dropping and inserting.
    state.drift.Update(static_cast<double>(state.ring->Readable()),
                       static_cast<double>(frames) * inv_rate_);
    const std::uint64_t step = StereoResampler::StepFromRatio(state.drift.ratio());
    const std::size_t needed = state.resampler.InputFramesNeeded(frames, step);

    float* in_left = state.in_left.data();
    float* in_right = state.in_right.data();
    const std::size_t taken = state.ring->Read(in_left, in_right, needed);
    if (taken < needed) {
        const std::size_t missing = needed - taken;
        std::memset(in_left + taken, 0, missing * sizeof(float));
        std::memset(in_right + taken, 0, missing * sizeof(float));
        state.stats->underruns.fetch_add(1, std::memory_order_relaxed);
        state.stats->underrun_frames.fetch_add(missing, std::memory_order_relaxed);
        state.primed = false;
    }

    state.resampler.Process(in_left, in_right, frames, step, left, right);

    state.stats->drift_frames.fetch_add(
        static_cast<std::int64_t>(needed) - static_cast<std::int64_t>(frames),
        std::memory_order_relaxed);
    state.stats->drift_ppm.store(static_cast<std::int32_t>(state.drift.correction_ppm()),
                                 std::memory_order_relaxed);
    state.stats->drift_steady_ppm.store(static_cast<std::int32_t>(state.drift.steady_ppm()),
                                        std::memory_order_relaxed);
    state.stats->average_fill_frames.store(
        static_cast<std::uint32_t>(state.drift.average_fill() < 0.0 ? 0.0
                                                                   : state.drift.average_fill()),
        std::memory_order_relaxed);
}

void RenderSink::MixInto(std::uint8_t* destination, std::size_t frames) noexcept {
    PullSource(chat_, frames);
    PullSource(mic_, frames);

    // Spec 4.7: the gate belongs to the microphone alone. Gating the sum would
    // cut the chat off whenever nobody in this room is talking.
    const DynamicsBlock gated = gate_.Process(mic_.left.data(), mic_.right.data(), frames);
    if (gated.active_frames > 0) {
        stats_.gate_frames.fetch_add(gated.active_frames, std::memory_order_relaxed);
        PublishMinGain(stats_.gate_min_gain, gated.min_gain);
    }

    const float* chat_left = chat_.left.data();
    const float* chat_right = chat_.right.data();
    const float* mic_left = mic_.left.data();
    const float* mic_right = mic_.right.data();
    float* out_left = out_left_.data();
    float* out_right = out_right_.data();

    float chat_gain = chat_.gain_current;
    float mic_gain = mic_.gain_current;
    const float chat_goal = chat_.gain_target;
    const float mic_goal = mic_.gain_target;
    const float coeff = gain_coeff_;

    for (std::size_t i = 0; i < frames; ++i) {
        chat_gain += (chat_goal - chat_gain) * coeff;
        mic_gain += (mic_goal - mic_gain) * coeff;
        out_left[i] = chat_left[i] * chat_gain + mic_left[i] * mic_gain;
        out_right[i] = chat_right[i] * chat_gain + mic_right[i] * mic_gain;
    }

    chat_.gain_current = chat_gain;
    mic_.gain_current = mic_gain;

    const DynamicsBlock limited = limiter_.Process(out_left, out_right, frames);
    if (limited.active_frames > 0) {
        stats_.limiter_frames.fetch_add(limited.active_frames, std::memory_order_relaxed);
        PublishMinGain(stats_.limiter_min_gain, limited.min_gain);
    }

    // Safety net. With the limiter enabled nothing can reach it, which is
    // exactly why a non-zero count here is worth seeing in the log.
    std::uint64_t clipped = 0;
    for (std::size_t i = 0; i < frames; ++i) {
        if (out_left[i] > 1.0f) {
            out_left[i] = 1.0f;
            ++clipped;
        } else if (out_left[i] < -1.0f) {
            out_left[i] = -1.0f;
            ++clipped;
        }
        if (out_right[i] > 1.0f) {
            out_right[i] = 1.0f;
            ++clipped;
        } else if (out_right[i] < -1.0f) {
            out_right[i] = -1.0f;
            ++clipped;
        }
    }
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

    cable_.Bind(chat_, mic_, config);
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

    LogInfo(L"mix: chat {:+.1f} dB, mic {:+.1f} dB, smoothing {:.1f} ms",
            config.mix.chat_gain_db, config.mix.mic_gain_db, config.mix.gain_smoothing_ms);
    if (config.mix.limiter_enabled) {
        LogInfo(L"limiter: peak, no lookahead, threshold {:+.1f} dBFS, release {:.0f} ms",
                config.mix.limiter_threshold_db, config.mix.limiter_release_ms);
    } else {
        LogWarn(L"limiter: disabled - simultaneous chat and microphone peaks will clamp");
    }
    if (config.gate.enabled) {
        LogInfo(L"gate: microphone only, threshold {:.1f} dBFS, attack {:.1f} ms, hold {:.0f} ms, "
                L"release {:.0f} ms",
                config.gate.threshold_db, config.gate.attack_ms, config.gate.hold_ms,
                config.gate.release_ms);
    }

    // The render thread takes a whole block out of each ring at once, so the
    // fill it regulates is really "one block plus the margin left over". That
    // margin, not the target itself, is what a scheduling hiccup eats into.
    const double margin_ms = config.audio.target_buffer_ms - cable_.buffer_ms();
    LogInfo(L"target ring fill {:.1f} ms, leaving {:.1f} ms after each render block; estimated "
            L"added latency {:.1f}-{:.1f} ms (ring {:.1f} ms + render buffer {:.1f} ms)",
            config.audio.target_buffer_ms, margin_ms, config.audio.target_buffer_ms,
            config.audio.target_buffer_ms + cable_.buffer_ms(), config.audio.target_buffer_ms,
            cable_.buffer_ms());
    if (margin_ms < cable_.buffer_ms() * 0.5) {
        LogWarn(L"  that margin is thin: raise audio.target_buffer_ms above {:.0f} ms, or lower "
                L"Max Latency in VBCABLE_ControlPanel.exe to shrink the {:.1f} ms render block",
                cable_.buffer_ms() * 1.5, cable_.buffer_ms());
    }
    if (config.drift.enabled && config.drift.max_rate_correction > 0.0) {
        LogInfo(L"drift compensation: up to {:+.0f} ppm of read-rate correction, fill averaged "
                L"over {:.1f} s, loop settles in about {:.0f} s",
                config.drift.max_rate_correction * 1.0e6, config.drift.measure_window_s,
                config.drift.response_s);
    } else {
        LogWarn(L"drift compensation: disabled - the ring fill will walk away from the target "
                L"until the resync valve drops a backlog");
    }
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
    const auto report = [&](const wchar_t* role, CaptureSource& source, const wchar_t* idle_hint) {
        const SourceStats& s = source.stats();
        const std::int64_t drift = s.drift_frames.load(std::memory_order_relaxed);

        // Three states worth telling apart. A source that has not delivered a
        // single packet is not slow to fill, it is not running at all: WASAPI
        // loopback emits nothing whatsoever on a render endpoint nobody is
        // playing into, so an untouched counter here means the chain was never
        // exercised rather than that it is behind. A refilling source is mixed
        // as silence, and its averaged fill and correction are frozen at
        // whatever they were when it went quiet.
        const bool primed = s.primed.load(std::memory_order_relaxed);
        const bool started = s.packets.load(std::memory_order_relaxed) != 0;
        const std::wstring level =
            !started ? std::format(L"NO DATA, not one packet - {}", idle_hint)
            : primed ? std::format(L"fill {:.1f} ms (avg {:.1f})",
                                   FramesToMs(s.fill_frames.load(std::memory_order_relaxed),
                                              sample_rate_),
                                   FramesToMs(s.average_fill_frames.load(std::memory_order_relaxed),
                                              sample_rate_))
                     : std::format(L"REFILLING, fill {:.1f} ms",
                                   FramesToMs(s.fill_frames.load(std::memory_order_relaxed),
                                              sample_rate_));

        // Headline the learned clock ratio and break out what the loop is
        // absorbing on top of it, rather than printing only their sum: a device
        // that hands over a lump of frames once a minute otherwise reads as a
        // clock wandering by hundreds of ppm.
        const std::int32_t total = s.drift_ppm.load(std::memory_order_relaxed);
        const std::int32_t steady = s.drift_steady_ppm.load(std::memory_order_relaxed);
        const std::wstring correction =
            std::format(L"drift {}{:+} ppm ({:+} transient)", primed ? L"" : L"held at ", steady,
                        total - steady);

        LogInfo(L"{} {}: {}, {}, corrected {:+.1f} ms, "
                L"frames {}, silent packets {}, discontinuities {}, underruns {} ({:.1f} ms), "
                L"overruns {} ({:.1f} ms), resyncs {} ({:.1f} ms)",
                prefix, role, level, correction,
                (drift < 0 ? -1.0 : 1.0) *
                    FramesToMs(static_cast<std::uint64_t>(drift < 0 ? -drift : drift), sample_rate_),
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

    report(L"chat", chat_, L"is anything actually playing into that endpoint?");
    report(L"mic", mic_, L"is the device still present?");

    RenderStats& r = cable_.stats();
    const std::uint64_t rendered = r.frames.load(std::memory_order_relaxed);
    const auto percent = [&](std::uint64_t frames) {
        return rendered == 0 ? 0.0 : 100.0 * static_cast<double>(frames) / static_cast<double>(rendered);
    };

    // Reading the peak also arms it for the next interval, so each report
    // describes its own window rather than the whole session.
    std::wstring dynamics;
    if (cable_.limiter().enabled()) {
        const std::uint32_t peak = r.limiter_min_gain.exchange(kGainQ16One, std::memory_order_relaxed);
        dynamics += std::format(L", limiter {:.1f} dB peak on {:.2f} % of frames",
                                DbFromGain(GainFromQ16(peak)),
                                percent(r.limiter_frames.load(std::memory_order_relaxed)));
    }
    if (cable_.gate().enabled()) {
        r.gate_min_gain.store(kGainQ16One, std::memory_order_relaxed);
        dynamics += std::format(L", gate attenuating {:.1f} % of frames",
                                percent(r.gate_frames.load(std::memory_order_relaxed)));
    }

    LogInfo(L"{} render: callbacks {}, frames {} ({:.1f} s){}, clipped samples {}, timeouts {}",
            prefix, r.callbacks.load(std::memory_order_relaxed), rendered,
            FramesToMs(rendered, sample_rate_) / 1000.0, dynamics,
            r.clipped_samples.load(std::memory_order_relaxed),
            r.timeouts.load(std::memory_order_relaxed));
}

void AudioEngine::LogSummary() { LogCounters(L"session"); }

}  // namespace vcmic
