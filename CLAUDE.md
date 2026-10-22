# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Working with the user

- The `scripts/make/` scripts are the shortcuts for build and test steps. Each script's header comment says what it
  does. A new script sources `common.sh` and is listed under "Build and test" below. The README never uses these
  scripts: it gives the plain CMake commands.
- Driver steps need PowerShell. Everything else runs in MSYS2 UCRT64 from the repo root.
- When a functionality is finished, add a few lines to `docs/user-guide.md` if players use it. A new screen needs a
  screenshot in `docs/images/`: ask the user to take it. Update the README status table if what works changes.
- Before guessing at a mapping or FFB problem, read the logs in `%LOCALAPPDATA%\wheelbri\logs`
  (`wheelbri-service-*`, `telemetry-*`, `ffb-*`, one file per run) and run `scripts/telemetry_summary.sh`. The
  `raw_a0`..`raw_a7` columns show which axis a control really moves.

## Project

WheelBri makes the user's rig (a wheel plus accessories, defined in the Rig tab) appear as a Logitech wheel to games,
on Windows and Linux/Proton. Targets: G920 (default), G923, G25 and G27. The user cannot add targets.

- README.md has the status table, the targets ("Targets": ids, button counts, how sure the button order is) and the
  G923's HID++ for G HUB ("G HUB and HID++").
- Verified: a direct drive wheel in NFS 2015 on Windows, both through the in-game shim and through the virtual G920.
- Not verified yet: the G923, G25 and G27 targets in a game, Linux/Proton on a Steam Deck, the shim on Windows 7.

Data flow: `rig --DirectInput/evdev--> wheelbri-service --UDP 127.0.0.1--> consumer`.

- The consumer is the `dinput8.dll` shim in the game (Mode Shim), the `wheelbri-wheel` driver on Windows (Mode
  Virtual), or `wheelbri-uhid` on Linux.
- UDP, not shared memory: under Proton the shim runs in Wine and the service is native Linux.

| Port | Direction | What |
|---|---|---|
| 41329 (`state_port`) | service → consumer | `StatePacket`; `LogLevelPacket` (`WELV`) from the same socket; one consumer at a time |
| 41330 (`ffb_port`) | consumer → service | `FfbPacket` (game force feedback) |

## Build and test

- CMake ≥ 3.24, C++20. spdlog and GoogleTest come through FetchContent, so the first configure needs internet.
- No SDL or other third-party library: do not add one back.
- The service uses only system APIs: DirectInput and Win32 on Windows, evdev and Xlib on Linux.

Configure (first time), build, test:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build
```

```bash
ctest --test-dir build
```

```bash
ctest --test-dir build -R Mapper
```

```bash
./build/tests/wheelbri_tests --gtest_filter='Pipeline.*'
```

32-bit shim (only the shim is ever 32-bit). Needs `pacman -S mingw-w64-i686-gcc` once:

```bash
PATH="/mingw32/bin:$PATH" cmake -S . -B build-x86 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw32.cmake -DWE_BUILD_SERVICE=OFF -DWE_BUILD_TESTS=OFF -DWE_BUILD_LOGI=OFF
```

```bash
PATH="/mingw32/bin:$PATH" cmake --build build-x86 --target wheelbri_shim
```

Drivers are built with Visual Studio + WDK from a Developer PowerShell, then installed from an Administrator
PowerShell. `driver\hide\wheelbri-hide.vcxproj` + `install-hide.ps1` work the same way.

```powershell
msbuild driver\wheelbri-wheel.vcxproj /p:Configuration=Release /p:Platform=x64
```

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File driver\install-driver.ps1
```

**Shortcuts** in `scripts/make/`:

| Script | What it does |
|---|---|
| `app.sh` | Builds incrementally and starts the app. Flags: `--test` (runs every test first, starts nothing on a failure), `--no-start`, `--as-user` (needs `wheelbri-helper --console`), `--config FILE` |
| `rebuild.sh` | Full clean rebuild |
| `ui.sh` | The window only |
| `shim.sh [--off] <game folder>` | Installs or removes the shim in a game folder |
| `shim-x86.sh` | The 32-bit shim |
| `logi.sh` | The Logitech SDK stand-in DLL |
| `drivers.sh` | The drivers |
| `all.sh` | Everything |
| `package.sh` | The installer |
| `clean-pc.sh` | Removes everything WheelBri put on the PC |
| `measure.sh` | Measures CPU, memory and handles from outside the app |

- When the app, installer or drivers start leaving something new on the PC, add it to `clean-pc.ps1`.
- Measure performance with `measure.sh` rather than adding counters to the app.

**CMake options:**

- `WE_BUILD_TESTS`
- `WE_BUILD_SERVICE`
- `WE_BUILD_SHIM`: Windows only
- `WE_BUILD_LOGI`: Windows x64 only
- `WE_BUILD_UHID`: Linux only

