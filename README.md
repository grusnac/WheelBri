# wheel-emulator

Presents a DirectInput wheel (target hardware: **Moza R3**) to PC games as a wheel they support
(the target: **Logitech G920**; the G29 profile was dropped on 2026-10-04, it is the same wheel with more buttons), on Windows and on Linux (Proton). First target game: *Need for Speed (2015)*.

```
Moza R3 ──DirectInput / evdev──► wheelbri-service ──UDP 127.0.0.1──► dinput8.dll shim (inside the game)
                   map · encode · FFB safety                reports "G920" to the game
```

UDP loopback instead of shared memory: under Proton the shim is a Windows DLL inside Wine and the
service is a native Linux process; named mappings do not cross that boundary, sockets do.

## Status

| Phase | What | State |
|---|---|---|
| 0 | Logging `dinput8.dll` proxy: records how the game finds and drives wheels | **working** (its logs found the NFS 2015 details below) |
| 1 | Core (mapping, G920 profile, FFB limiter, config, wire format) + service | **working with a real R3** |
| 2 | Shim answers as a G920 using the service's state | **working in NFS 2015**: wheel, pedals, buttons, D-pad |
| 3 | Game FFB → service (safety limiter, watchdog) → R3 | **working in NFS 2015**: constant, spring, damper, sine, square |
| 4 | Linux virtual G920: `tools/uhid` (`wheelbri-uhid`, kernel uhid) with the bridge's HID PID descriptor; Proton reads it raw (`PROTON_ENABLE_HIDRAW`), so no HID++ is needed | built and unit-tested, **not yet tried on a Steam Deck**; see [docs/linux.md](docs/linux.md) |
| 5 | Windows virtual G920 (every game, no DLL per game) | **working in NFS 2015 with our own driver** (`driver/`, UMDF 2, self-signed; `wheelbri-service --virtual-g920`): wheel, pedals, buttons, force feedback, the rig hidden by our own filter (`wheelbri-hide`). Step 0 (a HIDMaestro bridge) and the virtual Xbox controller were removed on 2026-10-03. Design: [docs/phase5-windows-virtual-device.md](docs/phase5-windows-virtual-device.md) |

Settled with NFS 2015 and an R3 on Windows:
- NFS 2015 finds wheels through `DirectInput8Create` (both A and W interfaces) and reads a G920 with
  `c_dfDIJoystick2`: wheel X, throttle Slider, brake Y, clutch Rz, pedals released at the top of the range;
- the R3's axes: steering 0, throttle 2, brake 5, clutch 6 (see `config/g920-virtual-device.ini`);
- NFS 2015 unloads its effects between screens and never releases them, so the shim frees a slot on `Unload`.

Still open:
- Linux/Proton: both ways (Phase 4 and the shim under Proton) are ready to try on a Steam Deck, [docs/linux.md](docs/linux.md);
- NFS's 20-byte Logitech `Escape` calls are not forwarded (the shim logs their bytes: whether NFS's forces need
  them is still to be seen in a session's shim log);
- effect envelopes (attack / fade) and ramps over time are implemented (`FfbShape.hpp`) and unit-tested, not yet
  felt in a game.

## Functionalities

One entry per finished functionality (it may span several commits), added in the commit that completes it:
a headline, why it was needed, how to use it, and where to look.

### Clutch pedal

- **Why:** the add-on clutch pedal was recognised but the G920's clutch never moved: the configs mapped axis 3,
  while the pedal moves R3 axis 6 (found in `raw_a6` in the telemetry).
- **Use:** plug the clutch into the R3 pedal set; `[clutch] axis = 6` in `config/g920-virtual-device.ini` and `config/g920-in-game-shim.ini`. If it doesn't reach 100% at the floor, lower `saturation_high`; if it doesn't
  return to 0%, raise `deadzone_low`. The game reads it as the G920's Rz axis.
- **Look:** `wheelbri-service --ui` (axis 6 shows "-> clutch", Clutch row 0–100%); `raw_a6`, `clutch` and
  `out_clutch` in `telemetry-<date>.log`. Code: `Mapper::mapPedal` (`core/src/Mapper.cpp`). Without a pedal,
  a test clutch can be sent over UDP (`core/include/wheelbri/TestClutch.hpp`).

### Virtual G920 (our own driver)

- **Why:** every game should see a G920 without anything copied into it, on any wheel; third-party virtual-device
  tools (HIDMaestro, ViGEm) are not allowed.
