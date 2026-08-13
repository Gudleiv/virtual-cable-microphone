# Testing notes

The target machine is the only place where the real signal chain exists, so
development leans on three substitutes. All are worth knowing about, mostly for
what they *cannot* tell us.

## Cross-compilation check

`x86_64-w64-mingw32-g++ -std=c++20 -Wall -Wextra` builds every source file. It
is not the shipping toolchain — MSVC is — but it catches typos, missing
includes and WASAPI misuse long before a Windows build is available. The
mingw-w64 headers cover `mmdeviceapi.h`, `audioclient.h` and `avrt.h`.

Two portability details fall out of it and are worth keeping:

- GUIDs come from `__uuidof(...)` rather than the `CLSID_*` / `IID_*` symbols,
  which avoids depending on which import library provides them.
- `INITGUID` is defined at the top of `device_registry.cpp` only, so the
  `PKEY_*` property keys are emitted there and nowhere else.

## Offline self-test

`tools/dsp_selftest.cpp` exercises everything stage 3 added without any audio
hardware at all — the drift and dynamics headers pull in no Windows API, so it
compiles and runs anywhere:

```sh
g++ -std=c++20 -O1 -fsanitize=address,undefined -Isrc \
    tools/dsp_selftest.cpp src/drift.cpp src/dynamics.cpp -o dsp_selftest && ./dsp_selftest
```

The sanitizer is the point of the first group: the resampler promises that
`InputFramesNeeded()` is exactly what `Process()` will consume, and the test
hands it buffers of precisely that size, so a disagreement of even one frame is
a heap overflow rather than a subtle glitch.

| Check | Result |
|---|---|
| resampler, ratio 1.0 | pass-through: tone at 0.5000, residual 138 dB down |
| resampler, ±198 ppm | tone at 0.49999, residual 90 dB down at 1 kHz, 55 dB at 4 kHz, 36 dB at 8 kHz |
| resampler, ±1000 ppm (the ceiling) | same figures; the artifact tracks frequency, not correction size |
| control loop, +198 / −198 / 0 / +900 ppm | learns +198.0 / −198.0 / +0.0 / +900.0 ppm, reached in 20-26 s |
| control loop, one hour each | fill held at 50.19 ms against a 50 ms target, 7.6 ms span, zero resyncs, zero underruns |
| correction wobble | 30 ppm, 1 sigma |
| a source handing over a 10 ms lump every 73 s | headline figure lands on the true rate and wobbles by 28 ppm where the total wobbles by 181 |
| the same hour with `drift.enabled = false` | 4 resyncs, i.e. one dropped backlog every ~12 minutes |
| beyond the ceiling (5000 ppm) | correction saturates, resync valve fires, fill still bounded |
| limiter | nothing exceeds the threshold, bit-transparent below it |
| gate | −60 dBFS noise stays shut, −20 dBFS speech passes at unity |

The timing noise in the simulated source is calibrated against the machine
rather than invented. A ten-minute session there ran the loop at
`response_s = 10` and showed the correction wobbling with a standard deviation
of about 157 ppm around the right answer — which at that setting means roughly
0.8 ms of jitter surviving the one-second averaging window. Feeding the
simulation a mean-reverting timing wander that reproduces that, then sweeping
the loop, gave:

| `response_s` | correction wobble, 1 sigma | time to reach the true rate |
|---|---|---|
| 10 s | 87-91 ppm | ~10 s |
| 20 s | 40 ppm | ~15 s |
| **30 s** | **30 ppm** | **20-26 s** |
| 45 s | 17 ppm | ~35 s |

30 s is the default: the wobble is small enough that the logged drift figure is
readable, and clock ratios do not change during a session anyway. Smoothing the
loop *output* instead was tried and dropped — it bought 12 % for an extra pole,
because the noise is slower than any smoother short enough to be safe.

The interpolation figures are the honest cost of Catmull-Rom: it rolls off
towards Nyquist, so at the worst phase it loses 0.2 dB at 8 kHz and 3.3 dB at
16 kHz, which appears as a slow amplitude modulation 36 dB below an 8 kHz tone.
Speech energy lives below 4 kHz, where the artifact is 55 dB down or better.
The specification allows plain linear interpolation, which is 6 dB down at
16 kHz and four times worse at 8 kHz; four points cost a few more multiplies.

## Wine plus PulseAudio null sinks

A container has no sound card, but PulseAudio's `module-null-sink` invents as
many endpoints as needed entirely in user space, and Wine's `winepulse.drv`
presents them to WASAPI. Sinks configured as `rate=48000 channels=2
format=float32le` show up as 48 kHz stereo float32 endpoints — the same shape
as the GC7 and the cable.

