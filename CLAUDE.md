# CLAUDE.md

Guidance for Claude Code in this repo.

## Role

Work as a senior C++ developer who writes Linux and Windows drivers: kernel and user-mode (KMDF/UMDF, WDK),
HID minidrivers and filter drivers on Windows; evdev, uhid/hidraw and kernel modules on Linux. Bring that judgement
to every change, including outside `driver/` and `tools/uhid/`.

Know game input APIs in all their versions and forms: DirectInput (`dinput` 1 to 7 and `dinput8`, ANSI and
Unicode interfaces, effects and force feedback), XInput (1.1, 1.3, 1.4, 9.1.0; the XUSB driver), and the newer
paths games also use (Raw Input, Windows.Gaming.Input, GameInput, SDL, Steam Input). Know every kind of gaming
device a game can talk to: wheels, pedals, shifters, handbrakes, button boxes, joysticks, HOTAS, gamepads, arcade
sticks; over HID, HID PID force feedback and vendor protocols (e.g. Logitech HID++ and its wheel SDK).

## Area rules

Some rules live in area files that load on their own only once a file there is touched. When a task involves an area
below (planning, answering, building, running, editing), read its file **before** doing that, unless it is already
loaded. Read only the section you need (`grep -n '^#'` first).

| When the task involves… | Read first |
|---|---|
| Building, installing or changing the drivers; the hide filter driver | `driver/CLAUDE.md` |
| Shim or logi code, the 32-bit build, anything running inside the game | `.claude/rules/in-game-dll.md` |
| Windows, views, layout, colours, shapes, Shadow view, UI tests | `.claude/rules/ui.md` |
| Writing or running tests, fuzzing, uhid tests | `tests/CLAUDE.md` |
| PowerShell scripts, installer, packaging | `.claude/rules/powershell.md` |
| Linux: uhid, `scripts/linux/` (wheelbri.sh, Deck build), Proton, Steam Deck testing | `.claude/rules/linux.md` |

A new area file gets a row here.

## Working with the user

- `scripts/make/*.sh`: build/test shortcuts; each header says what it does. A new one sources `common.sh` and goes in
  the list below. The docs never use them (plain CMake in `docs/building.md`).
- Commands for the user: MSYS2 UCRT64, repo root, for everything (copy with `cp`, not `Copy-Item`). PowerShell or CMD
  only when there is no other way, and say why.
- Git commands for the user: one command per code block, so each can be copied and run on its own (no `&&`
  chains, no several commands in one block). The exception: `git add` and its `git commit` always go together, as
  one `git add <files> && git commit -m "<message>"` in one code block that runs as pasted.
- After changing files, end by listing the ones you changed that are not staged yet and say they still have to be
  committed, with that one `git add ... && git commit ...` block for them.
- Docs: short install, status and what's new in `README.md` (history in `CHANGELOG.md`); full install, uninstall and
  scripted options in `docs/install.md`; features in `docs/capabilities.md`; usage in `docs/user-guide.md`; per game
  in `docs/games.md`; choosing a Target in `docs/targets.md`, each Target's layout in `docs/target-<target>.md`;
  build in `docs/building.md`. `docs/` stays flat (the packages copy `docs/*.md` only). Short, current facts only, no
  dated history. A new screen needs a screenshot in `docs/images/` (ask the user to take it).
- Logs: `%LOCALAPPDATA%\wheelbri\logs` (`wheelbri-service-*`, `telemetry-*`, `ffb-*`, one file per run);
  `scripts/telemetry_summary.sh` sums them up. `raw_a0`..`raw_a7` = which axis a control moves. A folder of logs
  (a test round): decide what would answer the question, then find only that: `scripts/run_summary.sh <folder>`
  (index), `--find pedals|game|input|state|ffb|problems`, `--grep <regex>`. Never dump whole logs.

## Project

