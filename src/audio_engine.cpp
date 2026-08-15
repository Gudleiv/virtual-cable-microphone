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

// A render event every ~10 ms is normal, so this many wait windows in a row
// with no event at all means the clock master has stopped without reporting a
// failure. Rebuilding is the only move left, and it costs nothing: there is no
// audio flowing to interrupt.
constexpr std::uint32_t kQuietRebuildAfter = 3;

// How long a capture stream may deliver nothing before its health is called
// into question. A stream can outlive its device without a single call ever
// failing - which is what a device removed and re-added under a fresh endpoint
// id leaves behind - and something has to notice.
//
// What silence proves depends on the stream. An ordinary shared-mode capture
// endpoint always delivers, silence included, so nothing for this long is proof
// enough on its own. For loopback it proves nothing at all: a render endpoint
// nobody is playing into legitimately delivers no packets whatsoever (spec 4.9).
// There, silence is only the cue to go and ask the enumerator whether the
// endpoint is still there, which is unambiguous.
constexpr double kCaptureSilenceMs = 5000.0;

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

// Everything the rings, the converters and the mixer were sized and specialised
// for at startup. A rebuilt stream that disagrees with any of it cannot simply
// be adopted, because adopting it would mean re-sizing buffers another thread
// is reading (spec 4.8 forbids stopping the render for one source's sake).
bool SameFormat(const FormatInfo& a, const FormatInfo& b) {
    return a.sample_rate == b.sample_rate && a.channels == b.channels &&
           a.container_bits == b.container_bits && a.valid_bits == b.valid_bits &&
           a.channel_mask == b.channel_mask && a.block_align == b.block_align &&
           a.sample_type == b.sample_type;
}

std::wstring DescribeFormatChange(const FormatInfo& was, const FormatInfo& now) {
    return std::format(
        L"came back as {} Hz / {} ch / {}-bit {}, but the engine was built for {} Hz / {} ch / "
        L"{}-bit {}; set the endpoint back or restart vcmic",
        now.sample_rate, now.channels, now.container_bits, SampleTypeName(now.sample_type),
        was.sample_rate, was.channels, was.container_bits, SampleTypeName(was.sample_type));
}

// Spec 4.8: a stream that dies is rebuilt on its own schedule, backing off so
// that an endpoint gone for good costs nothing, and being woken early by the
// device watcher when something plugs in. The logging lives here rather than at
// the call sites so a device missing for an hour writes a handful of lines
// instead of one every five seconds.
//
// This is not the audio path. It only runs while the stream is already dead,
// which is the one moment the log has something worth saying.
class StreamRetry {
public:
    StreamRetry(const wchar_t* what, std::uint32_t min_ms, std::uint32_t max_ms)
        : what_(what), min_ms_(min_ms), max_ms_(max_ms) {}

    // About to serve. Recorded so that a stream which dies again immediately
    // can be told apart from one that ran and then lost its device.
    void Running() { up_since_ms_ = ::GetTickCount64(); }

    // The stream just died.
    void Lost(const std::wstring& reason) {
        const ULONGLONG now = ::GetTickCount64();
        // A stream that ran for a while and then stopped is fresh news, and is
        // worth retrying at once: whatever took it down has usually finished by
        // the time we notice. One that died the moment it opened is flapping,
        // so the backoff carries on from where it left off - otherwise "open,
        // die, open" is a spin at full speed - and the log stops repeating.
        flapping_ = up_since_ms_ == 0 || now - up_since_ms_ < kSettledMs;
        if (flapping_) {
            ++attempts_;
            wait_ms_ = wait_ms_ == 0 ? min_ms_ : (std::min)(wait_ms_ * 2, max_ms_);
        } else {
            down_since_ms_ = now;
            attempts_ = 0;
            wait_ms_ = 0;
        }

        if (Newsworthy()) {
            LogWarn(L"{} stream lost: {} - rebuilding", what_, reason);
        } else {
            LogDebug(L"{} stream lost again: {}", what_, reason);
        }
    }

    // False when the stop event won the race; true to try again now.
    bool Wait(HANDLE stop_event, HANDLE wake_event) const {
        HANDLE waits[2] = {stop_event, wake_event};
        const DWORD result = ::WaitForMultipleObjects(2, waits, FALSE, wait_ms_);
        return result == WAIT_TIMEOUT || result == WAIT_OBJECT_0 + 1;
    }

    void Failed(const std::wstring& reason) {
        ++attempts_;
        if (Newsworthy()) {
            LogWarn(L"{}: rebuild attempt {} failed after {:.0f} s down: {}", what_, attempts_,
                    DownSeconds(), reason);
        } else {
            LogDebug(L"{}: rebuild attempt {} failed: {}", what_, attempts_, reason);
        }
        wait_ms_ = wait_ms_ == 0 ? min_ms_ : (std::min)(wait_ms_ * 2, max_ms_);
    }

    void Recovered() const {
        // An outage that ended is always worth a line. A stream that is merely
        // flapping announces itself through Lost() instead, at the same slow
        // cadence, so this does not repeat it.
        if (!flapping_ || attempts_ % kQuietEvery == 0) {
            LogInfo(L"{} stream back after {:.1f} s and {} attempt(s)", what_, DownSeconds(),
                    attempts_ + 1);
        }
    }

private:
    // How long a stream has to survive before its death counts as a new outage.
    static constexpr ULONGLONG kSettledMs = 5000;
    // At the backoff ceiling this works out at roughly one line a minute.
    static constexpr std::uint64_t kQuietEvery = 12;

