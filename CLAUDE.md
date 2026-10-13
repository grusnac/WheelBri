# CLAUDE.md

This file guides Claude when working in this repository.

## Working with the user

- After every change, finish with **how to build it** and **how to test it** (what to run, what to look for,
  what a failure looks like), even when the change needs no rebuild (say so).
- Build and test steps use the scripts in `scripts/make/` (`app.sh` builds everything and starts the app,
  `app.sh --test` also runs every test first; `rebuild.sh`, `ui.sh`, `shim.sh`, `drivers.sh`, `all.sh`, `package.sh`, `clean-pc.sh`, `measure.sh`; see `scripts/make/README.md`) as the shortcut
  next to the full commands (next rule). When a new workflow keeps needing the same commands, add a script there
  (sourcing `common.sh`) and list it in that README.
- **Every build or test step comes in two forms**: first the **full command(s)** that will run (exactly what the
  script does, so the user sees what happens), then the **shortcut**: the `scripts/make/` script that runs them.
  Example:

  Full command:

  ```bash
  cmake --build build
  ```

  Shortcut:

  ```bash
  scripts/make/app.sh --no-start
  ```

  When a script runs several commands, list each one in its own block, in order, before the shortcut.
- Put **every command in its own code block**, one command per block, so each can be copied on its own.
- The user builds and runs on Windows (`D:\cpp-projects\WheelBri`) in an MSYS2 UCRT64 shell. Give commands for
  that shell unless the step needs PowerShell (drivers), and say which shell.
- Running scripts is disabled on the user's PC (PowerShell execution policy): never give a bare `script.ps1`. Every
  PowerShell script is run as `powershell -NoProfile -ExecutionPolicy Bypass -File <script.ps1> [arguments]`.
- **Before every commit**, write down why the change was made: the commit message gets a short headline and a
  few lines of reasons.
- **Branches**: a feature with a branch of its own is committed there, never on `main`. The events loop ("push")
  was one, merged into main on 2026-10-07. The git commands for such a change start
  with switching to its branch (`git switch <branch>`, harmless when already on it) and `git status`, then
  `git add` / `git commit`. Before giving them, check which branch the PC is on (`.git/HEAD`).
- **Order of a reply after a change**: what changed, then build, test and what to check, and the git commands
  (`git add`, `git commit`, tags, push) always last, at the very end.
- **When a functionality is finished** (it may take several commits), add one entry for it under
  `## Functionalities` in README.md, in the commit that completes it, not before. Keep it short: a `###` headline,
  then **Why** (the reason), **Use** (how to use it, settings) and **Look** (where to see it: UI, logs and
  telemetry columns, code files). Ask the user whether a functionality is finished if it's unclear.
- Before guessing at a mapping or FFB problem, read the user's logs and telemetry
  (`%LOCALAPPDATA%\wheelbri\logs`: `wheelbri-service-<date>-<NN>.log`, `telemetry-<date>-<NN>.log`,
  `ffb-<date>-<NN>.log`, one file per run) and run `scripts/telemetry_summary.sh`. The `raw_a0`..`raw_a7` columns
  show which axis a control really moves.

## Project

WheelBri makes the user's rig (a wheel and its accessories, defined in the Rig tab) look like a wheel it emulates: the
Logitech G920, or the Logitech G25 for games that know it and not the G920 (the G29 profile was dropped: the same wheel
as the G920 with more buttons; WheelBri decides which targets exist, never the user), on Windows and Linux/Proton.
First target game: Need for Speed (2015).

Status (README.md has the full table):
- **Working with a real direct drive wheel in NFS 2015 on Windows**: mapping and service (Phase 1), the `dinput8.dll` shim
  (Phase 2: wheel, pedals, buttons, D-pad), game force feedback through the service (Phase 3: constant, spring,
  damper, sine, square), and the virtual G920 with our own UMDF driver (Phase 5).