**Outputs:**

- `build/service/wheelbri.exe`: the app
- `wheelbri-service.exe`: the CLI (usage in `printUsage`, `service/src/main.cpp`)
- `wheelbri-helper.exe`
- `build/shim/dinput8.dll`
- `build/logi/wheelbri-logi.dll`
- `build/tools/uhid/wheelbri-uhid` (Linux)

Warnings: `-Wall -Wextra -Wpedantic` (`/W4 /permissive-` on MSVC).

## Architecture

- **`core/`** (`wheelbri_core`): portable, no OS headers. The only exception is `UdpLoopback.cpp`. Headers are in
  `core/include/wheelbri/`, namespace `wheelbri`. The pipeline:
  - `RawInputState` → `Mapper` → `WheelState` (target-agnostic) → `IDeviceProfile::encode` → `TargetReport` →
    `StatePacket` → `IStatePublisher`.
  - `Pipeline::tick()` runs one pass. `IInputSource`, `IFfbSink`, `IStatePublisher` and `IStateReceiver` are the
    test seams.
  - A new target is a `DeviceProfileSpec` registered in `makeProfile()`, plus a `VirtualWheel` entry
    (`HidWheelDevice`).
  - FFB: every source is translated into `FfbEffect` (`DiEffect` from DirectInput, `HidPid` from HID PID reports).
    `FfbController` holds one `FfbSafetyLimiter` per effect (it keeps state) and the watchdog. Envelopes and ramps
    are worked out by the senders (`FfbShape.hpp`). The wire carries levels only.
  - Game profiles (`config/games/*.ini`, compiled in) hold **only** what a game changes from the target's
    defaults. They are laid over the defaults (`GameProfiles`, `overlay`), and a profile with any problem is not
    used. The user's own per-game values are `GameTuning`. They may lower limits, never raise them.
- **`service/`**: the app (`App.cpp`, built with `WHEELBRI_APP`) and the CLI share the same sources.
  - `runSession` in `main.cpp` runs a session on its own thread.
  - **The loop** sleeps in `platform::WaitSet` until the devices, sockets or a timer say there is something new.
    `EventLoopControl` decides when to wake and when to fall back to polling, and `WakePlanner` plans the wakes.
  - Anything regular on the loop goes by the clock, never by counting passes.
  - Privileged steps live in `privileged::` (`Privileged.hpp`). `PrivilegedSteps` calls them directly when the app
    is elevated, otherwise through the WheelBri Helper.
  - Mode Shim starts nothing that only the virtual device needs (no virtual wheel, no hide filter): keep that split
    for new components.
  - The UI is drawn through `UiWindow.hpp` (`UiWindowWin32.cpp` / `UiWindowX11.cpp`). Views lay out from
    `UiWindow::width()` / `height()` every frame, never from 960 / 748. Colours come only from `style::`
    (`UiStyle.hpp`); tests compare against `style::`, not literals.
  - Shapes (`UiWindow::fillPolygon`, `fillEllipse`, `fillRing`) are drawn as rows of `fill()` (core `ShapeSpans`), so
    every window and test fake draws them. The Mapping tab's Shadow view (`ShadowView.cpp`) draws the target with them;
    the G25 and the G27 have pictures (`ShadowView::hasPicture`), the others show the List view.
- **`service/helper/`**: the WheelBri Helper, a SYSTEM Windows service reached over a pipe, so the app runs as a
  standard user.
- **`shim/`**: proxies `IDirectInput8A/W`, written once as templates over `AnsiTraits` / `WideTraits`.
  - The physical wheel is presented as the target through `EmulatedDevice<T>`, which takes its state from
    `StateFeed`.
  - The game's effects go to the service through `FfbForwarder`.
  - Other devices stay `ProxyDevice<T>`.
- **`logi/`**: a logging pass-through for Logitech's Steering Wheel SDK helper DLL. Never ship Logitech's DLLs.
- **`driver/`** (UMDF 2, VS + WDK):
  - `wheelbri-wheel` is the HID minidriver for every virtual wheel, picked by hardware id.
  - `hide/` (`wheelbri-hide`) is an upper filter that hides the rig from everything but the service's `AllowedPid`.
    It fails open.
  - Both share core's `HidWheelDevice`, `HidPid`, `HidPp` and `StatePacket`.
- **`tools/uhid/`**: `wheelbri-uhid`, the Linux virtual wheel through the kernel's uhid. Proton reads it with
  `PROTON_ENABLE_HIDRAW`.
- **`installer/`, `scripts/make/package.ps1`**: the Windows package in `dist/windows/`, made with IExpress. When a new
  file must ship, add it to `package.ps1` and to the required list in `install.ps1`.