    // Loud while it is still news, then only now and then, so an endpoint gone
    // for an hour writes a handful of lines instead of one every five seconds.
    bool Newsworthy() const { return attempts_ <= 2 || attempts_ % kQuietEvery == 0; }

    double DownSeconds() const {
        return static_cast<double>(::GetTickCount64() - down_since_ms_) / 1000.0;
    }

    const wchar_t* what_;
    DWORD min_ms_;
    DWORD max_ms_;
    DWORD wait_ms_ = 0;
    std::uint64_t attempts_ = 0;
    ULONGLONG up_since_ms_ = 0;
    ULONGLONG down_since_ms_ = 0;
    bool flapping_ = false;
};

// What both streams do before they diverge: resolve the endpoint, activate a
// client, and take the endpoint's mix format - which in shared mode is the only
// format that will be accepted, so it is also the one that has to be validated.
// The two callers differ only in what they suggest when the rate is wrong.
struct OpenedEndpoint {
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    EndpointInfo info;
    MatchKind matched = MatchKind::None;
    std::vector<std::uint8_t> format_blob;
    FormatInfo format;
    SampleFormat sample_format = SampleFormat::Unsupported;
};

HRESULT OpenEndpoint(IMMDeviceEnumerator* enumerator, EDataFlow flow,
                     const DeviceSelector& selector, const AudioConfig& audio,
                     const wchar_t* rate_hint, OpenedEndpoint& out, std::wstring& error) {
    ResolvedDevice resolved;
    HRESULT hr = ResolveDevice(enumerator, flow, selector, false, resolved, error);
    if (FAILED(hr)) {
        return hr;
    }
    out.device = resolved.device;
    out.info = resolved.info;
    out.matched = resolved.matched;

    hr = out.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, out.client.PutVoid());
    if (FAILED(hr)) {
        error = std::format(L"Activate(IAudioClient) failed: {}", FormatHresult(hr));
        return hr;
    }

    WAVEFORMATEX* mix = nullptr;
    hr = out.client->GetMixFormat(&mix);
    if (FAILED(hr) || mix == nullptr) {
        error = std::format(L"GetMixFormat failed: {}", FormatHresult(hr));
        return FAILED(hr) ? hr : E_FAIL;
    }
    const CoTaskMemPtr<WAVEFORMATEX> owned_format(mix);

    out.format_blob = CloneFormat(mix);
    out.format = InspectFormat(mix);
    out.sample_format = ResolveSampleFormat(out.format);

    if (out.sample_format == SampleFormat::Unsupported) {
        error = std::format(L"unsupported mix format: {}", DescribeFormat(mix));
        return E_FAIL;
    }
    if (audio.require_sample_rate && out.format.sample_rate != audio.sample_rate) {
        error = std::format(L"runs at {} Hz but audio.sample_rate is {} Hz; {}",
                            out.format.sample_rate, audio.sample_rate, rate_hint);
        return E_FAIL;
    }
    if (out.format.channels == 0 || out.format.block_align == 0) {
        error = L"mix format reports no channels";
        return E_FAIL;
    }
    return S_OK;
}

}  // namespace

// ------------------------------------------------------------------ ClockKeeper

ClockKeeper::~ClockKeeper() { Close(); }

HRESULT ClockKeeper::Open(IMMDevice* device, std::wstring& error) {
    Close();
    if (device == nullptr) {
        error = L"the endpoint is not open";
        return E_POINTER;
    }

    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client_.PutVoid());
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
    const CoTaskMemPtr<WAVEFORMATEX> owned_format(mix);

    // Nothing but silence is ever written, so the buffer only has to outlast a
    // missed poll tick. Whatever the endpoint's own format is will do: no
    // sample ever has to be converted to reach it.
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kCaptureBufferDuration, 0, mix, nullptr);
    if (FAILED(hr)) {
        error = std::format(L"IAudioClient::Initialize failed: {}", FormatHresult(hr));
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

    hr = client_->Start();
    if (FAILED(hr)) {
        error = std::format(L"IAudioClient::Start failed: {}", FormatHresult(hr));
        return hr;
    }
    started_ = true;
    return S_OK;
}

void ClockKeeper::Close() {
    if (started_ && client_) {
        client_->Stop();
    }
    started_ = false;
    render_.Reset();
    client_.Reset();
    buffer_frames_ = 0;
}

HRESULT ClockKeeper::Pump() noexcept {
    UINT32 padding = 0;
    HRESULT hr = client_->GetCurrentPadding(&padding);
    if (FAILED(hr)) {
        return hr;
    }
    if (padding >= buffer_frames_) {
        return S_OK;
    }

    const UINT32 frames = buffer_frames_ - padding;
    BYTE* buffer = nullptr;
    hr = render_->GetBuffer(frames, &buffer);
    if (FAILED(hr)) {
        return hr;
    }
    // The SILENT flag means WASAPI never looks at the bytes, so the buffer is
    // handed straight back without being touched.
    return render_->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
}

// ---------------------------------------------------------------- CaptureSource

CaptureSource::CaptureSource(const wchar_t* label, EDataFlow flow, bool loopback)
    : label_(label), flow_(flow), loopback_(loopback), silence_is_normal_(loopback) {}

