# wheel-emulator

Presents a DirectInput wheel (target hardware: **Moza R3**) to PC games as a wheel they support
(first target: **Logitech G920**, with a G29 profile as an alternative), on Windows and on Linux (Proton). First target game: *Need for Speed (2015)*.

```
Moza R3 ──SDL3──► wheelemu-service ──UDP 127.0.0.1──► dinput8.dll shim (inside the game)
                   map · encode · FFB safety                reports "G920" to the game
```

UDP loopback instead of shared memory: under Proton the shim is a Windows DLL inside Wine and the
service is a native Linux process; named mappings do not cross that boundary, sockets do.

## Status

| Phase | What | State |
|---|---|---|
| 0 | Logging `dinput8.dll` proxy: records how the game finds and drives wheels | **built, untested in a game** |
| 1 | Core (mapping, G920 + G29 profiles, FFB limiter, config, wire format) + SDL3 service | **built, unit-tested; needs a real R3** |
| 2 | Shim answers as a G920 using the service's state (force feedback passed to the R3 as-is) | **built, untested in a game** |
| 3 | Game FFB → service → R3 | sink + self-test done; forwarding not started |
| 4 | Linux `uhid` virtual G920 (its FFB is HID++, not the classic Logitech protocol) | not started |

Unverified assumptions, to be settled with Phase 0 logs and a real device:
- how NFS 2015 detects wheels (DirectInput product GUID? Logitech SDK? raw HID?);
- the G920's (and G29's) DirectInput product name, axis layout and button order (`g920Spec()` / `g29Spec()` in
  `core/src/DeviceProfile.cpp`);
- the R3's SDL axis indices (`config/moza-r3-as-g920.ini`) and FFB direction.

## Layout

| Path | Contents |
|---|---|
| `core/` | Portable library, no OS or SDL dependencies: `Mapper`, `DeviceProfile`, `FfbSafetyLimiter`, `Pipeline`, `EmulatorConfig`/`IniDocument`, `StatePacket` + `UdpLoopback`, `LoopStats`, `Logging` |
| `service/` | `wheelemu-service`: SDL3 input source + force feedback sink, 1 kHz loop, `--list` / `--print` / `--ui` / `--ffb-test` |
| `shim/` | `dinput8.dll` proxy (Windows / Wine): wraps `IDirectInput8A/W`, devices and effects |
| `tests/` | GoogleTest unit tests for `core/` |
| `config/` | Example configuration |

## Build

Needs CMake ≥ 3.24, a C++20 compiler, and SDL3 for the service. spdlog and GoogleTest are fetched automatically.

```bash
# Linux / macOS: core, service, tests
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

```bash
# Windows shim (and core) cross-compiled from Linux/macOS with MinGW-w64
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake \
      -DCMAKE_BUILD_TYPE=Release -DWE_BUILD_SERVICE=OFF -DWE_BUILD_TESTS=OFF
