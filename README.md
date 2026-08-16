# vcmic — virtual cable microphone for ShadowPlay

ShadowPlay records system audio from one output endpoint only, and the Sound
Blaster GC7 mixes its game and chat outputs in hardware, after Windows — so
friends' voices never reach the recording. vcmic is a resident user-mode Windows
program that captures the chat output by WASAPI loopback, mixes it with the
microphone, and renders the result into VB-CABLE, which ShadowPlay then records
as its microphone track. No kernel-mode component, no third-party libraries,
Windows SDK only.

Stages 1–6 are done: the three streams, drift compensation, limiter and gate,
per-stream recovery from device loss, a tray icon that is the whole settings
surface, and autostart at log on.

- [`docs/how-it-works.md`](docs/how-it-works.md) — what it does, how to set it
  up, and what is inside it
- [`docs/gc7-virtual-mic-spec.md`](docs/gc7-virtual-mic-spec.md) — the
  specification this is built from
- [`docs/testing-notes.md`](docs/testing-notes.md) — what has been measured, and
  what has not

## Running

Needs Windows 10/11 and VB-CABLE A+B installed. In ShadowPlay, select
`CABLE-A Output` as the microphone and keep separate audio tracks enabled.

```powershell
vcmic --tray
```

That needs no arguments and no settings file. On a machine that has neither it
guesses the three endpoints — the default communications devices for chat and
microphone, whatever calls itself a VB-CABLE for the output — writes them to
`%APPDATA%\vcmic\config.toml`, and says so both in the log and in a balloon.
Anything it got wrong is one menu away. If it cannot fill all three the icon
comes up amber and the menu is the only thing that has to happen next.

```
vcmic                    run the mixer until Ctrl+C
vcmic --tray             the same, with an icon in the notification area
vcmic --list-devices     list every audio endpoint with id, roles and mix format
vcmic --active-only ...  with --list-devices: hide disabled/unplugged endpoints
vcmic --check-config     resolve the configured devices and validate formats
vcmic --config <path>    settings file, instead of the one found by default
vcmic --log-level <lvl>  trace|debug|info|warn|error|off
vcmic --version
vcmic --help

vcmic --install-autostart      start vcmic --tray when this user logs on
vcmic --logon-delay <s>        with --install-autostart, default 30
vcmic --uninstall-autostart    remove it
vcmic --autostart-status       print what is registered
```

Exit codes: `0` success, `1` bad command line, `2` failure.

Settings are read from the first of these that exists:

1. the path given to `--config`
2. `%APPDATA%\vcmic\config.toml`
3. `config.toml` next to `vcmic.exe`, for a portable copy

The log follows whichever one wins, so `%APPDATA%\vcmic\vcmic.log` by default.
`config.example.toml` documents every setting, though nothing has to be written
by hand — the tray menu covers the devices, the volumes and the processing, and
writes the file itself.

Only one mixer runs per session. A second `vcmic` finds the first holding a
named mutex and exits straight away rather than fighting it for the cable.

## Development

Visual Studio 2022 or newer with the Windows SDK, or the Build Tools (verified
with Visual Studio Community 2026, MSVC 19.51, toolset v180), and CMake 3.21+.
Windows only: WASAPI has no counterpart under WSL, and the CMake configure step
refuses to run there.

```powershell
cmake -B build -A x64
cmake --build build --config Release
```

Leaving `-G` off lets CMake pick the newest Visual Studio generator that is
actually installed, which avoids `MSB8020` when a specific toolset is not
present. Name it explicitly only if several versions are installed side by side
(`-G "Visual Studio 18 2026"`, `-G "Visual Studio 17 2022"`, ...).

If `cmake` is not on `PATH`, Visual Studio ships one:

```powershell
$env:PATH = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;$env:PATH"
```

The executable lands in `build\bin\Release\vcmic.exe` (`build\bin\vcmic.exe` for
single-config generators such as Ninja). The MSVC runtime is linked statically
by default, so no redistributable is needed; pass `-DVCMIC_STATIC_RUNTIME=OFF`
to change that.

`C4324` on `ring_buffer.h` is expected: it reports the padding `alignas(64)` was
asked for, which is what keeps the producer's and consumer's indices on separate
cache lines. Do not add `/WX` without suppressing it first.

### Self-test

`tools/dsp_selftest.cpp` exercises the resampler, the drift control loop, the
dynamics and the config round trip without any audio hardware, so it runs on any
host:

```sh
g++ -std=c++20 -O1 -fsanitize=address,undefined -Isrc \
    tools/dsp_selftest.cpp src/drift.cpp src/dynamics.cpp -o dsp_selftest && ./dsp_selftest
```

The config checks need the Windows-only helpers and are skipped there; the
cross-build that runs them under Wine, the cross-compilation syntax check, and
what each check actually proves are all in
[`docs/testing-notes.md`](docs/testing-notes.md).

### Repository layout

```
CMakeLists.txt
config.example.toml     annotated configuration template
docs/                   how it works, the specification, testing notes
tools/                  the offline self-test
src/
  main.cpp              command line, --list-devices, --check-config, autostart
  session.*             the run loop: startup wait, setup state, restarts, tray callbacks
  tray.*                the notification-area icon, its menus and its artwork
  autostart.*           the Task Scheduler logon task
  device_registry.*     endpoint enumeration, properties, id/name resolution, first guess
  device_watcher.*      IMMNotificationClient: wakes a broken stream early
  audio_format.*        WAVEFORMATEXTENSIBLE inspection and formatting
  audio_engine.*        the three streams, the mixer and the render callback
  drift.*               fractional-rate reader and the fill-level control loop
  dynamics.*            peak limiter and noise gate
  ring_buffer.h         lock-free SPSC ring of deinterleaved stereo float32
  sample_convert.*      capture/render format conversion, downmix and upmix
  audio_stats.h         the counters the audio threads publish
  config.*              the TOML-subset parser and writer, and where the file lives
  logging.*             levelled, rotating log file
  console.*             UTF-16/UTF-8 console output
  com.h                 ComPtr, PROPVARIANT and apartment RAII
  hresult.*             HRESULT to readable text, including the AUDCLNT_E_ codes
  paths.*, strings.*    small helpers
```
