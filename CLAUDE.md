# CLAUDE.md

This file guides Claude when working in this repository.

## Project

wheel-emulator makes a physical wheel (Moza R3) look like a wheel a game supports (Logitech G920 by
default, G29 also available via `target_profile`) on Windows and
Linux/Proton. First target game: Need for Speed (2015). See README.md for phase status and the assumptions that
are still unverified: how NFS detects wheels, G920/G29 identity, axis layout and button order, R3 axis indices and FFB direction.
Nothing has been run against real hardware or in a game yet, so don't treat values in `g920Spec()`/`g29Spec()`
or `config/moza-r3-as-g920.ini` as confirmed. The G920's FFB is HID++ (not the classic Logitech protocol), which
matters for Phase 3/4.

Data flow: `Moza R3 --SDL3--> wheelemu-service --UDP 127.0.0.1--> dinput8.dll shim (in the game)`.
Use UDP loopback, not shared memory: under Proton the shim runs inside Wine and the service is a native Linux
process, and named mappings don't cross that boundary.

## Build and test

CMake ≥ 3.24, C++20. spdlog 1.15.3 and GoogleTest 1.15.2 come in through FetchContent. The service needs SDL3
and is skipped with a warning if SDL3 isn't found.

```bash
# core + service + tests (Linux/macOS, or native Windows, which also builds the shim)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build                                   # all tests
ctest --test-dir build -R Mapper                         # one suite (gtest_discover_tests names: Suite.Test)
./build/tests/wheelemu_tests --gtest_filter='Pipeline.*' # or run the binary directly

# Windows shim cross-compiled with MinGW-w64 -> build-win/shim/dinput8.dll
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake \
      -DCMAKE_BUILD_TYPE=Release -DWE_BUILD_SERVICE=OFF -DWE_BUILD_TESTS=OFF
cmake --build build-win
```

Options: `WE_BUILD_TESTS` (ON; skipped when cross-compiling), `WE_BUILD_SERVICE` (ON), `WE_BUILD_SHIM` (ON only
on Windows targets). Warnings: `/W4 /permissive-` on MSVC, `-Wall -Wextra -Wpedantic` elsewhere. Keep new code
free of warnings.

Service CLI: `wheelemu-service [--config FILE] [--list] [--print] [--ui] [--ffb-test]`. `--ui` opens an SDL window
(`service/src/MappingView.cpp`, drawn with `SDL_RenderDebugText`, no font dependency) that shows every source
axis/button next to its target (G920/G29).

## Layout and architecture

- `core/` (`wheelemu_core`, static library): portable, **no OS headers and no SDL**. The one exception is
  `UdpLoopback.cpp`, which has `#ifdef _WIN32` socket code. Headers live in `core/include/wheelemu/`,
  namespace `wheelemu`.
  - `RawInputState` (axes in [-1,1] exactly as the OS reports them) → `Mapper` → `WheelState`
    (target-agnostic: steering -1..1 over the *target* rotation, pedals 0..1) → `IDeviceProfile::encode`
    → `TargetReport` (the emulated device's own ranges) → `StatePacket` → `IStatePublisher`.
  - `Pipeline::tick()` runs one pass of that chain. `IInputSource` / `IFfbSink` (`Devices.hpp`) and
    `IStatePublisher` / `IStateReceiver` are the seams for tests and platform code.
  - New emulated wheels are new `DeviceProfileSpec`s (or `IDeviceProfile` implementations), registered in
    `makeProfile()`.
  - `FfbEffect` is the common FFB model. Every source is translated into it and `FfbSafetyLimiter` is the
    last stage before hardware. The limiter holds state (slew), so use one limiter per played effect.
  - Config: `IniDocument` (minimal INI; `;`/`#` start a comment anywhere on a line) → `loadEmulatorConfig`,
    which collects *all* errors (unknown keys and sections, out-of-range values) instead of failing fast. When
    you add a config key, add it to the reader, `EmulatorConfig`, the example INI and `EmulatorConfigTest`.
  - `EmulatedJoystick` is the emulated wheel as DirectInput sees it (objects, the game's data format,
    ranges, state bytes, buffered changes). It mirrors DirectInput constants without `<dinput.h>` so it is
    unit-tested on Linux; `shim/src/DiConvert.hpp` static_asserts the constants against the real macros.
  - Wire format (`StatePacket.hpp`): 33 bytes, little-endian, magic `WEMU`, version 1. Any layout change
    must bump `kVersion` and update the layout comment and `StatePacketTest`.
- `service/` (`wheelemu-service`, namespace `wheelemu::service`): SDL3 input source, SDL haptic FFB sink
  (8 slots), FFB self-test, and a fixed-rate loop (default 1 kHz, drift-free, resynchronises when behind).
- `shim/` (`dinput8.dll`, Windows/Wine only, namespace `wheelemu::shim`): proxies `IDirectInput8A/W`,
  devices and effects. It is written once as templates over `AnsiTraits`/`WideTraits` (`Traits.hpp`).
  Exports are renamed through `dinput8.def` (`Proxy_*`) so they don't clash with SDK declarations. The real
  DLL is loaded from the system directory on first use (`RealDInput.cpp`). Phase 2: the physical wheel
  (matched by USB VID in `EnumDevices` and again in `CreateDevice` via `GetDeviceInfo`) is presented as the
  target profile by `EmulatedDevice<T>`, which answers input calls from `StateFeed` (UDP receiver, neutral
  state when stale) and forwards force feedback to the physical device. Other devices stay `ProxyDevice<T>`
  (log and forward). Settings: optional `wheelemu-shim.ini` next to the DLL (`ShimConfig`).
- `tests/`: GoogleTest for `core/` only (`wheelemu_tests`). Use fakes of the interfaces (see
  `PipelineTest.cpp`), not real devices or sockets, except `UdpLoopbackTest`.

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
  `FfbSafetyLimiter`; keep the defaults conservative (`max_magnitude` 0.8, slew limit). Known exception until
  Phase 3: the shim passes the game's DirectInput effects straight to the R3 driver (capped only by Pit House;
  `force_feedback = false` in `wheelemu-shim.ini` disables it). Don't widen that exception.
- Licence: nothing is copied from GIMX (GPLv3). If GIMX code is ever copied rather than reimplemented, the
  project must become GPLv3, so flag that before doing it.