That makes an end-to-end test possible: play known tones into the sinks, run
vcmic, record the cable sink's monitor with `parec`, and measure the result
with a Goertzel filter.

Stage 2 was verified this way:

| Check | Result |
|---|---|
| stereo chat + stereo mic, unity gain | both tones recovered at exactly their input amplitude, peak equal to the sum, no clipping |
| 5.1 chat downmix | front left/right at unity, front center at 0.7071, identical on both output channels |
| mono microphone | duplicated to both channels at unity, no 3 dB loss |
| ring buffers, mixing, render callback, counters | no underruns, no overruns, no dropped packets |

Stage 3 was re-run on the same rig, which is a useful adversary here because two
PulseAudio null sinks disagree by far more than any real hardware does:

| Check | Result |
|---|---|
| tones through the resampler, 250 ms windows | 440 Hz at 0.2997 and 1000 Hz at 0.2998 against 0.3000 in, peak 0.5999 = exact unity sum |
| drift correction under a mismatch past the ceiling | pinned at +1000 ppm, fill held in a 70-107 ms band, **zero** resyncs and underruns over a minute where stage 2 fired the valve about once a second |
| accumulated correction counter | +45 ms over 45 s at 1000 ppm, exactly as reported |
| limiter, both gains at +6 dB | sum of 1.1976 capped at 0.891251 = the −1 dBFS threshold, 0 samples over, 0 clipped, reported as −2.6 dB |

### What this rig cannot check

- **WASAPI loopback.** Wine 9.0 returns `AUDCLNT_E_WRONG_ENDPOINT_TYPE` from
  `GetService(IAudioCaptureClient)` on a render-dataflow client, so the
  `AUDCLNT_STREAMFLAGS_LOOPBACK` path cannot run here at all. The sequence in
  `CaptureSource::Open` is the documented Windows one (activate the render
  endpoint, initialize with the loopback flag, then ask for the capture
  service); it is a Wine gap, not a difference of opinion about the API. The
  loopback path is only ever exercised on real hardware. The rest of the
  pipeline was tested by pointing the chat source at a monitor *capture*
  endpoint through a local, uncommitted one-line patch.
- **Clock drift at realistic magnitudes.** Two PulseAudio null sinks are
  independent timers rather than a real clock, and they disagree by roughly
  1.5 % — about 150 times worse than the 100 ppm the specification budgets for,
  and past the correction ceiling. That makes it a good stress test of
  saturation and of the resync valve, but the loop is never asked to settle, so
  the convergence numbers come from the offline self-test instead. The real
  figures for this hardware were measured from the `frames` counters of live
  sessions on the target machine; see below.
- **Anything to do with device re-enumeration**, sleep/wake or USB replug. See
  the stage 4 section below for what that leaves untested and how to check it.

## The target machine, with both sources live

A seven-minute session with audio playing into the GC7 headset endpoint and the
microphone open is the one measurement that separates the control loop from the
hardware, because both capture sources run identical code, on the same machine,
at the same moment. They do not behave the same:

| | chat (GC7 loopback) | microphone (fifine, USB) |
|---|---|---|
| reported correction, 1 sigma | **35 ppm** | 189 ppm |
| swing of the averaged fill | **1.6 ms** | 9.5 ms |
| `discontinuities` | 1, at stream start | 0.82 per minute |
| clock against the cable | within a few tens of ppm | +88 ppm |

The chat figure of 35 ppm is what `drift.response_s = 30` was chosen to deliver,
so the loop does what the sweep said it would. Everything above that on the
microphone row is the device, not the loop: splitting the intervals by whether
`discontinuities` moved gives a mean correction of +280 ppm in the intervals
where it did and −27 ppm in the intervals where it did not, with almost no
overlap between the two populations. The microphone hands over a lump of frames
roughly once a minute and the loop spends the following half-minute draining it.

Two independent runs put the microphone's clock at +87 and +88 ppm, which
supersedes an earlier estimate of +198 ppm: that one was taken from the
accumulated correction, which also contains the lump absorption and therefore
runs high.

None of this costs anything audible — 25 minutes and 7 minutes both ended with
zero underruns, overruns, resyncs and clipped samples — but it is why the log
reports the learned rate and the absorption as separate numbers.

Two behaviours worth recognising in a log, neither of them a fault:

- When the chat endpoint goes idle, WASAPI delivers a few hundred milliseconds
  of packets flagged `AUDCLNT_BUFFERFLAGS_SILENT` and then stops delivering
  altogether. The ring drains, one underrun is counted for the shortfall, and
  the source flips to refilling and is mixed as silence from then on. The
  measured cost of that transition is a single 1.7 ms gap.