- **Use:** target **G920 virtual device** on the Mapping tab, then Start (no Administrator: the WheelBri Helper does
  it). Start installs the
  `wheelbri-g920` driver itself when Windows lacks it; Stop or closing the window removes the device.
- **Look:** joy.cpl lists "Logitech G920 Driving Force Racing Wheel USB"; `wheelbri-driver-<date>.log`, the service's
  `service.g920` lines. Code: `driver/Driver.cpp`, `service/src/VirtualG920.cpp`, core `HidG920`, `HidPid`.

### Hiding the rig from games

- **Why:** with the real wheel visible too, a game (NFS) may pick it instead of the G920.
- **Use:** automatic with the virtual G920: every device of the active rig is hidden while a session runs, and only
  WheelBri may open it. The first Start puts the `wheelbri-hide` filter on each device, once per installation (the
  device reconnects once, so the rig is gone for a few seconds): a popup says so ("Hiding your wheel from games: first
  time only") and closes by itself when it is done. `--no-hide` skips it, `--unhide` clears a hide after a crash (a crash never leaves one).
- **Look:** the state next to Start ("rig hidden (N of M devices)" or, in red, "NOT HIDDEN"); `wheelbri-hide-<date>.log`,
  `wheelbri-hide-install-<date>.log` (also when the WheelBri Helper runs it); the hide log names each program that
  opens the wheel or is refused ("refused process 8964 (steam.exe)"). Code: `driver/hide/`, `service/src/HideFilter.cpp`, `ensureHideFilter` in
  `main.cpp`.

### In-game shim and its games list

- **Why:** a way that needs no driver: a `dinput8.dll` next to the game presents the rig as a G920 to that game only
  (also under Proton).
- **Use:** target **G920 in-game shim**, **Games (N)** next to Start: **Add game** (the folder with the game's `.exe`),
  pick it, Start. After the session the app asks whether to delete the shim from the game's folder. Settings: an
  optional `wheelbri-shim.ini` next to the DLL.
- **Look:** `wheelbri-shim-<date>.log`; the list is `%LOCALAPPDATA%\wheelbri\shim-games.txt`. Code: `shim/`, core
  `ShimGames`, `service/src/ShimGamesView.cpp`.

### Rig tab

- **Why:** WheelBri works with any wheel and accessories, so the user says what each device and control is, once.
- **Use:** tab R: touch a control (Detect), name it and pick its role; **Save**. Several rigs, the active one
  remembered. The device with a Steering axis is the wheel.
- **Look:** `%LOCALAPPDATA%\wheelbri\rigs\<rig>.ini`; `service.setup` log lines. Code: core `Rig`,
  `service/src/WheelSetupView.cpp`.

### Mapping tab

- **Why:** the target (the G920) is fixed; which rig control drives each of its axes and buttons is the user's.
- **Use:** tab M while stopped: click a G920 input and pick a rig control, or **Auto-map** from the rig's roles;
  **Save** writes your own mapping, **Default** goes back to WheelBri's. While running the same tab is the live view.
- **Look:** `%LOCALAPPDATA%\wheelbri\configs\my-g920-<target>.ini`; telemetry `raw_*` and `out_*` columns. Code: core
  `MappingEdit`, `Mapper`, `service/src/MappingEditView.cpp`.

### Game force feedback through the safety limiter

- **Why:** rigs are often direct drive: no game force may reach the wheel unchecked.
- **Use:** automatic on both targets. `[ffb] master_gain`, `max_magnitude` (0.8), `max_slew_per_second` (50) in the config;
  `[ffb] invert = true` if forces pull the wrong way. The forces stop when the game goes quiet (500 ms).
- **Look:** `ffb-<date>.log` (`kind=wheel_*` lines: the delay of every call to the wheel); the service log's `ffb delay`
  table (with the usage table, every 10 minutes and at the end); the Force feedback tab (`--ffb-tab`). Code: core
  `FfbController`, `FfbSafetyLimiter` (`Ffb.cpp`), `AsyncFfbSink` (the wheel's calls on their own raised-priority
  thread: stops, then constant forces, then periodic, then spring and damper), `WheelOutput`.

### Installer and uninstaller

- **Why:** another PC needs WheelBri without a build tree, with only Windows' own tools.
- **Use:** `scripts/make/package.sh` makes `dist/windows/WheelBri-<version>-setup.exe`; run it, approve Administrator.
  Uninstall from the Start menu or Settings > Apps. `scripts/make/clean-pc.sh` removes everything, for tests. A
  reinstall that finds a saved rig puts the hiding filter on its plugged-in devices itself (`wheelbri-service
  --prepare-hide`), so the first Start after the restart is instant.
- **Look:** `wheelbri-install-<date>.log` in the logs folder. Code: `installer/`, `scripts/make/package.ps1`,
  `scripts/make/clean-pc.ps1`.

### Logs tab and one logs folder

- **Why:** one place to see why something failed: the app's, the shim's, both drivers' and the WheelBri Helper's logs
  together.
- **Use:** tab L (or O for the folder): one line per component, then their lines merged live (scroll, Wrap). Files
  are daily and old ones go after 2 days (`--old-files`).
- **Look:** `%LOCALAPPDATA%\wheelbri\logs`. Code: core `LiveLog`, `Retention`, `service/src/MappingView.cpp`.

### Running as a standard user (WheelBri Helper)

- **Why:** end users are standard users, and the app should not ask for Administrator every time it opens.
- **Use:** nothing to set: the installer adds the **WheelBri Helper** service (`WheelBriHelper`, starts with Windows);
  `wheelbri.exe` runs as you and asks it for the virtual G920, hiding the rig, installing a missing G920 driver and
  letting the drivers write into your logs folder. Started as Administrator (a development build), the app does those
  itself. Without either, the status next to Start says **HELPER NOT INSTALLED** in red (virtual device target only).
- **Look:** `wheelbri-helper-<date>.log` in the logs folder; the app's log says `through the WheelBri Helper` or
  `as Administrator`; `services.msc` > WheelBri Helper; `wheelbri-helper.exe --hello`. Code: `service/helper/`
  (pipe server, sessions, log), `service/src/PrivilegedSteps.cpp` (the app's side), `service/src/Privileged.cpp`
  (the steps), `core/include/wheelbri/HelperProtocol.hpp` (the requests and their rules).

### App icon

- **Why:** the app had Windows' generic icon everywhere.
- **Use:** nothing to set: both programs carry it, so Explorer, the taskbar, the window, the Start menu shortcut and
  Settings > Apps show it. The real wheel (solid) turning into pixels (the virtual one); the orange hub is the bridge.
- **Look:** `service/app/wheelbri.svg` (the drawing), `wheelbri.ico` (16 to 256 px, made from it), `wheelbri.rc.in`,
  `wheelbri-service.rc.in`; the window takes it in `service/src/UiWindowWin32.cpp`.

## Layout

| Path | Contents |
|---|---|
| `core/` | Portable library, no OS dependencies: `Mapper`, `DeviceProfile`, `FfbSafetyLimiter`, `Pipeline`, `EmulatorConfig`/`IniDocument`, `StatePacket` + `UdpLoopback`, `LoopStats`, `Logging`, `Telemetry` |
| `service/` | `wheelbri-service`: reads the wheel and plays its force feedback with the system's own API (DirectInput on Windows, evdev on Linux), 1 kHz loop, `--list` / `--print` / `--ui` (native window: Win32 on Windows, X11 on Linux) / `--ffb-test` |
| `shim/` | `dinput8.dll` proxy (Windows / Wine): wraps `IDirectInput8A/W`, devices and effects |
| `tests/` | GoogleTest unit tests for `core/` |
| `config/` | Example configuration |
| `scripts/make/` | Build scripts: build, full rebuild, tests, pre-commit check, mapping window, app, shim, drivers ([README](scripts/make/README.md)) |
| `scripts/` | `telemetry_summary.sh`: summary of a recorded session (bash and awk); `linux/`: start/stop and Steam launch wrapper, udev rules, the Linux bundle |
| `tools/uhid/` | Phase 4: `wheelbri-uhid`, the virtual G920 for Linux (C++, kernel uhid) |

## Build

The scripts in [`scripts/make/`](scripts/make/README.md) run all of this for you (`build.sh`, `rebuild.sh`,
`test.sh`, `check.sh`, `ui.sh`, `app.sh`, `shim.sh`, `drivers.sh`, `all.sh`, `package.sh`, `clean-pc.sh`) from an MSYS2 UCRT64 shell.

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
3. Read `wheelbri-shim-<date>.log` in `%LOCALAPPDATA%\wheelbri\logs` (see [Logging](#logging)). Things to look for:
   - `EnumDevices ... device '...' product=VID_xxxx&PID_xxxx ... -> game STOPPED here`: which device the game picked.
   - `GetProperty(DIPROP_VIDPID)` or `DIPROP_PRODUCTNAME`: the game checks the identity.
   - `CreateEffect(GUID_ConstantForce)` / `SetParameters`: the force feedback it sends.
   - Nothing at all: the game does not use `DirectInput8Create` (check for `LogitechSteeringWheel*.dll` in the
     game folder, or HID/XInput use), and Phase 2 needs a different shim.
4. Also check: does the game have anti-tamper that rejects a proxy DLL? (The log's first line appears
   or it does not.)

## Playing (Phases 2 and 3)

The app does all of this from its Mapping tab (target G920 in-game shim, Games). By hand:

1. Start the service and leave it running: `wheelbri-service --config config/g920-in-game-shim.ini`
   (add `--ui` to watch the mapping at the same time).
2. Copy `dinput8.dll` next to the game's `.exe` (Linux/Proton: also `WINEDLLOVERRIDES="dinput8=n,b" %command%`).
   Optionally copy `config/wheelbri-shim.ini` there too; the defaults match the example service config.
3. Start the game. It should list a "Logitech G920 Driving Force Racing Wheel USB" instead of the R3.

What the shim does: when the game enumerates devices, the R3 (vendor 346e) is offered as a G920 (VID 046D,
PID C262, driving-wheel type, X = wheel, Slider = accelerator, Y = brake, Rz = clutch, 19 buttons (the wheel's 11; 13-18 the Driving Force Shifter's gears 1-6, 19 reverse), 1 hat,
pedals released at the top of the range, as a real G920 and as NFS 2015 expects). If a game reads the pedals from other axes, or backwards, change
`throttle_axis` / `brake_axis` / `clutch_axis` / `invert_pedals` in `wheelbri-shim.ini`; no rebuild needed.
Every read (`GetDeviceState`, `GetDeviceData`) is answered from the latest packet the service sent, in the
data format and ranges the game asked for. If the service stops sending for 250 ms the wheel reads as centred
with pedals released. Everything else the game does is still logged to `wheelbri-shim-<date>.log`.

Force feedback (Phase 3): the game's DirectInput effects (constant, spring, damper, friction, inertia,
periodic; ramps from start to end over their duration; envelopes, i.e. attack and fade; no custom forces) are kept by the shim and sent to the service
on `ffb_port` (41330) as `FfbPacket`s. The service passes every effect through `FfbSafetyLimiter`
(`[ffb] master_gain`, `max_magnitude`, `max_slew_per_second`) and plays it on the R3 (DirectInput on Windows, evdev on Linux); if the game
goes quiet for 500 ms, every force stops. The service must be running before the game starts, because it
opens the wheel's force feedback exclusively; the shim then opens the R3 non-exclusively. `force_feedback =
passthrough` in `wheelbri-shim.ini` restores the Phase 2 behaviour (effects straight to the R3's driver,
limited only by Pit House), `off` disables force feedback. If forces pull the wrong way, set
`[ffb] invert = true` in the service config.

## Installing on another PC

`scripts/make/package.sh` builds everything in Release and makes `dist/windows/WheelBri-<version>-setup.exe` (plus
the same files as a folder and a zip). On a Windows 11 PC with nothing of it, run the setup and approve the
Administrator prompt (the only one: the app itself runs as the user); at the end it asks whether to restart Windows now (default yes), which makes sure every driver
is in use (the hiding filter often starts only after a restart). It installs **WheelBri** to `C:\Program Files\WheelBri`, the three
drivers (trusting the drivers' certificate on that PC), a Start menu folder (the app and "Uninstall WheelBri"), the WheelBri Helper service and a
Settings > Apps entry that uninstalls it all. The app hides the rig (every device in the Rig tab) from games on every Start. The
log is `%LOCALAPPDATA%\wheelbri\logs\wheelbri-install-<date>.log` (every log, the drivers' too, is in that one folder); `installer/README.txt` is the end user's guide.

## The app

`wheelbri.exe` (built next to `wheelbri-service.exe` in `build/service/`) is the everyday way to run it:
double-click it (it runs as you; the WheelBri Helper service does what needs Administrator), press **Start**, then
start the game. **Start** and **Stop**
are at the bottom right of every tab, next to the tab buttons (stopped or running), with the state just left of them
(ready, running and for how long, a lost wheel, a game not using it); after "Start failed (Logs)" the Logs tab says
why, in red. While stopped, Start can only be pressed when every device of the rig that the mapping uses (the wheel,
and each accessory the mapping takes an axis or button from) is connected: otherwise the state says, in red, **NOT
CONNECTED:** and their names (or **NO RIG** before a rig is set up, **NO MAPPING** when nothing of the rig is mapped onto the G920), and Start is greyed out until they appear (the
app looks every 2 s). Devices of the rig the mapping does not use do not matter. Start shows the Logs tab of the same live view as `--ui`
(axes, buttons, game force feedback, logs: click a line or press O for the folder). **Stop**, or closing the window,
undoes everything (virtual G920 removed, the R3 visible again).
Each Start reads the config again, so edits apply without restarting the app. If the wheel is not connected when
Start is pressed (Windows restarts it after WheelBri is reinstalled, and the first Start after that puts WheelBri's
filter back on it, which restarts it again), the service waits up to 15 s for it before giving up; Stop ends the wait.

The **Logs** tab's live log (every component's log merged, newest at the bottom) scrolls with the mouse wheel, the
arrows and PageUp / PageDown / Home / End, and with its scroll bars: drag the one on the right to go up and down fast,
or click its track to jump there. Long messages run off the side: drag the bar under them, use Shift + wheel (or a
sideways wheel) or Left / Right. **Wrap** (or W) turns word wrap on: a long message continues on the next rows
instead, and the sideways bar goes away. End (or scrolling to the bottom) follows the newest lines again.

NFS 2015 shows keyboard button prompts with the G920 (it does not treat it as an Xbox device). A virtual Xbox
controller to get Xbox prompts was tried and removed on 2026-10-03: with it there at the game's start NFS drove from
the pad, its buttons triggered the pad's own functions (the right stick click took photos), and stick nudges did not
change the prompts.

If the pedals are pressed for 20 s and the game has never played force feedback on the G920, the game is driving
from another controller (a gamepad or the keyboard): the state next to Start / Stop (Mapping tab) shows **GAME NOT
USING THE WHEEL** in red and the log says so. Close the game with WheelBri running, start it again and pick the wheel
in its controls settings.

**Target** (the top row of the Mapping tab, while stopped) picks the target with `<` `>`, like the Rig tab's device:
**G920 virtual device** (WheelBri's driver, every game sees it, the rig is hidden) or **G920 in-game shim** (`dinput8.dll`
next to the game's `.exe`, nothing hidden, nothing created). The G920 is the only target wheel; others come only when
WheelBri supports them. A target is fixed: the G920's axes, D-pad and buttons are what they are. What drives each of
them is the mapping, and that is the user's: each target's built-in config (`config/g920-virtual-device.ini`,
`config/g920-in-game-shim.ini`, next to the program or one or two folders up) holds WheelBri's default mapping, used
until you save your own on the Mapping tab (`%LOCALAPPDATA%\wheelbri\configs\my-g920-virtual-device.ini` or
`my-g920-in-game-shim.ini`). **Default** deletes yours. The target is remembered (`%LOCALAPPDATA%\wheelbri\app.ini`,
`[app] way`); `--config FILE`
(in a shortcut) starts with another config, and the other options below work too. `wheelbri-service --app` is the same window
from a console (and on Linux).

**Games** (only with the target **G920 in-game shim**): next to Start the app shows the game Start is for and a
**Games (N)** button. It opens the games panel: every game the shim was put into, with its folder and whether the shim
is there (shim in place / no shim / another dinput8.dll). Click a game to pick it; **Add game** opens Windows' folder
window: choose the folder that holds the game's `.exe` and WheelBri copies the shim there (`dinput8.dll`, and
`wheelbri-shim.ini` unless one is already there) and adds the game. **Put shim** / **Remove shim** per game, and **Forget** to take a game off the list (its folder is not touched; if WheelBri's shim is still there, the app says so). Start puts
the shim back, or replaces it with a newer build, when needed (close the game first: Windows locks the DLL while it
runs). When a shim session stops, the app asks **Would you like to delete the shim from the target folder?**: Yes
deletes both files (the game stays in the list, marked "no shim"), No leaves them. A `dinput8.dll` that is not
WheelBri's (another mod) is never overwritten or deleted. The list is `%LOCALAPPDATA%\wheelbri\shim-games.txt`.

**What Start runs** depends on the target: only what that target needs.

| Component | G920 virtual device | G920 in-game shim |
|---|---|---|
| Shim check / copy: `dinput8.dll` + `wheelbri-shim.ini` into the picked game | - | yes |
| Read the rig: wheel + accessories (DirectInput, the wheel opened exclusively) | yes | yes |
| Mapping loop, 1 kHz: rig -> G920 -> UDP 41329 | yes (to the driver) | yes (to the shim in the game) |
| Force feedback in: UDP 41330 -> safety limiter + watchdog -> wheel | yes (from the driver) | yes (from the shim) |
| Logs, telemetry logs, old-file cleanup | yes | yes |
| Game-use watch ("GAME NOT USING THE WHEEL") | yes | yes |
| Live view: Mapping / Rig / Logs tabs, Start / Stop | yes | yes |
| Stale hide check: clears a hide left by a crashed session, warns if another one hides the rig | yes | yes |
| Administrator or the WheelBri Helper (`PrivilegedSteps`): needed for the rows below | yes | - |
| G920 driver check: installs `wheelbri-g920` (`driver\install-driver.ps1`) when Windows does not have it | yes | - |
| Virtual G920 device (`wheelbri-g920` driver) and its DirectInput registry entries | yes | - |
| Hide filter (`wheelbri-hide`): rig hidden from games | yes | - (the game must see the rig) |
| Test clutch listener, UDP 41331 (developer tool) | yes | - |
| Logs tab rows for the G920 driver and the hide filter | yes | - (emulator and game shim only) |

`wheelbri.exe` runs as the user who starts it (`asInvoker`). With the virtual device, the privileged rows go through
the WheelBri Helper service, or are done in the app when it was started as Administrator.

The **Rig** tab (R) defines your **rig**: the devices you have (wheel, pedals, shifter, handbrake, button box) and
what each control is. Every connected device is read at once. With **Detect** on (the default), press a button or move
an axis (a pedal all the way) and its popup opens: type its name, pick what it is (Steering, Throttle, Brake, Clutch,
Handbrake, Axis; Button, D-pad up/right/down/left, Shift up/down, Gear 1-7, Reverse), Enter. A pedal found resting at
the top of its axis is marked **Inverted**. Clicking an axis row or a button tile opens the same popup. Touching a control
of a device adds it to the rig (at most 4 devices; **Remove from rig** takes one out); give each a name. There is no "it is a wheel / pedals / ..." choice: the device
with a **Steering** axis is the wheel (it plays force feedback).
**Save** writes `%LOCALAPPDATA%\wheelbri\rigs\<rig>.ini`; **New** / **Rename** / `<` `>` manage rigs, and the active one
is remembered. The first rig starts from names saved per device by earlier versions. The app opens on this tab until a
rig is saved, then on Mapping; Start switches to the Logs tab so you can see it come up.

**Mapping** (tab M, while stopped; while running the same tab is the live view) maps the active rig's controls onto
the G920: the wheel (the device that plays force feedback, **Change** to pick another), Steering / Throttle / Brake /
Clutch (each with **Invert**), the D-pad and the G920's buttons. **Auto-map** fills them from the rig's roles (axes,
D-pad, Shift up/down to the paddles, gears where the target has them); plain buttons you pick yourself. Click a source
and pick a control of the wheel or of an accessory (up to 3 besides the wheel), or "(nothing)". **Save** writes your own
mapping for the target (`my-g920-<target>.ini`, comments and other settings kept). **Revert** drops unsaved changes,
**Default** the saved mapping (WheelBri's default again). A source the default names but your rig has not named shows
"not in the rig" in orange: pick another or Auto-map. Start uses exactly what the tab shows (as saved).
In the config a source on an accessory is written `vid:pid/N` (`axis = 30b7:1001/0`); a session opens every
accessory its mapping names and does not start without them; one lost while running is waited for like the wheel.
Every device of the active rig (wheel and accessories) is hidden from games while the app runs; no rig = nothing
hidden.

## Service

```bash
wheelbri-service --list                                          # connected joysticks
wheelbri-service --config config/g920-virtual-device.ini --print     # live raw -> mapped -> G920 values
wheelbri-service --config config/g920-virtual-device.ini --ui        # window: every axis/button, its G920 mapping, and game force feedback
wheelbri-service --config config/g920-virtual-device.ini --ffb-test  # gentle spring / left / right / damper
```

## Logging

Built on spdlog. Every line carries time, level, component, thread and source location:

```
[2026-09-29 18:31:15.887] [info ] [service       ] [tid 1202769] source_device='MOZA' ... (main.cpp:107)
```

**Where**

| Program | File |
|---|---|
| service, Windows | `%LOCALAPPDATA%\wheelbri\logs\wheelbri-service-<date>.log` |
| service, Linux | `$XDG_STATE_HOME/wheelbri/` or `~/.local/state/wheelbri/wheelbri-service-<date>.log` |
| service, macOS | `~/Library/Logs/wheelbri/wheelbri-service-<date>.log` |
| shim (in the game) | `%LOCALAPPDATA%\wheelbri\logs\wheelbri-shim-<date>.log` (next to `dinput8.dll` if `LOCALAPPDATA` is not set) |

One file per day (`<date>` = `2026-09-30`, local time); a program running past midnight moves to the next
day's file. Old files are deleted after 2 days by default, see [Old files](#old-files). The `--ui` window
shows where everything is.

**Levels**: `trace` per-tick/per-call detail · `debug` decisions and state changes · `info` lifecycle ·
`warn` recoverable problems · `error` a feature is broken · `critical` the process cannot continue.
The service defaults to `info`, the shim to `debug`.

**Per-component levels without rebuilding**, through `SPDLOG_LEVEL`:

```bash
SPDLOG_LEVEL="info,core.pipeline=trace,service.ffb=debug" wheelbri-service --config ...
```

Components: `service`, `service.input`, `service.ffb`, `service.ffbtest`, `service.ui`, `service.g920`, `service.hidhide`,
`service.hide`, `helper`, `helper.pipe`, `service.config`, `service.mapping`, `service.shim`, `service.setup`, `service.devices`, `core.mapper`, `core.pipeline`, `core.net`, `core.ffb`, `core.telemetry`, `logging`,
`shim`, `shim.loader`, `shim.di`, `shim.device`, `shim.state`, `shim.effect`, `tools.uhid` (`wheelbri-uhid`, Linux).

**Rules the code follows** (see `core/include/wheelbri/Logging.hpp`)
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

While it runs, the service records to two more log files per day in the log folder
(`%LOCALAPPDATA%\wheelbri\logs` on Windows). Nothing is sent anywhere. Every file WheelBri writes is a log in the same
format (`[date time] [level] [component] message`; no CSV, no JSON); in these two the message of a row is
`key=value` pairs starting with `t_ms=`, and a key that does not apply is left out.

- `telemetry-<date>.log`: a row whenever something changes (checked 50 times a second, plus one row a minute when nothing does) with the physical wheel (raw axes, buttons, hat), the mapped
  state, the values the emulated G920 reports to the game, the game's force feedback per slot (what the game
  asked for and what reached the wheel after the limiter), and loop timing.
- `ffb-<date>.log`: every force feedback change the game sent, with all effect parameters (the shim's periodic
  re-sends of an unchanged effect are left out), and one `kind=wheel_play` / `wheel_stop` / `wheel_stopall` line per
  call the service made to the wheel: `wait_us` (queued before the call) and `call_us` (the wheel's driver).
  `scripts/telemetry_summary.sh` prints them as a delay table.

A new file starts with lines saying what the keys mean. Every session of the day is appended to that day's files,
after a `session started ...` line and lines with the version, the device and the config; a session running past
midnight continues in the next day's files. Each line has its date and time; `t_ms` is the time since the session
started. Settings are in
`[telemetry]`: `enabled`, `rate_hz`, `only_changes` (default `true`; `false` writes every sample),
`heartbeat_s`, `min_change` (smallest axis move that counts, default 0.0005 = 0.23° of steering: smaller
moves are sensor noise), `max_file_mb` (per file) and `directory` (on Windows inside `%LOCALAPPDATA%\wheelbri`;
elsewhere it is ignored, with a warning in the log). A value holds until the next row.

The `--ui` window lists the folder, today's log files (service and game), today's telemetry files and what
happens to old files in the **LOGS AND TELEMETRY** panel at the bottom; press **F** there to open the folder.

For a quick summary of the newest session (axis ranges, pedal use, force feedback per
effect type, loop timing):

```
scripts/telemetry_summary.sh            # or pass a telemetry-*.log / a folder; --all for every session of the day
```

## Old files

Logs (service and game) and telemetry older than 2 days are **deleted** by default. The service checks at
start-up and again after every midnight. Only its own files are touched (`wheelbri-service*`,
`wheelbri-shim*`, `telemetry-*`, `ffb-*` and archives made from them). `[retention]` in the config:

| Key | Values |
|---|---|
| `old_files` | `delete` (default), `archive`, `keep` |
| `older_than_days` | `2` (default): today and the 2 days before it are never touched |
| `archiver` | `auto` (default), `7z`, `xz`, `zstd`, `zip`, `gzip` |

`--old-files delete|archive|keep` on the command line overrides `old_files` for one run.

`archive` compresses each old day and deletes the originals once the compressor has succeeded. `auto` uses the
best compressor installed, in this order:

1. **7-Zip** (`7z.exe` on PATH or in `Program Files\7-Zip`): one `wheelbri-logs-<date>.7z` per day, LZMA2 at the
   highest level. The smallest files; recommended (install from 7-zip.org).
2. **xz** (MSYS2: `pacman -S xz`): each file becomes `<file>.xz`, almost as small.
3. **zstd** (`pacman -S mingw-w64-ucrt-x86_64-zstd`): `<file>.zst`, a little bigger, much faster to open.
4. **zip** with Windows' built-in `tar.exe` (System32, Windows 10 and later): `wheelbri-logs-<date>.zip`, always
   available, the largest.
5. **gzip**: `<file>.gz`.

When WheelBri runs as Administrator it ignores PATH and only uses `Program Files\7-Zip` and System32's `tar.exe`.

The compressor runs in the background on one CPU thread (7-Zip and xz at their best level use up to about
700 MB of RAM while they work).
If it fails (or none is found) the originals are kept, the error is logged and shown in the window.

## Security notes

Everything WheelBri saves (configs, rigs, device names, the app's state, the games list, logs, telemetry) is per user,
in `%LOCALAPPDATA%\wheelbri`, also when it runs as Administrator; the WheelBri Helper's log too (written as the
connected user).
The app normally runs as the user; what needs Administrator is done by the **WheelBri Helper** service (SYSTEM), which:

- creates its pipe first (nobody can pose as it), lets in local interactive users only, and serves only
  `wheelbri.exe` / `wheelbri-service.exe` from its own folder (Program Files), checked through the caller's process
  handle; the app in turn only trusts `wheelbri-helper.exe` next to it at the other end;
- takes a fixed list of requests, each checked (`HelperProtocol.hpp`): no paths, files or commands from the app;
  hiding names the caller's own process; the hide filter goes only on present HID game controllers (never a keyboard,
  mouse, system or consumer control, digitizer or the virtual G920); driver scripts come from its own folder;
- opens the user's logs folder as that user, refuses it when a link leads there, and changes its permission through
  that handle;
- undoes what a connection set up when it closes (Stop, a crash, logoff): the virtual G920 removed, the rig shown.

Only the drivers, the virtual devices, the helper service and their registry entries are system-wide. When the app
itself runs as Administrator (a development build), the user's files (which any program of the user can change) never
decide where the Administrator writes or what it runs:

- A config's `[logging] file` and `[telemetry] directory` must then be inside `%LOCALAPPDATA%\wheelbri`; anything else
  is ignored (the default is used) and the log says so.
- The drivers write their logs into the user's log folder (`%LOCALAPPDATA%\wheelbri\logs`) only; no config can point
  them elsewhere. The drivers do not follow links (junctions) in their log folder.
- Compressors for old logs then come from `Program Files\7-Zip` and System32 only (see Old files).
- If one of WheelBri's own folders under `%LOCALAPPDATA%\wheelbri` is a link to somewhere else, the app then says so
  and does not start; a log file that is a link is deleted (the link only) before logging starts.
- Game force feedback (UDP 41330) is taken only from the virtual G920's driver (`WUDFHost.exe` in System32) or from a
  game whose folder holds WheelBri's shim; the shim takes the wheel's state (41329) only from WheelBri. Other senders
  are ignored and named in the log. Every force still goes through the safety limiter. On Linux there is no such
  check. Not covered: the virtual G920's driver takes the wheel's state from any local sender (it runs as
  LocalService and cannot look into the app's process), and the test clutch port (41331) has no check.
- The driver signing certificate is made on the build PC (`driver\new-signing-cert.ps1`); the uninstaller removes only
  the certificates WheelBri recorded (`HKLM\SOFTWARE\wheelbri\SigningCertificates`).

## Licence note

Nothing is copied from GIMX (GPLv3). `tools/uhid` is written from the kernel's `linux/uhid.h` alone, and the
virtual G920's HID descriptor comes from HIDMaestro's builder (MIT, noted in `core/src/HidG920.cpp`). The Logitech
FFB command decoder, if it is ever needed, may borrow from GIMX; if code is copied rather than re-implemented,
this project must become GPLv3.
