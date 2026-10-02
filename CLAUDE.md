# CLAUDE.md

This file guides Claude when working in this repository.

## Working with the user

- After every change, finish with **how to build it** and **how to test it** (what to run, what to look for,
  what a failure looks like), even when the change needs no rebuild (say so).
- Build and test steps use the scripts in `scripts/make/` (`build.sh`, `rebuild.sh`, `test.sh`, `check.sh`,
  `ui.sh`, `app.sh`, `shim.sh`, `bridge.sh`, `drivers.sh`, `all.sh`; see `scripts/make/README.md`) as the shortcut
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
  that shell unless the step needs PowerShell (drivers, `dotnet`), and say which shell.
- **Before every commit**, write down why the change was made: the commit message gets a short headline and a
  few lines of reasons.
- **When a functionality is finished** (it may take several commits), add one entry for it under
  `## Functionalities` in README.md, in the commit that completes it, not before. Keep it short: a `###` headline,
  then **Why** (the reason), **Use** (how to use it, settings) and **Look** (where to see it: UI, logs and
  telemetry columns, code files). Ask the user whether a functionality is finished if it's unclear.
- Before guessing at a mapping or FFB problem, read the user's logs and telemetry
  (`%LOCALAPPDATA%\wheelemu\logs`: `wheelemu-service-<date>.log`, `telemetry-<date>.csv`, `ffb-<date>.csv`) and
  run `scripts/telemetry_summary.py`. The `raw_a0`..`raw_a7` columns show which axis a control really moves.

## Project

wheel-emulator makes a physical wheel (Moza R3) look like a wheel a game supports (Logitech G920 by default, G29
via `target_profile`) on Windows and Linux/Proton. First target game: Need for Speed (2015).

Status (README.md has the full table):
- **Working with a real R3 in NFS 2015 on Windows**: mapping and service (Phase 1), the `dinput8.dll` shim
  (Phase 2: wheel, pedals, buttons, D-pad), game force feedback through the service (Phase 3: constant, spring,
  damper, sine, square), and the virtual G920 with our own UMDF driver (Phase 5).
- **Settled**: NFS finds wheels through `DirectInput8Create` (A and W) and reads `c_dfDIJoystick2`: wheel X,
  throttle Slider, brake Y, clutch Rz, pedals released at the top of the range. R3 axes as the service numbers
  them: steering 0, throttle 2, brake 5, clutch 6 (add-on clutch pedal, checked from telemetry).
- **Not yet verified**: the G29 profile in a game; Linux/Proton (Phase 4 `wheelemu-uhid` is unit-tested only, not
  tried on a Steam Deck); effect envelopes (ignored); NFS's 20-byte Logitech `Escape` calls (not forwarded).
- The G920's own FFB is HID++, not the classic Logitech protocol; the virtual G920 exposes HID PID instead.

Data flow: `Moza R3 --DirectInput/evdev--> wheelemu-service --UDP 127.0.0.1--> consumer`, where the consumer is
one of: the `dinput8.dll` shim in the game, the `wheelemu-g920` driver (Windows), the HIDMaestro bridge
(`tools/vbridge`, Windows, step 0) or `wheelemu-uhid` (Linux). Use UDP loopback, not shared memory: under Proton
the shim runs inside Wine and the service is a native Linux process, and named mappings don't cross that boundary.

UDP ports (all 127.0.0.1):

| Port | Direction | What |
|---|---|---|
| 41329 (`state_port`) | service → consumer | `StatePacket`; only one consumer at a time |
| 41330 (`ffb_port`) | consumer → service | `FfbPacket` (game force feedback) |
| 41331 | ClutchProbe → service | `ClutchPacket` test clutch (`TestClutch.hpp`) |
| 41332 | service → `wheelemu-xusb` | Xbox pad state (`XboxPad.hpp`) |

## Build and test