- **Settled**: NFS finds wheels through `DirectInput8Create` (A and W) and reads `c_dfDIJoystick2`, pedals released
  at the top of the range. The emulated G920 (driver, shim, uhid) reports pedals as a real G920 on Windows: wheel X,
  throttle Y, brake Rz, clutch Slider (NFS Unbound reads them so). That is the default for every game; a game that
  reads the wheel differently gets a game profile (`config/games/<id>.ini`) holding only what it needs changed, laid
  over the defaults (`GameProfiles`): NFS 2015 (`NFS16.exe`) reads brake Y, clutch Rz, throttle Slider from its own
  G920 table (2026-10-09; the shim's axis audit saw no remapping call, real G920 owners report the same swap).
  A profile may also carry a steering rotation (`[steering] rotation_deg` + `note`, both or neither), applied while
  the game runs: NFS Payback (`NeedForSpeedPayback.exe`) 180, its own handling being slow with any wheel
  (2026-10-11). Core `GameRotation` (`rotationWhileRunning`: the user's `[game_rotation]` line, else the profile's;
  degrees only, an older "360/180" reads as 180). The steering axis always goes through 1:1 (no ratio since
  2026-10-13): the rotation is advice, so `GameWatch` (status, `service.game` log) tells the user to set the wheel's own
  software to it, and a game whose own settings follow the steering rotation gets it written there. A profile may also say which ways the game works with (`[game] ways` = virtual and/or shim + `ways_note`, both or
  neither; left out, both): NFS Payback is virtual only (a DirectX error with the shim). Core `wayAdvice` and
  `WayNotices` (fed by `GameWatch` from every `GameDetector` poll; a shim session is one that does not route the pedals)
  count each start of a game run the wrong way; the app (`WayNoticeFollower`) shows it once per start as a Windows
  notification (`UiWindow::notify`: Win32 notification-area balloon; none on X11) and a popup, and `GameWatch::warning`
  puts it in red next to Start. A profile may also carry a force feedback gain (`[ffb] gain` + `note`, both or neither, 0.25..4): NFS Unbound
  (`NeedForSpeedUnbound.exe`) 3, its force feedback being very light with no setting for it (2026-10-12: steering
  force 5% of full scale on average). `GameWatch::check` returns it, the loop hands it to
  `FfbController::setGameGain`, which multiplies only pushing forces (constant level, periodic magnitude and offset;
  conditions untouched) before `FfbSafetyLimiter`, so the cap and slew still hold. WheelBri never reads or sets the
  wheel's own software (no Moza SDK). The user's mapping is the same in every game. The tested wheel's axes as the service numbers them: steering 0,
  throttle 2, brake 5, clutch 6 (add-on clutch pedal, checked from telemetry).
- **Not yet verified**: the G25 target in any game (`docs/g25-target-plan.md`: built for the shim, the Windows virtual
  device and uhid; its button order other than the gears, and its axis letters, are a guess, the G920's layout);
  Linux/Proton (Phase 4 `wheelbri-uhid` is unit-tested only, not
  tried on a Steam Deck); effect envelopes and ramps in a game (implemented, unit-tested); NFS's 20-byte Logitech
  `Escape` calls are Logitech's rev lights (command 0: size 20, version 1, rpm 0..1, first light 0.01, redline 1),
  not forwarded; no axis remapping call seen (2026-10-04..07 shim logs; the axis audit checks every session).
- The G920's own FFB is HID++, not the classic Logitech protocol; the virtual G920 exposes HID PID instead.

Data flow: `rig --DirectInput/evdev--> wheelbri-service --UDP 127.0.0.1--> consumer`, where the consumer is
one of: the `dinput8.dll` shim in the game, the `wheelbri-g920` driver (Windows) or `wheelbri-uhid` (Linux). Use UDP loopback, not shared memory: under Proton
the shim runs inside Wine and the service is a native Linux process, and named mappings don't cross that boundary.

UDP ports (all 127.0.0.1):

| Port | Direction | What |
|---|---|---|
| 41329 (`state_port`) | service → consumer | `StatePacket`, and `LogLevelPacket` (8 bytes, `WELV`) from the same socket; only one consumer at a time |
| 41330 (`ffb_port`) | consumer → service | `FfbPacket` (game force feedback) |
| 41331 | test tool → service | `ClutchPacket` test clutch (`TestClutch.hpp`) |

## Build and test

CMake ≥ 3.24, C++20. spdlog 1.15.3 and GoogleTest 1.15.2 come in through FetchContent (needs internet on the first
configure). The service uses only the system's APIs and runs on Windows (DirectInput, Win32) and Linux (evdev,
Xlib; skipped with a warning without libx11-dev). No SDL or other third-party library: do not add one back.

The scripts in `scripts/make/` wrap everything below (`scripts/make/README.md`): `app.sh` (builds everything
incrementally, configures the first time, then starts the app; `--test` runs the whole test suite first and starts
nothing on a failure; `--no-start`), `rebuild.sh` (deletes `build/`, full rebuild), `ui.sh [config]`, `shim.sh [--off] <game folder>`, `drivers.sh [-Install]`, `all.sh [--clean]`, `package.sh [--no-build]` (the installer), `clean-pc.sh [-KeepData]
[-GameFolder <folder>] [-KeepThirdParty] [-Yes]` (removes everything WheelBri put on the PC, HidHide and HIDMaestro
included, for a fresh install; when the
app, installer or drivers start leaving something new on the PC, add it to `clean-pc.ps1`).
`measure.sh [-Phase name] [-Game exe] [-Seconds N]` measures WheelBri's CPU (per process and per thread), memory,
handles, GDI objects and the timer resolution from outside the app (`measure.ps1` in its own Administrator window;
M = new phase, Q = stop; one `perf-<date>-<time>.log` in the logs folder, which housekeeping leaves alone). Measure with it
rather than adding counters to the app, so the app's own use is what is seen.
The raw commands they run:

Windows, MSYS2 UCRT64, from the repo root. Configure (first time only):

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

Build core, service, shim and tests:

```bash
cmake --build build
```

All tests:

```bash
ctest --test-dir build
```

One suite (gtest_discover_tests names are `Suite.Test`):

```bash
ctest --test-dir build -R Mapper
```

Or the test binary directly:

```bash
./build/tests/wheelbri_tests --gtest_filter='Pipeline.*'
```

Outputs: `build/service/wheelbri-service.exe` (command line), `build/service/wheelbri.exe` (the app: window with
Start / Stop, runs as the user; `asInvoker`) and `build/shim/dinput8.dll`; all linked statically on MinGW.

Windows shim cross-compiled from Linux/macOS with MinGW-w64 (→ `build-win/shim/dinput8.dll`):

```bash
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake -DCMAKE_BUILD_TYPE=Release -DWE_BUILD_SERVICE=OFF -DWE_BUILD_TESTS=OFF
```

Drivers (`driver/`, see `driver/README.md`) are built with Visual Studio + WDK, not MSYS2, from a Developer
PowerShell, then installed from an Administrator PowerShell, e.g.:

```powershell
msbuild driver\wheelbri-g920.vcxproj /p:Configuration=Release /p:Platform=x64
```

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File driver\install-driver.ps1
```

(same pattern for `driver\hide\wheelbri-hide.vcxproj` + `install-hide.ps1`).

CMake options: `WE_BUILD_TESTS` (ON; skipped when cross-compiling), `WE_BUILD_SERVICE` (ON), `WE_BUILD_SHIM` (ON
only on Windows targets), `WE_BUILD_LOGI` (ON only on 64-bit Windows targets), `WE_BUILD_UHID` (ON only on Linux).
Warnings: `/W4 /permissive-` on MSVC, `-Wall -Wextra
-Wpedantic` elsewhere. Keep new code free of warnings.

Service CLI (`service/src/main.cpp`): `wheelbri-service [--config FILE] [--list] [--print] [--ui] [--ffb-test]
[--old-files delete|archive|keep] [--stop-file FILE] [--virtual-g920 [--no-hide]]
[--unhide] [--prepare-hide] [--app] [--telemetry-tab] [--ffb-tab]`. `--app` opens the app's window (Start / Stop) from a console. `--ui` opens a window (`service/src/MappingView.cpp`, also the app's live view) with tabs along the bottom
(click, or R / M / G / L, T / F; in this order: Rig, Mapping, Game profiles (the app only, `MappingView::setGamesTab`),
Logs, then Telemetry and Force feedback only with `--telemetry-tab` / `--ffb-tab`, never shown without them,
`MappingView::setOptionalTabs`): **Mapping** (the axes rig -> G920, every rig axis, the D-pad, the G920 buttons written
`G920 <n> <name> <- Rig <n> <name>` and the wheel's own buttons `Rig <n> <name>` from `[button_labels]`), **Logs** (one
line per component: emulator, game shim, G920 driver, hide filter, helper, each in its own colour,
with today's file, size, age, click opens its folder; the helper's line says what `HelperProbe` last heard from the
WheelBri Helper (core `describeHelper`) and turns to the warning colour when today's helper log has warnings or errors
(core `LogLevelCounter`); then a scrollable live log of all those files merged:
wheel, arrows, PageUp / PageDown, Home / End, a draggable vertical scroll bar, a sideways one for long messages
(Shift + wheel, Left / Right) and word wrap (Wrap button or W); drags come from `WindowInput::pointer` / `leftDown`,
`MappingView::onPointer`; core `LiveLog`), **Telemetry** (the telemetry files, buttons held, the
latest button / D-pad / pedal / FFB events, not scrollable), **Force feedback** (live effects, usage since
start, the latest effects), **Rig** (R, `WheelSetupView`: defines the rig, see `Rig.hpp`; the app shows it
when stopped too, reading every connected device itself) and **Games** (G, the app only: `MappingEditView`'s Games page,
stopped and running; see the game profiles below). The app has no log panel or toolbar: every tab (stopped and running) has, at the right end
of the tab bar, a short status and then Start and Stop, as tall as
the tab buttons (which are as wide as their labels, `MappingView::tabWidth`, so five tabs leave room for them). Stopped, Start is greyed out until
every rig device the saved mapping uses is connected (core `StartNeeds`: the wheel and the accessories the mapping
reads; "NO RIG" without a rig, "NO MAPPING" when not one of the target's controls comes from a device of the rig,
`mappingUsesRig`; those two come first in the status; `service/src/DeviceWatch.cpp` lists the game controllers on its own thread when Windows says one came or went: `platform::DeviceChanges`, `CM_Register_Notification` on HID interfaces / inotify on `/dev/input`, counted by core `ChangeCounter`, timed by core `RelistTimer`; `DeviceWatch` takes its lister, counter and timings as arguments (`systemLister()`, `systemOptions()`) so `DeviceWatchTest` drives it with a fake lister; `platform::FolderWatch` is the inotify watch, Linux only), with a listing every 30 s as a safety net and every 2 s where there are no notifications; the Rig tab and the session's wait for the wheel use the same), and the status says
"NOT CONNECTED: <names>" or "NO RIG" in red. `WheelBackend::list()` on Windows makes a new DirectInput object each
time, so a wheel switched on after the app started is found. Stopped, the app's own tabs are Mapping (`MappingEditView`,
the editor, with `ConfigPicker` as its top row; also the STEERING ROTATION section: one rotation
(`target_rotation_deg`), typed or by slider, ranges and formatting in core `AxisTuning`, with the reminder to set the
wheel's own software to the same (the axis goes through 1:1; `source_rotation_deg` is retired since 2026-10-13). There are no dead zones anywhere in
WheelBri (since 2026-10-13: the devices' own software sets them): the mapper uses steering as reported and pedals as
0-100% of their travel (`gamma` stays); older configs' `deadzone`, `deadzone_low`, `saturation_high` and older rigs'
`deadzone N%` / `full N%` load and are ignored (`SectionReader::retired`, `parseControl`), and `writeMapping` /
`writeRig` drop them; do not add them back. There are no roles and no Auto-map (since 2026-10-13: every wheel is
different, so WheelBri does not guess what a control is for; the user maps everything by hand); do not add them back
either. Each target button has one role assignment
(`buttonMap`) and up to `kMaxSecondaries` (8) secondaries, `MapperConfig::buttonSecondary` / `[buttons] target_N_or`
(a comma-separated list): buttons, or axes past a threshold (a pedal or lever makes sense; the picker offers every
named axis), each axis with its own 5% release gap in `Mapper::mapButtons`. A rig control is used once in a mapping: core `usesOf` /
`describeUse` / `controlsUsedTwice` (MappingEdit.hpp; only the target's named buttons count); both pickers grey a
used control with a hover tooltip (`UiInput::pointer` / `pointerInside`, `MappingEditView::setHover`, `tip`,
`drawTooltip`), Save refuses a control used twice and Start warns), Rig and Logs (a `MappingView` of its own, `logs_`, drawing only its
Logs page), in that order (there is no Status tab; a failed Start says "Start failed (Logs)");
it opens on Rig until a rig exists, else on Mapping; Start switches to Logs at once (the session logs while it comes
up) and the live view keeps the tab showing then. O opens the logs folder (every log is there). It draws through
`UiWindow.hpp` (filled/outlined rectangles, 8x8-cell monospaced text and clicks on a logical canvas of at least
960x748, `fitCanvas`: the design scales with the window up to `kMaxZoom` (1.5) times the display scale, and whatever
the window has beyond that shape or size is room on the canvas, no bars; the window is resizable from `kMinZoom`
(0.75) times the design up to the screen's work area). Views read `UiWindow::width()` / `height()` every frame and
lay out from them: full-width rows, bars, cells, fields and picker columns grow (pickers 5..8 columns), buttons at
the right edge stay there (Start / Stop, Save / Revert, Detect), the tab bar stays at the bottom, lists (live log,
events) get more rows, popups are centred. Code that places something must use those, never 960 or 748 (the design
constants `MappingView::kWidth` / `kHeight` are the minimum only); `UiCanvasTest` covers `fitCanvas`. Implemented
natively: `UiWindowWin32.cpp` (Win32 + GDI, DPI aware; records each frame as a core
`DrawList` and draws it only when it differs from the one on screen) on Windows, `UiWindowX11.cpp`
(Xlib) on Linux.

The app (`wheelbri`, `service/src/App.cpp`, same sources plus `WHEELBRI_APP`; on Windows a GUI-subsystem exe
with an `asInvoker` manifest (a standard user; the virtual device goes through the WheelBri Helper, or directly when
started as Administrator; the status says "HELPER NOT INSTALLED" in red and Start is greyed out when neither), `service/app/`: the `.rc` and manifest are `.in` templates CMake fills with the
project's version; both exes carry the icon `wheelbri.ico`, made from `wheelbri.svg`, "wheel going digital", resource
1, which `UiWindowWin32` also uses for the window) runs sessions (`runSession` in `main.cpp`) on a thread of its
own from Start / Stop buttons; sessions show themselves through `UiHost` (`SessionUi.hpp`): the app's window,
or `makeSessionWindow()` for `--ui`. `MappingView` only draws into a window it does not own. The window is drawn
60 times a second (`kWindowFramesPerSecond` in `WakePlanner.hpp`; at once after input), on a drift-free schedule: the
user found 30 laggy (2026-10-08). The session loop hands the window its state as often (`WakeSettings::windowNs`).
Every tab shows the build in its top-right corner (`Version.hpp`: `WHEELBRI_VERSION` and `WHEELBRI_BUILD`, the git
commit with `+` for uncommitted changes, from `build/service/generated/BuildInfo.hpp`, which `cmake/BuildInfo.cmake`
rewrites on every build only when it changes); views keep their own top-row controls left of `kVersionReserve`.

## Layout and architecture

- `core/` (`wheelbri_core`, static library): portable, **no OS headers and no SDL**. The one exception is
  `UdpLoopback.cpp`, which has `#ifdef _WIN32` socket code. Headers live in `core/include/wheelbri/`,
  namespace `wheelbri`.
  - `RawInputState` (8 axes in [-1,1] exactly as the OS reports them) → `Mapper` → `WheelState`
    (target-agnostic: steering -1..1 over the *target* rotation, pedals 0..1) → `IDeviceProfile::encode`
    → `TargetReport` (the emulated device's own ranges) → `StatePacket` → `IStatePublisher`.
  - `Pipeline::tick()` runs one pass of that chain (optionally with a clutch override, see `TestClutch`). The service
    calls `limitSending` (`Pacing.hpp`, `StateSendGate`): a packet only when the state changed, at most `state_rate_hz`
    (default 500) a second, and a keep-alive every 100 ms (receivers call 1500 ms stale); `sequence()` counts packets
    sent.
    `IInputSource` / `IFfbSink` (`Devices.hpp`, `kMaxSlots = 8` effect slots) and `IStatePublisher` /
    `IStateReceiver` are the seams for tests and platform code.
  - New emulated wheels are new `DeviceProfileSpec`s (or `IDeviceProfile` implementations), registered in
    `makeProfile()`. `DeviceMatch` matches `source_device` against a name or `vid:pid`.
  - `FfbEffect` is the common FFB model. Every source is translated into it (`DiEffect` from DirectInput
    parameters, `HidPid` from HID PID reports) and `FfbSafetyLimiter` is the last stage before hardware. The
    limiter holds state (slew), so use one limiter per played effect. `FfbController` holds the limiters and
    the watchdog; `FfbUsage` summarises per-effect use for the UI and log; `WheelOutput` converts effects to
    DirectInput/evdev units. The service plays on the wheel through `AsyncFfbSink`, a thread of its own at the loop's
    raised priority (the newest command per slot; `stopAll` first, then stops, constant, periodic, conditions, and
    anything that waited `kMaxWait` = 20 ms; a refused play is answered by the next `play` on that slot): a DirectInput
    call takes 4-15 ms on the R3 (27 ms to start an effect) and must not hold the loop. Every call is measured
    (`Applied`: wait, call), drained by the loop into `FfbDelayStats` (the log's `ffb delay` table) and the ffb log's
    `kind=wheel_*` lines (`wait_us`, `call_us`; `telemetry_summary.sh` reads them). Envelopes (attack / fade) and ramps are worked out by the senders, which know each
    effect's timing: `FfbShape.hpp` (header only, the driver compiles `HidPid.cpp` without the rest of core) and
    `shapeAt()`, called by the shim's `FfbForwarder` and by `HidPid` on every tick; the wire and the service only see
    levels.
  - The G920 has 19 buttons (`kG920Buttons`, `g920Spec().buttonNames`, 0-based): the wheel's 11 (0-10), an unused 11
    (joy.cpl 12) and the Driving Force Shifter's Gear 1-6 (12-17) and Gear R (18), as Logitech's shifter FAQ gives them
    (joy.cpl 13-19). Every target (shim, virtual device, uhid) reports all 19; `IDeviceProfile::buttonCount` counts
    the gap, and `encode` only passes named buttons.
  - `HidG920` is the virtual G920's HID descriptor and input report (from HIDMaestro's builder, MIT), the virtual
    G25's too: `VirtualWheel` (`virtualWheels()`, `virtualWheel("g25")`, `virtualWheelForHardwareId`) holds each
    wheel's identity (hardware id `wheelbri\G920` / `wheelbri\G25`, instance names, 046D:C262 / C299, product string,
    DirectInput name, button count) and `isVirtualWheelId` keeps both out of hiding;
    `HidPid` is device-side HID PID decoding. Both are shared by `driver/`, `tools/uhid` and the tests. pid.dll sends
    type-specific parameters with effect block 0 and then the Set Effect naming the block: `HidPid` keeps them pending
    for that Set Effect and never applies them early (Windows sends reports twice, a new effect's parameters before
    its first Set Effect, and Effect Operations in between; early guesses mixed up NFS's spring and damper). Unclaimed
    ones (a game that streams parameters with no Set Effect, as NFS Unbound does every 16 ms) are applied `kPendingMs`
    after the first one waited (a newer one replaces it without restarting the wait); a run of several of one kind
    goes to the blocks of that kind whose values each continues (`applyUnclaimed`, nearest values, ties by block
    number), and an exact repeat within `kCopyMs` is Windows' copy, ignored. A game that streamed (`kStreamMinMs`)
    and goes quiet for `kStreamQuietMs` while a pushing force plays (any above `kStreamPushingAbove`) stops the
    keep-alives (the service's watchdog then stops the forces); springs and dampers alone stay; the 10 s rule
    (`kGameQuietMs`, a felt force) covers games that set effects once. `AnyReportsAreSafe` throws random reports at
    it. A trace (`HidPid::enableTrace`, off by default) writes
    the next N reports and the slot changes they cause to the driver's log (`pid trace`), for checking the real order:
    an Administrator sets `HKLM\SOFTWARE\wheelbri PidTraceLines` (DWORD, e.g. 1000), read at each start of the device.
    One `pid:` line per effect event at debug (new effect or refused, a Set Effect that changes type, duration or
    delay, start / start solo / stop with why a start cannot play yet, free, every device control, a gain change),
    at most `kMaxEventLines` (100) per `kEventWindowMs` (10 s) and the rest counted. That says whether the game asked
    to start its effects (set the app's Level to debug first). `HidPid::Logger` takes each line's `LogLevel`: trace
    lines at trace, what goes wrong at warn (refused effect, unknown operation or device control, a block it never
    handed out, the game gone quiet).
  - `LiveLog`: follows the components' log files (tail on first read, then what is appended) and parses
    spdlog and `[time] text` lines for the Logs tab; `LiveLogTest` covers it with temporary files.
  - Game profiles (`GameProfiles`), the exception and not the rule. The defaults (the target's own layout:
    `layoutFor`, a real G920's throttle Y, brake Rz, clutch Slider) are the base for every game. A game that needs
    something else gets `config/games/<id>.ini` holding ONLY what it changes: `[game]` label / program (file name) /
    target (the target it was checked with), `[pedals]` throttle_axis / brake_axis / clutch_axis (x y z rx ry rz slider
    none), only the keys that differ. It is laid over the defaults like a sieve (`overlay`: a key set replaces the
    default, a key left out keeps it). The files are compiled in: core's CMakeLists.txt embeds every
    `config/games/*.ini` into a generated `GameProfileFiles.inc` (configure time, reconfigured when a file changes;
    `)wbgame` may not appear in a file); `parseGameProfile` collects every problem and a file with any is not used
    (`gameProfileProblems()`, logged by the service and the shim; `GameProfilesTest` fails on any). A game without a
    file is never named (nothing in the log or the window). `nfs2015.ini` (brake y, clutch rz, throttle slider),
    `nfspayback.ini` (`[steering]` 180) and `nfsunbound.ini` (`[ffb]` gain 3, `[game_settings]`) exist, all checked
    with `g920` only. A profile may set the game's own settings (`[game_settings]` file / note / `<id> = <game's name> <0..1 | steering>`):
    core `GameSettings` (`gameSettingTexts`: the user's `[game_settings] <game>.<id>` value, else the profile's;
    "steering" = the game's steering rotation; `editFrostbiteSettings`: Frostbite `FBCHUNKS` files, each name found
    by its own entry's length fields, exactly once, only same-length values written, any problem = nothing written;
    `writeGameSettingsFile`: the file under the Documents folder, no link followed (file, backup, folders: the service
    may run as Administrator), backup `<file>.before-wheelbri` written once from the bytes read, outcomes Written /
    AsSet / NoFile / Refused / Failed; `GameSettingsTest` covers it with temporary folders and damaged files).
    Nothing writes them by itself (since 2026-10-13; `GameWatch` no longer does): the Game profiles tab's **Save to
    file** (`MappingEditView::saveGameFile`, refused while the game's program runs, `platform::runningPrograms`) writes
    the values shown; **Locate file** (`locateGameFile`, `platform::pickFile`: GetOpenFileNameW, comdlg32; none on
    Linux, `canPickFile`) picks the file when it is not under Documents, kept only if it holds every setting, in
    `app.ini` as `game_file.<id>` (`gameFilePlace`: that file's folder as root, else Documents). The
    app's Game profiles tab edits them: `MappingEditView` with `Page::Games` (the same `MappingModel`, Save and Revert
    as the Mapping tab, which keeps only the general rotation; no target shown, the target is the Mapping tab's): a
    list of `gameProfiles()`, and for the one picked three sections, MAPPING (the profile's pedal overlay, its rotation
    row), FORCE FEEDBACK (its gain row), GAME CONFIG (its settings rows; left out when the profile has none): its rotation row
    (`Field::PerGame`, `[game_rotation]`, for every game; a game with a "steering" setting, `hasSteeringSetting`, gets
    that rotation written), its gain row (`Field::FfbGain`, `[game_ffb_gain] <game> = 0.25..4`, `gameFfbGain`: the
    user's, else the profile's, else 1; `GameWatch::check` returns it for `FfbController::setGameGain`) and its settings
    rows (`Field::GameSetting`), with what the file holds now (core `readGameSettingsState`: read-only, the writer's
    path and link checks, read every 2 s) against the values shown. While a session runs the page is locked and shows
    the game that runs from `UiFrame`'s game fields (`GameWatch::live`); the app (`updateGames`)
    switches the live view to the Game profiles tab and picks the game when `UiFrame::gameStarts` changes with a game
    followed (core `GameTabFollower`: once per start, reset at every Start; not while a name is typed in the Rig tab or
    a popup is open). The pedals: both ways use the same file:
    - virtual G920 / wheelbri-uhid: the service routes the published pedals (`routePedals(device, overlay(device,
      profile))` → `PedalRoute`, `Pipeline::setPedalRoute`, atomic, applied to the packet only; `lastReport()` and
      telemetry stay as mapped). The device's pedal axes are fixed (y, rz, slider), so a profile that is not a
      permutation of them is refused for it (logged once, pedals left).
    - in-game shim: the shim knows its game (`GetModuleFileNameW`), and `ShimConfig::layout()` is the sieve of three
      layers: the target's defaults, `wheelbri-shim.ini`'s *_axis, the game profile (if it fits the shim's target and
      the result is valid). The service then routes nothing (`runLoop(..., routePedals = virtualG920 || uhid)`).
    `GameDetector` (portable, every branch in `GameDetectorTest`, profiles passed in so tests cover two games) decides
    which profiled game the service follows: at once on start; dropped only after 2 misses in a row (a restart under
    a new pid keeps it, `Restarted`); with two profiled games the followed one stays (none followed: profile order),
    the others are reported; a failed program listing changes nothing (`ListFailed`/`ListBack`, logged only while a
    profile is in use); a profile for another target is named in the log but not used. Matching is by exact program
    file name, ASCII-lowered, from a name or a Windows/Unix path. The service's `GameWatch` (window thread, which runs
    with or without a window, every 2 s; never the loop) polls `platform::runningPrograms()` (Toolhelp; Linux
    `/proc/*/cmdline` first word, the next one after a wine loader; nullopt when it cannot list), logs under
    `service.game` with the game's full path and what the profile changes ("game: NFS 2015 started (pid …,
    D:\…\NFS16.exe): game profile nfs2015 laid over the defaults (throttle_axis = slider, …)"), and gives the app
    "Running 1:23, NFS 2015" (the profile's `label`). `Nfs2015Settings` reads NFS 2015's own wheel bindings
    (`Documents\Need For Speed\settings\PROFILEOPTIONS_profile`: `GstInput.CustomControlsWheel`, the type-3 slot of
    `ConceptMoveFB` / `ConceptBrake`, wheel inputs 24 Throttle, 25 Brake) whenever they change, silently, and only
    warns ("NFS 2015: RESET ITS WHEEL CONTROLS", red) when an old in-game workaround would undo the profile.
  - `GameUseWatch`: pedals pressed for 20 s in total while the game never sent force feedback = the game drives from
    another controller (a gamepad or the keyboard); the loop logs a warning and the app's status (next to Start / Stop) says
    "GAME NOT USING THE WHEEL" (`UiFrame::gameIgnoresWheel`).
  - `FfbIdleWatch`: the game has lasting effects loaded (constant, spring, damper, friction, inertia; `FfbController`
    `lastingLoaded()` / `lastingWanted()`, a slot is loaded from its first `SlotState` until a StopAll or the watchdog)
    but wants none playing, and the pedals were pressed for 10 s in total = it forgot to start them (NFS 2015 at the
    start of some races; an alt-tab makes it start them). The loop logs a warning, the status says
    "GAME FFB OFF: ALT-TAB" (`UiFrame::gameFfbIdle`). Short effects (sine, square, ...) do not count.
  - No virtual Xbox controller (removed 2026-10-03, with the HIDMaestro bridge): with an XInput pad there at the game's
    start NFS drove from the pad, the pad's buttons acted as its own (its right stick click is NFS's photo), and stick
    nudges did not change NFS's keyboard prompts. Do not add one back without asking. `HidHideCli` holds device-id
    string helpers (`HideFilter` uses them; HidHide itself is no longer used).
  - `TestClutch`: a test clutch for when no pedal is connected. A test tool sends `ClutchPacket`s (12 bytes, magic
    `WECL`, v1; `ClutchPacket::encode`) to 41331; the service uses that value instead of the mapped clutch until 1.5 s
    after the last packet. (Its C# sender, ClutchProbe, went with the bridge.)
  - The rig (`Rig.hpp`): every device the user has (`[device_N]` name, id, kind: guessed from the name, no longer
    shown or chosen in the Rig tab; `Rig::wheel()` is the first device of kind wheel, else the first) and each
    control's name and invert, written `label | invert`. No roles (since 2026-10-13): the popup's "It is" buttons
    (`kAxisNames`, `kButtonNames`) only fill in the name; an older rig's `| role` is read and ignored (`kOlderRoles`:
    a role-only control gets the role's name, a steering axis makes its device kind wheel) and not written again;
    one rig, `<name>.ini` in `rigsFolder()` (`%LOCALAPPDATA%\wheelbri\rigs`, "My rig" when first saved), named in
    the app's state file (`[app] rig`; else the first file there). No rig selector, New or Rename: other rig files
    are left alone, unused. The Rig tab (`WheelSetupView`) writes it: no device selector, every rig device (an unplugged
    one as saved: its named controls, `tileCount`) and every other connected device shown at once (one scrolled
    section each, `shownDevices`; the bottom-left list `listedDevices`, with Forget); a device joins the rig when one of its controls is named (`addFocusDevice`: the rig's devices
    are what the hide filter hides, so connected devices are never added by themselves). Detect: a touched control on
    any device opens its popup and scrolls to its section; the rest position taken after ~20 reads gives a pedal's
    invert; the Mapping tab maps from it, by hand. A session takes the names it shows from the active rig, else from the older
    per-device files (`DeviceLabels.hpp`, `deviceLabelsFolder()`), which the first rig also imports. Mapping is not
    done in the rig.
  - Several input devices: device 0 is the wheel (`source_device`; empty, as in the built-in `config/g920-*.ini`, = the active rig's wheel, `Rig::wheel()`, filled in by `loadForRun`; plays FFB), 1..3 are accessories
    (`MapperConfig::accessories`, `DeviceId` vid:pid). Every source has a device next to its index
    (`sourceDevice`, `buttonDevice[]`, `HatButtons::devices`); in the INI a source is `N` (the wheel's) or `vid:pid/N`
    (`MappingEdit.hpp`: `parseSource` / `formatSource`; `readSource` in `EmulatorConfig.cpp` fills the accessories in
    reading order). `RawInputs` holds one `RawInputState` per device; `Mapper::map(RawInputs)`, `Pipeline::
    setAccessories` (a lost accessory ends the tick like a lost wheel, `lostDevice()`); telemetry `raw_*` columns and
    the Wheel setup tab stay the wheel's. `MappingModel` / `writeMapping` are what the Mapping tab edits and saves
    (IniEditor; default button lines removed). The service opens accessories by id (`openAccessories` in main.cpp).
  - The config Start uses (`service/src/ConfigPicker.cpp`, the top row of the idle Mapping tab): two pickers with
    `<` `>`, Target (`targetName()`: G920 or G25; `profileName()` "g920" / "g25") and Run as (`wayName()`: Virtual
    or Shim; in the log "G25 virtual device" / "G920 in-game shim", `name()`) (`[app] target`, `[app] way` in
    `appStateFile()`, `app.ini`; `AppSettings::runSession(host, config, virtualDevice)`). Two layers: the **target**
    is fixed (the profile's axes, D-pad and buttons; never the user's to change), the **mapping** of the rig onto
    it is the user's. Each target and way has a built-in config, never changed, holding WheelBri's default mapping (the
    user's corrected mapping of 2026-10-04): `config/g920-virtual-device.ini` (also `defaultConfigPath()`, the command
    line's default), `config/g920-in-game-shim.ini`, `config/g25-virtual-device.ini`, `config/g25-in-game-shim.ini`
    (`builtInName(target, way)`). With Run as Shim, Start writes the target into the game's `wheelbri-shim.ini`
    (`setShimTarget` in core `ShimGames`, the user's other lines kept). The Mapping tab's Save writes the user's own as
    `userConfigFolder()/my-<built-in name>` (`%LOCALAPPDATA%\wheelbri\configs`), used instead; "Default" deletes it.
    The rig no longer rewrites a mapping (no rigMapping); it gives the sources' names, the wheel when `source_device`
    is empty. A source the active rig has not named shows "not in the rig" (`MappingEditView::missing`).
  - `ShimGames.hpp`: the games the app put the in-game shim into (`shimGamesFile()`:
    `%LOCALAPPDATA%\wheelbri\shim-games.txt`, per user like every setting). The file: tab-separated
    `game name folder` / `selected folder`, UTF-8 paths; not IniDocument, which cuts at `;` and `#`), `installShim` /
    `removeShim` / `shimState` (Installed, Outdated, Missing, Foreign: a `dinput8.dll` that is neither byte-identical to
    ours nor next to a `wheelbri-shim.ini` is another mod's and never touched), `gameFolderProblem` (needs an `.exe`).
    `ShimGamesTest` covers it with temporary folders. UI: `service/src/ShimGamesView.cpp`, only with the target G920
    in-game shim: "Game <name> [Games (N)]" next to Start, the games panel (pick, Add game through
    `UiWindow::pickFolder`: IFileOpenDialog on Windows, `/usr/bin/kdialog` or `/usr/bin/zenity` on X11; Put / Remove shim; Forget = `removeShimGame`, list only), and the App's popups: "Would
    you like to delete the shim from the target folder?" after a shim session (not when the window closes), notices.
    The shim's source is `AppSettings::shimDll` (`shim\dinput8.dll` next to the program, or `build/shim/`) and
    `shimSettings` (`config/wheelbri-shim.ini`).
  - A shim session starts only what the shim needs (README, "What Start runs"): no virtual G920, no hide filter, no
    Administrator check (all keyed on `Options::virtualG920`), no test clutch listener (`runLoop(...,
    listenTestClutch)`), and the Logs tab leaves out the driver rows (`MappingView::setShimMode`). The stale hide
    check (`HideFilter::clearStale`) still runs: a leftover hide would keep the rig from the game. Keep this split
    when adding components: anything for the virtual device only stays off with the shim. `ConfigLibrary.hpp` (listing, `; made from:` templates) is older and only reads what
    earlier versions left. `MappingEditView` picks sources from the rig's controls.
  - Config: `IniDocument` (minimal INI; `;`/`#` start a comment anywhere on a line) → `loadEmulatorConfig`,
    which collects *all* errors (unknown keys and sections, out-of-range values) instead of failing fast. When
    you add a config key, add it to the reader, `EmulatorConfig`, both `config/g920-*.ini` and `EmulatorConfigTest`.
    A key written twice: the reader takes the last copy, and `IniEditor` edits that one (and drops the others).
    A value that comes from outside (a device's name from Windows or evdev) goes through `iniSafeValue` when
    written, so `;`, `#` or a line break cannot cut it or add lines.
  - `EmulatedJoystick` is the emulated wheel as DirectInput sees it (objects, the game's data format,
    ranges, state bytes, buffered changes). It mirrors DirectInput constants without `<dinput.h>` so it is
    unit-tested on Linux; `shim/src/DiConvert.hpp` static_asserts the constants against the real macros.
  - `TelemetryRecorder` (`Telemetry.hpp`): log files per run in the log folder (`telemetry-YYYY-MM-DD-NN.log`, a row when
    something changes, at most `rate_hz` = 50/s, plus a heartbeat every `heartbeat_s` = 60; `ffb-YYYY-MM-DD-NN.log`
    one line per game FFB change and per call to the wheel), in the other logs' line format (`[date time] [info ]
    [telemetry     ] t_ms=... key=value ...`; values that do not apply are left out), each session appended after a
    `session started` line and its header lines (every session of one run in the same file), rolled over at local
    midnight. `telemetryColumns()` lists the keys
    in order; when you add one, write it in `writeSample` and `telemetry_summary.sh` reads it by name; bump
    `kFormatVersion` (`Telemetry.cpp`, now "3": log lines; "2" was CSV) when a key changes meaning.
  - **Every file WheelBri writes is a log** in that line format: no CSV, no JSON (the user's rule, 2026-10-06), the
    developer tools' output too (`measure.ps1` writes `perf-<date>-<time>.log`).
  - **One log file per run** (the user's choice, 2026-10-08): `<stem>-<date>-<NN>.log`, NN = the run's number that
    day (the next free one, created exclusively), and a new file at local midnight. Core `LogFileNames.hpp` (header
    only: names, `nextIndex`, `newestOf`; the drivers use it too) and `LogFiles.hpp` (`runFile`: this run's file,
    the app's telemetry/ffb take the app's number; `newestFile`). `logging::init` names its file through `runFile`
    (`wheelbri-service-YYYY-MM-DD-NN.log`); the shim logs to
    `%LOCALAPPDATA%\wheelbri\logs\wheelbri-shim-YYYY-MM-DD-NN.log` (under Wine/Proton the Linux user's
    `~/.local/state/wheelbri` and `~/.config/wheelbri/app.ini` instead of the game prefix's, through Wine's
    `wine_get_dos_file_name` and `WINEHOME`: `wineUserFolder` in `ShimLogging.cpp`); the drivers (`openRunFile` in each Driver.cpp) and
    the helper (`HelperLog` `UserFile`) choose their number the same way. The installers' transcripts stay daily.
    The Logs tab shows each component's newest file. **One folder for every log**, the user's
    (`%LOCALAPPDATA%\wheelbri\logs`; Linux `~/.local/state/wheelbri`): app, shim, telemetry, both drivers, installer,
    hide installer. The drivers run as LocalService: `shareLogFolder` (`ServiceLogging.hpp`, at every start as
    Administrator) and `install.ps1` / `install-hide.ps1` let LocalService write there and set `HKLM\SOFTWARE\wheelbri
    LogFolder`, which the drivers read per line (fallback `C:\ProgramData\wheelbri\logs`, moved in at the next start).
    The drivers refuse a log folder reached through a link by comparing the path Windows opened it under
    (`GetFinalPathNameByHandleW`) with the one asked for: LocalService may not read the profile folders above it, so
    checking each folder's attributes sent every line to the fallback folder.
    Do not add another log location. `LocalDate.hpp` has the date helpers.
  - `Retention.hpp` decides which files are old (date in the name, else last-write day) and only ever touches
    files starting `wheelbri-service`, `-shim`, `-logi`, `-bridge`, `-logs-`, `-driver`, `-hide`, `-install`, `-uhid`, `-helper`, `-sh`,
    `telemetry-`, `ffb-`.
    The service's `Housekeeping` deletes them (default, after 2 days) or archives them with the best compressor
    found (7z > xz > zstd > zip > gzip) as a background process via `platform::ChildProcess`, deleting
    originals only after the compressor exited 0. Running as Administrator it ignores PATH (any program of the user
    can write there) and only uses 7-Zip in Program Files (`SHGetKnownFolderPath`) and `tar.exe` in System32.
  - `FileText.hpp`: UTF-8 path conversion (`pathToUtf8`, `pathFromUtf8`, which refuses invalid UTF-8), `readFileText`
    (regular files only), `writeFileReplacing` (writes `<file>.tmp`, then renames it over: never half a file, never a
    file in place of a folder), `forEachEntry` (a folder listing that never throws), `truncateUtf8`. Use them for the
    user's files rather than raw streams.
  - `HelperProtocol.hpp` (namespace `wheelbri::helper`): the app <-> WheelBri Helper pipe messages (the helper
    service that lets the app run as a standard user): a fixed list of
    requests, their encoding and the rules each is checked against (`requestProblem`, `hideDeviceProblem`,
    `isInstalledApp`, `isWheelBriProgram`). Every new request goes there, with its checks and `HelperProtocolTest`.
    `HelperDispatch.hpp` (`answerMessage`, `mayTakeTheLog`): how the helper answers one message, portable and tested.
    `HelperStatus.hpp`: what a Hello's outcome means for the Logs tab. `DailyLogFile.hpp`: a daily log with no file
    yet (the helper's). `LogLevelCounter.hpp`: warnings and errors in one log file.
  - `SenderGate.hpp`: who may send to the UDP ports. The receiver looks up the process that owns the sender's port
    (Windows: `platform::udpPortOwner` / `processImage` in the service, the same through `iphlpapi.dll` loaded on
    first use in the shim) once per new port, caches the verdict (2 s accepted, 10 s refused) and caps lookups at 16
    a second. The service takes game force feedback only from `WUDFHost.exe` in System32 or a program whose folder
    holds our shim (unknown, or a program Windows will not name: let through, warned; `processImage` falls back to
    `NtQuerySystemInformation(SystemProcessIdInformation)` when the process cannot be opened, as WUDFHost.exe after a
    fresh install and restart); the shim takes the wheel's state only from `wheelbri.exe` /
    `wheelbri-service.exe` (unknown, e.g. the native service under Proton: let through). Linux: no check.
  - Wire format (`StatePacket.hpp`): 33 bytes, little-endian, magic `WEMU`, version 1. Any layout change
    must bump `kVersion` and update the layout comment, `StatePacketTest` and the driver.
- `service/` (`wheelbri-service`, namespace `wheelbri::service`):
  - The wheel behind `Wheel.hpp` (`WheelDinput.cpp` DirectInput + `DinputReader`, `WheelEvdev.cpp` evdev),
    numbering inputs as the service always has (`DinputReader`, core `EvdevLayout`) and sending the same force
    feedback numbers as before (core `WheelOutput`).
  - `Privileged.hpp` (library `wheelbri_privileged`, with `HideFilter`, `VirtualG920`, `DinputDevice` and the platform
    file): every step that needs Administrator, with explicit inputs (nothing read from the user's settings):
    `findDriverScript` (next to the program and up to N folders above, never a drive's root), `runDriverScript`,
    `installG920Driver`, `putHideFilter` (install-hide.ps1, then `HideFilter::waitUntilStarted`), `writeAllowedPid`,
    `allowOnly(process)`. The app calls them directly when elevated (`ensureG920Driver`, `ensureHideFilter` in
    main.cpp); the helper calls them for a standard-user app. New privileged steps go there, and into the helper.
  - `PrivilegedSteps.hpp`: the session's privileged steps as one interface, opened once per session
    (`PrivilegedSteps::open`): `DirectSteps` when the process is elevated (calls `privileged::` itself), else
    `HelperSteps` through the WheelBri Helper (`HelperClient`, one connection for the session, the server must be
    `wheelbri-helper.exe` next to this program; the user's HKCU DirectInput entries are kept here by
    `DirectInputUserEntries`). Neither: the session fails with "HELPER NOT INSTALLED" / "HELPER REFUSED". `runSession`,
    `ensureHideFilter`, `ensureG920Driver` and `--prepare-hide` only go through it.
  - OS helpers in `Platform.hpp` (clock, sleep (a high-resolution timer on Windows), thread priority, child process in
    a kill-on-close job, `SingleInstance` lock so only one session runs per computer, console close handler,
    `isElevated`); `FfbSelfTest` (`--ffb-test`); `Housekeeping` (retention, pumped from the loop); `MappingView`
    (`--ui`). **The loop** (one kind since 2026-10-07; the fixed 1 kHz loop, `loop_mode` / `loop_hz`, is gone: those
    keys are read as retired and ignored, so old configs still load) sleeps in `platform::WaitSet` (Windows:
    WaitForMultipleObjects on the devices' DirectInput notifications, `DinputReader::notification` /
    `Wheel::waitSource`, the sockets' events, WSAEventSelect, and a high-resolution timer; Linux: poll on the evdev and
    socket descriptors) until the wheel, an accessory, the game's force feedback or the test clutch has something new,
    and wakes by itself only for what must run on time (core `WakePlanner`: every 4 ms while forces play and for 100 ms
    after a pass that found new input, 60 a second with a window, else 100 ms; never two passes closer than 0.5 ms).
    Its decisions (when to wake, what woke it, when to poll) are core `EventLoopControl`, which runLoop calls around
    each pass and wait. **Polling** (`EventLoopControl::polling()`, `PollReason`) is its fallback, never a separate
    loop: a device DirectInput only polls or a socket the wait cannot take (at setup), or the timer finding new input
    20 times within 10 s that no notification announced, not even within 1 ms after (late ones are counted apart;
    `Pipeline::inputChanged`), make it also wake every `pollNs` (1 ms) for the rest of the session while it still waits
    for everything else; a failed wait (`WaitSet::kFailed`) makes it sleep instead of waiting (`canWait()`). Each is
    logged once. Once a minute the "loop:" line gives the wake-ups by reason (`WakeStats`), events or polling, and the
    missed and late notifications; a wake-up 50 ms after its deadline and a pass of 50 ms or more are warned about.
    Anything on the loop that must happen regularly goes by the clock, never by counting passes (the window's copy,
    `--print`, a lost wheel: 0.5 s of failed reads). Plan and measurements: `docs/push-input-plan.md`,
    `docs/cpu-measurements.md`.
  - Phase 5 (`--virtual-g920`, run as Administrator, no shim in the game): `VirtualG920`
    creates the `wheelbri\G920` software device for the `wheelbri-g920` driver while the service runs (Start first
    installs that driver with `install-driver.ps1` when Windows' driver store has no `wheelbri-g920.inf`,
    `ensureG920Driver` / `VirtualG920::driverState`; without it `create` fails at once with what to do, never
    "restart Windows") and keeps
    the DirectInput "driving" registry entry right (and Logitech's FFB entry out); `HideFilter` sets
    `HKLM\SOFTWARE\wheelbri\hide\AllowedPid` for the `wheelbri-hide` filter so only the service sees the rig.
    What is hidden is the active rig (Wheel setup), never a list of ids in the code: `rigHideTargets` (main.cpp)
    reads its devices (skipping 046d:c262, `mayHide`), `ensureHideFilter` runs `install-hide.ps1 -Devices` for
    those without the filter, `HideFilter::check` reports each (Ready / NotInstalled / NeedsRestart /
    NotConnected) and the window shows "rig hidden (N of M devices)" or, in red, "NOT HIDDEN: <device>"; no rig
    = nothing hidden, said in the window. Our own filter only, no HidHide or other third-party tools;
    `--no-hide` skips it, `--unhide` clears it after a crash.
  - Failure handling (keep it when changing `runSession`): a wheel not connected at Start is waited for up to 15 s
    (core `waitFor`, `WaitFor.hpp`, `WaitForTest`; Stop cancels): Windows restarts it after a reinstall or while
    `install-hide.ps1` puts the filter on it, and Start used to fail at once then; a lost wheel (`runLoop` returns `kWheelLost`) keeps the
    virtual devices and waits for the wheel to come back, then carries on; creating devices and waiting for the
    hide installer stop on Stop (`cancelled`), but a running installer is never killed (it may be switching a
    device's driver). Exceptions on the loop thread end the session instead of `std::terminate`.
- `shim/` (`dinput8.dll`, Windows/Wine only, namespace `wheelbri::shim`): proxies `IDirectInput8A/W`,
  devices and effects. It is written once as templates over `AnsiTraits`/`WideTraits` (`Traits.hpp`).
  Exports are renamed through `dinput8.def` (`Proxy_*`) so they don't clash with SDK declarations. The real
  DLL is loaded from the system directory on first use (`RealDInput.cpp`). Phase 2: the physical wheel
  (matched by USB VID in `EnumDevices` and again in `CreateDevice` via `GetDeviceInfo`) is presented as the
  target profile by `EmulatedDevice<T>`, which answers input calls from `StateFeed` (UDP receiver, neutral
  state when stale, default after 1500 ms). Phase 3: its effects are `EmulatedEffect`s whose parameters
  `FfbForwarder` sends to the service (timing, keep-alives, every slot re-sent every 250 ms, stopped and free ones
  too, so a lost "stop" heals; each slot belongs to the device that created its effect and carries a ticket, so
  one device's Unacquire, Release or reset leaves another's effects alone and a stale effect cannot touch a reused
  slot; the DLL pins itself on the first export call); in that mode the physical device is opened
  non-exclusively because the service owns its force feedback. Other devices stay `ProxyDevice<T>`
  (log and forward). Settings: optional `wheelbri-shim.ini` next to the DLL (`ShimConfig`). Once a minute (and at
  release) `StateFeed` logs an `input:` line (core `InputReadStats`): how often the game reads (NFS: ~90/s), packets per
  read, reads without new state, reads behind (`UdpLoopbackReceiver::lastPoll().capped`: kMaxPerPoll stopped the
  drain, so older state was handed over) and the shim's own time per read.
  Axis audit (`shim.audit`, `DeviceAudit` over core `AxisAudit`): every device records what the game asks and sets
  (first call of each kind logged in full; anything that could remap axes at warn: action maps, `SendDeviceData`,
  the control panel, calibration and other non-scaling `SetProperty`, any `Escape` except Logitech's rev lights) and
  logs a summary ("none could remap axes" or what could) at Unacquire, Release, after a minute of reading and every
  5 minutes. `ModuleWatch` logs the game's DLLs at `DirectInput8Create`, `CreateDevice` and each summary
  (input-related ones, `isInputModule`: Logitech SDK, hid.dll, XInput, Windows.Gaming.Input, SDL).
  `LogFolder.hpp` (namespace `wheelbri::gamelog`): where a DLL inside a game logs and its starting level; shared with
  `logi/` (each DLL compiles it).
- `logi/` (`wheelbri-logi.dll`, x64 Windows only, `WE_BUILD_LOGI`, namespace `wheelbri::logi`): a logging stand-in for
  Logitech's Steering Wheel SDK helper DLL. Games built with that SDK (NFS Payback) find the helper through the
  registry (`ServerBinary` of `HKCR\CLSID\{63BD165D-1584-4E75-AB56-08330350545F}`) and look up its 48 `Logi*`
  functions by name (core `LogiSdk.hpp`, the list and how to print each call; `wheelbri-logi.def` exports them).
  Each export takes four raw 64-bit register values and passes them to the same function in Logitech's real DLL
  (`RealLibrary.cpp`: `[logi] real_library` in a `wheelbri-logi.ini` next to it, else G HUB's newest
  `wheel_sdk\<version>\logi_steering_wheel_x64.dll`, else the old Logitech Gaming Software's), so the forwarding is
  exact whatever the declared types; only the two LED functions are typed (floats travel in XMM registers). It logs
  what the game asked and got (`logi` lifecycle and setup, `logi.query`, `logi.input` the state every frame at debug,
  `logi.ffb`, `logi.leds`; a call is logged when its answer changes, `ChangeLog`) and a summary a minute, to
  `wheelbri-logi-<date>-<NN>.log`, at `[logi] log_level` from `wheelbri-logi.ini` (`Settings.cpp`; `logi.sh` writes
  `debug`), else the app's level. Same rules as the shim inside the game: nothing in `DllMain`, lazy logging, no
  threads. Never ship Logitech's DLLs. `scripts/make/logi.sh` builds it and prints the registry commands.
- `driver/` (Windows, UMDF 2, self-signed test certificate, built with VS + WDK; see `driver/README.md`):
  - `wheelbri-g920` (`Driver.cpp`): HID minidriver for the virtual G920 (046D:C262) or G25 (046D:C299): its INF
    binds `wheelbri\G920` and `wheelbri\G25`, and `EvtDeviceAdd` reads the device's hardware id to pick the identity
    (`virtualWheelForHardwareId`; unknown = the G920). The service's `VirtualG920::create(wheel, ...)` creates the
    software device with that id (the helper's `CreateVirtualG920` carries "g25"; none = the G920). Reads
    `StatePacket`s on 41329, decodes HID PID output reports into `FfbPacket`s to 41330. Compiles core's `HidG920`, `HidPid`,
    `StatePacket`, `FfbPacket` sources. Logs to `wheelbri-driver-<date>-<NN>.log` in the user's logs folder (see below).
    Each state goes to Windows once (`Link::publish` / `answerRead`): a HID read gets the oldest state not handed out
    yet or waits for the next one (up to 8 kept in order, so quick presses are not lost); until 2026-10-07 each state
    answered every read that came meanwhile, the suspected cause of half a core busy (`docs/cpu-measurements.md`). Once a minute the
    log says how many states came and how the reads were answered ("input, last 60 s: ...").
  - `hide/` (`wheelbri-hide`): upper filter on the game-controller interfaces of each rig device: exactly the HID
    interfaces DirectInput lists as game controllers (`HideFilter::check` via `dinput::gameControllerPaths`,
    `hidhide::instanceIdOfHidPath`; not compatible ids: many wheels are multi-axis controllers without
    `HID_DEVICE_SYSTEM_GAME`, and the registry gave none). Device-agnostic, no ids in the INF or driver. The INF's
    only hardware id, `WHEELBRI_HIDE_FILTER`, belongs to no device, so Windows never puts it on anything by itself
    (when it matched compatible ids it went onto the virtual G920; the driver also never hides 046D:C262; DriverVer
    is fixed at 01/01/2001). `install-hide.ps1 -Devices <instance ids or vid:pid>` puts it on those interfaces with
    SetupAPI `DiInstallDevice`, taking it from the INF's class driver list (`SPDIT_CLASSDRIVER`, inline C#). Only the process in `AllowedPid` may open a filtered interface. Replaces
    HidHide. Fails open: a missing or unreadable setting, or an `AllowedPid` whose process is gone (or whose start
    time differs from `AllowedPidStart`: a reused id), hides nothing, so a crash never leaves anything hidden.
    `HideFilter::check` counts the filter only when it is ours and running: `WUDFRd` in UpperFilters, our INF's
    device description, *and* no `DN_NEED_RESTART` / problem on the devnode (after a forced driver switch Windows
    may keep the old stack until the device restarts; `install-hide.ps1` then runs `pnputil /restart-device`).
  - Install scripts share `driver/common.ps1` (admin check, pnputil exit codes 0/259/3010, finding installed
    packages without depending on pnputil's language). Each fails with a red line and exit 1; `install-hide.ps1`
    exits 2 when the filter is set on a device but runs only after a restart; without `-Devices` it only adds the
    package (and takes it off every device). The installer then runs the installed `wheelbri-service --prepare-hide`,
    which does what Start does (`ensureHideFilter` for the saved rig: the game controllers DirectInput lists, never
    046d:c262; USB ids by compatible id missed the Moza R3), so after a reinstall the first Start does not have to.
    With the wheel off at install, the first Start does it (never the app on its own: the user's rule, 2026-10-06,
    nothing privileged happens without pressing Start). During Start the session tells the app what takes long
    (`UiHost::step`: hiding, waiting for the wheel), shown after "Starting"; for `kStepFirstHide` the app also shows a
    popup (`App::showFirstHide`: once per installation, may take up to a minute, do not unplug) that closes by itself.
    The app runs `install-hide.ps1 -NoRestart` and restarts a device that waits for it at once
    (`waitUntilStarted(..., restartAfterMs = 0)`): pnputil /restart-device refused there (exit 50) and the script then
    waited 15 s, the app 6 s more (36 + 6 s on 2026-10-06). The script's lines carry their time. The WheelBri Helper (SYSTEM) passes the
    caller's logs folder to the install scripts (`-LogFolder`, `privileged::installG920Driver` / `putHideFilter`;
    ProgramData's before ShareLogFolder): their own `%LOCALAPPDATA%` would be SYSTEM's, and install-hide.ps1 also sets
    HKLM LogFolder from it. The hide driver logs each program that opens the wheel or is refused by name
    (`NtQuerySystemInformation`, as `processImage`), once per program until the hiding changes.
    A device takes a new filter only when Windows restarts it, which nothing may hold it open for: `ensureHideFilter`
    then waits, opening nothing, with `HideFilter::waitUntilStarted` (device status only; after 6 s it restarts the
    device itself with `CM_Query_And_Remove_SubTree` + `CM_Setup_DevNode`, and logs who vetoes). On 2026-10-05 the
    session opened the wheel a second after the installer and the filter never started. Never tell the user to restart
    Windows for this: close what uses the wheel, or replug it. It forces the filter onto a device even though another driver ranks higher (Windows'
    input.inf, or the wheel maker's own driver). `uninstall-hide.ps1` and `clean-pc.ps1` find the devices by
    their driver (`Get-DevicesUsing`, DriverInfPath), never by id. In these scripts wrap
    function results in `@(...)` before `.Count`: Windows PowerShell 5.1 gives no `.Count` for a single CIM object.
    Driver threads close their sockets only after joining, and repeating socket errors are logged once a minute.
- `service/helper/` (`wheelbri-helper.exe`, Windows only, namespace `wheelbri::helper`): the WheelBri Helper, a
  Windows service (`WheelBriHelper`, SYSTEM) that will do the steps needing Administrator for the app so the app
  runs as a standard user (README: "Running as a standard user"). `HelperServer`: the pipe
  `\\.\pipe\wheelbri-helper` (DACL: SYSTEM, Administrators, interactive users without creating instances; first
  instance only, no remote clients), up to 4 connections each on a thread; a caller is accepted only as
  `wheelbri.exe` in the helper's own folder (opened, named by its handle, pid compared again), others may only say
  Hello and are then closed; every message is answered by core `answerMessage` (`HelperDispatch.hpp`: the rules, a
  session that throws is a Failed reply, not a dead helper); the first message must come within 10 s; one pipe
  instance more than the 4 connections always waits unserved, so the name never disappears; a closed connection
  undoes what it set up. `HelperServer::Options` takes the pipe name, the session factory and the log taker (the
  tests use their own; only `HelperMain` makes the real ones). `HelperLog`: the helper's log goes into the log
  folder of whoever last ran WheelBri (`mayTakeTheLog`: an accepted app, or WheelBri from another folder saying
  Hello; `wheelbri-helper-<date>-<NN>.log`, the user's choice), opened only while impersonating that user; core
  `DailyLogFile` keeps lines in memory until then, rolls over at midnight and retries a failed file once a minute.
  Attaching the folder it already writes into keeps that file (`attach(..., where)`): 0.9.0 made a new, empty file at
  every Hello.
  The app says Hello at start and from the Logs tab (`service/src/HelperProbe.cpp`, own thread, every 5 min at most),
  so the helper's boot lines reach the user's folder even when the app runs as Administrator. `HelperClient`: the app's side
  (impersonation allowed, message mode, timeouts). `wheelbri-helper --console` (Administrator) runs it in a console,
  `--hello` asks a running helper (exit 0 accepted, 2 refused, 3 none). `HelperSession`: one per accepted
  connection, carries the requests out through `privileged::` (HideRig names the caller's own process; PutHideFilter
  checks each interface again with `HideFilter::interfaceProblem`, never a keyboard, mouse, system or consumer control
  or digitizer, `hidhide::neverHiddenUsage`; ShareLogFolder opens the caller's own logs folder as the caller, refuses a
  link, grants LocalService through that handle, sets HKLM LogFolder; the virtual G920 without HKCU entries, which the
  app keeps); its destructor removes the G920 and shows the rig again. The helper clears a stale AllowedPid at start
  and looks for driver scripts only next to itself (3 folders up with `--console`). Links core, `wheelbri_privileged`
  and Windows libraries, not the app's objects.
- `tools/uhid/` (C++, Linux only, `WE_BUILD_UHID`): Phase 4, `wheelbri-uhid`. The same virtual G920 as `driver/`
  (`core` `HidG920` descriptor, `HidPid` device-side PID decoding) through the kernel's uhid; Proton games read it
  raw with `PROTON_ENABLE_HIDRAW=0x046D/0xC262`. Its descriptor is `g920ProtonReportDescriptor` (same report,
  clutch Usage Z, throttle Usage Rz): Proton's DirectInput has a hack for 046D:C262 (Z -> Rz, Rz -> Slider), and
  with the Windows descriptor NFS read the clutch as throttle (2026-10-08). `--target g25` (the service passes the
  config's target): the Windows descriptor, 046D:C299 on `BUS_VIRTUAL` (on USB the kernel's `hid-lg` claims C299 and
  then refuses a device not really on USB), `PROTON_ENABLE_HIDRAW=0x046D/0xC299`. `UhidG920` holds the uhid protocol
  with no file descriptors so
  `UhidG920Test` covers it; `UhidLoop` (`UhidLoopTest`) holds the command line (ports checked) and `InputFeed`
  (newest state, a keep-alive 100 ms after the last report written, neutral after 1.5 s); `main.cpp` is the fd/UDP
  loop: it waits on /dev/uhid and the state socket (`UdpLoopbackReceiver::nativeSocket`) and reads every queued uhid
  event per wake (the kernel queues 32). It marks the device `uniq = kG920VirtualUniq`; the evdev listing
  (`WheelEvdev.cpp`) leaves such a device out, so the service never reads its own output (and Steam Input's virtual
  pads, 28de:11ff, copies of other controllers). The app's Start with the target "G920 virtual device" starts it on
  Linux (`--virtual-g920` there, `Options::uhid`, `service/src/UhidProcess.cpp`: next to the program or
  build/tools/uhid/, `--stop-file` in the log folder and `--parent <app pid>` so it goes when the app is killed; one
  already running, wheelbri.sh's, is used and left alone). `wheelbri.sh` sees an app session through the service's
  `SingleInstance` lock (`flock`). The kernel's `hid-logitech-hidpp` claims 046D:C262 and gives up because output
  report 0x11 is too short for HID++ ("not enough values in hidpp report 17"); `UhidG920Test` keeps it so, or it
  would send HID++ commands that `HidPid` reads as force feedback. Steam's virtual pad is never hidden from a game
  (the user's rule, 2026-10-09): it is the only thing that controls the Steam Deck, so it always stays; no hiding of
  it, no advice to turn Steam Input off (docs/linux.md, "Steam's virtual pad").
- `scripts/make/`: the build scripts (above); `common.sh` holds the shared helpers. `package.ps1` assembles the
  Windows installer package in `dist/windows/` (folder, zip, `WheelBri-<version>-setup.exe` made with IExpress,
  which is part of Windows); the version comes from `project(VERSION)` in `CMakeLists.txt`.
- `installer/`: what the package carries besides the build outputs. The product is called **WheelBri** (as the repo
  folder) everywhere the user sees it: install folder, Start menu, Settings > Apps, window titles, Setup.exe; lower-case
  `wheelbri` for everything else (exe names, log and ProgramData folders, registry keys, drivers, device ids, the C++
  namespace and `core/include/wheelbri/`). Until 2026-10-03 the project was called **wheelemu**: never use that name
  for anything new; it only appears where old installs are found and removed (`$legacy` / `$names` in `install.ps1`,
  `uninstall.ps1`, `clean-pc.ps1`; `Remove-LegacyPackages` in `driver/common.ps1`; `moveFoldersFromOldName` in
  `ServiceLogging.cpp`, which moves `%LOCALAPPDATA%\wheelemu` to `wheelbri` on first start; old log names in
  `Retention.cpp`; `wheelemu-shim.ini` as the shim's fallback). The wire tags (`WEMU`, `WEXP`, `WECL`) and the `WE_LOG_*`
  macros keep their letters. `install.ps1` asks one question, at the end of a successful install: restart Windows now
  (default yes; `shutdown /r /t 2`; firmer wording when `install-hide.ps1` exited 2; none with `-Quiet`). Only the
  installer asks this, never the app or the uninstaller (self-elevating; checks, closes a running copy, copies to `%ProgramFiles%\WheelBri` and moves an older install
  (from before the rename too: its program files, Settings > Apps entry, devices and drivers go, its config stays), runs the three driver install scripts, registers and starts the WheelBri Helper service (`WheelBriHelper`,
  `Install-HelperService` in `driver/common.ps1`; the uninstaller and `clean-pc.ps1` remove it first with
  `Remove-HelperService`), shortcuts, Settings > Apps entry, Start
  menu folder with the app and "Uninstall WheelBri", closes at once after a success (only a failure waits for Enter), log in
  `%LOCALAPPDATA%\wheelbri\logs\wheelbri-install-<date>.log`), `uninstall.ps1` + `Uninstall.cmd` (waits for a driver installer the app started (`Wait-DriverInstallers`), removes
  the drivers, the virtual devices (`Remove-VirtualDevices` in `uninstall-driver.ps1`), the UMDF registrations and
  DirectInput's G920 entries; the folder from
  Settings > Apps' `InstallLocation`; deletes it only when it holds `wheelbri.exe` and no `install.ps1`, i.e. never a
  package; runs from a copy in `%SystemRoot%\Temp\wheelbri-uninstall-<guid>` that only Administrators may change; from the package it cleans up an install whose folder is gone), `Setup.cmd`
  (Setup.exe's bootstrap: unzips and runs `install.ps1`, waiting for it), `README.txt` (for the end user). The
  installed app runs the driver path by default (virtual G920, rig hidden on every Start; "G920 in-game shim" on
  the Mapping tab switches); the shim ships in `shim\`. The package mirrors the repo layout (`driver\...\x64\Release\<name>\`), because the
  driver scripts and the app's `ensureHideFilter` look there. Only Windows' own tools: no third-party installer.
  When a new file must ship, add it to `package.ps1` and to the required list in `install.ps1`.
- `scripts/`: `telemetry_summary.sh` (bash + awk, reads telemetry columns by name); `linux/` (`deck-setup.sh` makes the
  `wheelbri-build` distrobox container: Arch Linux (SteamOS is Arch underneath) from pacman (base-devel, cmake, ninja,
  git, libx11, mingw-w64-gcc), pinned through the Arch Linux Archive to the day Arch still had SteamOS's glibc
  version, so its programs load on SteamOS; `deck-build.sh` builds, tests and installs into `~/wheelbri` from a git
  checkout on the Deck (it refuses a container whose glibc is newer than SteamOS's). Its GCC and CMake are recent
  Arch ones: keep the code warning-free with them; start/stop and Steam launch wrapper `wheelbri.sh` (`--virtual`, the default, or
  `--shim`: puts `dinput8.dll` and `wheelbri-shim.ini` into the folder of the `.exe` in `%command%`, else the Steam
  install folder of its `AppId=` (NFS 2015 starts as `steam2ea://...` through the EA app, no `.exe`), and takes them out
  when the game ends, `--keep-shim` leaves them for testing, `--virtual` takes a leftover out; never touches another
  mod's `dinput8.dll`, the same rule as `ShimGames`), udev rules,
  `package.sh` bundle).
- `config/`: `g920-virtual-device.ini`, `g920-in-game-shim.ini` (service: the target and its default mapping),
  `wheelbri-shim.ini` (shim).
- `docs/`: `phase5-windows-virtual-device.md` (Phase 5 design and step 0 lessons), `linux.md` (Steam Deck guide),
  `future-functionalities.md` (worked-out ideas not built yet: RS Handbrake, G29; and the standard-user test still to
  do), `push-input-plan.md` (the service loop from polling to Windows' notifications: merged into main on 2026-10-07), `cpu-measurements.md` (measure.sh rounds: what was measured, the numbers and what they showed; add new rounds there).
- `tests/`: GoogleTest for `core/` (`wheelbri_tests`), plus `UhidG920Test` on Linux. Use fakes of the interfaces
  (see `PipelineTest.cpp`), not real devices or sockets, except `UdpLoopbackTest` and `WaitSetTest` (the service's
  `platform::WaitSet` with loopback sockets, a pipe or an event), `FolderWatchTest` (inotify on a temporary folder),
  `DeviceWatchTest` (real thread, fake lister) and `HelperProbeTest` (real thread, fake check); linked when
  `wheelbri_privileged` is built. On Windows also `HelperPipeTest`: `HelperServer` and `HelperClient` on a pipe of
  the test's own with fake sessions, and `HelperLog`'s file written as the user (no Administrator needed; it also
  runs under Wine, which skips the first-instance test). On Linux also `PlatformPosixTest`: the single-instance lock
  and child processes with real files and processes.
  **Test the service loop on the emulators** in `tests/emulators/PushEmulators.hpp` (`PushLoopTest`): devices
  that push input like DirectInput / evdev or misbehave (`EmulatedPushDevice::Behaviour`: polled, dropped, stopped,
  late or spurious notifications, unplugged), game force feedback datagrams (`EmulatedSocket`), a wait that can fail
  (`EmulatedWaitSet`) and `PushLoopRig`, which runs `EventLoopControl` and the real `Pipeline` as runLoop does, on a
  virtual clock, and reports each change's latency, what woke each pass and when it started polling. A change to the
  loop comes with a scenario there; keep the rig's loop in step with runLoop.

## Conventions

- Line endings: LF in every file, on Windows too; CRLF only for `.cmd`, `.bat`, `.inx` and `.inf` (`.gitattributes`
  enforces it, `.editorconfig` tells editors). Write files with LF; never convert a whole file's line endings as a
  side effect of an edit.
- Game names, wherever WheelBri shows them (a game profile's `label`, the log, the window, docs): short, and Need for
  Speed games as "NFS <name>" (NFS 2015, NFS Payback, NFS Heat, NFS Unbound), never "Need for Speed ...". A profile's
  `label` is shown as it is ("Running 1:23, NFS 2015"); `parseGameProfile` refuses labels over 20 characters or
  starting with "Need for Speed".
- No third-party runtime tools or drivers (HidHide, ViGEm, HIDMaestro): what the
  emulator needs on the PC is built in this repository.
- Scripts never wait for the user when they end ("press Enter to close", `pause`, a window left open with
  `-NoExit`): they ask only for a real decision (clean-pc's "Remove all of the above?"). When a script continues in its
  own Administrator window, that window closes when done and the calling window shows its result or log (clean-pc.ps1
  `-LogPath`, drivers.ps1, measure.sh). The end user's installer and uninstaller are the exception (Setup.exe closes
  its window otherwise before the user could read a failure).
- No Python. Scripts are bash (MSYS2 UCRT64 and Linux; awk for data, POSIX awk only: MSYS2 has gawk, Linux may have
  mawk, so no gawk extensions and no deep recursion) or, for Windows-only work, PowerShell (`.ps1`) / `.cmd`.

- Style: C++20, 4-space indent, `camelCase` functions and variables, `PascalCase` types, `trailing_` members,
  `kConstant` constants, `#pragma once`, `[[nodiscard]]` on queries, `const` parameters in definitions,
  float literals as `1.0F`. Headers in `core/include/wheelbri/`, included as `"wheelbri/X.hpp"`.
- Every thread WheelBri starts is named (`platform::nameThisThread("wheelbri <role>")` first thing on the thread; the
  G920 driver's with `SetThreadDescription`), so `measure.ps1` shows what costs what. Name new threads the same way.
- Code on the loop (`Mapper::map`, `Pipeline::tick`, `DeviceProfile::encode`) must stay free of
  allocations and blocking calls.
- **Logging** (full rules in `core/include/wheelbri/Logging.hpp` and README):
  - Always log through the `WE_LOG_*` macros, never `printf`/`std::cout`. The exceptions are `--list`,
    `--print` and `--ffb-test` output, which are user interface.
  - Use one named logger per component, fetched via a function-local static `log()` helper. Component names
    are dotted (`core.net`, `service.ffb`, `shim.device`); add new ones to the README list.
  - Hot paths must use `logging::CallSampler` or `logging::RateLimiter` / `WE_LOG_*_LIMITED`.
  - Levels: trace (per call/tick), debug (decisions), info (lifecycle), warn (recoverable), error (feature
    broken), critical (process cannot continue).
  - The level is the app's Level button (Logs tab, V): warn (default), info, debug, trace, round again; never quieter
    than warn (`LogLevel.hpp` `clampToApp`). `service/src/AppLogLevel` holds it (`[app] log_level` in app.ini) and
    applies it to this process; the session sends `LogLevelPacket` (header only, `LogLevelBeacon`: at start, on a
    change, every 2 s) from the state socket (`UdpLoopbackPublisher::publishLogLevel`, never a second socket: the shim
    checks each new sender port on the game's thread) to the shim (`applyLogLevel`, which starts from app.ini) and
    wheelbri-uhid (`UdpLoopbackReceiver::takeLogLevel`); the helper takes it from the app's Hello (its one string) and
    `SetLogLevel` (request 9, also writes `HKLM\SOFTWARE\wheelbri LogLevel`); both drivers read that value at most
    once a second and write `[time] [level] text` lines. The drivers' registry value is privileged work: written at
    Start (`PrivilegedSteps::setDriverLogLevel`) and on a click only while a session runs. The command-line service
    keeps its config's `[logging] level` (default warn). Default level everywhere: warn (`LogConfig`).
    `logging::currentLevel()`, never `spdlog::get_level()` (it needs spdlog's default logger, gone after shutdown).
- **Shim rules** (it runs inside the game process):
  - Do nothing in `DllMain` beyond `DisableThreadLibraryCalls` and the detach hook: no logging, no
    `LoadLibrary`, no threads. Logging starts lazily through `shim::logger()`.
  - Don't start background threads, and flush every log line.
  - Nothing may throw into the game: every COM method, export and enumeration callback runs its body through
    `guarded()` / `guardedOr()` (`Guard.hpp`), which log the exception and return an error code instead. Singletons
    are never destroyed (a static's destructor would close sockets and call WSACleanup inside DllMain).
  - The shim links statically (see the toolchain file). Don't add runtime DLL dependencies, and never link
    `dinput8`.
  - COM wrappers own one real reference and delete themselves at refcount 0. Anything handed back unwrapped
    (aggregation, `EnumDevicesBySemantics`, `DllGetClassObject`, `EnumCreatedEffectObjects`) is logged
    with a warning on purpose.
- FFB safety: rigs are often direct drive. Every force the service sends to hardware must go through
  `FfbSafetyLimiter` (in `FfbController`, which also has a watchdog); keep the defaults conservative. A game that
  crashes while using the virtual G920 sends nothing more and the driver cannot tell: `HidPid` stops keep-alives
  after 10 s without a report while a pushing force (constant, ramp, periodic) plays, so the service's watchdog
  stops it (springs and dampers keep going; the next report brings everything back)
  (`max_magnitude` 0.8, slew limit). A condition whose dead band covers the whole travel is sent empty (the R3 drove
  itself to full lock with one), and a saturation of 0 (which the R3 reads as "no limit") gets the cap. Game effects reach the service as `FfbPacket`s (shim, driver or
  uhid). The only way around the limiter is the opt-in `force_feedback = passthrough` in `wheelbri-shim.ini`;
  keep it opt-in and don't add others.
- Files: everything the app saves (configs, rigs, device names, app.ini, the shim games list, logs, telemetry, the
  uninstaller's config backup) is per user, under `%LOCALAPPDATA%\wheelbri` (XDG folders on Linux), also when it runs
  as Administrator, and the WheelBri Helper's log (written as the connected user). Only drivers, virtual devices, the
  helper service and their registry entries are system-wide. Never add a file location under ProgramData, Program
  Files or HKLM for settings. End users are standard users (the app is `asInvoker`; the helper does the privileged
  steps); the developer runs development builds as Administrator (`scripts/make/app.sh`; `--as-user` with
  `wheelbri-helper --console`).
- Security, while running as Administrator (`platform::isElevated()`): everything under `%LOCALAPPDATA%\wheelbri`
  can be written by any program of the user, so nothing from there may choose a privileged action. Log and telemetry
  paths from a config must be inside the user's WheelBri folders (`isInsideUserFolders`, else ignored with a
  warning); the shared log folder is always `logsFolder()`; helper programs come from fixed system places
  (`platform::systemFolder()`, Program Files), not PATH; the app refuses to start when one of its user folders is a
  link (`userFoldersProblem`) and deletes links among its log files (`removeLinkedLogFiles`). As a standard user
  these checks are off (they would protect nothing). Keep new code to these rules.
- Security of the WheelBri Helper (SYSTEM, serving a standard user): anything a step needs comes from the helper's
  own checks, never from the app's word. Every request type is in `HelperProtocol.hpp` with its checks in
  `requestProblem` and `HelperProtocolTest`; no request carries a path, a file name, a command line or a process id
  (HideRig is always the caller's own process, ShareLogFolder always the caller's own folder, opened as the caller).
  How a message is answered (who may ask, unknown types, refusals, failures) is `answerMessage` with
  `HelperDispatchTest`; change the rules there, not in the pipe code.
  What only Windows can tell is checked again in the helper (`HideFilter::interfaceProblem`). Scripts come from the
  helper's own folder only. Anything a connection sets up is undone in `~HelperSession`. A new privileged step goes
  into `privileged::` (used by both sides), a request, the helper's session and `PrivilegedSteps` (both ways), never
  around the helper.
- Licence: nothing is copied from GIMX (GPLv3). If GIMX code is ever copied rather than reimplemented, the
  project must become GPLv3, so flag that before doing it.