CaptureSource::~CaptureSource() {
    JoinThread();
    Close();
}

HRESULT CaptureSource::Open(IMMDeviceEnumerator* enumerator, const DeviceSelector& selector,
                            const Config& config, std::wstring& error) {
    selector_ = selector;
    audio_ = config.audio;
    backoff_min_ms_ = config.resilience.backoff_min_ms;
    backoff_max_ms_ = config.resilience.backoff_max_ms;
    // Spec 4.9's flag is about the chat endpoint, and the chat source is the
    // loopback one; an ordinary microphone has no clock to hold open.
    keep_clock_alive_ = loopback_ && config.resilience.keep_chat_clock_alive;

    if (wake_event_ == nullptr) {
        wake_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (wake_event_ == nullptr) {
            error = L"CreateEvent for the device watcher failed";
            return HRESULT_FROM_WIN32(::GetLastError());
        }
    }

    const HRESULT hr = OpenStream(enumerator, false, error);
    if (FAILED(hr)) {
        return hr;
    }

    ring_.Reset(MsToFrames(audio_.ring_capacity_ms, format_.sample_rate));
    // Sized once and never again. A rebuilt stream has to come back with the
    // same format, and DrainAvailable chunks a packet that is larger than the
    // scratch, so nothing on the recovery path ever allocates.
    scratch_left_.assign(buffer_frames_, 0.0f);
    scratch_right_.assign(buffer_frames_, 0.0f);
    return S_OK;
}

// Called on the capture thread when rebuilding, so it touches only what that
// thread owns. The descriptive members it rewrites - info_, format_blob_,
// matched_ - are read from the startup summary and nowhere else.
HRESULT CaptureSource::OpenStream(IMMDeviceEnumerator* enumerator, bool reopening,
                                  std::wstring& error) {
    OpenedEndpoint opened;
    HRESULT hr = OpenEndpoint(enumerator, flow_, selector_, audio_, L"there is no resampler yet",
                              opened, error);
    if (FAILED(hr)) {
        return hr;
    }
    if (reopening && !SameFormat(opened.format, format_)) {
        error = DescribeFormatChange(format_, opened.format);
        return E_FAIL;
    }

    device_ = std::move(opened.device);
    client_ = std::move(opened.client);
    info_ = std::move(opened.info);
    matched_ = opened.matched;
    format_blob_ = std::move(opened.format_blob);
    format_ = opened.format;
    sample_format_ = opened.sample_format;
    block_align_ = format_.block_align;

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

    // Poll at roughly half the device period, clamped to something sane. The
    // thread arms its timer from this once and keeps it across rebuilds, which
    // is safe because the format - and with it the period - has to match.
    REFERENCE_TIME default_period = 0;
    REFERENCE_TIME min_period = 0;
    double period_ms = 0.0;
    if (SUCCEEDED(client_->GetDevicePeriod(&default_period, &min_period))) {
        period_ms = static_cast<double>(default_period) / 10000.0;
    }
    poll_ms_ = period_ms > 0.0 ? period_ms / 2.0 : kMaxPollMs;
    poll_ms_ = (std::max)(kMinPollMs, (std::min)(kMaxPollMs, poll_ms_));
    silence_limit_ticks_ = (std::max)(1u, static_cast<std::uint32_t>(kCaptureSilenceMs / poll_ms_));

    return S_OK;
}

void CaptureSource::OpenClockKeeper() {
    if (!keep_clock_alive_) {
        return;
    }
    std::wstring error;
    if (FAILED(keeper_.Open(device_.Get(), error))) {
        keeper_.Close();
        // Never fatal: the capture works either way, it just falls silent
        // whenever the endpoint does.
        LogWarn(L"{}: cannot hold the endpoint clock open: {}", label_, error);
    }
}

void CaptureSource::CloseStream() {
    keeper_.Close();
    capture_.Reset();
    client_.Reset();
    device_.Reset();
}

void CaptureSource::Close() {
    CloseStream();
    if (wake_event_ != nullptr) {
        ::CloseHandle(wake_event_);
        wake_event_ = nullptr;
    }
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

HRESULT CaptureSource::DrainAvailable() noexcept {
    // Audio path. The clock keeper goes first so the endpoint stays fed even on
    // a tick where the loopback has nothing to hand over.
    if (keeper_.active() && FAILED(keeper_.Pump())) {
        // Losing it costs the loopback its steady cadence, not its data, so the
        // capture carries on and a counter records what happened. It comes back
        // with the next rebuild: whatever invalidated the render side of this
        // endpoint is about to invalidate the capture side too.
        keeper_.Close();
        stats_.clock_keeper_faults.fetch_add(1, std::memory_order_relaxed);
    }

    const std::size_t scratch_frames = scratch_left_.size();

    UINT32 packet_frames = 0;
    HRESULT hr = capture_->GetNextPacketSize(&packet_frames);
    if (FAILED(hr)) {
        return hr;
    }

    // Zero packets is normal and means nobody is playing into the endpoint;
    // the render side mixes silence for us (spec 4.9).
    while (packet_frames > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) {
            return S_OK;
        }
        if (FAILED(hr)) {
            return hr;
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
            return hr;
        }

        hr = capture_->GetNextPacketSize(&packet_frames);
        if (FAILED(hr)) {
            return hr;
        }
    }
    return S_OK;
}