CMake ≥ 3.24, C++20. spdlog 1.15.3 and GoogleTest 1.15.2 come in through FetchContent (needs internet on the first
configure). The service uses only the system's APIs and runs on Windows (DirectInput, Win32) and Linux (evdev,
Xlib; skipped with a warning without libx11-dev). No SDL or other third-party library: do not add one back.
`service/src/WheelSdl.cpp` is dead code from before that decision and is not compiled.

The scripts in `scripts/make/` wrap everything below (`scripts/make/README.md`): `build.sh` (incremental,
configures the first time), `rebuild.sh` (deletes `build/`, full rebuild), `test.sh [filter]`, `check.sh` (before a
commit), `ui.sh [config]`, `app.sh [--no-start]`, `shim.sh [--off] <game folder>`, `bridge.sh`, `drivers.sh [-Install]`, `all.sh [--clean]`.
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
./build/tests/wheelemu_tests --gtest_filter='Pipeline.*'
```

Outputs: `build/service/wheelemu-service.exe` (command line), `build/service/wheelemu.exe` (the app: window with
Start / Stop / Open logs, asks for Administrator) and `build/shim/dinput8.dll`; all linked statically on MinGW. `scripts/wheelemu.sh --build` rebuilds the service and the bridge.

Windows shim cross-compiled from Linux/macOS with MinGW-w64 (→ `build-win/shim/dinput8.dll`):

```bash
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake -DCMAKE_BUILD_TYPE=Release -DWE_BUILD_SERVICE=OFF -DWE_BUILD_TESTS=OFF
```

Drivers (`driver/`, see `driver/README.md`) are built with Visual Studio + WDK, not MSYS2, from a Developer
PowerShell, then installed from an Administrator PowerShell, e.g.:

```powershell
msbuild driver\wheelemu-g920.vcxproj /p:Configuration=Release /p:Platform=x64
```

```powershell
driver\install-driver.ps1
```

(same pattern for `driver\hide\wheelemu-hide.vcxproj` + `install-hide.ps1` and `driver\xusb\wheelemu-xusb.vcxproj`
+ `install-xusb.ps1`). C# tools (.NET 10) in `tools/vbridge`: `dotnet build tools/vbridge/wheelemu-bridge.csproj -c
Release`; `dotnet run --project tools/vbridge/SelfTest` checks `Wire.cs` and runs on any OS.

CMake options: `WE_BUILD_TESTS` (ON; skipped when cross-compiling), `WE_BUILD_SERVICE` (ON), `WE_BUILD_SHIM` (ON
only on Windows targets), `WE_BUILD_UHID` (ON only on Linux). Warnings: `/W4 /permissive-` on MSVC, `-Wall -Wextra
-Wpedantic` elsewhere. Keep new code free of warnings.

Service CLI (`service/src/main.cpp`): `wheelemu-service [--config FILE] [--list] [--print] [--ui] [--ffb-test]
[--old-files delete|archive|keep] [--stop-file FILE] [--virtual-g920 [--no-hide] [--xbox-pad buttons|idle|off]]
[--unhide] [--app]`. `--app` opens the app's window (Start / Stop / Open logs) from a console. `--ui` opens a window (`service/src/MappingView.cpp`, also the app's live view) with tabs along the bottom
(click, or M / L / T / F): **Mapping** (the axes R3 -> G920, every R3 axis, the D-pad, the G920 buttons written
`G920 <n> <name> <- R3 <n> <name>` and the R3's own buttons `R3 <n> <name>` from `[button_labels]`), **Logs** (one
line per component: emulator, game shim, G920 driver, hide filter, Xbox driver, bridge, each in its own colour,
with today's file, size, age, click opens its folder; then a scrollable live log of all those files merged:
wheel, arrows, PageUp / PageDown, Home / End; core `LiveLog`), **Telemetry** (the CSV files, buttons held, the
latest button / D-pad / pedal / FFB events, not scrollable) and **Force feedback** (live effects, usage since
start, the latest effects). O opens the logs folder, D the drivers' folder. It draws through
`UiWindow.hpp` (filled/outlined rectangles, 8x8-cell monospaced text and clicks on a 960x748 logical canvas,
letterboxed), implemented natively: `UiWindowWin32.cpp` (Win32 + GDI, DPI aware) on Windows, `UiWindowX11.cpp`
(Xlib) on Linux.

The app (`wheelemu`, `service/src/App.cpp`, same sources plus `WHEELEMU_APP`; on Windows a GUI-subsystem exe
with an Administrator manifest, `service/app/`) runs sessions (`runSession` in `main.cpp`) on a thread of its
own from Start / Stop buttons; sessions show themselves through `UiHost` (`SessionUi.hpp`): the app's window,
or `makeSessionWindow()` for `--ui`. `MappingView` only draws into a window it does not own.

## Layout and architecture

- `core/` (`wheelemu_core`, static library): portable, **no OS headers and no SDL**. The one exception is
  `UdpLoopback.cpp`, which has `#ifdef _WIN32` socket code. Headers live in `core/include/wheelemu/`,
  namespace `wheelemu`.
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
  - `HidG920` is the virtual G920's HID descriptor and input report (from HIDMaestro's builder, MIT);
    `HidPid` is device-side HID PID decoding. Both are shared by `driver/`, `tools/uhid` and the tests.
  - `LiveLog`: follows the components' log files (tail on first read, then what is appended) and parses
    spdlog and `[time] text` lines for the Logs tab; `LiveLogTest` covers it with temporary files.
  - `XboxPad` (`XboxMirror`) mirrors the wheel onto an Xbox 360 pad for button prompts. `HidHideCli` parses
    `HidHideCLI.exe` output.
  - `TestClutch`: a test clutch for when no pedal is connected. `tools/vbridge/ClutchProbe` sends `ClutchPacket`s
    (12 bytes, magic `WECL`, v1) to 41331; the service uses that value instead of the mapped clutch until 1.5 s
    after the last packet. `ClutchPacket` must stay byte-compatible with `ClutchPacket` in `tools/vbridge/Wire.cs`.
  - Config: `IniDocument` (minimal INI; `;`/`#` start a comment anywhere on a line) → `loadEmulatorConfig`,
    which collects *all* errors (unknown keys and sections, out-of-range values) instead of failing fast. When
    you add a config key, add it to the reader, `EmulatorConfig`, both example INIs and `EmulatorConfigTest`.
  - `EmulatedJoystick` is the emulated wheel as DirectInput sees it (objects, the game's data format,
    ranges, state bytes, buffered changes). It mirrors DirectInput constants without `<dinput.h>` so it is
    unit-tested on Linux; `shim/src/DiConvert.hpp` static_asserts the constants against the real macros.
  - `TelemetryRecorder` (`Telemetry.hpp`): daily CSVs in the log folder (`telemetry-YYYY-MM-DD.csv`, a row when
    something changes, at most `rate_hz` = 50/s, plus a heartbeat every `heartbeat_s` = 60; `ffb-YYYY-MM-DD.csv`
    one row per game FFB change), each session appended after a `# session started` block, rolled over at local
    midnight. Columns come from `telemetryColumns()`; when you add one, write it in `writeSample` in the same
    order and bump `kFormatVersion` (`Telemetry.cpp`, now "2") if a column moves.
  - Logs are daily too (`logging::init` adds the date: `wheelemu-service-YYYY-MM-DD.log`); the shim logs to
    `%LOCALAPPDATA%\wheelemu\logs\wheelemu-shim-YYYY-MM-DD.log`. `LocalDate.hpp` has the date helpers.
  - `Retention.hpp` decides which files are old (date in the name, else last-write day) and only ever touches
    files starting `wheelemu-service`, `wheelemu-shim`, `wheelemu-bridge`, `wheelemu-logs-`, `telemetry-`, `ffb-`.
    The service's `Housekeeping` deletes them (default, after 7 days) or archives them with the best compressor
    found (7z > xz > zstd > zip > gzip) as a background process via `platform::ChildProcess`, deleting
    originals only after the compressor exited 0.
  - Wire format (`StatePacket.hpp`): 33 bytes, little-endian, magic `WEMU`, version 1. Any layout change
    must bump `kVersion` and update the layout comment, `StatePacketTest`, `tools/vbridge/Wire.cs` and the driver.
