# wheel-emulator

Presents a DirectInput wheel (target hardware: **Moza R3**) to PC games as a wheel they support
(first target: **Logitech G920**, with a G29 profile as an alternative), on Windows and on Linux (Proton). First target game: *Need for Speed (2015)*.

```
Moza R3 ──DirectInput / evdev──► wheelemu-service ──UDP 127.0.0.1──► dinput8.dll shim (inside the game)
                   map · encode · FFB safety                reports "G920" to the game
```

UDP loopback instead of shared memory: under Proton the shim is a Windows DLL inside Wine and the
service is a native Linux process; named mappings do not cross that boundary, sockets do.

## Status

| Phase | What | State |
|---|---|---|
| 0 | Logging `dinput8.dll` proxy: records how the game finds and drives wheels | **working** (its logs found the NFS 2015 details below) |
| 1 | Core (mapping, G920 + G29 profiles, FFB limiter, config, wire format) + service | **working with a real R3** |
| 2 | Shim answers as a G920 using the service's state | **working in NFS 2015**: wheel, pedals, buttons, D-pad |
| 3 | Game FFB → service (safety limiter, watchdog) → R3 | **working in NFS 2015**: constant, spring, damper, sine, square |
| 4 | Linux virtual G920: `tools/uhid` (`wheelemu-uhid`, kernel uhid) with the bridge's HID PID descriptor; Proton reads it raw (`PROTON_ENABLE_HIDRAW`), so no HID++ is needed | built and unit-tested, **not yet tried on a Steam Deck**; see [docs/linux.md](docs/linux.md) |
| 5 | Windows virtual G920 (every game, no DLL per game) | **working in NFS 2015 with our own driver** (`driver/`, UMDF 2, self-signed; `wheelemu-service --virtual-g920`): wheel, pedals, buttons, force feedback, the R3 hidden through HidHide. Step 0 (`tools/vbridge`, HIDMaestro) still there, with the Xbox controller for button prompts. Design: [docs/phase5-windows-virtual-device.md](docs/phase5-windows-virtual-device.md) |

Settled with NFS 2015 and an R3 on Windows:
- NFS 2015 finds wheels through `DirectInput8Create` (both A and W interfaces) and reads a G920 with
  `c_dfDIJoystick2`: wheel X, throttle Slider, brake Y, clutch Rz, pedals released at the top of the range;
- the R3's axes: steering 0, throttle 2, brake 5, clutch 6 (see `config/moza-r3-as-g920.ini`);
- NFS 2015 unloads its effects between screens and never releases them, so the shim frees a slot on `Unload`.

Still open:
- the G29 profile (built, not tried in a game);
- Linux/Proton: both ways (Phase 4 and the shim under Proton) are ready to try on a Steam Deck, [docs/linux.md](docs/linux.md);
- effect envelopes (attack/fade) are ignored, and NFS's 20-byte Logitech `Escape` calls are not forwarded.

## Functionalities

One entry per finished functionality (it may span several commits), added in the commit that completes it:
a headline, why it was needed, how to use it, and where to look.

### Clutch pedal

- **Why:** the add-on clutch pedal was recognised but the G920's clutch never moved: the configs mapped axis 3,
  while the pedal moves R3 axis 6 (found in `raw_a6` in the telemetry).
- **Use:** plug the clutch into the R3 pedal set; `[clutch] axis = 6` in `config/moza-r3-as-g920.ini` and
  `config/moza-r3-as-g29.ini`. If it doesn't reach 100% at the floor, lower `saturation_high`; if it doesn't
  return to 0%, raise `deadzone_low`. The game reads it as the G920's Rz axis.
- **Look:** `wheelemu-service --ui` (axis 6 shows "-> clutch", Clutch row 0–100%); `raw_a6`, `clutch` and
  `out_clutch` in `telemetry-<date>.csv`. Code: `Mapper::mapPedal` (`core/src/Mapper.cpp`). Without a pedal,
  `tools/vbridge/ClutchProbe` drives a test clutch (`core/include/wheelemu/TestClutch.hpp`).

## Layout

