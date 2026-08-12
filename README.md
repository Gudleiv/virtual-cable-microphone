# vcmic — virtual cable microphone for ShadowPlay

A user-mode Windows service that builds the microphone track NVIDIA ShadowPlay
is missing.

ShadowPlay records system audio as a WASAPI loopback of exactly **one** endpoint
— the default output device. The Sound Blaster GC7 exposes two independent
outputs (`Speakers` for the game, `Headset` for Discord) and mixes them in
hardware, *after* Windows, with the GameVoice Mix knob. The chat audio therefore
never reaches the recording, and the hardware has no What-U-Hear to get it back.

The only remaining input into ShadowPlay is the microphone selector. So this
program assembles a virtual microphone that carries `friends + your own voice`:

```
Game ─────────────────► Speakers (GC7) ──► [ShadowPlay: system track]
                                       └──► ears

Discord ──────────────► Headset (GC7) ───► ears (GameVoice Mix, 0 ms)
                             │
                             │ WASAPI loopback (read-only copy of the stream)
                             ▼
Microphone (GC7) ────►  [vcmic: mix]  ──────► CABLE-A Input
                             │                      │
                             │                      ▼
                             └── (Discord keeps      CABLE-A Output
                                  the mic open in         │
                                  shared mode)            ▼
                                                 [ShadowPlay: microphone track]
```

Loopback capture of another endpoint does not insert itself into Discord's path
and costs it nothing. Discord and the Windows defaults are never reconfigured.

There is **no kernel-mode component**. The signed kernel half is VB-CABLE A+B,
installed separately; vcmic is a plain user-mode process that anti-cheat systems
have no reason to look at.

The full specification is in [`docs/gc7-virtual-mic-spec.md`](docs/gc7-virtual-mic-spec.md).

## Status

| Stage | Contents | State |
|---|---|---|
| 1 | project skeleton, `--list-devices`, config parser, logging | **done** |
| 2 | three streams, lock-free SPSC rings, mixing | not started |
| 3 | drift compensation, limiter, counters | not started |
| 4 | `IMMNotificationClient`, device-invalidation recovery, backoff | not started |
| 5 | tray icon, autostart | not started |

Stage 1 does not move any audio yet. It identifies devices and validates the
configuration, which is what the later stages are configured from.

## Requirements

- Windows 10/11
- Visual Studio 2022 or newer with the Windows SDK, or the Build Tools
  (verified with Visual Studio Community 2026, MSVC 19.51, toolset v180)
- CMake 3.21+
- VB-CABLE A+B installed, for stage 2 onward

No third-party libraries. Only the Windows SDK. Windows only: WASAPI has no
counterpart under WSL, and the CMake configure step refuses to run there.

## Build

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
by default, so no redistributable is needed; pass
`-DVCMIC_STATIC_RUNTIME=OFF` to change that.

## Usage

```
vcmic --list-devices     list every audio endpoint with id, roles and mix format
vcmic --active-only ...  with --list-devices: hide disabled/unplugged endpoints
vcmic --check-config     resolve the configured devices and validate formats
vcmic --config <path>    config file (default: config.toml next to the exe)
vcmic --log-level <lvl>  trace|debug|info|warn|error|off
vcmic --version
vcmic --help
```

Exit codes: `0` success, `1` bad command line, `2` failure.

### Setting it up

`config.toml` in the repository root is already filled in for this machine from
the survey in [`docs/devices-2026-08-12.md`](docs/devices-2026-08-12.md), so the
check is just:

```powershell
build\bin\Release\vcmic.exe --check-config --config config.toml
```

`--config` takes a path relative to the current directory; without it, vcmic
reads `config.toml` next to the executable.

To redo the survey after a hardware change:

1. `vcmic --list-devices > devices.txt` and find the three endpoints:
   - the GC7 render endpoint Discord plays into (the Default Communications
     Device — `Наушники гарнитуры`),
   - the microphone Discord records from,
   - `CABLE-A Input` (the **render** side of the cable, not `CABLE-A In 16ch`).

   Every endpoint is listed with its state, so devices that Windows currently
   hides also show up.

2. Paste the ids into `config.toml`. The bottom of the `--list-devices` output
   contains a guessed `[devices]` block to start from — check it rather than
   trust it. `config.example.toml` documents every available setting.

3. `vcmic --check-config` — it resolves each device, prints what it found and
   verifies that all three run at 48 kHz.

4. In ShadowPlay, select `CABLE-A Output` (the **capture** side) as the
   microphone, and keep separate audio tracks enabled.

### Why this is not a Windows service

Audio endpoints belong to the user's session. A service runs in Session 0 and
sees no devices at all. Autostart belongs in Task Scheduler ("at log on") or in
the `Run` key, so that the process lives in the interactive session.

## Configuration

`config.toml` sits next to the executable. See `config.example.toml` for the
full annotated list. Device matching uses `IMMDevice::GetId()`; the
`*_name` keys are a substring fallback that logs a warning whenever it is used,
because friendly names are localized and change on re-enumeration.

The log file is written next to the executable too, and rotates by size
(`log.max_bytes`, `log.keep_files`).

## Known external pitfalls

- **ShadowPlay drops the microphone selection** when the Windows Recording tab
  has fewer than four active devices. Show disabled devices and enable enough of
  them.
- ShadowPlay's system track follows whatever output is assigned to the
  `NVIDIA Container` process under *App volume and device preferences*. If the
  game disappears from the recording, look there.
- In `VBCABLE_ControlPanel.exe`, pin 48000 Hz and the lowest Max Latency that
  stays stable.

## Repository layout

```
CMakeLists.txt
config.example.toml     annotated configuration template
docs/                   the specification this is built from
src/
  main.cpp              command line, --list-devices, --check-config
  device_registry.*     endpoint enumeration, properties, id/name resolution
  audio_format.*        WAVEFORMATEXTENSIBLE inspection and formatting
  config.*              the TOML-subset parser and the typed configuration
  logging.*             levelled, rotating log file
  console.*             UTF-16/UTF-8 console output
  com.h                 ComPtr, PROPVARIANT and apartment RAII
  hresult.*             HRESULT to readable text, including the AUDCLNT_E_ codes
  paths.*, strings.*    small helpers
```