- `service/` (`wheelemu-service`, namespace `wheelemu::service`):
  - The wheel behind `Wheel.hpp` (`WheelDinput.cpp` DirectInput + `DinputReader`, `WheelEvdev.cpp` evdev),
    numbering inputs as the service always has (`DinputReader`, core `EvdevLayout`) and sending the same force
    feedback numbers as before (core `WheelOutput`).
  - OS helpers in `Platform.hpp` (clock, precise sleep, thread priority, child process); `FfbSelfTest`
    (`--ffb-test`); `Housekeeping` (retention, pumped from the loop); `MappingView` (`--ui`); a fixed-rate loop
    (default 1 kHz, drift-free, resynchronises when behind).
  - Phase 5 (`--virtual-g920`, run as Administrator, no shim in the game, no bridge running): `VirtualG920`
    creates the `wheelemu\G920` software device for the `wheelemu-g920` driver while the service runs and keeps
    the DirectInput "driving" registry entry right (and Logitech's FFB entry out); `HideFilter` sets
    `HKLM\SOFTWARE\wheelemu\hide\AllowedPid` for the `wheelemu-hide` filter so only the service sees the R3
    (`HidHide` via `HidHideCLI.exe` is the fallback; `--no-hide` skips it, `--unhide` clears it after a crash);
    `VirtualXboxPad` creates the `wheelemu\XUSB` device and the loop sends pad state to 41332.
- `shim/` (`dinput8.dll`, Windows/Wine only, namespace `wheelemu::shim`): proxies `IDirectInput8A/W`,
  devices and effects. It is written once as templates over `AnsiTraits`/`WideTraits` (`Traits.hpp`).
  Exports are renamed through `dinput8.def` (`Proxy_*`) so they don't clash with SDK declarations. The real
  DLL is loaded from the system directory on first use (`RealDInput.cpp`). Phase 2: the physical wheel
  (matched by USB VID in `EnumDevices` and again in `CreateDevice` via `GetDeviceInfo`) is presented as the
  target profile by `EmulatedDevice<T>`, which answers input calls from `StateFeed` (UDP receiver, neutral
  state when stale, default after 1500 ms). Phase 3: its effects are `EmulatedEffect`s whose parameters
  `FfbForwarder` sends to the service (timing, keep-alives); in that mode the physical device is opened
  non-exclusively because the service owns its force feedback. Other devices stay `ProxyDevice<T>`
  (log and forward). Settings: optional `wheelemu-shim.ini` next to the DLL (`ShimConfig`).
- `driver/` (Windows, UMDF 2, self-signed test certificate, built with VS + WDK; see `driver/README.md`):
  - `wheelemu-g920` (`Driver.cpp`): HID minidriver for the virtual G920 (046D:C262). Reads `StatePacket`s on
    41329, decodes HID PID output reports into `FfbPacket`s to 41330. Compiles core's `HidG920`, `HidPid`,
    `StatePacket`, `FfbPacket` sources. Logs to `C:\ProgramData\wheelemu\logs\wheelemu-driver-<date>.log`.
  - `hide/` (`wheelemu-hide`): upper filter on the R3's HID interface (`HID\VID_346E&PID_0005&MI_02`); only the
    process in `AllowedPid` may open it. Replaces HidHide.
  - `xusb/` (`wheelemu-xusb`): virtual Xbox 360 controller (XInput only) fed from 41332.
- `tools/vbridge/` (C#, .NET 10, Windows): Phase 5 step 0, superseded by `driver/` but kept (mainly for the
  Xbox controller button prompts; `scripts/wheelemu.sh` still starts this path). HIDMaestro virtual G920 fed
  from the service's `StatePacket`s; decodes HID PID reports (`PidForwarder.cs`) into `FfbPacket`s. `Wire.cs`
  must stay byte-compatible with `core/src/StatePacket.cpp`, `FfbPacket.cpp` and `TestClutch.cpp`; `SelfTest/`
  checks the portable parts with `dotnet run` (any OS, no NuGet packages needed). Probes (need the service
  running): `FfbProbe/` plays DirectInput effects on the virtual G920; `ClutchProbe/` sends the test clutch and
  reads Rz back (`steps`, `sweep`, `hold`). Reports go to `%LOCALAPPDATA%\wheelemu\logs`.
- `tools/uhid/` (C++, Linux only, `WE_BUILD_UHID`): Phase 4, `wheelemu-uhid`. The same virtual G920 as the bridge
  (`core` `HidG920` descriptor, `HidPid` device-side PID decoding) through the kernel's uhid; Proton games read it
  raw with `PROTON_ENABLE_HIDRAW=0x046D/0xC262`. `UhidG920` holds the uhid protocol with no file descriptors so
  `UhidG920Test` covers it; `main.cpp` is the fd/UDP loop.
- `scripts/make/`: the build scripts (above); `common.sh` holds the shared helpers.
- `scripts/`: `wheelemu.sh` (Windows, MSYS2 UCRT64: starts the service with `--ui` and the bridge as
  Administrator, stops both on Enter; `--build`, `--no-ui`, `--xbox-pad`, `--config`); `telemetry_summary.py`
  (reads telemetry columns by name); `linux/` (start/stop and Steam launch wrapper `wheelemu.sh`, udev rules,
  `package.sh` bundle).
- `config/`: `moza-r3-as-g920.ini`, `moza-r3-as-g29.ini` (service), `wheelemu-shim.ini` (shim).
- `docs/`: `phase5-windows-virtual-device.md` (Phase 5 design and step 0 lessons), `linux.md` (Steam Deck guide).
- `tests/`: GoogleTest for `core/` (`wheelemu_tests`), plus `UhidG920Test` on Linux. Use fakes of the interfaces
  (see `PipelineTest.cpp`), not real devices or sockets, except `UdpLoopbackTest`.

## Conventions

- Style: C++20, 4-space indent, `camelCase` functions and variables, `PascalCase` types, `trailing_` members,
  `kConstant` constants, `#pragma once`, `[[nodiscard]]` on queries, `const` parameters in definitions,
  float literals as `1.0F`. Headers in `core/include/wheelemu/`, included as `"wheelemu/X.hpp"`.
- Code on the 1 kHz loop (`Mapper::map`, `Pipeline::tick`, `DeviceProfile::encode`) must stay free of
  allocations and blocking calls.
- **Logging** (full rules in `core/include/wheelemu/Logging.hpp` and README):
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
  - The shim links statically (see the toolchain file). Don't add runtime DLL dependencies, and never link
    `dinput8`.
  - COM wrappers own one real reference and delete themselves at refcount 0. Anything handed back unwrapped
    (aggregation, `EnumDevicesBySemantics`, `DllGetClassObject`, `EnumCreatedEffectObjects`) is logged
    with a warning on purpose.
- FFB safety: the R3 is direct drive. Every force the service sends to hardware must go through
  `FfbSafetyLimiter` (in `FfbController`, which also has a watchdog); keep the defaults conservative
  (`max_magnitude` 0.8, slew limit). Game effects reach the service as `FfbPacket`s (shim, driver, bridge or
  uhid). The only way around the limiter is the opt-in `force_feedback = passthrough` in `wheelemu-shim.ini`;
  keep it opt-in and don't add others.
- Licence: nothing is copied from GIMX (GPLv3). If GIMX code is ever copied rather than reimplemented, the
  project must become GPLv3, so flag that before doing it.