Makes the user's rig (wheel + accessories, Rig tab) look like a Logitech wheel to games, Windows and Linux/Proton.
Targets: G920 (default), G923, G25, G27; the user cannot add targets.

- Verified (Windows 11, Moza R3): all four targets in Mode Virtual in ACC and DiRT Rally 2.0; G920 in NFS 2015, Payback,
  Heat and Unbound (Mode Shim too, except Payback); G25 in Race Driver: GRID, both Modes. Steam Deck (SteamOS 3.8, Proton,
  Game Mode too): ACC all four targets (Virtual); NFS 2015 (G920) and GRID (G25), both Modes.
  `README.md` Status is the source: change it there first, then here.
- Not verified: DiRT Rally 2.0 and NFS Heat, Unbound, Payback on Linux; Windows 7, 8, 8.1 and 10; button orders on
  real Logitech wheels.
- Mode Shim is for older DirectInput games only; ACC, DiRT Rally 2.0 and games using Logitech's SDK DLL don't take it.

Data flow: `rig --DirectInput/evdev--> wheelbri-service --UDP 127.0.0.1--> consumer`. Consumer: `dinput8.dll` shim in
the game (Mode Shim), `wheelbri-wheel` driver (Windows, Mode Virtual) or `wheelbri-uhid` (Linux). UDP, not shared
memory: under Proton the shim is in Wine, the service native Linux.

| Port | Direction | What |
|---|---|---|
| 41329 (`state_port`) | service → consumer | `StatePacket`; `LogLevelPacket` (`WELV`) on the same socket; one consumer at a time |
| 41330 (`ffb_port`) | consumer → service | `FfbPacket` (game FFB) |
| 41331 (`kTrueForceStatePort`) | service → driver | the G923's TrueForce interface: steering state (`TrueForce.hpp`) |

## Build and test

