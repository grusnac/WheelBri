# CLAUDE.md

Guidance for Claude Code in this repo.

## Working with the user

- `scripts/make/*.sh`: build/test shortcuts; each header says what it does. A new one sources `common.sh` and goes in
  the list below. README never uses them (plain CMake there).
- Driver steps: PowerShell. Everything else: MSYS2 UCRT64, repo root.
- User docs: a few lines in `docs/user-guide.md`; a new screen needs a screenshot in `docs/images/` (ask the user to
  take it). README status: its status table.
- Logs: `%LOCALAPPDATA%\wheelbri\logs` (`wheelbri-service-*`, `telemetry-*`, `ffb-*`, one file per run);
  `scripts/telemetry_summary.sh` sums them up. `raw_a0`..`raw_a7` = which axis a control moves.

## Project

Makes the user's rig (wheel + accessories, Rig tab) look like a Logitech wheel to games, Windows and Linux/Proton.
Targets: G920 (default), G923, G25, G27; the user cannot add targets.

- README: status table, "Targets" (ids, button counts, button-order certainty), "G HUB and HID++" (G923).
- Verified: direct drive wheel, NFS 2015, Windows, via shim and via virtual G920.
- Not verified: G923/G25/G27 in a game, Linux/Proton on Steam Deck, shim on Windows 7.

Data flow: `rig --DirectInput/evdev--> wheelbri-service --UDP 127.0.0.1--> consumer`. Consumer: `dinput8.dll` shim in
the game (Mode Shim), `wheelbri-wheel` driver (Windows, Mode Virtual) or `wheelbri-uhid` (Linux). UDP, not shared
memory: under Proton the shim is in Wine, the service native Linux.

| Port | Direction | What |
|---|---|---|
| 41329 (`state_port`) | service → consumer | `StatePacket`; `LogLevelPacket` (`WELV`) on the same socket; one consumer at a time |
| 41330 (`ffb_port`) | consumer → service | `FfbPacket` (game FFB) |

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
(window only), `shim.sh [--off] <game folder>` (put/remove shim), `shim-x86.sh`, `logi.sh` (Logitech SDK stand-in
DLL), `drivers.sh`, `all.sh`, `package.sh` (installer), `clean-pc.sh` (removes all WheelBri put on the PC),
`measure.sh` (CPU, memory, handles from outside).

- New things left on the PC by app, installer or drivers → add to `clean-pc.ps1`.
- Performance: measure with `measure.sh`, don't add counters to the app.

Options: `WE_BUILD_TESTS`, `WE_BUILD_SERVICE`, `WE_BUILD_SHIM` (Windows), `WE_BUILD_LOGI` (Windows x64),
`WE_BUILD_UHID` (Linux), `WE_BUILD_FUZZ` (`wheelbri_fuzz`, Clang only; see `tests/CLAUDE.md`).

Outputs: `build/service/wheelbri.exe` (app), `wheelbri-service.exe` (CLI; usage in `printUsage`,
`service/src/main.cpp`), `wheelbri-helper.exe`, `build/shim/dinput8.dll`, `build/logi/wheelbri-logi.dll`,
`build/tools/uhid/wheelbri-uhid` (Linux). Warnings: `-Wall -Wextra -Wpedantic` (MSVC `/W4 /permissive-`).

## Architecture

- **`core/`** (`wheelbri_core`): portable, no OS headers except `UdpLoopback.cpp`. Headers `core/include/wheelbri/`,
  namespace `wheelbri`.
  - Pipeline: `RawInputState` → `Mapper` → `WheelState` (target-agnostic) → `IDeviceProfile::encode` →
    `TargetReport` → `StatePacket` → `IStatePublisher`. `Pipeline::tick()` = one pass. Test seams: `IInputSource`,
    `IFfbSink`, `IStatePublisher`, `IStateReceiver`.
  - New target = `DeviceProfileSpec` registered in `makeProfile()` + `VirtualWheel` entry (`HidWheelDevice`).
  - FFB: every source → `FfbEffect` (`DiEffect` from DirectInput, `HidPid` from HID PID). `FfbController` holds one
    stateful `FfbSafetyLimiter` per effect + the watchdog. Senders work out envelopes/ramps (`FfbShape.hpp`); the wire
    carries levels only.
  - Game profiles (`config/games/*.ini`, compiled in) hold **only** what a game changes from the target's defaults,
    laid over them (`GameProfiles`, `overlay`); a profile with any problem is not used. User's per-game values:
    `GameTuning`; may lower limits, never raise.
