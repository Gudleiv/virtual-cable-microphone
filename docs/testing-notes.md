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
| control loop, +198 / −198 / 0 / +900 ppm | learns +198.8 / −197.3 / +0.2 / +900.7 ppm |
| control loop, one hour each | fill held at 49.99 ms against a 50 ms target, 4.4 ms span, zero resyncs, zero underruns |
| the same hour with `drift.enabled = false` | 4 resyncs, i.e. one dropped backlog every ~12 minutes |
| beyond the ceiling (5000 ppm) | correction saturates, resync valve fires, fill still bounded |
| limiter | nothing exceeds the threshold, bit-transparent below it |
| gate | −60 dBFS noise stays shut, −20 dBFS speech passes at unity |

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
  figure for this hardware, +198 ppm on the fifine microphone against the cable
  with the GC7 loopback effectively locked to it, was measured from the
  `frames` counters of two live sessions on the target machine.
- **Anything to do with device re-enumeration**, sleep/wake or USB replug.
