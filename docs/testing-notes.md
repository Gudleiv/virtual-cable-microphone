# Testing notes

The target machine is the only place where the real signal chain exists, so
development leans on two substitutes. Both are worth knowing about, mostly for
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
  1.5 % — about 150 times worse than the 100 ppm the specification budgets for.
  That is useful as a stress test of the resync valve (it fired about once a
  second and held the fill level bounded) but says nothing about how the drift
  compensation of stage 3 will behave.
- **Anything to do with device re-enumeration**, sleep/wake or USB replug.
