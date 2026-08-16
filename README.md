# vcmic — virtual cable microphone for ShadowPlay

A resident user-mode Windows program that builds the microphone track NVIDIA
ShadowPlay is missing. Deliberately not a service — see
[below](#why-this-is-not-a-windows-service).

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
| 2 | three streams, lock-free SPSC rings, mixing | **done** |
| 3 | drift compensation, limiter, noise gate | **done** |
| 4 | `IMMNotificationClient`, device-invalidation recovery, backoff | **done** |
| 5 | tray icon, autostart | **done** |
| 6 | settings in the tray, config in `%APPDATA%`, first run with no config | **done** |

Running `vcmic` with no mode option mixes chat and microphone into the cable
until Ctrl+C, correcting for the three clocks as it goes, and rebuilding any of
the three streams that loses its device. `vcmic --tray` does the same behind an
icon in the notification area, where the three endpoints, the two volumes and
the processing can all be set from the menu, and `vcmic --install-autostart`
makes that happen at log on. See
[`docs/testing-notes.md`](docs/testing-notes.md) for what has been measured and
what has not.

Stage 4 left a device missing **at startup** as a fatal error, on the grounds
that telling a misconfigured id apart from an unplugged one is worth more at
that moment than starting anyway. Autostart makes that distinction impossible to
draw — at log on the USB stack is still enumerating — so stage 5 gives startup
its own retry window, `resilience.startup_wait_s`, defaulting to a minute. Run
by hand with `startup_wait_s = 0` and the old behaviour is back: a wrong id
fails immediately instead of hanging about. With a tray icon there is a menu to
fix it from, so stage 6 goes further: the wait never expires, because an
endpoint that turns up ten minutes late is still worth picking up and the icon
is there to say what is being waited for.

Stage 6 also settled where a config lives. Stage 5 looked for it next to the
executable, which on a machine where vcmic was compiled rather than installed is
a build output folder — so `--install-autostart` registered a task that started,
found no settings, and stopped. Settings now default to
`%APPDATA%\vcmic\config.toml`, the autostart task always records the resolved
absolute path, and none of it has to exist beforehand.

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

`vcmic --tray` needs no arguments and no settings file. On a machine that has
neither it guesses the three endpoints — the default communications devices for
chat and microphone, whatever calls itself a VB-CABLE for the output — writes
them to `%APPDATA%\vcmic\config.toml`, and says so both in the log and in a
balloon. Anything it got wrong is one menu away. If it cannot fill all three the
icon comes up amber and the menu is the only thing that has to happen next.

Where settings are read from, in order:

1. the path given to `--config`
2. `%APPDATA%\vcmic\config.toml`
3. `config.toml` next to `vcmic.exe`, for a portable copy

The log follows whichever one wins, so `%APPDATA%\vcmic\vcmic.log` by default.
Roaming rather than local because that is the folder people know how to find;
the one genuinely machine-specific thing in there is the endpoint ids, and a
config that roams onto a machine without those devices comes up in the tray's
setup state rather than doing any harm.

Only one mixer runs per session. A second `vcmic` finds the first holding a
named mutex and exits straight away rather than fighting it for the cable.

While running, vcmic logs a counter report every `log.stats_interval_s`
seconds: the fill level of each ring and the drift correction being applied to
it, plus underruns, overruns, resyncs, discontinuities, limiter activity and
clipped samples. A healthy session shows a fill level sitting on
`audio.target_buffer_ms`, a drift figure that settles within the first half
minute and then stays put, and zeros everywhere else. `corrected` is
cumulative: it is the skew that would otherwise have accumulated, and it is
expected to grow steadily.

`drift` is reported as two numbers, because they answer different questions:

```
stats mic: fill 39.2 ms (avg 39.9), drift +92 ppm (+238 transient), corrected +70.8 ms
```

The headline is the clock ratio the loop has learned. It is a property of the
hardware, it settles and then sits still, and it is the number to quote when
asking how far apart two devices run. The figure in brackets is everything the
loop is doing on top of that to absorb a disturbance — a device that handed
over a lump of frames at once, or a scheduling stall. It is large and
short-lived by design, and a source that keeps showing a big one is a source
worth looking at: on this machine it tracks the `discontinuities` counter
almost exactly.

A source that has never delivered a packet says `NO DATA` rather than
`REFILLING`. The two are not the same problem: WASAPI loopback emits nothing at
all on a render endpoint nobody is playing into, so `NO DATA` on the chat row
usually means the audio is going somewhere else, not that the ring is behind.

## How the audio path works

Three threads, and the render side owns the clock:

- **Chat capture** — WASAPI loopback on the endpoint Discord plays into,
  polled from its own thread with a high-resolution waitable timer. Loopback is
  unreliable with an event callback in shared mode, hence polling. Reading a
  copy of that stream does not disturb Discord or add latency to it.
- **Microphone capture** — ordinary shared-mode capture, polled the same way,
  never exclusive, so Discord keeps the microphone open at the same time.
- **Cable render** — event-driven and the clock master. Every event it pulls
  what both rings have, mixes, and hands the result to WASAPI.

Between them sit two lock-free single-producer/single-consumer rings of
deinterleaved float32. Nothing in the audio path allocates, locks or logs;
threads run under MMCSS "Pro Audio", and counters are plain relaxed atomics
that a low-priority thread reports.

Each source is held silent until its ring reaches `audio.target_buffer_ms`, so
the steady-state latency is what the config asks for rather than whatever the
startup race produces. A source that stalls goes back to refilling, and a
backlog past four times the target is dropped rather than carried as permanent
latency.

### Three clocks

The GC7's capture side, its render side and the cable each run on their own
crystal, and none of them is the cable's. On this machine the microphone runs
about 88 ppm fast, which is 317 ms of skew per hour — the whole reason
`audio.target_buffer_ms` cannot simply be left to look after itself. The GC7's
render endpoint, by contrast, is within a few tens of ppm of the cable; it is
the microphone that needs the correction.

So the render thread reads each ring at a rate slightly different from the one
it plays at, interpolating between input samples with a four-point Catmull-Rom
kernel. A PI loop drives that rate from the ring's own fill level, averaged over
`drift.measure_window_s`: the integral term converges on the true clock ratio
and holds it there, so the fill sits on target instead of walking away. Hard
drops stay as the emergency valve for a stall the correction cannot absorb, and
they are counted as resyncs.

`drift.response_s` is deliberately slow. Whatever timing jitter survives the
averaging window comes back out of the proportional term as a wobble on the
read rate, in proportion to `1/response_s`; the default of 30 s keeps that
around 30 ppm and still reaches a new clock ratio inside half a minute. The
figures behind that choice, measured against this machine's own timing noise,
are in [`docs/testing-notes.md`](docs/testing-notes.md).

The one thing to know when picking `audio.target_buffer_ms`: the render thread
takes a **whole block** out of each ring per callback, so the margin against an
underrun is `target_buffer_ms` minus the cable's block, not the target itself.
The startup log prints both, and warns when the difference gets thin. With
VB-CABLE's default 22 ms block, a 25 ms target leaves nothing; either raise the
target to ~40 ms or lower Max Latency in `VBCABLE_ControlPanel.exe`.

The sum then goes through a peak limiter with no lookahead — the gain is never
allowed above what the current sample permits, so nothing can overshoot the
threshold — and an optional noise gate sits on the microphone alone.

### When a device disappears

A USB DAC re-enumerates on sleep/wake and whenever the cable is pulled, and the
streams that were reading it die with `AUDCLNT_E_DEVICE_INVALIDATED`. Each of
the three streams recovers on its own, on its own thread — the thread that owns
those WASAPI objects is the only one that ever touches them, so a rebuild needs
no lock anywhere near the audio path.

- **Only the broken stream stops.** A capture source that is gone is mixed as
  silence; the render keeps handing buffers to the cable, so ShadowPlay never
  sees the microphone end. This matters more than it sounds: a gap in the stream
  can look to ShadowPlay exactly like the device being removed.
- **The first retry is immediate**, then 100 ms, 200 ms, 400 ms … up to 5 s.
  A stream that comes back and dies again straight away keeps backing off
  instead of resetting, so a device that is broken rather than absent cannot
  turn recovery into a spin.
- **Endpoint notifications cut the wait short.** `IMMNotificationClient` is not
  used to drive recovery — those callbacks repeat, can be missed, and can arrive
  before an endpoint can actually be opened. All they do is pulse the event the
  broken stream is waiting on, so a replug is picked up in milliseconds instead
  of at the end of a backoff. Correctness never depends on one arriving.
- **A rebuilt endpoint has to come back with the same format.** The rings, the
  converters and the mixer were all sized for it at startup, and re-sizing them
  would mean stopping the render for one source's sake. If the sample rate or
  channel count changed, the log says exactly what changed and the stream keeps
  retrying until it is put back. The one exception is the cable's own block
  length, which is the render thread's to resize: lowering Max Latency in
  `VBCABLE_ControlPanel.exe` restarts the cable, and vcmic adopts the new block.
- **A render that goes quiet is rebuilt too.** Three wait windows in a row with
  no callback at all (6 s) means the clock master has stopped without reporting
  a failure. There is no audio flowing at that point either way.
- **A stream can outlive its device without a single call failing.** Remove a
  capture endpoint and re-add it a moment later and the old client keeps
  answering `S_OK` and delivering nothing, forever. So silence is watched as
  well as errors: five seconds of no packets at all is proof enough on an
  ordinary capture stream, because a live one always delivers, silence
  included. On the loopback stream it proves nothing — an idle render endpoint
  legitimately delivers no packets — so there the same five seconds only
  prompts a question to the enumerator about whether the endpoint is still
  there, which cannot be misread.

Rebuilds are counted per stream and appear in the periodic report and the exit
summary as `rebuilt Nx`, and a stream that is down at that moment reads `DOWN,
rebuilding` instead of a fill level.

### Idle chat endpoints, and `keep_chat_clock_alive`

A render endpoint nobody is playing into stops its audio engine, and WASAPI
loopback on a stopped engine delivers **no packets at all** — not even silent
ones. So when Discord goes quiet, the chat ring drains, the source drops to
`REFILLING`, and it costs one small underrun on the way down.

Setting `resilience.keep_chat_clock_alive = true` holds a started render client
of vcmic's own on that endpoint. It renders digital silence, so nothing is
audible and nothing about the device changes, but the endpoint's clock keeps
running and loopback keeps handing over silence — the ring stays at its target
and the drift loop never has to reconverge. It is off by default because it is a
real stream on somebody else's output device; it is on in this machine's
`config.toml` because the logs showed exactly that idle behaviour.

### Setting it up

The short version is `vcmic --tray`, then check the three endpoints in its menu
and pick `CABLE-A Output` as the microphone in ShadowPlay. Everything below is
for doing it by hand, or for understanding what the menu chose.

`config.toml` in the repository root is the survey record for this machine, from
[`docs/devices-2026-08-12.md`](docs/devices-2026-08-12.md). It is not on the
search path any more — settings live in `%APPDATA%\vcmic\config.toml` — so
either point at it explicitly or install it:

```powershell
build\bin\Release\vcmic.exe --check-config --config config.toml
copy config.toml "$env:APPDATA\vcmic\config.toml"
```

To redo the survey after a hardware change:

1. `vcmic --list-devices > devices.txt` and find the three endpoints:
   - the GC7 render endpoint Discord plays into (the Default Communications
     Device — `Наушники гарнитуры`),
   - the microphone Discord records from,
   - `CABLE-A Input` (the **render** side of the cable, not `CABLE-A In 16ch`).

   Every endpoint is listed with its state, so devices that Windows currently
   hides also show up.

2. Either pick them from the tray menu, which writes them out for you, or paste
   the ids into the config by hand. The bottom of the `--list-devices` output
   contains a guessed `[devices]` block to start from — check it rather than
   trust it. `config.example.toml` documents every available setting.

3. `vcmic --check-config` — it resolves each device, prints what it found and
   verifies that all three run at 48 kHz.

4. In ShadowPlay, select `CABLE-A Output` (the **capture** side) as the
   microphone, and keep separate audio tracks enabled.

### The tray icon

`vcmic --tray` puts a microphone in the notification area, coloured by state:
green when all three streams are up, amber while one is being rebuilt or
something is muted or the devices have not been chosen yet, red when the mixer
has stopped. The tooltip spells out which. Muted counts as amber on purpose — a
forgotten mute produces a recording with half the audio missing, and the icon is
the only thing that would have said so before the clip was already made.

The menu is the whole settings surface:

- **Mute chat**, **mute microphone**
- **Chat source**, **Microphone**, **Output** — every endpoint Windows currently
  has, with the configured one marked. A configured device that is not plugged
  in is listed too, as `(not connected)`, so the menu can still answer "what is
  this set to".
- **Chat volume**, **Microphone volume** — a ladder from −24 to +12 dB, finer
  near unity, plus ±1 dB nudges for anything in between. The current value is in
  the submenu's title, because a nudged value is not on the ladder.
- **Processing** — limiter, noise gate, gate threshold, drift compensation.
- **Reload settings from the file**, **write a counter report to the log now**,
  **open the log**, **open the config file**, **exit**.

Every change is written straight back to the config file, so what is running and
what is saved never diverge. The file is rewritten whole and generated with its
own comments; hand-written ones do not survive a change made from the menu.

Changes divide into two kinds. **The two gains, the limiter and the gate** are
applied to the running engine — the audio does not stop and nothing clicks,
because a gain change of any kind rides the `mix.gain_smoothing_ms` ramp and mute
is simply a gain of zero. **A different endpoint, or the drift setting**, cannot
be slipped underneath a running stream: each stream sizes its buffers and
configures its drift controller as it opens. Those stop and reopen the engine,
which takes about a second and is announced in the tooltip.

**Reload settings from the file** does the same triage: if the file only differs
in the live settings it is applied without a gap, and otherwise the mixer
restarts onto it. Either way what ends up running is what the file says. A file
with errors is refused whole; nothing is applied by halves.

The icon is drawn at run time rather than shipped as a resource, so it comes out
at whatever `SM_CXSMICON` says and there is no `.ico` in the repository.

Everything above happens on the main thread. The audio threads never touch a
window, and the tray never touches WASAPI: mute reaches the render thread as one
relaxed store, and a live settings change as one release/acquire flag, both read
at a block boundary. Enumerating the endpoints for the menu, computing filter
coefficients and writing the file all happen on the message thread, where a
blocking call costs a moment of menu latency and nothing else.

### Autostart, and why this is not a Windows service

Audio endpoints belong to the user's session. A service runs in Session 0 and
sees no devices at all. Autostart therefore belongs in Task Scheduler ("at log
on"), so that the process lives in the interactive session.

```powershell
vcmic --install-autostart
vcmic --autostart-status
vcmic --uninstall-autostart
```

That registers a per-user task called `vcmic` in the root folder of
`taskschd.msc`, running this executable with `--tray --config <path>`. It needs
no administrator rights: a task that runs as you, with an interactive token, is
yours to create.

The config path recorded is always the fully resolved absolute one — whatever
`--config` was given, or whichever file the search order found, or the
`%APPDATA%` path a first run will create. Never what was typed, because the task
starts with a working directory of the scheduler's choosing, and never nothing
at all, because that used to mean "look next to the executable" and land in a
build output folder. `--autostart-status` prints the command line as registered,
read back out of the scheduler rather than out of what was just sent to it.

Several of the scheduler's defaults are actively wrong for a resident audio
mixer, and the installer overrides all of them:

| Default | What it would do | Set to |
|---|---|---|
| `ExecutionTimeLimit` 3 days | kill a healthy mixer after 72 hours | unlimited |
| `Priority` 7 | run the process at `BELOW_NORMAL_PRIORITY_CLASS` | 5, the normal class |
| `DisallowStartIfOnBatteries` | never start on an unplugged laptop | off |
| `StopIfGoingOnBatteries` | stop mid-session when the charger comes out | off |
| `Hidden` off | flash a console window at every log on | on |
| no restart on failure | a mixer that dies stays dead until you notice | 3 retries, one minute apart |

`--logon-delay` (30 s by default) is the setting that matters most. At log on
the USB stack is still enumerating and the sound card does not exist yet;
`resilience.startup_wait_s` covers the same gap from the other end, so the two
together mean a cold boot does not need a manual restart.

Any setting the scheduler declines is printed as a warning rather than
swallowed — a task that registers and then behaves in a way nothing in the
config explains is worse than one that fails out loud.

## Configuration

Settings default to `%APPDATA%\vcmic\config.toml`; see the search order under
[Usage](#usage), and `config.example.toml` for the full annotated list of
settings. Nothing has to be written by hand — the tray menu covers the devices,
the volumes and the processing, and writes the file itself.

Device matching uses `IMMDevice::GetId()`. The `*_name` keys are a substring
fallback that logs a warning whenever it is used, because friendly names are
localized and change on re-enumeration; the tray writes both, so a device whose
id changes under a driver reinstall is still found by name.

The log file is written next to the config, wherever that turned out to be, and
rotates by size (`log.max_bytes`, `log.keep_files`).

## Known external pitfalls

- **ShadowPlay will not keep a `LineLevel` endpoint as the microphone.** The
  selection can be made and then silently reverts. Confirmed on this machine:
  every endpoint that persisted reported `PKEY_AudioEndpoint_FormFactor =
  Microphone`, including virtual ones, and both VB-CABLE outputs reported
  `LineLevel`. Rewriting the property fixes it. As administrator, with the
  endpoint GUID taken from `--list-devices`:

  ```powershell
  $k = "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\{a789742f-ab35-41b1-b651-1c86c68a1787}"
  reg export $k "$env:USERPROFILE\cable-a-output-backup.reg"
  reg add "$k\Properties" /v "{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},0" /t REG_DWORD /d 4 /f
  ```

  4 is `Microphone`, 2 is `LineLevel`. Disable and re-enable the device
  afterwards, then check with `vcmic --list-devices` that the form factor
  changed. VB-CABLE reinstalls and driver updates may reset it.
- **ShadowPlay drops the microphone selection** when the Windows Recording tab
  has fewer than four active devices. Show disabled devices and enable enough of
  them.
- **Separate audio tracks means the microphone is track 2**, and most players
  only play the first one. A clip that sounds like it contains nothing but the
  game usually contains both; check with a player that can switch audio tracks
  before concluding the microphone was not recorded.
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
