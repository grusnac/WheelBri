# CLAUDE.md

This file guides Claude when working in this repository.

## Working with the user

- After every change, finish with **how to build it** and **how to test it** (what to run, what to look for,
  what a failure looks like), even when the change needs no rebuild (say so).
- Build and test steps use the scripts in `scripts/make/` (`build.sh`, `rebuild.sh`, `test.sh`, `check.sh`,
  `ui.sh`, `app.sh`, `shim.sh`, `drivers.sh`, `all.sh`, `package.sh`, `clean-pc.sh`; see `scripts/make/README.md`) as the shortcut
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
  scripts/make/build.sh
  ```

  When a script runs several commands, list each one in its own block, in order, before the shortcut.
- Put **every command in its own code block**, one command per block, so each can be copied on its own.
- The user builds and runs on Windows (`D:\cpp-projects\WheelBri`) in an MSYS2 UCRT64 shell. Give commands for
  that shell unless the step needs PowerShell (drivers), and say which shell.
- **Before every commit**, write down why the change was made: the commit message gets a short headline and a
  few lines of reasons.
- **When a functionality is finished** (it may take several commits), add one entry for it under
  `## Functionalities` in README.md, in the commit that completes it, not before. Keep it short: a `###` headline,
  then **Why** (the reason), **Use** (how to use it, settings) and **Look** (where to see it: UI, logs and
  telemetry columns, code files). Ask the user whether a functionality is finished if it's unclear.
- Before guessing at a mapping or FFB problem, read the user's logs and telemetry
  (`%LOCALAPPDATA%\wheelbri\logs`: `wheelbri-service-<date>.log`, `telemetry-<date>.csv`, `ffb-<date>.csv`) and
  run `scripts/telemetry_summary.py`. The `raw_a0`..`raw_a7` columns show which axis a control really moves.

## Project

WheelBri makes the user's rig (a wheel and its accessories, defined in the Rig tab) look like the one wheel it emulates, the Logitech G920 (the G29 profile was dropped: the same wheel with more
buttons; WheelBri decides which targets exist, never the user), on Windows and Linux/Proton. First target game: Need for Speed (2015).

Status (README.md has the full table):
- **Working with a real direct drive wheel in NFS 2015 on Windows**: mapping and service (Phase 1), the `dinput8.dll` shim
  (Phase 2: wheel, pedals, buttons, D-pad), game force feedback through the service (Phase 3: constant, spring,
  damper, sine, square), and the virtual G920 with our own UMDF driver (Phase 5).
- **Settled**: NFS finds wheels through `DirectInput8Create` (A and W) and reads `c_dfDIJoystick2`: wheel X,
  throttle Slider, brake Y, clutch Rz, pedals released at the top of the range. The tested wheel's axes as the service numbers
  them: steering 0, throttle 2, brake 5, clutch 6 (add-on clutch pedal, checked from telemetry).
- **Not yet verified**: Linux/Proton (Phase 4 `wheelbri-uhid` is unit-tested only, not
  tried on a Steam Deck); effect envelopes (ignored); NFS's 20-byte Logitech `Escape` calls (not forwarded).
- The G920's own FFB is HID++, not the classic Logitech protocol; the virtual G920 exposes HID PID instead.

Data flow: `rig --DirectInput/evdev--> wheelbri-service --UDP 127.0.0.1--> consumer`, where the consumer is
one of: the `dinput8.dll` shim in the game, the `wheelbri-g920` driver (Windows) or `wheelbri-uhid` (Linux). Use UDP loopback, not shared memory: under Proton
the shim runs inside Wine and the service is a native Linux process, and named mappings don't cross that boundary.

UDP ports (all 127.0.0.1):

| Port | Direction | What |
|---|---|---|
| 41329 (`state_port`) | service → consumer | `StatePacket`; only one consumer at a time |
| 41330 (`ffb_port`) | consumer → service | `FfbPacket` (game force feedback) |
| 41331 | test tool → service | `ClutchPacket` test clutch (`TestClutch.hpp`) |