- **`tests/`**: GoogleTest with fakes (see `PipelineTest.cpp`), plus `wheelbri_ui_tests` (scripted window).
  - A change to the loop comes with a scenario in `tests/emulators/PushEmulators.hpp` (`PushLoopTest`), and the
    rig's loop stays in step with `runLoop`.
  - Windows-only code is tested in the Windows binary, or under Wine from Linux.

## Checklists

- **New config key:** add it to the reader, `EmulatorConfig`, the four `config/g*.ini` and `EmulatorConfigTest`.
  - Retired keys are still read and ignored, so old configs load.
  - Values that come from outside (device names) go through `iniSafeValue`.
- **New telemetry column:** add it to `telemetryColumns()` and `writeSample`. `telemetry_summary.sh` reads it by name.
  Bump `kFormatVersion` when a key changes meaning.
- **`StatePacket` layout change:** bump `kVersion`, and update the layout comment, `StatePacketTest` and the driver.
- **New helper request:** add it to `HelperProtocol.hpp` with its checks in `requestProblem` and
  `HelperProtocolTest`. How messages are answered is `answerMessage` (`HelperDispatchTest`). A request never
  carries a path, a command line or a process id. Whatever a connection sets up is undone in `~HelperSession`.
- **New privileged step:** it goes into `privileged::`, a helper request, `HelperSession` and `PrivilegedSteps`.
  Never route it around the helper.

## Decided: do not add back

- Dead zones, Auto-map or roles, a steering ratio (steering is always 1:1), a virtual Xbox controller.
- HidHide, ViGEm, HIDMaestro or any other third-party runtime tool or driver.
- Any SDK for the wheel's own software: WheelBri never reads or sets it.
- Hiding Steam's virtual pad on the Deck, or advising the user to turn Steam Input off.
- Anything privileged that happens without the user pressing Start.
- Advice to restart Windows for the hide filter. Tell the user to close what uses the wheel, or replug it.

## Conventions

- **Names:**
  - **WheelBri** wherever the user sees it, `wheelbri` everywhere else.
  - Never use the old name `wheelemu`, except where old installs are found and removed. The `WEMU` / `WEFB` /
    `WELV` wire tags and the `WE_LOG_*` macros keep their letters.
  - Game names are short: "NFS 2015", never "Need for Speed ...". Labels are at most 20 characters.
- **C++ style:** C++20, 4-space indent, `camelCase` functions/variables, `PascalCase` types, `trailing_` members,
  `kConstant` constants, `#pragma once`, `[[nodiscard]]` on queries, `const` parameters in definitions, `1.0F`
  float literals. Headers included as `"wheelbri/X.hpp"`.
- **Comments:** `//` for intent and constraints, as in the existing headers (no Doxygen).
- **The hot path** (`Mapper::map`, `Pipeline::tick`, `encode`): no allocations, no blocking calls.
- **Threads:** name every thread first thing with `platform::nameThisThread("wheelbri <role>")`.
- **Logging** (full rules in `Logging.hpp`):
  - Always use the `WE_LOG_*` macros, one dotted logger per component, never printf.
  - Hot paths use `CallSampler` or `RateLimiter`.
  - The default level is warn.
  - Every file WheelBri writes is a log in that line format: no CSV, no JSON.
  - One file per run: `<stem>-<date>-<NN>.log`.
  - One folder for every log (`logsFolder()`). Do not add another log location.
- **Shim and logi rules** (both run inside the game process):
  - `DllMain` does nothing beyond `DisableThreadLibraryCalls` and the detach hook.
  - Logging starts lazily. No threads, and every log line is flushed.
  - Nothing may throw into the game: wrap bodies in `guarded()` / `guardedOr()`.
  - Singletons are never destroyed.
  - Link statically and never link `dinput8`.
- **FFB safety** (rigs are often direct drive):
  - Every force sent to hardware goes through `FfbSafetyLimiter` and the watchdog.
  - Keep the guards: slew 50/s, frozen-force fade, and DiEffect's "not a force" values.
  - The only bypass is the opt-in `force_feedback = passthrough` in `wheelbri-shim.ini`. Do not add others.
- **Files:**
  - Everything the app saves is per user under `%LOCALAPPDATA%\wheelbri` (XDG on Linux).
  - Never put settings under ProgramData, Program Files or HKLM.
  - Use `FileText.hpp` for user files: it writes through a temp file and rename.
- **Security when elevated:** nothing under `%LOCALAPPDATA%\wheelbri` may choose a privileged action.
  - Config paths must be inside the user's folders (`isInsideUserFolders`).
  - Helper programs come from fixed system places, never PATH.
  - Links are refused.
- **PowerShell 5.1:** wrap function results in `@(...)` before `.Count`.
- **Licence:** nothing is copied from GIMX (GPLv3). Flag it before ever doing that.