CMake ≥ 3.24, C++20. spdlog + GoogleTest via FetchContent (first configure needs internet). No SDL or other
third-party library (don't add one). Service uses system APIs only: DirectInput + Win32 / evdev + Xlib.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # first time
cmake --build build
ctest --test-dir build                                    # -R Mapper for some
./build/tests/wheelbri_tests --gtest_filter='Pipeline.*'
```

32-bit shim (only the shim is ever 32-bit): `.claude/rules/in-game-dll.md`.

Drivers (VS + WDK, PowerShell): `driver/CLAUDE.md`.

`scripts/make/`: `app.sh` (incremental build + start app; `--test` runs all tests first and starts nothing on a failure,
`--no-start`, `--as-user` needs `wheelbri-helper --console`, `--config FILE`), `rebuild.sh` (clean rebuild), `ui.sh`
(window only), `shim.sh [--off] <game folder>` (put/remove shim), `shim-x86.sh`, `setup-msvcrt.sh` (the installer,
msvcrt), `logi.sh` (Logitech SDK stand-in DLL), `drivers.sh`, `all.sh`, `package.sh` (installer), `clean-pc.sh`
(removes all WheelBri put on the PC),
`measure.sh` (CPU, memory, handles from outside), `g923-capture.sh` (guided USB capture and decode of a real
G923: `tools/g923-capture/README.md`).

- New things left on the PC by app, installer or drivers → add to `clean-pc.ps1`.
- Performance: measure with `measure.sh`, don't add counters to the app.

Options: `WE_BUILD_TESTS`, `WE_BUILD_SERVICE`, `WE_BUILD_SHIM` (Windows), `WE_BUILD_LOGI` (Windows x64),
`WE_BUILD_UHID` (Linux), `WE_BUILD_FUZZ` (`wheelbri_fuzz`, Clang only; see `tests/CLAUDE.md`), `WE_BUILD_G923CAP`
(`wheelbri-g923cap`, on by default), `WE_BUILD_SETUP` (`wheelbri-setup.exe`, Windows), `WE_STRICT_TESTS` (a test
file no test program builds fails the configure).

Outputs: `build/service/wheelbri.exe` (app), `wheelbri-service.exe` (CLI; usage in `printUsage`,
`service/src/main.cpp`, mirrored in `docs/user-guide.md` "Command line"), `wheelbri-helper.exe`, `wheelbri-setup.exe`
(the installer; shipped from `build-setup/`; installed as `Uninstaller.exe`), `build/shim/dinput8.dll`,
`build/logi/wheelbri-logi.dll`, `build/tools/uhid/wheelbri-uhid` (Linux), `build/tools/g923-capture/wheelbri-g923cap.exe`.
Warnings: `-Wall -Wextra -Wpedantic` (MSVC `/W4 /permissive-`).

## Architecture

- **`core/`** (`wheelbri_core`): portable, no OS headers except `UdpLoopback.cpp` (sockets) and `LogFiles.cpp` (the
  C runtime's exclusive create on Windows). Headers `core/include/wheelbri/`, namespace `wheelbri`.
  - Pipeline: `RawInputState` → `Mapper` → `WheelState` (target-agnostic) → `IDeviceProfile::encode` →
    `TargetReport` → `StatePacket` → `IStatePublisher`. `Pipeline::tick()` = one pass. Test seams: `IInputSource`,
    `IFfbSink`, `IStatePublisher`, `IStateReceiver`.
  - New target = `DeviceProfileSpec` registered in `makeProfile()` + `VirtualWheel` entry (`HidWheelDevice`).
  - FFB: every source → `FfbEffect` (`DiEffect` from DirectInput, `HidPid` from HID PID). `FfbController` holds one
    stateful `FfbSafetyLimiter` per effect + the watchdog; each sender (UDP port) has slots and a watchdog of its own.
    Senders work out envelopes/ramps (`FfbShape.hpp`); the wire carries levels only.
  - Game profiles (`config/games/*.ini`, compiled in) hold **only** what a game changes from the target's defaults,
    laid over them (`GameProfiles`, `overlay`); a profile with any problem is not used. User's per-game values:
    `GameTuning` (pedals, buttons; one Games sub-tab, Axes and buttons). No force feedback settings per game or of
    WheelBri's own strength: the game and the wheel's own app set those; the Live tab only shows, per kind of effect,
    what the game asks and what reaches the wheel (`FfbGroups.hpp`).
- **`service/`**: app (`App.cpp`, `WHEELBRI_APP`) and CLI share sources.
  - `runSession` (`main.cpp`) runs a session on its own thread.
  - **Loop** (`ServiceLoop.cpp`, on its own thread; `runLoop` sets it up and runs the window side next to it) sleeps
    in `platform::WaitSet` until devices, sockets or a timer have news; `EventLoopControl` decides waking vs falling
    back to polling, `WakePlanner` plans wakes. Regular work goes by the clock, never pass counts. Everything it asks
    of the system goes through `LoopSystem`, the game's FFB through `LoopFfb` (so `PushLoopTest` runs it as it is).
  - Live changes: the app saves the mapping as it changes; `runLoop` reads the config again when its file changes and
    swaps in what a running session can take (`LiveMapping.hpp`: the mapping, button order; `GameWatch::setValues`:
    per-game values), via `Pipeline::setMapper`. Mappers are kept for the session. The rotation the virtual wheel
    reports (`StatePacket::rotationDeg`) follows the mapping's or the running game's (`GameWatch::rotationNow`).
    Anything else waits for the next Start and is logged.
  - App tabs: Mapping is always the editor (`MappingEditView`, running too); Live next to it is `MappingView`'s live
    page (Shadow / List), greyed while stopped. Side by side (`B`, app.ini `side_by_side`): the window's design two
    pages wide (`UiWindow::setDesignColumns`), editor and live view each in a `PaneWindow` half. The CLI `--ui`
    window has no editor: Mapping is its live page (`MappingView::setEditorTabs(false)`).
  - Privileged steps: `privileged::` (`Privileged.hpp`); `PrivilegedSteps` calls them directly when elevated, else via
    the WheelBri Helper.
  - Mode Shim starts nothing only the virtual device needs (no virtual wheel, no hide filter); keep that split.
  - The hide filter is in the rig's stack only while Mode Virtual runs (a user-mode filter costs ~half a core on every
    report): Start puts it in and is not Running until the rig is hidden; Stop takes it out (`takeHideFilterOff`).
  - UI via `UiWindow.hpp` (`UiWindowWin32.cpp` / `UiWindowX11.cpp`); rules in `.claude/rules/ui.md`.
- **`service/helper/`**: WheelBri Helper, SYSTEM Windows service over a pipe, so the app runs as a standard user.
- **`shim/`**: `dinput8.dll` proxy in the game showing the rig as the target. **`logi/`**: logging pass-through for
  Logitech's Steering Wheel SDK helper DLL; never ship Logitech's DLLs. Both run inside the game:
  `.claude/rules/in-game-dll.md`.
- **`driver/`** (UMDF 2): virtual wheel minidriver + rig hide filter; details in `driver/CLAUDE.md`.
- **`tools/uhid/`**: `wheelbri-uhid`, Linux virtual wheel via kernel uhid; Proton reads it with `PROTON_ENABLE_HIDRAW`.
- **`tools/g923-capture/`**: `wheelbri_g923cap` (portable library: USBPcap captures, TrueForce and HID++ decoding,
  reports, the read-only probe) and `wheelbri-g923cap` (the tool `g923-capture.sh` runs; the probe's HID is Windows
  only). Compares against `TrueForceDevice::parameterBits()`. Tests: `G923Cap*`.
- **`installer/setup/`**: `wheelbri-setup.exe`, the installer and (as `Uninstaller.exe`) the uninstaller, no
  PowerShell: the steps in `SetupSteps.cpp` (portable, on `SetupSystem`; tests on `SetupFakes.hpp`), the Win32 side in
  `SetupSystemWin32.cpp`. Built against `msvcrt.dll` (`setup-msvcrt.sh`, MINGW64) so it starts on a bare Windows 7, 8,
  8.1, where it installs KB2999226 from the package's `ucrt\` first. Windows 11's driver steps stay `driver\*.ps1`.
  Windows API used there: nothing newer than Windows 7 SP1.
- **`scripts/make/package.ps1`**: Windows package; `Setup.exe` is IExpress with the files flat + `package-files.txt`.
  A new shipped file goes in `package.ps1`'s `$expected` and `checkPc`'s required list (`SetupSteps.cpp`).
- **`tests/`**: GoogleTest + `wheelbri_ui_tests`; details in `tests/CLAUDE.md`.
  - A loop change comes with a scenario in `PushLoopTest`: it runs the service's own loop (`ServiceLoop`) on
    `tests/emulators` (emulated devices, the game's socket, a virtual clock; the real Pipeline and FfbController).
  - Every reader of data WheelBri doesn't control (configs, profiles, datagrams, HID reports, DirectInput values) is a
    target in `tests/fuzz/FuzzTargets.cpp`. New reader → new target.

## Checklists

- **New config key:** reader, `EmulatorConfig`, the four `config/g*.ini`, `EmulatorConfigTest`. Retired keys are still
  read and ignored (old configs load). Outside values (device names) go through `iniSafeValue`.
- **New telemetry column:** `telemetryColumns()` + `writeSample` (`telemetry_summary.sh` reads by name). Bump
  `kFormatVersion` when a key changes meaning.
- **`StatePacket` layout change:** bump `kVersion`; update the layout comment, `StatePacketTest`, the driver.
- **New helper request:** `HelperProtocol.hpp`, its checks in `requestProblem`, `HelperProtocolTest`. Answering:
  `answerMessage` (`HelperDispatchTest`). Never carries a path, command line or process id. Whatever a connection
  sets up is undone in `~HelperSession`.
- **New privileged step:** `privileged::`, a helper request, `HelperSession`, `PrivilegedSteps`. Never around the
  helper.

## Decided: do not add back

- Dead zones and pedal curves (gamma): the wheel's own app sets them. A steering ratio (always 1:1), a virtual Xbox
  controller. The anti-dead zone (`AntiDeadZone.hpp`) is not one: it makes up for a game's own dead zone.
- Force feedback strength, per-effect gains, caps or on/off per game, or WheelBri's own master gain: the game's and
  the wheel's own app's to set. The fixed safety guards stay; nothing to tune them in the app.
- Auto-map beyond the core inputs (`AutoMap.hpp`: steering, pedals, D-pad, A/B/X/Y, paddles, from the Rig's "It is"):
  every other button is the user's to map; Invert is never taken from the Rig.
- HidHide, ViGEm, HIDMaestro or any third-party runtime tool or driver.
- Any SDK for the wheel's own software: WheelBri never reads or sets it.
- Hiding Steam's virtual pad on the Deck, or advising to turn Steam Input off.
- Anything privileged without the user pressing Start.
- Advice to restart Windows for the hide filter: say close what uses the wheel, or replug it. (The installer's one
  restart, which finishes installing the drivers, is not this.)

## Conventions

- **Names:** **WheelBri** where the user sees it, `wheelbri` elsewhere; wire tags `WEMU` / `WEFB` / `WELV` and
  `WE_LOG_*` keep their letters. Short game names ("NFS 2015", never "Need for Speed ..."). Labels ≤ 20 characters.
- **C++:** C++20, 4-space indent, `camelCase` functions/variables, `PascalCase` types, `trailing_` members,
  `kConstant` constants, `#pragma once`, `[[nodiscard]]` on queries, `const` parameters in definitions, `1.0F`
  literals, headers as `"wheelbri/X.hpp"`. Comments: `//` for intent and constraints, as in the headers (no Doxygen).
- **Hot path** (`Mapper::map`, `Pipeline::tick`, `encode`): no allocations, no blocking calls.
- **Threads:** name each first thing with `platform::nameThisThread("wheelbri <role>")`.
- **Logging** (full rules: `Logging.hpp`): only `WE_LOG_*`, one dotted logger per component, never printf; hot paths
  use `CallSampler` / `RateLimiter`; default level warn; every file WheelBri writes is a log in that line format (no
  CSV, no JSON); one file per run `<stem>-<date>-<NN>.log` (the app: per Start, `logging::startNewRun`); one folder
  for all logs (`logsFolder()`), no other.
- **FFB safety** (rigs often direct drive): every force to hardware goes through `FfbSafetyLimiter` + watchdog. Keep
  the guards: slew 50/s, frozen-force fade, DiEffect's "not a force" values. A config may only make them gentler
  (`max_slew_per_second` 1-50, `frozen_force_s` 0.5-60). Only bypass: opt-in `force_feedback = passthrough` in
  `wheelbri-shim.ini`; add no others.
- **Files:** everything the app saves is per user under `%LOCALAPPDATA%\wheelbri` (XDG on Linux); never ProgramData,
  Program Files or HKLM. User files via `FileText.hpp` (temp file + rename).
- **Security when elevated:** nothing under `%LOCALAPPDATA%\wheelbri` may choose a privileged action; config paths
  must be inside the user's folders (`isInsideUserFolders`); helper programs from fixed system places, never PATH;
  links refused.
- **Licence:** GPL-3.0-or-later; every C++ file starts with `// SPDX-License-Identifier: GPL-3.0-or-later`. Nothing copied from GIMX (GPLv3); flag it before ever doing that. Code from elsewhere gets an
  entry in `THIRD-PARTY-NOTICES.md`.