- **`service/`**: app (`App.cpp`, `WHEELBRI_APP`) and CLI share sources.
  - `runSession` (`main.cpp`) runs a session on its own thread.
  - **Loop** sleeps in `platform::WaitSet` until devices, sockets or a timer have news; `EventLoopControl` decides
    waking vs falling back to polling, `WakePlanner` plans wakes. Regular work goes by the clock, never pass counts.
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
- **`installer/`, `scripts/make/package.ps1`**: Windows package. A new shipped file goes in `package.ps1` and
  `install.ps1`'s required list.
- **`tests/`**: GoogleTest + `wheelbri_ui_tests`; details in `tests/CLAUDE.md`.
  - A loop change comes with a scenario in `tests/emulators/PushEmulators.hpp` (`PushLoopTest`); the rig's loop stays
    in step with `runLoop`.
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

- Dead zones, a steering ratio (always 1:1), a virtual Xbox controller.
- Auto-map beyond the core inputs (`AutoMap.hpp`: steering, pedals, D-pad, A/B/X/Y, paddles, from the Rig's "It is"):
  every other button is the user's to map; Invert is never taken from the Rig.
- HidHide, ViGEm, HIDMaestro or any third-party runtime tool or driver.
- Any SDK for the wheel's own software: WheelBri never reads or sets it.
- Hiding Steam's virtual pad on the Deck, or advising to turn Steam Input off.
- Anything privileged without the user pressing Start.
- Advice to restart Windows for the hide filter: say close what uses the wheel, or replug it.

## Conventions

- **Names:** **WheelBri** where the user sees it, `wheelbri` elsewhere. Never `wheelemu`, except where old installs are
  found and removed; wire tags `WEMU` / `WEFB` / `WELV` and `WE_LOG_*` keep their letters. Short game names ("NFS
  2015", never "Need for Speed ..."). Labels ≤ 20 characters.
- **C++:** C++20, 4-space indent, `camelCase` functions/variables, `PascalCase` types, `trailing_` members,
  `kConstant` constants, `#pragma once`, `[[nodiscard]]` on queries, `const` parameters in definitions, `1.0F`
  literals, headers as `"wheelbri/X.hpp"`. Comments: `//` for intent and constraints, as in the headers (no Doxygen).
- **Hot path** (`Mapper::map`, `Pipeline::tick`, `encode`): no allocations, no blocking calls.
- **Threads:** name each first thing with `platform::nameThisThread("wheelbri <role>")`.
- **Logging** (full rules: `Logging.hpp`): only `WE_LOG_*`, one dotted logger per component, never printf; hot paths
  use `CallSampler` / `RateLimiter`; default level warn; every file WheelBri writes is a log in that line format (no
  CSV, no JSON); one file per run `<stem>-<date>-<NN>.log`; one folder for all logs (`logsFolder()`), no other.
- **FFB safety** (rigs often direct drive): every force to hardware goes through `FfbSafetyLimiter` + watchdog. Keep
  the guards: slew 50/s, frozen-force fade, DiEffect's "not a force" values. Only bypass: opt-in
  `force_feedback = passthrough` in `wheelbri-shim.ini`; add no others.
- **Files:** everything the app saves is per user under `%LOCALAPPDATA%\wheelbri` (XDG on Linux); never ProgramData,
  Program Files or HKLM. User files via `FileText.hpp` (temp file + rename).
- **Security when elevated:** nothing under `%LOCALAPPDATA%\wheelbri` may choose a privileged action; config paths
  must be inside the user's folders (`isInsideUserFolders`); helper programs from fixed system places, never PATH;
  links refused.
- **Licence:** nothing copied from GIMX (GPLv3); flag it before ever doing that.