- A source that has never delivered a packet reports `NO DATA`, not
  `REFILLING`. On the chat row it means nothing is playing into that endpoint;
  loopback on an idle render endpoint emits no packets at all, not silent ones.

## Stage 4: recovery, measured on the Wine rig

The rig turned out to reach further than expected. `pactl unload-module` takes a
null sink away from underneath a running stream, which is as close to yanking a
USB cable as a container gets, and `pactl suspend-sink` produces the other case
that matters: an endpoint that is still there but has stopped delivering.

| Check | Result |
|---|---|
| capture endpoint removed | `AUDCLNT_E_DEVICE_INVALIDATED`, source reads `DOWN, rebuilding`, one 1.1 ms underrun as the ring drained, then mixed as silence |
| ... while it was gone | the other capture source and the render were untouched; render callbacks kept climbing straight through the outage |
| backoff | 109, 210, 409, 809, 1609, 3208, 5009 ms — the intended 100/200/400/800/1600/3200 and then the 5 s ceiling |
| log volume | attempts 1-2 at WARN, the rest at DEBUG, recovery at INFO |
| recovery | `mic stream back after 16.4 s and 9 attempt(s)`, then `rebuilt 1x` in the report |
| new endpoint id after the reload | resolved through the name fallback, as a re-enumerated USB device would be |
| render endpoint removed | same sequence on the clock master, `rebuilt 1x`, and the rings behaved as predicted: 1621 overruns / 16.2 s dropped while it was gone, then 4 resyncs trimming the backlog |
| `ClockKeeper` on a live endpoint | sink went `SUSPENDED` → `RUNNING` while it pumped and back to `IDLE` when closed; 800 pumps over 4 s, zero failures |
| loopback silence policy: endpoint suspended but present | 20 s of no packets, **no** rebuild — silence alone must never be treated as death there |

The zombie case is why the silence watchdog exists. Removing a capture endpoint
and re-adding it a second later left the stream answering `S_OK` from every
call and delivering nothing, for as long as the test ran: `frames` frozen at
355200 for 25 s, no error, no rebuild, the source silent forever. With the
watchdog, the same sequence reads `mic stream lost: no capture packet for 5 s -
rebuilding` and is serving again 8 ms later.

### What the rig still could not check

- **`IMMNotificationClient`.** `winepulse` never fires the callbacks — zero
  notifications across every run, though registration itself succeeded. So every
  recovery above was driven purely by the backoff timer. That is the design
  intent, and it is reassuring that it holds up alone, but the claim that a
  returning device is picked up in milliseconds rather than at the next retry is
  unverified.
- **The format-change rejection.** `winepulse` advertises 48000 Hz whatever the
  sink's real rate is, so a sink reloaded at 44100 Hz still presents as 48000
  and `SameFormat` accepts it. The branch has never run.
- **The loopback endpoint-gone probe.** Wine keeps removed devices in the
  enumerator and still reports them ACTIVE — visible in the runs above, where
  rebuild attempts got all the way past `Activate` and failed only at
  `Initialize`. So `EndpointStillActive` always answers yes here. Its
  false-positive half was checked and passed; its true-positive half was not.
- **The render's no-callback watchdog.** The cable always reported
  `AUDCLNT_E_DEVICE_INVALIDATED` promptly, so `timeouts` stayed 0 and the three
  quiet windows never accumulated.
- **Sleep/wake and a real USB replug**, and the clock keeper against a real GC7
  endpoint rather than a null sink.

### On the machine, from spec §6.7

1. **Sleep and wake.** Expect `stream lost: AUDCLNT_E_DEVICE_INVALIDATED -
   rebuilding` and then `stream back after N s` on each affected stream, with
   `rebuilt 1x` in the next report and no restart needed.
2. **Unplug and replug the GC7.** The chat source should go `DOWN, rebuilding`
   and come back. While it is down the render row must keep counting callbacks.
3. **Leave it unplugged for a few minutes.** Retries should settle at 5 s and
   the log at roughly a line a minute. Replugging should be picked up at once —
   this is the notification path the rig could not test.
4. **Lower Max Latency in `VBCABLE_ControlPanel.exe` while vcmic runs.** Expect
   a rebuild and `cable render block is now N frames`.
5. **`keep_chat_clock_alive = true`** (on in this machine's `config.toml`): with
   Discord silent the chat row should hold `fill ~40 ms` with `silent packets`
   climbing, instead of falling to `REFILLING`.