## Build and test

CMake ≥ 3.24, C++20. spdlog 1.15.3 and GoogleTest 1.15.2 come in through FetchContent (needs internet on the first
configure). The service uses only the system's APIs and runs on Windows (DirectInput, Win32) and Linux (evdev,
Xlib; skipped with a warning without libx11-dev). No SDL or other third-party library: do not add one back.
`service/src/WheelSdl.cpp` is dead code from before that decision and is not compiled.

The scripts in `scripts/make/` wrap everything below (`scripts/make/README.md`): `build.sh` (incremental,
configures the first time), `rebuild.sh` (deletes `build/`, full rebuild), `test.sh [filter]`, `check.sh` (before a
commit), `ui.sh [config]`, `app.sh [--no-start]`, `shim.sh [--off] <game folder>`, `drivers.sh [-Install]`, `all.sh [--clean]`, `package.sh [--no-build]` (the installer), `clean-pc.sh [-KeepData]
[-GameFolder <folder>] [-KeepThirdParty] [-Yes]` (removes everything WheelBri put on the PC, HidHide and HIDMaestro
included, for a fresh install; when the
app, installer or drivers start leaving something new on the PC, add it to `clean-pc.ps1`).
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
Start / Stop, asks for Administrator) and `build/shim/dinput8.dll`; all linked statically on MinGW.

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
driver\install-driver.ps1
```

(same pattern for `driver\hide\wheelbri-hide.vcxproj` + `install-hide.ps1`).

CMake options: `WE_BUILD_TESTS` (ON; skipped when cross-compiling), `WE_BUILD_SERVICE` (ON), `WE_BUILD_SHIM` (ON
only on Windows targets), `WE_BUILD_UHID` (ON only on Linux). Warnings: `/W4 /permissive-` on MSVC, `-Wall -Wextra
-Wpedantic` elsewhere. Keep new code free of warnings.

Service CLI (`service/src/main.cpp`): `wheelbri-service [--config FILE] [--list] [--print] [--ui] [--ffb-test]
[--old-files delete|archive|keep] [--stop-file FILE] [--virtual-g920 [--no-hide]]
[--unhide] [--app] [--telemetry-tab] [--ffb-tab]`. `--app` opens the app's window (Start / Stop) from a console. `--ui` opens a window (`service/src/MappingView.cpp`, also the app's live view) with tabs along the bottom
(click, or M / R / L, T / F; in this order: Mapping, Rig, Logs, then Telemetry and Force feedback only with
`--telemetry-tab` / `--ffb-tab`, never shown without them, `MappingView::setOptionalTabs`): **Mapping** (the axes rig -> G920, every rig axis, the D-pad, the G920 buttons written
`G920 <n> <name> <- Rig <n> <name>` and the wheel's own buttons `Rig <n> <name>` from `[button_labels]`), **Logs** (one
line per component: emulator, game shim, G920 driver, hide filter, each in its own colour,
with today's file, size, age, click opens its folder; then a scrollable live log of all those files merged:
wheel, arrows, PageUp / PageDown, Home / End, a draggable vertical scroll bar, a sideways one for long messages
(Shift + wheel, Left / Right) and word wrap (Wrap button or W); drags come from `WindowInput::pointer` / `leftDown`,
`MappingView::onPointer`; core `LiveLog`), **Telemetry** (the CSV files, buttons held, the
latest button / D-pad / pedal / FFB events, not scrollable), **Force feedback** (live effects, usage since
start, the latest effects) and **Rig** (R, `WheelSetupView`: defines the rig, see `Rig.hpp`; the app shows it
when stopped too, reading every connected device itself). The app has no log panel or toolbar: only the Mapping tab (the editor while stopped, the
live one while running) has a short status, Start and Stop, in its top right corner. Stopped, the app's own tabs are Mapping (`MappingEditView`,
the editor, with `ConfigPicker` as its top row), Rig and Logs (a `MappingView` of its own, `logs_`, drawing only its
Logs page), in that order (there is no Status tab; a failed Start says "Start failed (Logs)");
it opens on Rig until a rig exists, else on Mapping; Start switches to Logs at once (the session logs while it comes
up) and the live view keeps the tab showing then. O opens the logs folder (every log is there). It draws through
`UiWindow.hpp` (filled/outlined rectangles, 8x8-cell monospaced text and clicks on a 960x748 logical canvas,
letterboxed), implemented natively: `UiWindowWin32.cpp` (Win32 + GDI, DPI aware) on Windows, `UiWindowX11.cpp`
(Xlib) on Linux.

The app (`wheelbri`, `service/src/App.cpp`, same sources plus `WHEELBRI_APP`; on Windows a GUI-subsystem exe
with an Administrator manifest, `service/app/`) runs sessions (`runSession` in `main.cpp`) on a thread of its
own from Start / Stop buttons; sessions show themselves through `UiHost` (`SessionUi.hpp`): the app's window,
or `makeSessionWindow()` for `--ui`. `MappingView` only draws into a window it does not own.

## Layout and architecture

- `core/` (`wheelbri_core`, static library): portable, **no OS headers and no SDL**. The one exception is
  `UdpLoopback.cpp`, which has `#ifdef _WIN32` socket code. Headers live in `core/include/wheelbri/`,
  namespace `wheelbri`.
  - `RawInputState` (8 axes in [-1,1] exactly as the OS reports them) → `Mapper` → `WheelState`
    (target-agnostic: steering -1..1 over the *target* rotation, pedals 0..1) → `IDeviceProfile::encode`
    → `TargetReport` (the emulated device's own ranges) → `StatePacket` → `IStatePublisher`.
  - `Pipeline::tick()` runs one pass of that chain (optionally with a clutch override, see `TestClutch`).
    `IInputSource` / `IFfbSink` (`Devices.hpp`, `kMaxSlots = 8` effect slots) and `IStatePublisher` /
    `IStateReceiver` are the seams for tests and platform code.
  - New emulated wheels are new `DeviceProfileSpec`s (or `IDeviceProfile` implementations), registered in
    `makeProfile()`. `DeviceMatch` matches `source_device` against a name or `vid:pid`.
  - `FfbEffect` is the common FFB model. Every source is translated into it (`DiEffect` from DirectInput
    parameters, `HidPid` from HID PID reports) and `FfbSafetyLimiter` is the last stage before hardware. The
    limiter holds state (slew), so use one limiter per played effect. `FfbController` holds the limiters and
    the watchdog; `FfbUsage` summarises per-effect use for the UI and log; `WheelOutput` converts effects to
    DirectInput/evdev units.
  - The G920 has 19 buttons (`kG920Buttons`, `g920Spec().buttonNames`, 0-based): the wheel's 11 (0-10), an unused 11
    (joy.cpl 12) and the Driving Force Shifter's Gear 1-6 (12-17) and Gear R (18), as Logitech's shifter FAQ gives them
    (joy.cpl 13-19). Every target (shim, virtual device, uhid) reports all 19; `IDeviceProfile::buttonCount` counts
    the gap, and `encode` only passes named buttons. The names are what Auto-map looks for ("Gear 1", "Gear R").
  - `HidG920` is the virtual G920's HID descriptor and input report (from HIDMaestro's builder, MIT);
    `HidPid` is device-side HID PID decoding. Both are shared by `driver/`, `tools/uhid` and the tests.
  - `LiveLog`: follows the components' log files (tail on first read, then what is appended) and parses
    spdlog and `[time] text` lines for the Logs tab; `LiveLogTest` covers it with temporary files.
  - `GameUseWatch`: pedals pressed for 20 s in total while the game never sent force feedback = the game drives from
    another controller (a gamepad or the keyboard); the loop logs a warning and the app's status (next to Start / Stop) says
    "GAME NOT USING THE WHEEL" (`UiFrame::gameIgnoresWheel`).
  - No virtual Xbox controller (removed 2026-10-03, with the HIDMaestro bridge): with an XInput pad there at the game's
    start NFS drove from the pad, the pad's buttons acted as its own (its right stick click is NFS's photo), and stick
    nudges did not change NFS's keyboard prompts. Do not add one back without asking. `HidHideCli` holds device-id
    string helpers (`HideFilter` uses them; HidHide itself is no longer used).
  - `TestClutch`: a test clutch for when no pedal is connected. A test tool sends `ClutchPacket`s (12 bytes, magic
    `WECL`, v1; `ClutchPacket::encode`) to 41331; the service uses that value instead of the mapped clutch until 1.5 s
    after the last packet. (Its C# sender, ClutchProbe, went with the bridge.)
  - The rig (`Rig.hpp`): every device the user has (`[device_N]` name, id, kind: guessed from the name, no longer
    shown or chosen in the Rig tab; `Rig::wheel()` is the device with a Steering axis, the kind only a fallback) and
    each control's label, role
    (`ControlRole`: steering, throttle, ..., d-pad up, left / right paddle, shift up, gear N; new roles go at the end
    of the enum, since autoMap counts gears by value) and invert, written `label | role | invert`;
    one `<name>.ini` per rig in `rigsFolder()` (`%LOCALAPPDATA%\wheelbri\rigs`), the active one in the app's state
    file (`[app] rig`). The Wheel setup tab (`WheelSetupView`) writes it (Detect: a touched control opens its popup;
    the rest position taken after ~20 reads gives a pedal's invert); the Mapping tab maps from it (`autoMap` in
    `MappingEdit.hpp` uses the roles). A session takes the names it shows from the active rig, else from the older
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
  - The config Start uses (`service/src/ConfigPicker.cpp`, the top row of the idle Mapping tab): the target, picked
    with `<` `>` like the Rig tab's device: "G920 virtual device" or "G920 in-game shim" (`[app] way` in
    `appStateFile()`, `app.ini`; `AppSettings::runSession(host, config, virtualDevice)`). Two layers: the **target**
    is fixed (the G920 profile's axes, D-pad and buttons; never the user's to change), the **mapping** of the rig onto
    it is the user's. Each target has a built-in config, never changed, holding WheelBri's default mapping (the user's
    corrected mapping of 2026-10-04): `config/g920-virtual-device.ini` (also `defaultConfigPath()`, the command line's
    default) and `config/g920-in-game-shim.ini`. The Mapping tab's Save writes the user's own as
    `userConfigFolder()/my-<built-in name>` (`%LOCALAPPDATA%\wheelbri\configs`), used instead; "Default" deletes it.
    The rig no longer rewrites a mapping (no rigMapping); it gives the sources' names, the wheel when `source_device`
    is empty, and Auto-map. A source the active rig has not named shows "not in the rig" (`MappingEditView::missing`).
  - `ShimGames.hpp`: the games the app put the in-game shim into (`shimGamesFile()`:
    `%LOCALAPPDATA%\wheelbri\shim-games.txt`, per user like every setting). The file: tab-separated
    `game name folder` / `selected folder`, UTF-8 paths; not IniDocument, which cuts at `;` and `#`), `installShim` /
    `removeShim` / `shimState` (Installed, Outdated, Missing, Foreign: a `dinput8.dll` that is neither byte-identical to
    ours nor next to a `wheelbri-shim.ini` is another mod's and never touched), `gameFolderProblem` (needs an `.exe`).
    `ShimGamesTest` covers it with temporary folders. UI: `service/src/ShimGamesView.cpp`, only with the target G920
    in-game shim: "Game <name> [Games (N)]" next to Start, the games panel (pick, Add game through
    `UiWindow::pickFolder`: IFileOpenDialog on Windows, none on X11; Put / Remove shim; Forget = `removeShimGame`, list only), and the App's popups: "Would
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
  - `TelemetryRecorder` (`Telemetry.hpp`): daily CSVs in the log folder (`telemetry-YYYY-MM-DD.csv`, a row when
    something changes, at most `rate_hz` = 50/s, plus a heartbeat every `heartbeat_s` = 60; `ffb-YYYY-MM-DD.csv`
    one row per game FFB change), each session appended after a `# session started` block, rolled over at local
    midnight. Columns come from `telemetryColumns()`; when you add one, write it in `writeSample` in the same
    order and bump `kFormatVersion` (`Telemetry.cpp`, now "2") if a column moves.
  - Logs are daily too (`logging::init` adds the date: `wheelbri-service-YYYY-MM-DD.log`); the shim logs to
    `%LOCALAPPDATA%\wheelbri\logs\wheelbri-shim-YYYY-MM-DD.log`. **One folder for every log**, the user's
    (`%LOCALAPPDATA%\wheelbri\logs`; Linux `~/.local/state/wheelbri`): app, shim, telemetry, both drivers, installer,
    hide installer. The drivers run as LocalService: `shareLogFolder` (`ServiceLogging.hpp`, at every start as
    Administrator) and `install.ps1` / `install-hide.ps1` let LocalService write there and set `HKLM\SOFTWARE\wheelbri
    LogFolder`, which the drivers read per line (fallback `C:\ProgramData\wheelbri\logs`, moved in at the next start).
    Do not add another log location. `LocalDate.hpp` has the date helpers.
  - `Retention.hpp` decides which files are old (date in the name, else last-write day) and only ever touches
    files starting `wheelbri-service`, `-shim`, `-bridge`, `-logs-`, `-driver`, `-hide`, `-install`, `-uhid`, `-sh`,
    `telemetry-`, `ffb-`.
    The service's `Housekeeping` deletes them (default, after 2 days) or archives them with the best compressor
    found (7z > xz > zstd > zip > gzip) as a background process via `platform::ChildProcess`, deleting
    originals only after the compressor exited 0. Running as Administrator it ignores PATH (any program of the user
    can write there) and only uses 7-Zip in Program Files (`SHGetKnownFolderPath`) and `tar.exe` in System32.
  - `FileText.hpp`: UTF-8 path conversion (`pathToUtf8`, `pathFromUtf8`, which refuses invalid UTF-8), `readFileText`
    (regular files only), `writeFileReplacing` (writes `<file>.tmp`, then renames it over: never half a file, never a
    file in place of a folder), `forEachEntry` (a folder listing that never throws), `truncateUtf8`. Use them for the
    user's files rather than raw streams.
  - `SenderGate.hpp`: who may send to the UDP ports. The receiver looks up the process that owns the sender's port
    (Windows: `platform::udpPortOwner` / `processImage` in the service, the same through `iphlpapi.dll` loaded on
    first use in the shim) once per new port, caches the verdict (2 s accepted, 10 s refused) and caps lookups at 16
    a second. The service takes game force feedback only from `WUDFHost.exe` in System32 or a program whose folder
    holds our shim (unknown: let through, warned); the shim takes the wheel's state only from `wheelbri.exe` /
    `wheelbri-service.exe` (unknown, e.g. the native service under Proton: let through). Linux: no check.
  - Wire format (`StatePacket.hpp`): 33 bytes, little-endian, magic `WEMU`, version 1. Any layout change
    must bump `kVersion` and update the layout comment, `StatePacketTest` and the driver.
- `service/` (`wheelbri-service`, namespace `wheelbri::service`):
  - The wheel behind `Wheel.hpp` (`WheelDinput.cpp` DirectInput + `DinputReader`, `WheelEvdev.cpp` evdev),
    numbering inputs as the service always has (`DinputReader`, core `EvdevLayout`) and sending the same force
    feedback numbers as before (core `WheelOutput`).
  - OS helpers in `Platform.hpp` (clock, precise sleep, thread priority, child process in a kill-on-close job,
    `SingleInstance` lock so only one session runs per computer, console close handler, `isElevated`); `FfbSelfTest`
    (`--ffb-test`); `Housekeeping` (retention, pumped from the loop); `MappingView` (`--ui`); a fixed-rate loop
    (default 1 kHz, drift-free, resynchronises when behind).
  - Phase 5 (`--virtual-g920`, run as Administrator, no shim in the game): `VirtualG920`
    creates the `wheelbri\G920` software device for the `wheelbri-g920` driver while the service runs and keeps
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
  `FfbForwarder` sends to the service (timing, keep-alives); in that mode the physical device is opened
  non-exclusively because the service owns its force feedback. Other devices stay `ProxyDevice<T>`
  (log and forward). Settings: optional `wheelbri-shim.ini` next to the DLL (`ShimConfig`).
- `driver/` (Windows, UMDF 2, self-signed test certificate, built with VS + WDK; see `driver/README.md`):
  - `wheelbri-g920` (`Driver.cpp`): HID minidriver for the virtual G920 (046D:C262). Reads `StatePacket`s on
    41329, decodes HID PID output reports into `FfbPacket`s to 41330. Compiles core's `HidG920`, `HidPid`,
    `StatePacket`, `FfbPacket` sources. Logs to `wheelbri-driver-<date>.log` in the user's logs folder (see below).
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
    package (the installer). It forces the filter onto a device even though another driver ranks higher (Windows'
    input.inf, or the wheel maker's own driver). `uninstall-hide.ps1` and `clean-pc.ps1` find the devices by
    their driver (`Get-DevicesUsing`, DriverInfPath), never by id. In these scripts wrap
    function results in `@(...)` before `.Count`: Windows PowerShell 5.1 gives no `.Count` for a single CIM object.
    Driver threads close their sockets only after joining, and repeating socket errors are logged once a minute.
- `tools/uhid/` (C++, Linux only, `WE_BUILD_UHID`): Phase 4, `wheelbri-uhid`. The same virtual G920 as `driver/`
  (`core` `HidG920` descriptor, `HidPid` device-side PID decoding) through the kernel's uhid; Proton games read it
  raw with `PROTON_ENABLE_HIDRAW=0x046D/0xC262`. `UhidG920` holds the uhid protocol with no file descriptors so
  `UhidG920Test` covers it; `main.cpp` is the fd/UDP loop.
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
  (default yes; `shutdown /r /t 15`; firmer wording when `install-hide.ps1` exited 2; none with `-Quiet`). Only the
  installer asks this, never the app or the uninstaller (self-elevating; checks, closes a running copy, copies to `%ProgramFiles%\WheelBri` and moves an older install
  (from before the rename too: its program files, Settings > Apps entry, devices and drivers go, its config stays), runs the three driver install scripts, shortcuts, Settings > Apps entry, Start
  menu folder with the app and "Uninstall WheelBri", closes itself 10 s after success when no restart, log in
  `%LOCALAPPDATA%\wheelbri\logs\wheelbri-install-<date>.log`), `uninstall.ps1` + `Uninstall.cmd` (the folder from
  Settings > Apps' `InstallLocation`; deletes it only when it holds `wheelbri.exe` and no `install.ps1`, i.e. never a
  package; runs from a copy in `%SystemRoot%\Temp\wheelbri-uninstall-<guid>` that only Administrators may change; from the package it cleans up an install whose folder is gone), `Setup.cmd`
  (Setup.exe's bootstrap: unzips and runs `install.ps1`, waiting for it), `README.txt` (for the end user). The
  installed app runs the driver path by default (virtual G920, rig hidden on every Start; "G920 in-game shim" on
  the Mapping tab switches); the shim ships in `shim\`. The package mirrors the repo layout (`driver\...\x64\Release\<name>\`), because the
  driver scripts and the app's `ensureHideFilter` look there. Only Windows' own tools: no third-party installer.
  When a new file must ship, add it to `package.ps1` and to the required list in `install.ps1`.
- `scripts/`: `telemetry_summary.py` (reads telemetry columns by name); `linux/` (start/stop and Steam launch wrapper `wheelbri.sh`, udev rules,
  `package.sh` bundle).
- `config/`: `g920-virtual-device.ini`, `g920-in-game-shim.ini` (service: the target and its default mapping),
  `wheelbri-shim.ini` (shim).
- `docs/`: `phase5-windows-virtual-device.md` (Phase 5 design and step 0 lessons), `linux.md` (Steam Deck guide),
  `future-functionalities.md` (worked-out ideas not built yet: running as a standard user, RS Handbrake, G29).
- `tests/`: GoogleTest for `core/` (`wheelbri_tests`), plus `UhidG920Test` on Linux. Use fakes of the interfaces
  (see `PipelineTest.cpp`), not real devices or sockets, except `UdpLoopbackTest`.

## Conventions

- No third-party runtime tools or drivers (HidHide, ViGEm, HIDMaestro): what the
  emulator needs on the PC is built in this repository.

- Style: C++20, 4-space indent, `camelCase` functions and variables, `PascalCase` types, `trailing_` members,
  `kConstant` constants, `#pragma once`, `[[nodiscard]]` on queries, `const` parameters in definitions,
  float literals as `1.0F`. Headers in `core/include/wheelbri/`, included as `"wheelbri/X.hpp"`.
- Code on the 1 kHz loop (`Mapper::map`, `Pipeline::tick`, `DeviceProfile::encode`) must stay free of
  allocations and blocking calls.
- **Logging** (full rules in `core/include/wheelbri/Logging.hpp` and README):
  - Always log through the `WE_LOG_*` macros, never `printf`/`std::cout`. The exceptions are `--list`,
    `--print` and `--ffb-test` output, which are user interface.
  - Use one named logger per component, fetched via a function-local static `log()` helper. Component names
    are dotted (`core.net`, `service.ffb`, `shim.device`); add new ones to the README list.
  - Hot paths must use `logging::CallSampler` or `logging::RateLimiter` / `WE_LOG_*_LIMITED`.
  - Levels: trace (per call/tick), debug (decisions), info (lifecycle), warn (recoverable), error (feature
    broken), critical (process cannot continue).
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
  (`max_magnitude` 0.8, slew limit). Game effects reach the service as `FfbPacket`s (shim, driver or
  uhid). The only way around the limiter is the opt-in `force_feedback = passthrough` in `wheelbri-shim.ini`;
  keep it opt-in and don't add others.
- Files: everything the app saves (configs, rigs, device names, app.ini, the shim games list, logs, telemetry, the
  uninstaller's config backup) is per user, under `%LOCALAPPDATA%\wheelbri` (XDG folders on Linux), also when it runs
  as Administrator. Only drivers, virtual devices and their registry entries are system-wide. Never add a file
  location under ProgramData, Program Files or HKLM for settings. End users are standard users; the developer runs
  as Administrator.
- Security, while running as Administrator (`platform::isElevated()`): everything under `%LOCALAPPDATA%\wheelbri`
  can be written by any program of the user, so nothing from there may choose a privileged action. Log and telemetry
  paths from a config must be inside the user's WheelBri folders (`isInsideUserFolders`, else ignored with a
  warning); the shared log folder is always `logsFolder()`; helper programs come from fixed system places
  (`platform::systemFolder()`, Program Files), not PATH; the app refuses to start when one of its user folders is a
  link (`userFoldersProblem`) and deletes links among its log files (`removeLinkedLogFiles`). As a standard user
  these checks are off (they would protect nothing). Keep new code to these rules.
- Licence: nothing is copied from GIMX (GPLv3). If GIMX code is ever copied rather than reimplemented, the
  project must become GPLv3, so flag that before doing it.