bool CaptureSource::EndpointStillActive(IMMDeviceEnumerator* enumerator) const {
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDevice(info_.id.c_str(), device.Put()))) {
        return false;
    }
    DWORD state = DEVICE_STATE_NOTPRESENT;
    if (FAILED(device->GetState(&state))) {
        return false;
    }
    return state == DEVICE_STATE_ACTIVE;
}

HRESULT CaptureSource::Drain(IMMDeviceEnumerator* enumerator, HANDLE stop_event, HANDLE timer,
                             std::wstring& reason) {
    HANDLE waits[2] = {stop_event, timer};
    std::uint64_t last_packets = stats_.packets.load(std::memory_order_relaxed);
    std::uint32_t silent_ticks = 0;

    for (;;) {
        const DWORD result = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (result != WAIT_OBJECT_0 + 1) {
            return S_OK;  // stop requested, or the wait itself failed
        }

        const HRESULT hr = DrainAvailable();
        if (FAILED(hr)) {
            reason = FormatHresult(hr);
            return hr;
        }

        const std::uint64_t packets = stats_.packets.load(std::memory_order_relaxed);
        if (packets != last_packets) {
            last_packets = packets;
            silent_ticks = 0;
            continue;
        }
        if (++silent_ticks < silence_limit_ticks_) {
            continue;
        }

        // Long enough. What that means depends on the stream, and only the
        // loopback one has to go and ask. Both branches are off the audio path
        // by construction: nothing has arrived for seconds.
        silent_ticks = 0;
        if (!silence_is_normal_) {
            reason = std::format(L"no capture packet for {:.0f} s", kCaptureSilenceMs / 1000.0);
            return E_FAIL;
        }
        if (!EndpointStillActive(enumerator)) {
            reason = L"the endpoint stopped being active while it was silent";
            return E_FAIL;
        }
    }
}

void CaptureSource::ThreadMain(HANDLE stop_event) {
    const ComApartment apartment;
    const MmcssTask mmcss(L"Pro Audio");

    // The thread rebuilds its own stream (spec 4.8), so it needs an enumerator
    // of its own rather than one borrowed from whoever called Open().
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CreateDeviceEnumerator(enumerator);
    if (FAILED(hr)) {
        fault_.store(hr, std::memory_order_relaxed);
        return;
    }

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

    OpenClockKeeper();
    StreamRetry retry(label_, backoff_min_ms_, backoff_max_ms_);

    // One pass per life of the stream. Open() built the first one, so the first
    // pass goes straight to draining.
    for (bool first = true;; first = false) {
        if (!first) {
            if (!retry.Wait(stop_event, wake_event_)) {
                break;
            }
            std::wstring error;
            if (FAILED(OpenStream(enumerator.Get(), true, error))) {
                CloseStream();
                retry.Failed(error);
                continue;
            }
            OpenClockKeeper();
            retry.Recovered();
            stats_.restarts.fetch_add(1, std::memory_order_relaxed);
        }

        retry.Running();
        std::wstring reason;
        HRESULT failure = client_->Start();
        if (SUCCEEDED(failure)) {
            stats_.live.store(true, std::memory_order_relaxed);
            failure = Drain(enumerator.Get(), stop_event, timer, reason);
            client_->Stop();
        } else {
            reason = FormatHresult(failure);
        }
        stats_.live.store(false, std::memory_order_relaxed);
        CloseStream();

        if (SUCCEEDED(failure)) {
            break;  // asked to stop
        }
        retry.Lost(reason);
    }

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
                         const Config& config, std::wstring& error) {
    selector_ = selector;
    audio_ = config.audio;
    backoff_min_ms_ = config.resilience.backoff_min_ms;
    backoff_max_ms_ = config.resilience.backoff_max_ms;

    // Both events outlive any individual stream, so a rebuild reuses them.
    if (event_ == nullptr) {
        event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event_ == nullptr) {
            error = L"CreateEvent for the render callback failed";
            return HRESULT_FROM_WIN32(::GetLastError());
        }
    }
    if (wake_event_ == nullptr) {
        wake_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (wake_event_ == nullptr) {
            error = L"CreateEvent for the device watcher failed";
            return HRESULT_FROM_WIN32(::GetLastError());
        }
    }

    return OpenStream(enumerator, false, error);
}

HRESULT RenderSink::OpenStream(IMMDeviceEnumerator* enumerator, bool reopening,
                               std::wstring& error) {
    OpenedEndpoint opened;
    HRESULT hr = OpenEndpoint(enumerator, eRender, selector_, audio_,
                              L"set Internal Sample Rate in VBCABLE_ControlPanel.exe", opened,
                              error);
    if (FAILED(hr)) {
        return hr;
    }
    if (reopening && !SameFormat(opened.format, format_)) {
        error = DescribeFormatChange(format_, opened.format);
        return E_FAIL;
    }

    device_ = std::move(opened.device);
    client_ = std::move(opened.client);
    info_ = std::move(opened.info);
    matched_ = opened.matched;
    format_blob_ = std::move(opened.format_blob);
    format_ = opened.format;
    sample_format_ = opened.sample_format;
    block_align_ = format_.block_align;

    upmix_ = BuildUpmix(format_);

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

    return S_OK;
}

void RenderSink::CloseStream() {
    render_.Reset();
    client_.Reset();
    device_.Reset();
}