| Path | Contents |
|---|---|
| `core/` | Portable library, no OS dependencies: `Mapper`, `DeviceProfile`, `FfbSafetyLimiter`, `Pipeline`, `EmulatorConfig`/`IniDocument`, `StatePacket` + `UdpLoopback`, `LoopStats`, `Logging`, `Telemetry` |
| `service/` | `wheelemu-service`: reads the wheel and plays its force feedback with the system's own API (DirectInput on Windows, evdev on Linux), 1 kHz loop, `--list` / `--print` / `--ui` (native window: Win32 on Windows, X11 on Linux) / `--ffb-test` |
| `shim/` | `dinput8.dll` proxy (Windows / Wine): wraps `IDirectInput8A/W`, devices and effects |
| `tests/` | GoogleTest unit tests for `core/` |
| `config/` | Example configuration |
| `scripts/make/` | Build scripts: build, full rebuild, tests, pre-commit check, mapping window, app, shim, bridge, drivers ([README](scripts/make/README.md)) |
| `scripts/` | `telemetry_summary.py`: summary of a recorded session; `wheelemu.sh`: start and stop everything on Windows (MSYS2); `linux/`: start/stop and Steam launch wrapper, udev rules, the Linux bundle |
| `tools/uhid/` | Phase 4: `wheelemu-uhid`, the virtual G920 for Linux (C++, kernel uhid) |
| `tools/vbridge/` | Phase 5 step 0: `wheelemu-bridge`, a virtual G920 through HIDMaestro (C#, Windows) |

## Build

The scripts in [`scripts/make/`](scripts/make/README.md) run all of this for you (`build.sh`, `rebuild.sh`,
`test.sh`, `check.sh`, `ui.sh`, `app.sh`, `shim.sh`, `bridge.sh`, `drivers.sh`, `all.sh`) from an MSYS2 UCRT64 shell.

Needs CMake ≥ 3.24 and a C++20 compiler; on Linux also the X11 headers (`libx11-dev`). No other libraries: the
service uses DirectInput / Win32 on Windows and evdev / X11 on Linux (it runs on those two only). spdlog and
GoogleTest are fetched automatically.

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
3. Read `wheelemu-shim-<date>.log` in `%LOCALAPPDATA%\wheelemu\logs` (see [Logging](#logging)). Things to look for:
   - `EnumDevices ... device '...' product=VID_xxxx&PID_xxxx ... -> game STOPPED here`: which device the game picked.
   - `GetProperty(DIPROP_VIDPID)` or `DIPROP_PRODUCTNAME`: the game checks the identity.
   - `CreateEffect(GUID_ConstantForce)` / `SetParameters`: the force feedback it sends.
   - Nothing at all: the game does not use `DirectInput8Create` (check for `LogitechSteeringWheel*.dll` in the
     game folder, or HID/XInput use), and Phase 2 needs a different shim.
4. Also check: does the game have anti-tamper that rejects a proxy DLL? (The log's first line appears
   or it does not.)

## Playing (Phases 2 and 3)

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
with pedals released. Everything else the game does is still logged to `wheelemu-shim-<date>.log`.

Force feedback (Phase 3): the game's DirectInput effects (constant, spring, damper, friction, inertia,
periodic; ramps as their average; no custom forces or envelopes) are kept by the shim and sent to the service
on `ffb_port` (41330) as `FfbPacket`s. The service passes every effect through `FfbSafetyLimiter`
(`[ffb] master_gain`, `max_magnitude`, `max_slew_per_second`) and plays it on the R3 (DirectInput on Windows, evdev on Linux); if the game
goes quiet for 500 ms, every force stops. The service must be running before the game starts, because it
opens the wheel's force feedback exclusively; the shim then opens the R3 non-exclusively. `force_feedback =
passthrough` in `wheelemu-shim.ini` restores the Phase 2 behaviour (effects straight to the R3's driver,
limited only by Pit House), `off` disables force feedback. If forces pull the wrong way, set
`[ffb] invert = true` in the service config.

## Installing on another PC

`scripts/make/package.sh` builds everything in Release and makes `dist/windows/WheelBri-<version>-setup.exe` (plus
the same files as a folder and a zip). On a Windows 11 PC with nothing of it, run the setup and approve the
Administrator prompt; it asks nothing else. It installs **WheelBri** to `C:\Program Files\WheelBri`, the three
drivers (trusting the drivers' certificate on that PC), a Start menu folder (the app and "Uninstall WheelBri") and a
Settings > Apps entry that uninstalls it all. The app hides the R3 from games on every Start. The
log is `C:\ProgramData\wheelemu\logs\wheelemu-install-<date>.log`; `installer/README.txt` is the end user's guide.

## The app

`wheelemu.exe` (built next to `wheelemu-service.exe` in `build/service/`) is the everyday way to run it:
double-click it, let Windows start it as Administrator, press **Start**, then start the game. **Start** and **Stop**
are only on the Mapping tab (top right, next to the Xbox pad toggle, stopped or running), with the state next to them
(stopped, running and for how long, a lost wheel, a game not using it); the Status tab says why a start failed. Start shows the Logs tab of the same live view as `--ui`
(axes, buttons, game force feedback, logs: click a line or press O for the folder). **Stop**, or closing the window,
undoes everything (virtual G920 and Xbox controller removed, the R3 visible again).
Each Start reads the config again, so edits apply without restarting the app.

**Xbox pad (EX)** (EX: experimental; the toggle next to Start on the Mapping tab, while stopped; off until turned on,
remembered in `app.ini` as `[app] xbox_buttons`) says whether Start adds the Xbox controller at all. Off: the game
shows keyboard prompts. Not shown to work yet: NFS 2015 does not treat the G920 as an Xbox device and shows keyboard
icons for it.
The Xbox controller (for the game's Xbox button prompts) is added only once the game uses the G920, i.e. after its
first force feedback: a game that finds the pad when it starts may drive from it instead of the wheel (NFS did, and
then steering and pedals did nothing). The pad never presses anything: every button, pedal and the steering go to the
G920, and after each button, D-pad or pedal press the pad's right stick is nudged by about 30 % for 60 ms
(`XboxMirror::kNudge`, past XInput's dead zone; at most a brief camera look; not after steering), to make NFS show Xbox
icons instead of keyboard ones. A nudge inside the dead zone did nothing. The log says "first icon nudge" and counts
them once a minute.

If the pedals are pressed for 20 s and the game has never played force feedback on the G920, the game is driving
from another controller (the Xbox pad, whose triggers never move, or the keyboard): the state next to Start / Stop (Mapping tab) shows **GAME NOT
USING THE WHEEL** in red and the log says so. Close the game with WheelBri running, start it again and pick the wheel
in its controls settings.

**Mapping config** (on the Status page, while stopped) picks the config Start uses, with `<` `>`. The built-in ones
come with the program (`config/*.ini` next to it or one or two folders up) and are the defaults, never changed.
**New** makes a config of your own: type a name, pick the built-in one to start from, Create; it is saved as
`%LOCALAPPDATA%\wheelemu\configs\<name>.ini` and picked at once. **Reset** (after a "Yes, reset") copies the
built-in it was made from over the picked config again, dropping every change; built-in configs have nothing to
reset. **Open folder** shows the file. The pick is remembered (`%LOCALAPPDATA%\wheelemu\app.ini`); `--config FILE`
(in a shortcut) starts with another, and the other options below work too. `wheelemu-service --app` is the same window
from a console (and on Linux).

**Wheel setup** (tab W) defines your **rig**: the devices you have (wheel, pedals, shifter, handbrake, button box) and
what each control is. Every connected device is read at once. With **Detect** on (the default), press a button or move
an axis (a pedal all the way) and its popup opens: type its name, pick what it is (Steering, Throttle, Brake, Clutch,
Handbrake, Axis; Button, D-pad up/right/down/left, Shift up/down, Gear 1-7, Reverse), Enter. A pedal found resting at
the top of its axis is marked **Inverted**. Clicking an axis row or a button tile opens the same popup. Touching a control
of a device adds it to the rig (at most 4 devices; **Remove from rig** takes one out); give each a name and a kind.
**Save** writes `%LOCALAPPDATA%\wheelemu\rigs\<rig>.ini`; **New** / **Rename** / `<` `>` manage rigs, and the active one
is remembered. The first rig starts from names saved per device by earlier versions. The app opens on this tab until a
rig is saved, then on Mapping; Start switches to the Logs tab so you can see it come up.

**Mapping** (tab M, while stopped; while running the same tab is the live view) maps the active rig's controls onto
the G920: the wheel (the device that plays force feedback, **Change** to pick another), Steering / Throttle / Brake /
Clutch (each with **Invert**), the D-pad and the G920's buttons. **Auto-map** fills them from the rig's roles (axes,
D-pad, Shift up/down to the paddles, gears where the target has them); plain buttons you pick yourself. Click a source
and pick a control of the wheel or of an accessory (up to 3 besides the wheel), or "(nothing)". **Save**
writes the config picked on the Status page, keeping its comments and other settings; a built-in config is saved as
a copy of your own (**Save as**, then picked). **Revert**, picking another config or **Reset** drop unsaved edits.
In the config a source on an accessory is written `vid:pid/N` (`axis = 30b7:1001/0`); a session opens every
accessory its mapping names and does not start without them; one lost while running is waited for like the wheel.
Only the R3 is hidden from games; accessories stay visible.

## Service

```bash
wheelemu-service --list                                          # connected joysticks
wheelemu-service --config config/moza-r3-as-g920.ini --print     # live raw -> mapped -> G920 values
wheelemu-service --config config/moza-r3-as-g920.ini --ui        # window: every axis/button, its G920 mapping, and game force feedback
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
| service, Windows | `%LOCALAPPDATA%\wheelemu\logs\wheelemu-service-<date>.log` |
| service, Linux | `$XDG_STATE_HOME/wheelemu/` or `~/.local/state/wheelemu/wheelemu-service-<date>.log` |
| service, macOS | `~/Library/Logs/wheelemu/wheelemu-service-<date>.log` |
| shim (in the game) | `%LOCALAPPDATA%\wheelemu\logs\wheelemu-shim-<date>.log` (next to `dinput8.dll` if `LOCALAPPDATA` is not set) |

One file per day (`<date>` = `2026-09-30`, local time); a program running past midnight moves to the next
day's file. Old files are deleted after 7 days by default, see [Old files](#old-files). The `--ui` window
shows where everything is.

**Levels**: `trace` per-tick/per-call detail · `debug` decisions and state changes · `info` lifecycle ·
`warn` recoverable problems · `error` a feature is broken · `critical` the process cannot continue.
The service defaults to `info`, the shim to `debug`.

**Per-component levels without rebuilding**, through `SPDLOG_LEVEL`:

```bash
SPDLOG_LEVEL="info,core.pipeline=trace,service.ffb=debug" wheelemu-service --config ...
```

Components: `service`, `service.input`, `service.ffb`, `service.ffbtest`, `service.ui`, `service.g920`, `service.hidhide`,
`service.hide`, `service.config`, `service.mapping`, `service.setup`, `service.xbox`, `core.mapper`, `core.pipeline`, `core.net`, `core.ffb`, `core.telemetry`, `logging`,
`shim`, `shim.loader`, `shim.di`, `shim.device`, `shim.state`, `shim.effect`, `tools.uhid` (`wheelemu-uhid`, Linux).

**Rules the code follows** (see `core/include/wheelemu/Logging.hpp`)
- Log through the `WE_LOG_*` macros, never `printf`; `--print` and `--ffb-test` instructions are user interface
  and go to stdout on purpose.
- Hot paths are sampled (`CallSampler`: first N calls, then every Mth) or rate-limited (`RateLimiter`,
  `WE_LOG_*_LIMITED`), and report how many messages were dropped.
- Warnings and above are flushed at once; the shim flushes every line because it runs no background thread.
- The shim never logs, loads libraries or starts threads in `DllMain`; logging starts on the first export call.
- The service logs a startup banner (version, OS, config) and its exit code. Loop timing and
  game force feedback status are checked every 10 s but logged only when they change (ticks start or stop
  running late; effects playing, failures, watchdog stops, the game starting or stopping to send), with a
  loop summary at exit.
- Known limitation: log paths are passed to spdlog as narrow strings; a non-ASCII path on Windows may fail
  to open (the program then logs to the console only and says so).

## Telemetry

While it runs, the service records to two CSV files per day in the log folder
(`%LOCALAPPDATA%\wheelemu\logs` on Windows). Nothing is sent anywhere.

- `telemetry-<date>.csv`: a row whenever something changes (checked 50 times a second, plus one row a minute when nothing does) with the physical wheel (raw axes, buttons, hat), the mapped
  state, the values the emulated G920 reports to the game, the game's force feedback per slot (what the game
  asked for and what reached the wheel after the limiter), and loop timing.
- `ffb-<date>.csv`: every force feedback change the game sent, with all effect parameters (the shim's periodic
  re-sends of an unchanged effect are left out).

Every session of the day is appended to that day's files, after a `# session started ...` block with the
version, the device and a copy of the config; a session running past midnight continues in the next day's
files. `time` is the local time of day, `t_ms` the time since the session started. Settings are in
`[telemetry]`: `enabled`, `rate_hz`, `only_changes` (default `true`; `false` writes every sample),
`heartbeat_s`, `min_change` (smallest axis move that counts, default 0.0005 = 0.23° of steering: smaller
moves are sensor noise), `max_file_mb` (per file) and `directory`. A value holds until the next row.

The `--ui` window lists the folder, today's log files (service and game), today's telemetry files and what
happens to old files in the **LOGS AND TELEMETRY** panel at the bottom; press **F** there to open the folder.

For a quick summary of the newest session (axis ranges, pedal use, force feedback per
effect type, loop timing):

```
python scripts/telemetry_summary.py            # or pass a telemetry-*.csv / a folder
```

## Old files

Logs (service and game) and telemetry older than 7 days are **deleted** by default. The service checks at
start-up and again after every midnight. Only its own files are touched (`wheelemu-service*`,
`wheelemu-shim*`, `telemetry-*`, `ffb-*` and archives made from them). `[retention]` in the config:

| Key | Values |
|---|---|
| `old_files` | `delete` (default), `archive`, `keep` |
| `older_than_days` | `7` (default): today and the 7 days before it are never touched |
| `archiver` | `auto` (default), `7z`, `xz`, `zstd`, `zip`, `gzip` |

`--old-files delete|archive|keep` on the command line overrides `old_files` for one run.

`archive` compresses each old day and deletes the originals once the compressor has succeeded. `auto` uses the
best compressor installed, in this order:

1. **7-Zip** (`7z.exe`, found on PATH or in `Program Files\7-Zip`): one `wheelemu-logs-<date>.7z` per day,
   LZMA2 at the highest level. The smallest files; recommended (install from 7-zip.org).
2. **xz** (MSYS2: `pacman -S xz`): each file becomes `<file>.xz`, almost as small.
3. **zstd** (`pacman -S mingw-w64-ucrt-x86_64-zstd`): `<file>.zst`, a little bigger, much faster to open.
4. **zip** with Windows' built-in `tar.exe` (Windows 10 and later): `wheelemu-logs-<date>.zip`, always
   available, the largest.
5. **gzip**: `<file>.gz`.

The compressor runs in the background on one CPU thread (7-Zip and xz at their best level use up to about
700 MB of RAM while they work).
If it fails (or none is found) the originals are kept, the error is logged and shown in the window.

## Licence note

Nothing is copied from GIMX (GPLv3). `tools/uhid` is written from the kernel's `linux/uhid.h` alone, and the
virtual G920's HID descriptor comes from HIDMaestro's builder (MIT, noted in `core/src/HidG920.cpp`). The Logitech
FFB command decoder, if it is ever needed, may borrow from GIMX; if code is copied rather than re-implemented,
this project must become GPLv3.