cmake --build build-win     # -> build-win/shim/dinput8.dll
```

On Windows natively (MSVC or MSYS2) the plain `cmake -S . -B build` also builds the shim (`WE_BUILD_SHIM`
defaults to ON on Windows).

## Phase 0: recording what the game does

1. Copy `dinput8.dll` next to the game's `.exe`.
   - Linux/Proton: same folder, and set the Steam launch option `WINEDLLOVERRIDES="dinput8=n,b" %command%`.
2. Plug in the wheel and start the game. Open its controller settings and drive for a minute.
3. Read `wheelemu-shim.log` next to the DLL. Things to look for:
   - `EnumDevices ... device '...' product=VID_xxxx&PID_xxxx ... -> game STOPPED here`: which device the game picked.
   - `GetProperty(DIPROP_VIDPID)` or `DIPROP_PRODUCTNAME`: the game checks the identity.
   - `CreateEffect(GUID_ConstantForce)` / `SetParameters`: the force feedback it sends.
   - Nothing at all: the game does not use `DirectInput8Create` (check for `LogitechSteeringWheel*.dll` in the
     game folder, or HID/XInput use), and Phase 2 needs a different shim.
4. Also check: does the game have anti-tamper that rejects a proxy DLL? (The log's first line appears
   or it does not.)

## Playing (Phase 2)

1. Start the service and leave it running: `wheelemu-service --config config/moza-r3-as-g920.ini`
   (add `--ui` to watch the mapping at the same time).
2. Copy `dinput8.dll` next to the game's `.exe` (Linux/Proton: also `WINEDLLOVERRIDES="dinput8=n,b" %command%`).
   Optionally copy `config/wheelemu-shim.ini` there too; the defaults match the example service config.
3. Start the game. It should list a "Logitech G920 Driving Force Racing Wheel USB" instead of the R3.

What the shim does: when the game enumerates devices, the R3 (vendor 346e) is offered as a G920 (VID 046D,
PID C262, driving-wheel type, X = wheel, Slider = accelerator, Y = brake, Rz = clutch, 11 buttons, 1 hat,
pedals released at the top of the range, as a real G920 and as NFS 2015 expects; the G29 profile uses
Y / Rz / Slider instead). If a game reads the pedals from other axes, or backwards, change
`throttle_axis` / `brake_axis` / `clutch_axis` / `invert_pedals` in `wheelemu-shim.ini`; no rebuild needed.
Every read (`GetDeviceState`, `GetDeviceData`) is answered from the latest packet the service sent, in the
data format and ranges the game asked for. If the service stops sending for 250 ms the wheel reads as centred
with pedals released. Force feedback goes straight to the R3's own driver, so its strength is capped by
Pit House, not by the service's `[ffb]` limiter (Phase 3 changes that); `force_feedback = false` in
`wheelemu-shim.ini` turns it off. Everything else the game does is still logged to `wheelemu-shim.log`.

## Service

```bash
wheelemu-service --list                                          # connected joysticks
wheelemu-service --config config/moza-r3-as-g920.ini --print     # live raw -> mapped -> G920 values
wheelemu-service --config config/moza-r3-as-g920.ini --ui        # window: every axis/button and its G920 mapping
wheelemu-service --config config/moza-r3-as-g920.ini --ffb-test  # gentle spring / left / right / damper
```

## Logging

Built on spdlog. Every line carries time, level, component, thread and source location:

```
[2026-09-29 18:31:15.887] [info ] [service       ] [tid 1202769] source_device='MOZA' ... (main.cpp:107)
```

**Where**

| Program | File |
|---|---|
| service, Windows | `%LOCALAPPDATA%\wheelemu\logs\wheelemu-service.log` |
| service, Linux | `$XDG_STATE_HOME/wheelemu/` or `~/.local/state/wheelemu/wheelemu-service.log` |
| service, macOS | `~/Library/Logs/wheelemu/wheelemu-service.log` |
| shim | `wheelemu-shim.log` next to `dinput8.dll` |

Files rotate (service: 10 MiB × 6 by default, `[logging]` in the config; shim: 20 MiB × 4).

**Levels**: `trace` per-tick/per-call detail · `debug` decisions and state changes · `info` lifecycle ·
`warn` recoverable problems · `error` a feature is broken · `critical` the process cannot continue.
The service defaults to `info`, the shim to `debug`.

**Per-component levels without rebuilding**, through `SPDLOG_LEVEL`:

```bash
SPDLOG_LEVEL="info,core.pipeline=trace,service.ffb=debug" wheelemu-service --config ...
```

Components: `service`, `service.sdl`, `service.input`, `service.ffb`, `service.ffbtest`, `service.ui`, `sdl` (SDL's own
messages; more with `SDL_LOGGING`), `core.mapper`, `core.pipeline`, `core.net`, `core.ffb`, `logging`,
`shim`, `shim.loader`, `shim.di`, `shim.device`, `shim.state`, `shim.effect`.

**Rules the code follows** (see `core/include/wheelemu/Logging.hpp`)
- Log through the `WE_LOG_*` macros, never `printf`; `--print` and `--ffb-test` instructions are user interface
  and go to stdout on purpose.
- Hot paths are sampled (`CallSampler`: first N calls, then every Mth) or rate-limited (`RateLimiter`,
  `WE_LOG_*_LIMITED`), and report how many messages were dropped.
- Warnings and above are flushed at once; the shim flushes every line because it runs no background thread.
- The shim never logs, loads libraries or starts threads in `DllMain`; logging starts on the first export call.
- The service logs a startup banner (version, OS, SDL version, config), 10-second loop statistics
  (period and work time, overruns, publish failures), and its exit code.
- Known limitation: log paths are passed to spdlog as narrow strings; a non-ASCII path on Windows may fail
  to open (the program then logs to the console only and says so).

## Licence note

Nothing is copied from GIMX (GPLv3). Two future pieces may borrow from it (the Logitech FFB command decoder and the
`uhid` device code); if code is copied rather than re-implemented, this project must become GPLv3.