void RenderSink::Close() {
    CloseStream();
    if (event_ != nullptr) {
        ::CloseHandle(event_);
        event_ = nullptr;
    }
    if (wake_event_ != nullptr) {
        ::CloseHandle(wake_event_);
        wake_event_ = nullptr;
    }
}

void RenderSink::Bind(CaptureSource& chat, CaptureSource& mic, const Config& config) {
    const std::uint32_t rate = format_.sample_rate;
    const std::size_t target = MsToFrames(config.audio.target_buffer_ms, rate);

    inv_rate_ = rate == 0 ? 0.0 : 1.0 / static_cast<double>(rate);
    bind_rate_ = rate;

    const auto bind_one = [&](SourceState& state, CaptureSource& source, double gain_db) {
        state.ring = &source.ring();
        state.stats = &source.stats();
        state.primed = false;
        state.target_frames = target == 0 ? std::size_t{1} : target;
        // Four times the target, but never more than half the ring: past that
        // the backlog is latency nobody asked for, so it gets dropped.
        state.max_frames = (std::min)(state.target_frames * 4, source.ring().capacity() / 2);
        state.max_frames = (std::max)(state.max_frames, state.target_frames + 1);
        state.gain_target.store(GainFromDb(gain_db), std::memory_order_relaxed);
        state.gain_current = state.gain_target.load(std::memory_order_relaxed);

        state.drift_enabled = config.drift.enabled && config.drift.max_rate_correction > 0.0;
        state.drift.Configure(static_cast<double>(state.target_frames), rate, config.drift);
    };

    bind_one(chat_, chat, config.mix.chat_gain_db);
    bind_one(mic_, mic, config.mix.mic_gain_db);
    gain_coeff_ = SmoothingCoefficient(config.mix.gain_smoothing_ms, rate);

    gate_.Configure(config.gate, rate);
    limiter_.Configure(config.mix, rate);

    SizeBuffers();
}

void RenderSink::PublishLiveSettings(const Config& config) {
    // Everything expensive - the exp() and pow() behind the coefficients -
    // happens here, on whichever thread asked for the reload. The render thread
    // only copies the result in.
    pending_.limiter = PeakLimiter::SettingsFrom(config.mix, bind_rate_);
    pending_.gate = NoiseGate::SettingsFrom(config.gate, bind_rate_);
    pending_.gain_coeff = SmoothingCoefficient(config.mix.gain_smoothing_ms, bind_rate_);
    pending_ready_.store(true, std::memory_order_release);
}

void RenderSink::SizeBuffers() {
    const auto size_one = [&](SourceState& state) {
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

    size_one(chat_);
    size_one(mic_);
    out_left_.assign(buffer_frames_, 0.0f);
    out_right_.assign(buffer_frames_, 0.0f);
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
    // A reload waiting to be picked up (spec 4.12). Nothing is computed here,
    // only copied, and only on the block that follows the click on the menu.
    if (pending_ready_.load(std::memory_order_acquire)) {
        limiter_.Adopt(pending_.limiter);
        gate_.Adopt(pending_.gate);
        gain_coeff_ = pending_.gain_coeff;
        pending_ready_.store(false, std::memory_order_relaxed);
    }

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
    const float chat_goal = chat_.gain_target.load(std::memory_order_relaxed);
    const float mic_goal = mic_.gain_target.load(std::memory_order_relaxed);
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

HRESULT RenderSink::Serve(HANDLE stop_event, std::wstring& reason) {
    // Prime the whole buffer with silence so the first event is not missed.
    BYTE* prefill = nullptr;
    if (SUCCEEDED(render_->GetBuffer(buffer_frames_, &prefill))) {
        render_->ReleaseBuffer(buffer_frames_, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    HRESULT hr = client_->Start();
    if (FAILED(hr)) {
        reason = FormatHresult(hr);
        return hr;
    }
    stats_.live.store(true, std::memory_order_relaxed);

    HRESULT failure = S_OK;
    std::uint32_t quiet = 0;
    HANDLE waits[2] = {stop_event, event_};
    for (;;) {
        const DWORD result = ::WaitForMultipleObjects(2, waits, FALSE, kRenderWaitMs);
        if (result == WAIT_TIMEOUT) {
            stats_.timeouts.fetch_add(1, std::memory_order_relaxed);
            // The clock master has stopped ticking without saying so. There is
            // no audio either way by this point, so rebuilding can only help.
            if (++quiet >= kQuietRebuildAfter) {
                reason = std::format(L"no render callback for {:.0f} s",
                                     static_cast<double>(kQuietRebuildAfter) *
                                         static_cast<double>(kRenderWaitMs) / 1000.0);
                failure = E_FAIL;
                break;
            }
            continue;
        }
        if (result != WAIT_OBJECT_0 + 1) {
            break;  // stop requested, or the wait failed
        }
        quiet = 0;

        UINT32 padding = 0;
        hr = client_->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            failure = hr;
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
            failure = hr;
            break;
        }

        MixInto(buffer, frames);

        hr = render_->ReleaseBuffer(frames, 0);
        if (FAILED(hr)) {
            failure = hr;
            break;
        }
    }

    client_->Stop();
    stats_.live.store(false, std::memory_order_relaxed);
    if (FAILED(failure) && reason.empty()) {
        reason = FormatHresult(failure);
    }
    return failure;
}

void RenderSink::AdoptRebuiltStream() {
    // Lowering Max Latency in VBCABLE_ControlPanel.exe restarts the cable, and
    // the stream that comes back has a different block length. Resizing is an
    // allocation on an audio thread, which is only allowed here because the
    // stream is stopped and nothing is being mixed.
    if (out_left_.size() != buffer_frames_) {
        LogInfo(L"cable render block is now {} frames ({:.2f} ms); resizing the mixer buffers",
                buffer_frames_, buffer_ms());
        SizeBuffers();
    }

    // Whatever the rings hold is stale by however long the rebuild took. Both
    // sources re-prime, which re-seats their averaged fill and hands the
    // backlog to the resync valve; the learned clock ratios survive.
    chat_.primed = false;
    mic_.primed = false;
}

void RenderSink::ThreadMain(HANDLE stop_event) {
    const ComApartment apartment;
    const MmcssTask mmcss(L"Pro Audio");

    ComPtr<IMMDeviceEnumerator> enumerator;
    const HRESULT init = CreateDeviceEnumerator(enumerator);
    if (FAILED(init)) {
        fault_.store(init, std::memory_order_relaxed);
        return;
    }

    StreamRetry retry(L"cable render", backoff_min_ms_, backoff_max_ms_);

    for (bool first = true;; first = false) {
        if (!first) {
            if (!retry.Wait(stop_event, wake_event_)) {
                break;
            }
            std::wstring error;
            if (FAILED(OpenStream(enumerator.Get(), true, error))) {
                CloseStream();
                retry.Failed(error);
                continue;
            }
            AdoptRebuiltStream();
            retry.Recovered();
            stats_.restarts.fetch_add(1, std::memory_order_relaxed);
        }

        retry.Running();
        std::wstring reason;
        const HRESULT failure = Serve(stop_event, reason);
        CloseStream();

        if (SUCCEEDED(failure)) {
            break;  // asked to stop
        }
        retry.Lost(reason);
    }
}

// ----------------------------------------------------------------- AudioEngine

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() { Stop(); }

HRESULT AudioEngine::Start(IMMDeviceEnumerator* enumerator, const Config& config,
                           std::wstring& error) {
    // A device missing at startup is a configuration problem, and saying so now
    // beats retrying quietly forever. Once the streams are up it is the other
    // way round: everything that goes wrong is rebuilt in place (spec 4.8).
    HRESULT hr = chat_.Open(enumerator, config.devices.chat_render, config, error);
    if (FAILED(hr)) {
        error = std::format(L"chat loopback source: {}", error);
        return hr;
    }
    hr = mic_.Open(enumerator, config.devices.mic_capture, config, error);
    if (FAILED(hr)) {
        error = std::format(L"microphone: {}", error);
        return hr;
    }
    hr = cable_.Open(enumerator, config.devices.output_render, config, error);
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

    // Spec 4.8. Only now are the endpoint ids known, which is what lets the
    // watcher name the stream a notification belongs to.
    watcher_ = ComPtr<DeviceWatcher>(new DeviceWatcher());
    watcher_->Watch(chat_.wake_event(), chat_.info().id, L"chat");
    watcher_->Watch(mic_.wake_event(), mic_.info().id, L"microphone");
    watcher_->Watch(cable_.wake_event(), cable_.info().id, L"cable");
    const HRESULT watch_hr = watcher_->Register(enumerator);
    if (FAILED(watch_hr)) {
        // Never fatal: rebuilds still happen, they just wait out the backoff
        // instead of starting the moment the endpoint comes back.
        LogWarn(L"cannot subscribe to endpoint notifications ({}); a rebuild will take up to "
                L"{} ms longer to notice a device returning",
                FormatHresult(watch_hr), config.resilience.backoff_max_ms);
        watcher_.Reset();
    }

    config_ = config;
    LogStartupSummary(config);
    return S_OK;
}

void AudioEngine::PushGains() {
    // Mute is a gain of zero rather than a bypass, so the render thread ramps
    // into and out of it over mix.gain_smoothing_ms and nothing clicks.
    cable_.SetChatGain(chat_muted_ ? 0.0f : GainFromDb(config_.mix.chat_gain_db));
    cable_.SetMicGain(mic_muted_ ? 0.0f : GainFromDb(config_.mix.mic_gain_db));
}

void AudioEngine::SetChatMuted(bool muted) {
    if (chat_muted_ == muted) {
        return;
    }
    chat_muted_ = muted;
    PushGains();
    LogInfo(L"chat {}", muted ? L"muted" : L"unmuted");
}

void AudioEngine::SetMicMuted(bool muted) {
    if (mic_muted_ == muted) {
        return;
    }
    mic_muted_ = muted;
    PushGains();
    LogInfo(L"microphone {}", muted ? L"muted" : L"unmuted");
}

void AudioEngine::ApplyLiveConfig(const Config& next, std::vector<std::wstring>& blocked) {
    // Anything that sized a buffer, opened a device or started a thread cannot
    // change under a running engine. Say which ones differ rather than
    // pretending the reload took, or silently doing half of it.
    const auto selector_differs = [](const DeviceSelector& a, const DeviceSelector& b) {
        return a.id != b.id || a.name_contains != b.name_contains;
    };
    if (selector_differs(next.devices.chat_render, config_.devices.chat_render) ||
        selector_differs(next.devices.mic_capture, config_.devices.mic_capture) ||
        selector_differs(next.devices.output_render, config_.devices.output_render)) {
        blocked.push_back(L"[devices]");
    }
    if (next.audio.sample_rate != config_.audio.sample_rate ||
        next.audio.require_sample_rate != config_.audio.require_sample_rate ||
        next.audio.target_buffer_ms != config_.audio.target_buffer_ms ||
        next.audio.ring_capacity_ms != config_.audio.ring_capacity_ms) {
        blocked.push_back(L"[audio]");
    }
    if (next.drift.enabled != config_.drift.enabled ||
        next.drift.measure_window_s != config_.drift.measure_window_s ||
        next.drift.response_s != config_.drift.response_s ||
        next.drift.max_rate_correction != config_.drift.max_rate_correction) {
        blocked.push_back(L"[drift]");
    }
    if (next.resilience.backoff_min_ms != config_.resilience.backoff_min_ms ||
        next.resilience.backoff_max_ms != config_.resilience.backoff_max_ms ||
        next.resilience.keep_chat_clock_alive != config_.resilience.keep_chat_clock_alive ||
        next.resilience.startup_wait_s != config_.resilience.startup_wait_s) {
        blocked.push_back(L"[resilience]");
    }
    if (next.log.file != config_.log.file || next.log.max_bytes != config_.log.max_bytes ||
        next.log.keep_files != config_.log.keep_files || next.log.console != config_.log.console) {
        blocked.push_back(L"[log] file, max_bytes, keep_files, console");
    }

    // What is left is a handful of numbers the audio path reads per block.
    config_.mix = next.mix;
    config_.gate = next.gate;
    config_.log.level = next.log.level;
    config_.log.stats_interval_s = next.log.stats_interval_s;

    cable_.PublishLiveSettings(config_);
    PushGains();
    Logger::SetLevel(config_.log.level);

    LogInfo(L"reloaded: chat {:+.1f} dB{}, mic {:+.1f} dB{}, smoothing {:.1f} ms, limiter {}, "
            L"gate {}, log level {}",
            config_.mix.chat_gain_db, chat_muted_ ? L" (muted)" : L"", config_.mix.mic_gain_db,
            mic_muted_ ? L" (muted)" : L"", config_.mix.gain_smoothing_ms,
            config_.mix.limiter_enabled ? L"on" : L"off", config_.gate.enabled ? L"on" : L"off",
            LogLevelName(config_.log.level));
    for (const std::wstring& section : blocked) {
        LogWarn(L"reload: {} changed, but that needs a restart - still running the old values",
                section);
    }
}

EngineStatus AudioEngine::Status() const {
    EngineStatus status;
    status.running = running_;
    status.chat_muted = chat_muted_;
    status.mic_muted = mic_muted_;

    const SourceStats& chat = chat_.stats();
    const SourceStats& mic = mic_.stats();
    const RenderStats& render = cable_.stats();

    status.chat_live = chat.live.load(std::memory_order_relaxed);
    status.mic_live = mic.live.load(std::memory_order_relaxed);
    status.render_live = render.live.load(std::memory_order_relaxed);
    status.chat_started = chat.packets.load(std::memory_order_relaxed) != 0;
    status.mic_started = mic.packets.load(std::memory_order_relaxed) != 0;

    status.restarts = chat.restarts.load(std::memory_order_relaxed) +
                      mic.restarts.load(std::memory_order_relaxed) +
                      render.restarts.load(std::memory_order_relaxed);
    status.dropouts = chat.underruns.load(std::memory_order_relaxed) +
                      mic.underruns.load(std::memory_order_relaxed) +
                      chat.overruns.load(std::memory_order_relaxed) +
                      mic.overruns.load(std::memory_order_relaxed) +
                      render.timeouts.load(std::memory_order_relaxed);

    status.faulted = FAILED(chat_.fault()) || FAILED(mic_.fault()) || FAILED(cable_.fault());
    return status;
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

    LogInfo(L"resilience: each stream rebuilds itself, retrying from {} ms up to {} ms; endpoint "
            L"notifications {}", config.resilience.backoff_min_ms, config.resilience.backoff_max_ms,
            watcher_ ? L"subscribed, so a device that returns is picked up at once"
                     : L"UNAVAILABLE, so only the retry timer notices a device returning");
    if (chat_.clock_keeper_enabled()) {
        LogInfo(L"chat endpoint clock held open by a silent render client, so loopback keeps "
                L"delivering while nobody is talking");
    } else {
        LogInfo(L"chat endpoint clock not held open: while that endpoint is idle its loopback "
                L"delivers no packets at all and the chat ring refills when audio resumes "
                L"(resilience.keep_chat_clock_alive = true avoids that)");
    }
}

bool AudioEngine::StartThreads(HANDLE stop_event) {
    stop_event_ = stop_event;
    // Captures first, so the rings are already filling when the clock master
    // starts asking for frames.
    if (!chat_.StartThread(stop_event) || !mic_.StartThread(stop_event) ||
        !cable_.StartThread(stop_event)) {
        LogError(L"could not start the audio threads");
        return false;
    }
    running_ = true;
    since_report_ms_ = 0;
    PushGains();
    return true;
}

bool AudioEngine::Tick(std::uint32_t elapsed_ms, std::uint32_t stats_interval_s) {
    if (CheckFaults()) {
        return false;
    }
    since_report_ms_ += elapsed_ms;
    if (stats_interval_s != 0 && since_report_ms_ >= stats_interval_s * 1000) {
        LogCounters(L"stats");
        since_report_ms_ = 0;
    }
    return true;
}

void AudioEngine::Run(HANDLE stop_event, std::uint32_t stats_interval_s) {
    constexpr DWORD kPollMs = 500;

    if (!StartThreads(stop_event)) {
        return;
    }
    LogInfo(L"running; press Ctrl+C to stop");

    for (;;) {
        const DWORD result = ::WaitForSingleObject(stop_event, kPollMs);
        if (!Tick(kPollMs, stats_interval_s)) {
            break;
        }
        if (result == WAIT_OBJECT_0) {
            LogInfo(L"stop requested");
            break;
        }
        if (result != WAIT_TIMEOUT) {
            break;
        }
    }
}

bool AudioEngine::CheckFaults() {
    // Losing a device never reaches here any more: each stream rebuilds itself
    // and logs its own outage at the moment it happens (spec 4.8). What is left
    // is a thread that could not be brought up at all - no enumerator, no
    // waitable timer - which no amount of retrying fixes.
    const HRESULT chat_fault = chat_.fault();
    if (FAILED(chat_fault) && !chat_fault_reported_) {
        chat_fault_reported_ = true;
        LogError(L"chat loopback thread stopped: {}", FormatHresult(chat_fault));
    }

    const HRESULT mic_fault = mic_.fault();
    if (FAILED(mic_fault) && !mic_fault_reported_) {
        mic_fault_reported_ = true;
        LogError(L"microphone thread stopped: {}", FormatHresult(mic_fault));
    }

    const HRESULT render_fault = cable_.fault();
    if (FAILED(render_fault)) {
        LogError(L"cable render thread stopped: {}", FormatHresult(render_fault));
        return true;
    }
    return false;
}

void AudioEngine::Stop() {
    // Before the threads go, so a late notification cannot pulse an event that
    // is about to be closed.
    if (watcher_) {
        watcher_->Unregister();
        watcher_.Reset();
    }

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

        // Four states worth telling apart. A stream being rebuilt has no
        // endpoint at all. A source that has not delivered a single packet is
        // not slow to fill, it is not running: WASAPI loopback emits nothing
        // whatsoever on a render endpoint nobody is playing into, so an
        // untouched counter here means the chain was never exercised rather
        // than that it is behind. A refilling source is mixed as silence, and
        // its averaged fill and correction are frozen at whatever they were
        // when it went quiet.
        const bool down = running_ && !s.live.load(std::memory_order_relaxed);
        const bool primed = s.primed.load(std::memory_order_relaxed);
        const bool started = s.packets.load(std::memory_order_relaxed) != 0;
        const std::wstring level =
            down       ? std::wstring(L"DOWN, rebuilding")
            : !started ? std::format(L"NO DATA, not one packet - {}", idle_hint)
            : primed   ? std::format(L"fill {:.1f} ms (avg {:.1f})",
                                     FramesToMs(s.fill_frames.load(std::memory_order_relaxed),
                                                sample_rate_),
                                     FramesToMs(
                                         s.average_fill_frames.load(std::memory_order_relaxed),
                                         sample_rate_))
                       : std::format(L"REFILLING, fill {:.1f} ms",
                                     FramesToMs(s.fill_frames.load(std::memory_order_relaxed),
                                                sample_rate_));

        // Silent unless something actually happened, so a healthy session's
        // report stays the same length it always was.
        std::wstring resilience;
        if (const std::uint64_t restarts = s.restarts.load(std::memory_order_relaxed)) {
            resilience += std::format(L", rebuilt {}x", restarts);
        }
        if (const std::uint64_t lost = s.clock_keeper_faults.load(std::memory_order_relaxed)) {
            resilience += std::format(L", clock keeper lost {}x", lost);
        }

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
                L"overruns {} ({:.1f} ms), resyncs {} ({:.1f} ms){}",
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
                FramesToMs(s.resync_frames.load(std::memory_order_relaxed), sample_rate_),
                resilience);
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
        const std::uint32_t deepest = r.gate_min_gain.exchange(kGainQ16One, std::memory_order_relaxed);
        dynamics += std::format(L", gate {:.1f} dB deepest on {:.1f} % of frames",
                                DbFromGain(GainFromQ16(deepest)),
                                percent(r.gate_frames.load(std::memory_order_relaxed)));
    }

    std::wstring resilience;
    if (running_ && !r.live.load(std::memory_order_relaxed)) {
        resilience = L", DOWN and rebuilding";
    }
    if (const std::uint64_t restarts = r.restarts.load(std::memory_order_relaxed)) {
        resilience += std::format(L", rebuilt {}x", restarts);
    }

    LogInfo(L"{} render: callbacks {}, frames {} ({:.1f} s){}, clipped samples {}, timeouts {}{}",
            prefix, r.callbacks.load(std::memory_order_relaxed), rendered,
            FramesToMs(rendered, sample_rate_) / 1000.0, dynamics,
            r.clipped_samples.load(std::memory_order_relaxed),
            r.timeouts.load(std::memory_order_relaxed), resilience);
}

void AudioEngine::LogSummary() { LogCounters(L"session"); }

}  // namespace vcmic
