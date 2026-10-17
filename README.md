# WheelBri

Presents your rig (a wheel and its accessories; built and tested with a **Moza R3**) to PC games as a wheel they
support (the targets, in the app's order: the **Logitech G25** and **G27** for games that know them and not the G920, the
**Logitech G920**, and its successor the **G923** (Xbox One and PC); the G29 profile was dropped on 2026-10-04, it is
the same wheel as the G920 with more buttons), on Windows and on Linux (Proton). First target game:
*Need for Speed (2015)*.

```
rig ──DirectInput / evdev──► wheelbri-service ──UDP 127.0.0.1──► one consumer (the game sees the target):
          map · encode · FFB safety      41329 state ►         Mode Virtual: wheelbri-g920 driver (Windows 11)
                                         ◄ 41330 game FFB                    or wheelbri-uhid (Linux)
                                                               Mode Shim:    dinput8.dll next to the game's .exe
```

UDP loopback instead of shared memory: under Proton the shim is a Windows DLL inside Wine and the
service is a native Linux process; named mappings do not cross that boundary, sockets do.

## How to …

| Task | Where |
|---|---|
| Install on Windows 11 (setup, first start, the WheelBri Helper for standard users) | [Installing on another PC](#installing-on-another-pc), [The app](#the-app), [Running as a standard user](#running-as-a-standard-user-wheelbri-helper), `installer/README.txt` |
| Install on Windows 7 to 10 (shim only, prerequisites) | [Windows 7 to 10](#windows-7-to-10-the-in-game-shim-only) |
| Set up the rig (wheel, pedals, accessories, names, invert, buttons) | [Rig tab](#rig-tab), [The app](#the-app) |
| Pick the target and mode, map axes and buttons, Save / Default | [Mapping tab](#mapping-tab), [The app](#the-app) |
| Play with the virtual wheel (Start / Stop, hiding the rig, after a crash) | [Virtual G920](#virtual-g920-our-own-driver), [Hiding the rig](#hiding-the-rig-from-games) |
| Play with the in-game shim (Games panel, 32-bit games, Ultimate ASI Loader) | [In-game shim](#in-game-shim-and-its-games-list), [Playing (Phases 2 and 3)](#playing-phases-2-and-3) |
| Per-game settings (NFS 2015, Payback, Unbound: pedals, rotation, force feedback gain) | [Game profiles tab](#game-profiles-tab), [docs/games.md](docs/games.md) |
| Force feedback limits, the frozen-push fade, `--ffb-test` | [Game force feedback](#game-force-feedback-through-the-safety-limiter) |
| Logs, log levels, telemetry, old files, `telemetry_summary.sh` | [Logs tab](#logs-tab-and-one-logs-folder), [Logging](#logging), [Telemetry](#telemetry), [Old files](#old-files) |
| G HUB next to the virtual G923 | [G HUB and the virtual G923](#g-hub-and-the-virtual-g923), [docs/ghub-hidpp-plan.md](docs/ghub-hidpp-plan.md) |
| Games that use Logitech's wheel SDK (NFS Payback) | [Games with Logitech's wheel SDK](#games-with-logitechs-wheel-sdk-nfs-payback) |
| Linux and the Steam Deck | [docs/linux.md](docs/linux.md) |
| Upgrade, uninstall, clean a PC | [Installing on another PC](#installing-on-another-pc) |
| Something does not work | [Troubleshooting](#troubleshooting) |
| Build, test, package, drivers, signing certificate | [Build](#build), [scripts/make/README.md](scripts/make/README.md), [driver/README.md](driver/README.md) |
| `wheelbri-service` command line | [Service command line](#service-command-line) |

## Status

| Phase | What | State |
|---|---|---|
| 0 | Logging `dinput8.dll` proxy: records how the game finds and drives wheels | **working** (its logs found the NFS 2015 details below) |
| 1 | Core (mapping, G920 profile, FFB limiter, config, wire format) + service | **working with a real R3** |
| 2 | Shim answers as a G920 using the service's state | **working in NFS 2015**: wheel, pedals, buttons, D-pad |
| 3 | Game FFB → service (safety limiter, watchdog) → R3 | **working in NFS 2015**: constant, spring, damper, sine, square |
| 4 | Linux virtual G920: `tools/uhid` (`wheelbri-uhid`, kernel uhid) with the Windows driver's HID report (two pedal axes renamed for Proton) and HID PID force feedback (core `HidG920`, `HidPid`); the app's Start in Mode Virtual starts it; Proton reads it raw (`PROTON_ENABLE_HIDRAW`), so no HID++ is needed | built and unit-tested, **not yet tried on a Steam Deck**; see [docs/linux.md](docs/linux.md) |
| G923 | The Logitech G923 for Xbox One and PC as a fourth target (shim, Windows virtual device, Linux uhid): the G920 with Plus, Minus and the dial, 24 buttons (046D:C26E, PC mode) | built and unit-tested, **not yet tried in a game**; the order of the 5 new buttons is a guess: [docs/g923-target-plan.md](docs/g923-target-plan.md) |
| G HUB | The virtual G923 answers G HUB's HID++ 2.0 (identity, firmware, name; rotation and gain kept, not applied; uploaded effects logged, not played); its HID PID reports moved 0x10 up to make room | built and unit-tested; **tried with G HUB on 2026-10-16**: the wheel went ACTIVE as `g923_xbox` and G HUB saved its settings; the unsigned axes and the G923's pedal layout came after that try and are **untried**: [docs/ghub-hidpp-plan.md](docs/ghub-hidpp-plan.md) |
| G27 | The Logitech G27 as a third target (shim, Windows virtual device, Linux uhid): the G25's with its 23 buttons (046D:C29B) | built and unit-tested, **not yet tried in a game**; the button order is from users' reports: [docs/g27-target-plan.md](docs/g27-target-plan.md) |
| G25 | The Logitech G25 as a second target (shim, Windows virtual device, Linux uhid): same layout and force feedback as the G920, its own identity (046D:C299) | built and unit-tested, **not yet tried in a game**; the button order is a guess: [docs/g25-target-plan.md](docs/g25-target-plan.md) |
| 5 | Windows virtual G920 (every game, no DLL per game) | **working in NFS 2015 with our own driver** (`driver/`, UMDF 2, self-signed; `wheelbri-service --virtual-g920`): wheel, pedals, buttons, force feedback, the rig hidden by our own filter (`wheelbri-hide`). Step 0 (a HIDMaestro bridge) and the virtual Xbox controller were removed on 2026-10-03. Drivers: [driver/README.md](driver/README.md) |

Which games were played and what to know about each: [docs/games.md](docs/games.md).

Settled with NFS 2015 and an R3 on Windows:
- NFS 2015 finds wheels through `DirectInput8Create` (both A and W interfaces) and reads a G920 with
  `c_dfDIJoystick2`, pedals released at the top of the range;
- the emulated G920 reports its pedals as a real one does on Windows: wheel X, throttle Y, brake Rz, clutch Slider
  (NFS Unbound reads it so), the default for every game. A game that reads it differently gets a game profile,
  `config/games/<id>.ini`, holding only what that game needs changed, laid over the defaults like a sieve; it is
  compiled in and used automatically, for the G920 virtual device and the in-game shim alike. NFS 2015 has one
  (it reads a G920's brake from Y, clutch from Rz and throttle from the Slider): while `NFS16.exe` runs, the app's
  status says "NFS 2015" and the log (`service.game`, `shim.config`) says what it changes. A game without a
  profile is never mentioned. If NFS 2015's own wheel controls were changed to work around its G920 table, the
  status says "NFS 2015: RESET ITS WHEEL CONTROLS"; reset them to default in the game. A profile can also give
  the game's steering rotation (`[steering] rotation_deg` and `note`): while the game runs the service steers with
  it, wheel and game full lock both at that number, then the mapping's own again. NFS Payback's (`nfspayback.ini`,
  `NeedForSpeedPayback.exe`) is 180: the status shows "NFS Payback, 180 deg" and the log says to set the wheel's
  own software to 180 too. The Game profiles tab changes it per game or turns it off (`[game_rotation]`).
  A profile can also say which ways a game works with (`[game] ways` and `ways_note`; left out, both): when the
  game starts in a session run the other way, the app tells you with a Windows notification and a popup, and the
  status next to Start says so in red ("<game> NEEDS THE VIRTUAL DEVICE"). The Game profiles tab shows it too. No
  built-in profile needs it so far: every profiled game was played both ways.
  A profile can also make a game's force feedback stronger (`[ffb] gain` and `note`): while the game runs, the
  service multiplies its pushing forces (steering force, rumble; not springs or dampers) by it before the safety
  limits, which still apply. NFS Unbound's (`nfsunbound.ini`) is 3: its force feedback is very light and the game
  has no setting for it;
- the R3's axes: steering 0, throttle 2, brake 5, clutch 6 (see `config/g920.ini`);
- NFS 2015 unloads its effects between screens and never releases them, so the shim frees a slot on `Unload`.

Still open:
- Linux/Proton: both ways (Phase 4 and the shim under Proton) are ready to try on a Steam Deck,
  [docs/linux.md](docs/linux.md) (build it on the Deck from a git checkout: `scripts/linux/deck-setup.sh` once, then
  `deck-build.sh` after each pull);
- NFS 2015's 20-byte Logitech `Escape` calls are its rev lights (rpm, first light, redline), not forwarded; the
  shim's axis audit (`shim.audit`) logs every call that could remap axes and the game's input-related DLLs;
- effect envelopes (attack / fade) and ramps over time are implemented (`FfbShape.hpp`) and unit-tested, not yet
  felt in a game;
- NFS Payback: see [Games with Logitech's wheel SDK](#games-with-logitechs-wheel-sdk-nfs-payback).

### Games with Logitech's wheel SDK (NFS Payback)

NFS Payback does not read wheels itself: it asks Logitech's Steering Wheel SDK, built into the game, which loads a
helper DLL named in the registry (the default value of
`HKCR\CLSID\{63BD165D-1584-4E75-AB56-08330350545F}\ServerBinary`). Without a working helper the game ignores every
wheel, a real G920 included (found 2026-10-10: on the test PC that value still named a removed Logitech Gaming
Software). Pointed at a Logitech helper that exists, Payback takes the virtual G920 as a G920: Xbox prompts, buttons,
pedals, steering, force feedback through the safety limiter. Two helpers worked: G HUB's (`C:\Program
Files\Logi\wheel_sdk\9_1_1\logi_steering_wheel_x64.dll`, the better force feedback) and the old Logitech Gaming
Software's (`...\Logitech\Gaming Software\SDKs\LogitechSteeringWheel.dll`). Logitech's DLLs are Logitech's: WheelBri
never ships them.

Steering: the game turns in late and little with a wheel (the keyboard does not), with either helper. The stand-in below
showed (2026-10-11) that the SDK hands the game WheelBri's steering exactly, within about 4 ms, with no steering curve:
the slowness is the game's own handling (G29 owners report the same). What helps is a small rotation: 180 degrees (90
each way to full lock), set the same in the Mapping tab and in the wheel's own software (Pit House for Moza); with the
wheel set wider than the game's full lock it turns on past it and the force feedback drops there. The game profile
`nfspayback.ini` applies 180/180 while the game runs (set Pit House to 180 too). To see what the SDK hands the game,
**`wheelbri-logi.dll`** (`logi/`, built by `scripts/make/logi.sh`) is a logging stand-in for the helper: point the
registry value at it and it passes every call on to Logitech's own helper (G HUB's newest, else the old one, or `[logi]
real_library = <path>` in a `wheelbri-logi.ini` next to it, where `log_level` sets its own log level: `logi.sh` writes
`debug` there), logging what the game asks and gets: the state it reads every frame (`logi.input`, debug), any steering
curve (`LogiGenerateNonLinearValues`, `LogiGetNonLinearValue`), the range, the controller properties, every force
(`logi.ffb`), a summary a minute. Its log is `wheelbri-logi-<date>-<NN>.log` in the log folder (the app's Logs tab does
not list it yet). x64 only.

Steps for NFS Payback: WheelBri running (Target G920), the registry value above naming a Logitech helper that exists
on the PC (or `wheelbri-logi.dll`: `scripts/make/logi.sh` builds it and prints the Administrator PowerShell commands
that point the value at it, and back at G HUB's library), steering rotation 180 in the game's profile (the default)
and in the wheel's own software. More in [docs/games.md](docs/games.md).

### G HUB and the virtual G923

G HUB finds Logitech wheels by their ids and talks to them in HID++ 2.0. With Target **G923** and Mode **Virtual**,
the virtual wheel answers G HUB's identity questions (Root, FeatureSet, firmware version, name and type) and its
force feedback feature: G HUB's Operating Range and gain are kept and logged, not applied (WheelBri passes the
steering 1:1 and never sets the wheel's own software), and effects G HUB uploads are logged, not played. The game's
own force feedback still goes through the safety limiter. The G920, G25 and G27 targets do not answer HID++. After a
driver update, reinstall the driver first. The `hid++:` lines are in `wheelbri-driver-<date>-<NN>.log` (every
request at Level debug). What is known and what is open: [docs/ghub-hidpp-plan.md](docs/ghub-hidpp-plan.md).

## Functionalities

One entry per finished functionality (it may span several commits), added in the commit that completes it:
a headline, why it was needed, how to use it, and where to look.

### Clutch pedal

- **Why:** the add-on clutch pedal was recognised but the G920's clutch never moved: the configs mapped axis 3,
  while the pedal moves R3 axis 6 (found in `raw_a6` in the telemetry).
- **Use:** plug the clutch into the R3 pedal set; `[clutch] axis = 6` in `config/g920.ini`. If it doesn't reach 100% at
  the floor, or doesn't return to 0%, set its range or dead zone in Pit House (WheelBri uses the axis as the device
  reports it). The game reads it as the G920's Rz axis.
- **Look:** `wheelbri-service --ui` (axis 6 shows "-> clutch", Clutch row 0–100%); `raw_a6`, `clutch` and
  `out_clutch` in `telemetry-<date>-<NN>.log`. Code: `Mapper::mapPedal` (`core/src/Mapper.cpp`).

### Virtual G920 (our own driver)

- **Why:** every game should see a G920 without anything copied into it, on any wheel; third-party virtual-device
  tools (HIDMaestro, ViGEm) are not allowed.
- **Use:** **Mode** **Virtual** on the Mapping tab, then Start (no Administrator: the WheelBri Helper does
  it). Start installs the
  `wheelbri-g920` driver itself when Windows lacks it; Stop or closing the window removes the device.
- **Look:** joy.cpl lists "Logitech G920 Driving Force Racing Wheel USB"; `wheelbri-driver-<date>-<NN>.log`, the
  service's `service.g920` lines. Code: `driver/Driver.cpp`, `service/src/VirtualG920.cpp`, core `HidG920`, `HidPid`.

### Hiding the rig from games

- **Why:** with the real wheel visible too, a game (NFS) may pick it instead of the G920.
- **Use:** automatic with the virtual G920: every device of the active rig is hidden while a session runs, and only
  WheelBri may open it. The first Start puts the `wheelbri-hide` filter on each device, once per installation (the
  device reconnects once, so the rig is gone for a few seconds): a popup says so ("Hiding your wheel from games: first
  time only") and closes by itself when it is done. `--no-hide` skips it. A crash should leave nothing hidden (the
  filter hides nothing once the allowed process is gone, and the WheelBri Helper shows the rig again when the app's
  connection closes); `wheelbri-service --unhide` (Administrator) clears the setting by hand if a rig stays hidden.
- **Look:** the state next to Start ("rig hidden (N of M devices)" or, in red, "NOT HIDDEN");
  `wheelbri-hide-<date>-<NN>.log`, `wheelbri-hide-install-<date>.log` (also when the WheelBri Helper runs it); the hide
  log names each program that opens the wheel or is refused ("refused process 8964 (steam.exe)"). Code: `driver/hide/`,
  `service/src/HideFilter.cpp`, `ensureHideFilter` in `main.cpp`.

### In-game shim and its games list

- **Why:** a way that needs no driver: a `dinput8.dll` next to the game presents the rig as a G920 to that game only
  (also under Proton).
- **Use:** **Mode** **Shim**, **Games (N)** next to Start: **Add game** (the folder with the game's `.exe`),
  pick it, Start. After the session the app asks whether to delete the shim from the game's folder. Settings: an
  optional `wheelbri-shim.ini` next to the DLL.
- **32-bit games** (NFS Carbon and its era) load only a 32-bit DLL: the app reads the game's largest `.exe` and puts
  in the 32-bit shim (`shim\x86\dinput8.dll`, built by `scripts/make/shim-x86.sh`) or the 64-bit one. Only the shim
  is 32-bit; the app, the service and the drivers stay 64-bit.
- **With the Ultimate ASI Loader** (ThirteenAG's `dinput8.dll`, as in the Widescreen Fixes Pack): the app leaves it
  in place and puts the shim next to it as `dinput8Hooked.dll`, which the loader hands DirectInput to instead of
  Windows' `dinput8.dll`. Install the pack first (its `dinput8.dll` and `scripts\`), then Add game.
- **Look:** `wheelbri-shim-<date>-<NN>.log`; the list is `%LOCALAPPDATA%\wheelbri\shim-games.txt`. Code: `shim/`, core
  `ShimGames` (`gameArchitecture`, `shimDllIn`), `service/src/ShimGamesView.cpp`.

### Rig tab

- **Why:** WheelBri works with any wheel and accessories, so the user names each device and control, once.
- **Use:** tab R: touch a control (Detect) and name it: type a name, or press one of the names the popup offers (**It
  is**: Steering, Throttle, Brake, Clutch, Handbrake for an axis; A, B, X, Y, Menu, View, RSB, LSB, Xbox, the D-pad (D
  Up, D Right, D Down, D Left), the paddles, Shift up/down, Gear 1-7, Reverse for a button); Enter; **Save**. The choice
  picked stays lit whatever you do with the name (rename it, delete it; pressed again it is unpicked), and it fills in
  the name only when there is none (never replacing one), except **Button**, whose name you type. A control with no name
  shows its choice. Saved after the name (`Horn | button`, `| a`); WheelBri still maps nothing from it. A control is
  only a name: WheelBri does not guess what a control is for (every wheel is different), so the Mapping tab is where you
  say what drives what. One rig (no rig selector, New or Rename). Older rigs' roles (`Gas | throttle`) load and are
  ignored (a steering axis makes its device the wheel), and go when saved again. There is no device to pick: every
  device of the rig and every other connected game controller (what Windows' Game Controllers lists) is shown at once,
  one section each (its axes, its buttons), scrolled (mouse wheel, the scroll bar at the right, PageUp / PageDown, Home
  / End) when they do not all fit; a rig device that is unplugged is shown as saved (its names, still editable). Each
  device shows its name (an input for a rig device), its id and **Online** / **Offline**. The bottom-left corner lists
  them all the same way (**Forget** takes a device out of the rig). A device joins the rig when one of its controls is
  named (the rig's devices are hidden from games, at most 4). **No dead zones**: WheelBri uses every axis as the device
  reports it (steering from the centre, pedals 0-100% of their travel); set dead zones and pedal ranges in the device's
  own software (Pit House). Older rigs' `deadzone N%` / `full N%` and configs' `deadzone`, `deadzone_low`,
  `saturation_high` load and are ignored, and go when saved again.
- **Look:** `%LOCALAPPDATA%\wheelbri\rigs\<rig>.ini`; `service.setup` log lines. Code: core `Rig`,
  `service/src/WheelSetupView.cpp`.

### The window

- **Why:** screens differ; the window should use the room it has without black bars or tiny text.
- **Use:** drag the window's edges or maximise it. Up to 1.5 times the design (times the Windows display scale)
  everything grows with it; past that, or when the window is wider or taller than the design's shape, the tabs get
  the room: wider fields, bars and button cells, more picker columns, more live log and event rows, the buttons at
  the right edge stay there and the tab bar stays at the bottom. It cannot be made smaller than three quarters of the
  design (960 x 748 at 100 %), nor bigger than the screen.
- **Look:** code: `service/src/UiWindow.hpp` (`fitCanvas`), `UiWindowWin32.cpp` (`WM_GETMINMAXINFO`), `UiWindowX11.cpp`
  (size hints); the views read `UiWindow::width()` / `height()`.

### Mapping tab

- **Why:** the target (the G920) is fixed; which rig control drives each of its axes and buttons is the user's.
- **Use:** tab M while stopped: click a G920 input and pick a rig control (there is no Auto-map: you decide);
  **Save** writes your own mapping ("custom" then shows left of **Default**; hover it for why), **Default** goes back
  to WheelBri's. While running the same tab is the live view. The buttons are grouped by what they are: FACE (A, B,
  X, Y) and PADDLES in the left column, STICKS (LSB, RSB) and SYSTEM (Menu, View, Xbox) in the right, the SHIFTER's
  gears below both (the G25: BLACK and PADDLES, WHEEL and RED, SHIFTER; the G27: WHEEL and PADDLES, RED and
  BLACK, SHIFTER; the G923: the G920's with PLUS MINUS on the left and DIAL on the right; `ButtonGroup` in each
  profile).
  Every G920 input has one **role assignment** (its main source), and every G920 button can also have up to 8
  **secondary assignments** ("or"): buttons, or named axes (a pedal or a lever makes sense: one-sided) that press it
  past a threshold of their travel (25-75%, 50% by default), e.g. G920 X <- the wheel's X *or* a handbrake lever *or*
  a button box button. Saved as
  `[buttons] target_N_or = 30b7:1001/axis 3 > 50%, 1234:5678/0`; the live view marks such a button with "+".
  **Each rig control is used once** in a mapping: as one input's role assignment or as one button's secondary. Both
  pickers show a control used elsewhere greyed, and hovering over it says where ("Role: G920 A", "Secondary of G920
  X"); clear it there first. A control used twice (an older mapping) is red, with both places in its tooltip; Save
  refuses until it is fixed, and Start warns about it. A source pointing at a control the rig has not named is amber.
  There is no wheel line: the wheel (it plays force feedback) is the rig's first device of kind wheel, else its first
  device (an older config's `source_device` still names it).
  **Steering rotation**: the game's full lock (slider, typed degrees, or 180 / 270 / 360 / 540 / 720 / 900 / 1080).
  **Set your wheel's own software (Pit House for Moza) to the same number**: WheelBri passes the wheel through 1:1 and
  never reads or sets the wheel's own settings, so the wheel's end stop is the game's full lock only when they match.
  There is no separate wheel rotation (since 2026-10-13; an older config's `source_rotation_deg` is ignored and goes
  when saved). A game's own rotation (NFS Payback: 180) is set in the Game profiles tab. Config key: `[steering]
  target_rotation_deg`. No dead zones: set them in the devices' own software.
- **Look:** `%LOCALAPPDATA%\wheelbri\configs\my-<target>.ini` (`my-g920.ini`, ...); telemetry `raw_*` and `out_*`
  columns. Code: core `MappingEdit`, `AxisTuning`, `Mapper`, `service/src/MappingEditView.cpp`.

### Game profiles tab

- **Why:** a few games need something changed (NFS 2015's pedals, NFS Payback's rotation, NFS Unbound's force
  feedback); everything about one game is in one place, with what WheelBri does for it and why.
- **Use:** tab G. On the left the games that have a profile; pick one to see it in three sections, each with what the
  profile does (and why) and your own values: **Mapping** (the pedals the profile remaps, the steering rotation),
  **Force feedback** (the gain), **Game config** (the game's own settings and what its settings file holds now;
  left out for a game whose own settings WheelBri does not change). No target here: the target is the Mapping tab's.
  Your values are saved in the mapping as you change them (this tab has no Default, Save or Revert: those are the
  Mapping tab's, and its unsaved changes stay unsaved), used from the next Start:
  **Steering rotation**: type another (`180`), **On/Off** (Off = the Mapping tab's), or **Default** for the profile's;
  saved as `[game_rotation] nfspayback = 180 | off` (an older `360/180` reads as 180). Set the wheel's own software to
  the same while you play that game, and back afterwards. A game that keeps its own rotation (NFS Unbound's
  `WheelMaxRotation`) shows it again in Game config as **steering rotation**: one value in two places, so changing
  either changes both, and Save to file writes it. **Force feedback gain**: 0.25 to 4 or **Default**, on the game's
  pushing forces (steering force, rumble) before the safety limits; saved as `[game_ffb_gain] nfsunbound = 3`.
  **Game config** (NFS
  Unbound's hidden wheel settings: force feedback, spring): 0 to 1 or **Default**, saved as
  `[game_settings] nfsunbound.spring = 0.8`. WheelBri never writes them by itself: **Save to file** writes the values
  shown into the game's settings file (refused while the game runs: it writes its own values back when it closes),
  keeping the original as `<file>.before-wheelbri`. When the file is not where the profile says (Documents), the
  button is **Locate file**: pick it in the Open dialog (it must hold the game's settings; remembered in `app.ini` as
  `game_file.<id>`), and it becomes Save to file (**Locate...** picks another). Windows only for now (no file dialog
  on Linux). The tab reads the file every 2 s and says whether it holds the values shown; the column on the right is
  the value in the file (amber until it is written). While WheelBri runs, the tab opens by itself when a game with a
  profile starts, on that game, and says that it runs (pid, for how long, the rotation and gain in use); it
  does so once per start, so you can go to another tab while you play. Values cannot be changed while it runs.
- **Look:** the service log (`service.game` lines; `Game profiles tab: <game> started`); `config/games/<id>.ini`. Code:
  core `GameProfiles`, `GameTab` (`GameTabFollower`), `GameSettings` (`readGameSettingsState`),
  `service/src/GameWatch.cpp`, the Games page in `service/src/MappingEditView.cpp`.

### Game force feedback through the safety limiter

- **Why:** rigs are often direct drive: no game force may reach the wheel unchecked.
- **Use:** automatic on both targets. `[ffb] master_gain`, `max_magnitude` (0.8), `max_slew_per_second` (50) in the
  config; `[ffb] invert = true` if forces pull the wrong way. The forces stop when the game goes quiet (500 ms). A
  constant push the game leaves exactly as it is for `[ffb] frozen_force_s` (2 s; 0 = never) fades out over half a
  second, with its rumble: NFS 2015 on a loading screen kept sending 83% and kicks for 21 s. Springs and dampers stay;
  the next change brings the forces back. The service log says when ("ffb: the game has held a ... push unchanged"). A
  game profile's force feedback gain (Game profiles tab) multiplies the game's pushing forces before these limits. The
  settings are in the target's config (`config/g920.ini`, ...; `master_gain` 0.7 there) and in your own mapping once
  saved (`my-<target>.ini`); there is no force feedback page in the app. To check the direction and that forces reach
  the wheel, with no session running (Stop in the app): `wheelbri-service --ffb-test` (`--config` for your own mapping;
  a gentle spring, a push left, a push right, a damper, through the same limiter; it says what to feel at each step, and
  to set `[ffb] invert = true` if the left push goes right).
- **Look:** `ffb-<date>-<NN>.log` (`kind=wheel_*` lines: the delay of every call to the wheel); the service log's `ffb
  delay` table (with the usage table, every 10 minutes and at the end); the Force feedback tab (`--ffb-tab`). Code: core
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

### Windows 7 to 10: the in-game shim only

- **Why:** the virtual G920's driver needs Windows 11 (on Windows 7 it would need a kernel driver of ours, which
  64-bit Windows 7 loads only in test-signing mode), but the in-game shim needs no driver at all.
- **Use:** the same Setup.exe. Before Windows 11 it installs the app, the configs and the shim, without the drivers, the
  WheelBri Helper or a restart; Windows 7 needs KB2999226 (the Universal C Runtime) and Windows Management Framework 5.1
  first (Setup says which is missing): in `installer/win7/`, `download-prerequisites.cmd` downloads them (with .NET
  4.5.2, which WMF 5.1 needs) into that folder and `install-prerequisites.cmd` installs them on the Windows 7 PC (both
  cmd and Windows PowerShell 2.0 or newer only, so they run on Windows 7 itself). In the app, Mode stays Shim (its
  arrows are off).
- **Look:** the app's log says `Windows build <N>` and `Mode: Shim only`. Code: `platform::windowsBuild`,
  `ConfigPicker::virtualDeviceSupported` (build 22000), `installer/install.ps1` (`$withDrivers`), `Setup.cmd`.
- **Why it can run there:** every Windows function the programs and the shim import exists on Windows 7 (the newest
  are Vista and 7 ones); the Windows 8+ ones (`SwDeviceCreate`, `CM_Register_Notification`, the DPI calls, thread
  names) are looked up at run time and skipped when missing. The one dependency Windows 7 lacks is the Universal C
  Runtime (KB2999226): without it the app does not start and a game cannot load the shim.
- **Status: not yet tried on a real Windows 7 (or 10).** To test: a fully updated Windows 7 SP1 x64 PC or VM with
  WMF 5.1 and the wheel passed through over USB; install (app, configs and shim; no drivers, no Helper), then in the
  app Mode Shim (Virtual greyed out), Add game, Start, a game (NFS 2015 runs on Windows 7) with steering, pedals,
  buttons and force feedback, the shim's and the service's logs; then the uninstaller.

### Logs tab and one logs folder

- **Why:** one place to see why something failed: the app's, the shim's, both drivers' and the WheelBri Helper's logs
  together.
- **Use:** tab L (or O for the folder): one line per component, then their lines merged live (scroll, Wrap, and Level:
  warn, info, debug, trace for every component, see [Logging](#logging)). One file per run of each program, and old ones
  go after 2 days (see [Old files](#old-files), `--old-files`). The **Helper** line also says what the app last heard
  from the WheelBri Helper (`running 0.9.0, serves this app`, `not running ...`, `not for this copy of WheelBri` for a
  development build), and turns red with `2 err, 1 warn, last 14:03` when today's helper log has warnings or errors. The
  app says Hello to the helper when it opens (and every 5 minutes on this tab), which also makes the helper write what
  it logged since boot into your logs folder.
- **Look:** `%LOCALAPPDATA%\wheelbri\logs`. Code: core `LiveLog`, `LogLevelCounter`, `HelperStatus`, `Retention`,
  `service/src/MappingView.cpp`, `service/src/HelperProbe.cpp`.

### Running as a standard user (WheelBri Helper)

- **Why:** end users are standard users, and the app should not ask for Administrator every time it opens.
- **Use:** nothing to set: the installer adds the **WheelBri Helper** service (`WheelBriHelper`, starts with Windows);
  `wheelbri.exe` runs as you and asks it for the virtual G920, hiding the rig, installing a missing G920 driver and
  letting the drivers write into your logs folder. Started as Administrator (a development build), the app does those
  itself. Without either, the status next to Start says **HELPER NOT INSTALLED** in red (Mode Virtual only).
- **Look:** `wheelbri-helper-<date>-<NN>.log` in the logs folder; the app's log says `through the WheelBri Helper` or
  `as Administrator`; the Logs tab's Helper line; `services.msc` > WheelBri Helper; `wheelbri-helper.exe --hello`.
  Code: `service/helper/` (pipe server, sessions, log), `service/src/PrivilegedSteps.cpp` (the app's side),
  `service/src/Privileged.cpp` (the steps), `core/include/wheelbri/HelperProtocol.hpp` (the requests and their rules),
  `HelperDispatch.hpp` (how each message is answered), `DailyLogFile.hpp` (its log before anyone connected).

### App icon

- **Why:** the app had Windows' generic icon everywhere.
- **Use:** nothing to set: both programs carry it, so Explorer, the taskbar, the window, the Start menu shortcut and
  Settings > Apps show it. The real wheel (solid) turning into pixels (the virtual one); the orange hub is the bridge.
- **Look:** `service/app/wheelbri.svg` (the drawing), `wheelbri.ico` (16 to 256 px, made from it), `wheelbri.rc.in`,
  `wheelbri-service.rc.in`; the window takes it in `service/src/UiWindowWin32.cpp`.

### Service loop that wakes on input (events)

- **Why:** polling the wheel 1000 times a second cost the loop about 8 % of a core, mostly on ticks with nothing new.
- **Use:** nothing to set (the only loop since 2026-10-07; `loop_mode` and `loop_hz` in an older config are ignored):
  the loop sleeps until the wheel, an accessory or the game's force feedback has something new, and wakes by itself only
  for what must run on time (every 4 ms while forces play and for 100 ms after new input). A device DirectInput can only
  poll, or notifications that miss input, make it also poll every millisecond, as the old loop did, while it still wakes
  at once for everything else.
- **Look:** the service log's "running on events" line (or "running on events, and polling every 1 ms (why)") and, once
  a minute, "loop:": wake-ups by reason, events or polling, and the notifications missed or late. Measured in NFS
  2015: the loop thread at 2.3 % of a core instead of 8 % (`docs/cpu-measurements.md`). Code: core
  `EventLoopControl`, `WakePlanner`, `platform::WaitSet` (`PlatformWin32.cpp`, `PlatformPosix.cpp`), `DinputReader`;
  tests `PushLoopTest` (emulators), `WaitSetTest`; measurements `docs/cpu-measurements.md`.

## Layout

| Path | Contents |
|---|---|
| `core/` | Portable library, no OS dependencies: `Mapper`, `DeviceProfile`, `FfbSafetyLimiter`, `Pipeline`, `EmulatorConfig`/`IniDocument`, `StatePacket` + `UdpLoopback`, `Rig`, `GameProfiles`, `HidG920` / `HidPid` / `HidPp`, `HelperProtocol`, `Logging`, `Telemetry` |
| `service/` | `wheelbri.exe` (the app) and `wheelbri-service`: reads the rig and plays its force feedback with the system's own API (DirectInput on Windows, evdev on Linux), a loop that wakes on input, the window (Win32 on Windows, X11 on Linux), `--list` / `--print` / `--ui` / `--ffb-test` |
| `service/helper/` | `wheelbri-helper.exe`, the WheelBri Helper service (Windows) |
| `shim/` | `dinput8.dll` proxy (Windows / Wine): wraps `IDirectInput8A/W`, devices and effects |
| `logi/` | `wheelbri-logi.dll`: logging stand-in for Logitech's wheel SDK helper DLL (games built with the SDK: NFS Payback) |
| `driver/` | The UMDF 2 drivers `wheelbri-g920` (the virtual wheel) and `hide/` (`wheelbri-hide`), their install scripts and the signing certificate script ([README](driver/README.md)) |
| `installer/` | What the Windows package carries besides the build outputs: `install.ps1`, `uninstall.ps1`, `Setup.cmd`, `README.txt` (the end user's guide), `win7/` prerequisites |
| `tests/` | GoogleTest unit tests for `core/` (and a few service parts) |
| `config/` | Each target's built-in config with WheelBri's default mapping (`g25.ini`, `g27.ini`, `g920.ini`, `g923.ini`), `wheelbri-shim.ini`, and `games/` (the game profiles, compiled in) |
| `docs/` | Per-game notes (`games.md`), Linux (`linux.md`), each target's plan, the G HUB plan, design notes and measurements |
| `scripts/make/` | Build scripts: build and start the app, full rebuild, tests, mapping window, shim, Logitech SDK stand-in, drivers, package, clean PC, measure ([README](scripts/make/README.md)) |
| `scripts/` | `telemetry_summary.sh`: summary of a recorded session (bash and awk); `linux/`: start/stop and Steam launch wrapper, udev rules, the Linux bundle |
| `tools/uhid/` | Phase 4: `wheelbri-uhid`, the virtual G920 for Linux (C++, kernel uhid) |

## Build

The scripts in [`scripts/make/`](scripts/make/README.md) run all of this for you (`app.sh [--test]`,
`rebuild.sh`, `ui.sh`, `shim.sh`, `drivers.sh`, `all.sh`, `package.sh`, `clean-pc.sh`) from an MSYS2 UCRT64 shell.

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

```bash
# The 32-bit shim, for 32-bit games (Windows: scripts/make/shim-x86.sh, MSYS2's mingw-w64-i686-gcc, which needs
# /mingw32/bin first on PATH: the script sets it)
cmake -S . -B build-x86 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw32.cmake \
      -DCMAKE_BUILD_TYPE=Release -DWE_BUILD_SERVICE=OFF -DWE_BUILD_TESTS=OFF -DWE_BUILD_LOGI=OFF
cmake --build build-x86 --target wheelbri_shim     # -> build-x86/shim/dinput8.dll
```

Tests: `ctest --test-dir build` (all), `ctest --test-dir build -R Mapper` (one suite), or
`scripts/make/app.sh --test` (everything built and tested before the app starts; nothing starts on a failure).

Drivers (Windows): built with Visual Studio and the WDK, not MSYS2 (`scripts/make/drivers.sh`, `-Install` to install
them). They are signed with a certificate made once on the build PC from an Administrator PowerShell
(`driver\new-signing-cert.ps1`); see [driver/README.md](driver/README.md) for building, signing and installing them by
hand. The installer for another PC: `scripts/make/package.sh` (see [Installing on another
PC](#installing-on-another-pc)). Linux and the Steam Deck: [docs/linux.md](docs/linux.md)
(`scripts/linux/deck-setup.sh`, `deck-build.sh`, `package.sh`).

## Phase 0: recording what the game does

1. Copy `dinput8.dll` next to the game's `.exe`.
   - Linux/Proton: same folder, and set the Steam launch option `WINEDLLOVERRIDES="dinput8=n,b" %command%`.
2. Plug in the wheel and start the game. Open its controller settings and drive for a minute.
3. Read `wheelbri-shim-<date>-<NN>.log` in `%LOCALAPPDATA%\wheelbri\logs` (see [Logging](#logging)). Things to look for:
   - `EnumDevices ... device '...' product=VID_xxxx&PID_xxxx ... -> game STOPPED here`: which device the game picked.
   - `GetProperty(DIPROP_VIDPID)` or `DIPROP_PRODUCTNAME`: the game checks the identity.
   - `CreateEffect(GUID_ConstantForce)` / `SetParameters`: the force feedback it sends.
   - Nothing at all: the game does not use `DirectInput8Create` (check for `LogitechSteeringWheel*.dll` in the
     game folder, or HID/XInput use), and Phase 2 needs a different shim.
4. Also check: does the game have anti-tamper that rejects a proxy DLL? (The log's first line appears
   or it does not.)

## Playing (Phases 2 and 3)

The app does all of this from its Mapping tab (Mode Shim, Games). By hand:

1. Start the service and leave it running: `wheelbri-service --config config/g920.ini`
   (add `--ui` to watch the mapping at the same time).
2. Copy `dinput8.dll` next to the game's `.exe` (Linux/Proton: also `WINEDLLOVERRIDES="dinput8=n,b" %command%`).
   Optionally copy `config/wheelbri-shim.ini` there too; the defaults match the example service config.
3. Start the game. It should list a "Logitech G920 Driving Force Racing Wheel USB" instead of the R3.

What the shim does: when the game enumerates devices, the wheel with USB vendor id `source_vid` (`346e`, Moza, by
default; set your wheel's vendor id, and `source_pid` if needed, in the game's `wheelbri-shim.ini`: the Rig tab shows
each device's `vid:pid`; the app does not set it) is offered as the target, e.g. a G920 (VID 046D, PID C262,
driving-wheel type, X = wheel, Y = throttle, Rz = brake, Slider = clutch as a real G920 reports them on Windows; NFS
2015's game profile changes that to throttle Slider, brake Y, clutch Rz; 19 buttons (the wheel's 11; 13-18 the Driving
Force Shifter's gears 1-6, 19 reverse), 1 hat, pedals released at the top of the range). If a game reads the pedals from
other axes, or backwards, change `throttle_axis` / `brake_axis` / `clutch_axis` / `invert_pedals` in
`wheelbri-shim.ini`; no rebuild needed. Every read (`GetDeviceState`, `GetDeviceData`) is answered from the latest
packet the service sent, in the data format and ranges the game asked for. If the service stops sending for
`stale_after_ms` (1500 ms) the wheel reads as centred with pedals released. Everything else the game does is still
logged to `wheelbri-shim-<date>-<NN>.log`.

Force feedback (Phase 3): the game's DirectInput effects (constant, spring, damper, friction, inertia, periodic; ramps
from start to end over their duration; envelopes, i.e. attack and fade; no custom forces) are kept by the shim and sent
to the service on `ffb_port` (41330) as `FfbPacket`s. The service passes every effect through `FfbSafetyLimiter` (`[ffb]
master_gain`, `max_magnitude`, `max_slew_per_second`) and plays it on the R3 (DirectInput on Windows, evdev on Linux);
if the game goes quiet for 500 ms, every force stops. The service must be running before the game starts, because it
opens the wheel's force feedback exclusively; the shim then opens the R3 non-exclusively. `force_feedback = passthrough`
in `wheelbri-shim.ini` restores the Phase 2 behaviour (effects straight to the R3's driver, limited only by Pit House),
`off` disables force feedback. If forces pull the wrong way, set `[ffb] invert = true` in the service config.

## Installing on another PC

`scripts/make/package.sh` builds everything in Release and makes `dist/windows/WheelBri-<version>-setup.exe` (plus the
same files as a folder and a zip). On a Windows 11 PC with nothing of it, run the setup and approve the Administrator
prompt (the only one: the app itself runs as the user); at the end it asks whether to restart Windows now (default yes),
which makes sure every driver is in use (the hiding filter often starts only after a restart). It installs **WheelBri**
to `C:\Program Files\WheelBri`, the two drivers (`wheelbri-g920`, the virtual wheel for every target, and
`wheelbri-hide`; trusting the drivers' certificate on that PC), a Start menu folder (the app and "Uninstall WheelBri"),
the WheelBri Helper service and a Settings > Apps entry that uninstalls it all (Windows 7 SP1 to 10: the app and the
shim only, no drivers or Helper). The uninstaller never removes `%LOCALAPPDATA%\wheelbri` (mappings, rigs, logs); with
`-Quiet` (Settings > Apps' quiet uninstall) it moves the configs changed in the install folder to
`%LOCALAPPDATA%\wheelbri\config-backup-<date>-<time>` instead of deleting them. The app hides the rig (every device in
the Rig tab) from games on every Start. The log is `%LOCALAPPDATA%\wheelbri\logs\wheelbri-install-<date>.log` (every
log, the drivers' too, is in that one folder); `installer/README.txt` is the end user's guide.

First start: open WheelBri from the Start menu (no Administrator prompt). It opens on the Rig tab until a rig is
saved: name your controls there and Save, then map them in the Mapping tab (Target, Mode), Save, and press Start.

**Upgrading**: run the newer setup over the old install. It closes a running WheelBri, replaces the program and the
drivers and keeps an edited config in the install folder (the new default goes next to it as `<name>.ini.new`); your
rig, mappings and `app.ini` in `%LOCALAPPDATA%\wheelbri` are not touched. An install from before the rename
(wheelemu) is replaced, its settings moved to `%LOCALAPPDATA%\wheelbri` at the app's first start.

**Uninstalling**: Start menu > WheelBri > Uninstall WheelBri, Settings > Apps, or `Uninstall.cmd` in the install
folder (Administrator; options `-KeepConfig`, `-RemoveCertificate`, `-Quiet`, `-InstallDir <folder>`). It removes
the WheelBri Helper, the drivers, the virtual devices, the shortcuts, the Settings > Apps entry and the program
folder; it asks whether to remove the drivers' certificate. If the program folder is gone, `Uninstall.cmd` from the
unpacked package removes what is left.

**A clean PC** (for testing a fresh install; development tree only): `scripts/make/clean-pc.sh` removes everything
WheelBri put on the PC, `%LOCALAPPDATA%\wheelbri` included unless `-KeepData`; it lists what it found and asks first
([scripts/make/README.md](scripts/make/README.md)).

## The app

`wheelbri.exe` (built next to `wheelbri-service.exe` in `build/service/`) is the everyday way to run it: double-click it
(it runs as you; the WheelBri Helper service does what needs Administrator), press **Start**, then start the game.
**Start** and **Stop** are at the bottom right of every tab, next to the tab buttons (stopped or running), with the
state just left of them (ready, running and for how long, a lost wheel, a game not using it); after "Start failed
(Logs)" the Logs tab says why, in red. While stopped, Start can only be pressed when every device of the rig that the
mapping uses (the wheel, and each accessory the mapping takes an axis or button from) is connected: otherwise the state
says, in red, **NOT CONNECTED:** and their names (or **NO RIG** before a rig is set up, **NO MAPPING** when nothing of
the rig is mapped onto the G920), and Start is greyed out until they appear (the app sees a device the moment Windows
does). Devices of the rig the mapping does not use do not matter. Start shows the Logs tab of the same live view as
`--ui` (axes, buttons, game force feedback, logs: click a line or press O for the folder). **Stop**, or closing the
window, undoes everything (the virtual wheel removed, the rig visible to games again). Each Start reads the config
again, so edits apply without restarting the app. If the wheel is not connected when Start is pressed (Windows restarts
it after WheelBri is reinstalled, and the first Start after that puts WheelBri's filter back on it, which restarts it
again), the service waits up to 15 s for it before giving up; Stop ends the wait.

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
from another controller (a gamepad or the keyboard): the state next to Start / Stop shows **GAME NOT
USING WHEEL** in red and the log says so. Close the game with WheelBri running, start it again and pick the wheel
in its controls settings.

The top row of the Mapping tab (while stopped) has two pickers, each with `<` `>`: **Target**, the wheel WheelBri
emulates, in this order: the **G25** or **G27** for games that know them and not the G920
([docs/g25-target-plan.md](docs/g25-target-plan.md), [docs/g27-target-plan.md](docs/g27-target-plan.md)), the **G920**,
or the **G923** (Xbox One and PC: [docs/g923-target-plan.md](docs/g923-target-plan.md)); others come only when WheelBri
supports them), and **Mode**: **Virtual** (WheelBri's driver, every game sees it, the rig is hidden) or **Shim**
(`dinput8.dll` next to the game's `.exe`, nothing hidden, nothing created; Start writes the target into the game's
`wheelbri-shim.ini`). A target is fixed: its axes, D-pad and buttons are what they are. What drives each of them is the
mapping, and that is the user's: each target's built-in config, one for both modes (`config/g920.ini`, `g25.ini`,
`g27.ini`, `g923.ini`, next to the program or one or two folders up), holds WheelBri's default mapping, used until you
save your own on the Mapping tab (`%LOCALAPPDATA%\wheelbri\configs\my-g920.ini`, `my-g25.ini`, `my-g27.ini`,
`my-g923.ini`), again for both modes: switching Mode never changes the mapping. **Default** deletes yours. A mapping
saved when each mode had its own (`my-g920-virtual-device.ini`, `my-g920-in-game-shim.ini`, before 2026-10-15) becomes
`my-g920.ini` once: the newer of the two (the older files are left as they are). The target and the way are remembered
(`%LOCALAPPDATA%\wheelbri\app.ini`, `[app] target`, `[app] way`). The virtual G25, G27 and G923 need a `wheelbri-g920`
driver that binds `wheelbri\G25`, `wheelbri\G27` and `wheelbri\G923` (one built since they came):
`scripts/make/drivers.sh -Only g920 -Install` once (the installer's driver has them). Each Start uses the Target's
mapping as the Mapping tab shows it (as saved) and the Mode picked; `--config FILE` given to the app only sets where the
app's own logging starts from, and `--no-virtual-g920` is overridden by Mode. `wheelbri-service --app` is the same
window from a console (and on Linux, where Mode Virtual starts `wheelbri-uhid`: [docs/linux.md](docs/linux.md)).

**Games** (only with **Mode** **Shim**): next to Start the app shows the game Start is for and a **Games (N)** button.
It opens the games panel: every game the shim was put into, with its folder and whether the shim is there (shim in place
/ no shim / another dinput8.dll). Click a game to pick it; **Add game** opens Windows' folder window: choose the folder
that holds the game's `.exe` and WheelBri copies the shim there (`dinput8.dll`, and `wheelbri-shim.ini` unless one is
already there) and adds the game. **Put shim** / **Remove shim** per game, and **Forget** to take a game off the list
(its folder is not touched; if WheelBri's shim is still there, the app says so). Start puts the shim back, or replaces
it with a newer build, when needed (close the game first: Windows locks the DLL while it runs). When a shim session
stops, the app asks **Would you like to delete the shim from the target folder?**: Yes deletes both files (the game
stays in the list, marked "no shim"), No leaves them. A `dinput8.dll` that is not WheelBri's (another mod) is never
overwritten or deleted. The list is `%LOCALAPPDATA%\wheelbri\shim-games.txt`.

**What Start runs** depends on the Mode (the same for every Target): only what that Mode needs.

| Component | Mode Virtual | Mode Shim |
|---|---|---|
| Shim check / copy: `dinput8.dll` + `wheelbri-shim.ini` into the picked game | - | yes |
| Read the rig: wheel + accessories (DirectInput, the wheel opened exclusively) | yes | yes |
| Mapping loop, woken by input: rig -> G920 -> UDP 41329 | yes (to the driver) | yes (to the shim in the game) |
| Force feedback in: UDP 41330 -> safety limiter + watchdog -> wheel | yes (from the driver) | yes (from the shim) |
| Logs, telemetry logs, old-file cleanup | yes | yes |
| Game-use watch ("GAME NOT USING WHEEL") | yes | yes |
| Game FFB idle watch ("GAME FFB OFF: ALT-TAB": effects loaded, none started while you drive) | yes | yes |
| Live view: Rig / Mapping / Game profiles / Logs tabs, Start / Stop | yes | yes |
| Stale hide check: clears a hide left by a crashed session (the app when it runs as Administrator; through the WheelBri Helper, the helper does it), warns if another one hides the rig | yes | yes |
| Administrator or the WheelBri Helper (`PrivilegedSteps`): needed for the rows below | yes | - |
| G920 driver check: installs `wheelbri-g920` (`driver\install-driver.ps1`) when Windows does not have it | yes | - |
| Virtual G920 device (`wheelbri-g920` driver) and its DirectInput registry entries | yes | - |
| Hide filter (`wheelbri-hide`): rig hidden from games | yes | - (the game must see the rig) |
| Logs tab rows for the G920 driver and the hide filter | yes | - (emulator and game shim only) |

`wheelbri.exe` runs as the user who starts it (`asInvoker`). With the virtual device, the privileged rows go through
the WheelBri Helper service, or are done in the app when it was started as Administrator.

The **Rig** tab (R) defines your **rig**: the devices you have (wheel, pedals, shifter, handbrake, button box) and what
each control is. Every connected device is read at once. With **Detect** on (the default), press a button or move an
axis (a pedal all the way) and its popup opens: type its name, or pick what it is (Steering, Throttle, Brake, Clutch,
Handbrake, Axis; Button, Handbrake, A, B, X, Y, Menu, View, RSB, LSB, Xbox, D Up/Right/Down/Left, L / R Paddle, Shift
up/down, Gear 1-7, Reverse: kept while you rename it; it fills in the name only when there is none, Button never),
Enter. A pedal found resting at the top of its axis is marked **Inverted**. Clicking an axis row or a button tile opens
the same popup. Every connected device is shown at once (scroll for more); naming a control of a device adds it to the
rig (at most 4 devices; **Forget** in the device list at the bottom left takes one out); give each a name. There is no
"it is a wheel / pedals / ..." choice: the device with a **Steering** axis is the wheel (it plays force feedback).
**Save** writes the one rig, `%LOCALAPPDATA%\wheelbri\rigs\My rig.ini` (an older install keeps the rig it last used;
other rig files from when the tab managed several stay in the folder, unused). The rig starts from names saved per
device by earlier versions. The app opens on this tab until a rig is saved, then on Mapping; Start switches to the Logs
tab so you can see it come up.

**Mapping** (tab M, while stopped; while running the same tab is the live view) maps the active rig's controls onto
the G920: Steering / Throttle / Brake /
Clutch (each with **Invert**), the D-pad and the G920's buttons, each picked by you. Click a source
and pick a control of the wheel or of an accessory (up to 3 besides the wheel), or "(nothing)". **Save** writes your own
mapping for the target (`my-<target>.ini`, comments and other settings kept). **Revert** drops unsaved changes,
**Default** the saved mapping (WheelBri's default again). A source the default names but your rig has not named shows
"not in the rig" in orange: pick another, or name it in the Rig tab. Start uses exactly what the tab shows (as saved).
In the config a source on an accessory is written `vid:pid/N` (`axis = 30b7:1001/0`); a session opens every
accessory its mapping names and does not start without them; one lost while running is waited for like the wheel.
Every device of the active rig (wheel and accessories) is hidden from games while a Mode Virtual session runs; no
rig = nothing hidden.

## Troubleshooting

Read the Logs tab first (press O for the folder): a failed Start says why there, in red.

- **The game sees no wheel.** The state next to Stop must say "Running". Mode Virtual: Windows' Game Controllers
  (Win+R, `joy.cpl`) should list the target ("Logitech G920 Driving Force Racing Wheel USB" for the G920); if not,
  look at the G920 driver row of the Logs tab. Mode Shim: the game's row in the Games panel must say the shim is in
  place, `wheelbri-shim-<date>-<NN>.log` must exist after the game started (none: the game did not load the shim; a
  32-bit game needs the 32-bit one, which Add game picks), and `source_vid` in the game's `wheelbri-shim.ini` must be
  your wheel's vendor id. Some games read wheels through Logitech's SDK instead:
  [Games with Logitech's wheel SDK](#games-with-logitechs-wheel-sdk-nfs-payback). Start the game after Start.
- **Two wheels in the game** (the real one next to the target): in Mode Virtual the state should say "rig hidden
  (N of M devices)"; "NOT HIDDEN: <device>" in red means the filter is not running on it yet: close the wheel maker's
  software and anything else using it, or replug it, then Start again. Every device must be in the rig (Rig tab).
  In Mode Shim nothing is hidden: the shim replaces the wheel only in the game's DirectInput, and only the device
  matching `source_vid` / `source_pid`.
- **The game drives from an Xbox pad or the keyboard**: the state says **GAME NOT USING WHEEL**. Close the game
  with WheelBri running, start it again and pick the wheel in its controls settings. WheelBri makes no Xbox pad.
- **No force feedback**: **GAME FFB OFF: ALT-TAB** means the game loaded its effects and started none (NFS 2015):
  alt-tab out and back. Check that `[ffb] game_effects = true`, that the wheel's own software has force feedback on,
  and with the app stopped `wheelbri-service --ffb-test`. In Mode Shim the shim's `force_feedback` must be `service`
  (the default). The `ffb-<date>-<NN>.log` file shows what the game sent and what reached the wheel. Forces the wrong
  way: `[ffb] invert = true`.
- **A crash left the rig hidden**: normally nothing stays hidden (see [Hiding the rig](#hiding-the-rig-from-games)).
  If a game still cannot see the rig without WheelBri, run `wheelbri-service.exe --unhide` from an Administrator
  prompt in the install folder (no session may be running).
- **HELPER NOT INSTALLED**: start the WheelBri Helper (Win+R, `services.msc`) or run the setup again.
- **NOT CONNECTED / NO RIG / NO MAPPING**: plug in or switch on the devices named, save a rig in the Rig tab, or map
  the rig's controls in the Mapping tab.

## Service command line

`wheelbri-service` (in `build/service/` or the install folder) runs one session from a console, or a window:

```
wheelbri-service [--config FILE] [--list] [--print] [--ui] [--ffb-test] [--old-files delete|archive|keep]
                 [--stop-file FILE] [--virtual-g920 [--no-hide]] [--unhide] [--prepare-hide]
                 [--app [--no-virtual-g920]] [--telemetry-tab] [--ffb-tab]
```

| Option | What |
|---|---|
| `--config FILE` | the config to run: a target's `config/<target>.ini` or your own `%LOCALAPPDATA%\wheelbri\configs\my-<target>.ini` (without it, the built-in defaults) |
| `--list` | lists the connected game controllers (`vid:pid  name`) and exits |
| `--print` | prints live raw and mapped values |
| `--ui` | a window with the live view (Rig, Mapping, Logs tabs; keys R, M, L, O for the logs folder) |
| `--ffb-test` | plays a short, gentle force feedback sequence on the wheel and exits |
| `--old-files delete\|archive\|keep` | overrides `[retention] old_files` for this run |
| `--stop-file FILE` | stops cleanly when FILE appears (deleted then, and at start) |
| `--virtual-g920` | the virtual wheel of the config's `target_profile` (G25, G27, G920 or G923) while running, the rig hidden; Administrator or the WheelBri Helper; on Linux it starts `wheelbri-uhid` |
| `--no-hide` | with `--virtual-g920`: leaves the rig visible to games |
| `--unhide` | after a crash: makes the rig visible to games again and exits (Administrator; not while a session runs) |
| `--prepare-hide` | puts the hide filter on the saved rig's plugged-in devices and exits (the installer; Administrator) |
| `--app` | the app's window with Start / Stop (what `wheelbri.exe` is) |
| `--no-virtual-g920` | with `--app`: accepted, but each Start follows the Mode picker |
| `--telemetry-tab`, `--ffb-tab` | adds the Telemetry (T) / Force feedback (F) tab to the window; hidden without them |

Any other argument prints the usage and exits with code 2. Only one session runs per computer at a time (the app's
included). Examples:

```bash
wheelbri-service --list                                          # connected joysticks
wheelbri-service --config config/g920.ini --print                # live raw -> mapped -> G920 values
wheelbri-service --config config/g920.ini --ui                   # window: the rig, its G920 mapping, game FFB
wheelbri-service --config config/g920.ini --ffb-test             # gentle spring / left / right / damper
```

## Logging

Built on spdlog. Every line carries time, level, component, thread and source location:

```
[2026-09-29 18:31:15.887] [info ] [service       ] [tid 1202769] source_device='MOZA' ... (main.cpp:107)
```

**Where**

| Program | File |
|---|---|
| service, Windows | `%LOCALAPPDATA%\wheelbri\logs\wheelbri-service-<date>-<NN>.log` |
| service, Linux | `$XDG_STATE_HOME/wheelbri/` or `~/.local/state/wheelbri/wheelbri-service-<date>-<NN>.log` |
| service, macOS | `~/Library/Logs/wheelbri/wheelbri-service-<date>-<NN>.log` |
| shim (in the game) | `%LOCALAPPDATA%\wheelbri\logs\wheelbri-shim-<date>-<NN>.log` (next to `dinput8.dll` if `LOCALAPPDATA` is not set) |
| Logitech SDK stand-in (in the game) | `%LOCALAPPDATA%\wheelbri\logs\wheelbri-logi-<date>-<NN>.log` |
| drivers, WheelBri Helper, installers (Windows) | the same folder: `wheelbri-driver-`, `wheelbri-hide-`, `wheelbri-helper-<date>-<NN>.log`; `wheelbri-install-`, `wheelbri-driver-install-`, `wheelbri-hide-install-<date>.log` (daily) |
| `wheelbri-uhid` (Linux) | the service's folder, `wheelbri-uhid-<date>-<NN>.log` |

One file per run of each program (`<date>` = `2026-09-30`, local time; `<NN>` = `01`, `02`, ... the run's number that
day), so every start of the app, the game (shim), a driver or the helper has a file of its own; a program running past
midnight moves to a new file of the next day. The app's telemetry and ffb files take the app's run number
(`wheelbri-service-2026-10-08-03.log` goes with `telemetry-2026-10-08-03.log`). The Logs tab follows each
component's latest run. Old files are deleted after 2 days by default, see [Old files](#old-files). The Logs tab
shows each component's file (click a line, or O, to open the folder).

**Levels**: `trace` per-tick/per-call detail · `debug` decisions and state changes · `info` lifecycle ·
`warn` recoverable problems · `error` a feature is broken · `critical` the process cannot continue.

**The app's Level button** (Logs tab, or V there): `warn` (the default: warnings and errors only), click for `info`,
`debug`, `trace`, and back to `warn`. Warnings and errors are always written; nothing quieter can be picked. It is
remembered (`[app] log_level` in `%LOCALAPPDATA%\wheelbri\app.ini`) and applies to every component:

| Component | How it gets the level |
|---|---|
| app and session | at once |
| shim in the game | `app.ini` when the game starts, then the session's `LogLevelPacket` on 41329 (at once, and every 2 s) |
| `wheelbri-uhid` (Linux) | the session's `LogLevelPacket` |
| WheelBri Helper | the app's Hello (at start, every 5 min on the Logs tab, at once on a click) and `SetLogLevel` |
| G920 driver, hide filter | `HKLM\SOFTWARE\wheelbri LogLevel`, written at each Start and on a click while a session runs (as Administrator, or by the helper); read at most once a second |

Effect events in the driver log (`pid: start block 1 ...`) are `debug`: set the Level to debug before a race you want
to look at. The driver's force feedback trace (`PidTraceLines`) writes at `trace`. The drivers write their level in
brackets like the other logs: `[2026-10-08 21:14:05.120] [debug] pid: ...`. The command-line service
(`wheelbri-service`) keeps its config's `[logging] level` (default `warn` now) and passes that on the same way.

**Per-component levels without rebuilding**, through `SPDLOG_LEVEL` (the command-line service and the shim at its
start; in the app the Level button sets every component's level instead):

```bash
SPDLOG_LEVEL="info,core.pipeline=trace,service.ffb=debug" wheelbri-service --config ...
```

Components: `service`, `service.input`, `service.ffb`, `service.ffbtest`, `service.ui`, `service.g920`, `service.uhid`
(the app starting wheelbri-uhid, Linux), `service.files` (old files), `service.hide`, `service.helper`, `service.game`
(game detection, game profiles), `service.log` (the Level button), `helper`, `helper.pipe`, `service.config`,
`service.mapping`, `service.shim`, `service.setup`, `service.devices`, `core.mapper`, `core.pipeline`, `core.net`,
`core.ffb`, `core.telemetry`, `logging`, `shim`, `shim.loader`, `shim.di`, `shim.device`, `shim.state`, `shim.feed`,
`shim.emu`, `shim.effect`, `shim.ffb`, `shim.config`, `shim.audit`, `logi`, `logi.query`, `logi.input`, `logi.ffb`,
`logi.leds` (`wheelbri-logi.dll`), `tools.uhid` (`wheelbri-uhid`, Linux).

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

While it runs, the service records to two more log files per run in the log folder
(`%LOCALAPPDATA%\wheelbri\logs` on Windows). Nothing is sent anywhere. Every file WheelBri writes is a log in the same
format (`[date time] [level] [component] message`; no CSV, no JSON); in these two the message of a row is
`key=value` pairs starting with `t_ms=`, and a key that does not apply is left out.

- `telemetry-<date>-<NN>.log`: a row whenever something changes (checked 50 times a second, plus one row a minute when
  nothing does) with the physical wheel (raw axes, buttons, hat), the mapped state, the values the emulated G920 reports
  to the game, the game's force feedback per slot (what the game asked for and what reached the wheel after the
  limiter), and loop timing.
- `ffb-<date>-<NN>.log`: every force feedback change the game sent, with all effect parameters (the shim's periodic
  re-sends of an unchanged effect are left out), and one `kind=wheel_play` / `wheel_stop` / `wheel_stopall` line per
  call the service made to the wheel: `wait_us` (queued before the call) and `call_us` (the wheel's driver).
  `scripts/telemetry_summary.sh` prints them as a delay table.

A new file starts with lines saying what the keys mean. Every session of the run is appended to the run's files,
after a `session started ...` line and lines with the version, the device and the config; a session running past
midnight continues in the next day's files. Each line has its date and time; `t_ms` is the time since the session
started. Settings are in
`[telemetry]`: `enabled`, `rate_hz`, `only_changes` (default `true`; `false` writes every sample),
`heartbeat_s`, `min_change` (smallest axis move that counts, default 0.0005 = 0.23° of steering: smaller
moves are sensor noise), `max_file_mb` (per file) and `directory` (on Windows inside `%LOCALAPPDATA%\wheelbri`;
elsewhere it is ignored, with a warning in the log). A value holds until the next row.

The Logs tab (O opens the folder) shows the folder and each component's newest log; the **Telemetry** tab (T, only
with `--telemetry-tab`) shows the telemetry files and the latest button, pedal and force feedback events.

For a quick summary of the newest session (axis ranges, pedal use, force feedback per
effect type, loop timing):

```
scripts/telemetry_summary.sh            # or pass a telemetry-*.log / a folder; --all for every session of the run
```

## Old files

Logs (every component's) and telemetry older than 2 days are **deleted** by default. The service checks at start-up and
again after every midnight. Only its own files are touched (`wheelbri-` followed by `service`, `shim`, `logi`, `driver`,
`hide`, `install`, `helper`, `uhid`, `sh`, `bridge` or `logs-`; `telemetry-*`, `ffb-*`; the same with the old name
`wheelemu-`, and archives made from them). `[retention]` in the config:

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
user who last ran WheelBri: it connected, or said Hello).
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
  LocalService and cannot look into the app's process).
- The driver signing certificate is made on the build PC (`driver\new-signing-cert.ps1`); the uninstaller removes, by
  thumbprint, only the certificates WheelBri recorded at install (`HKLM\SOFTWARE\wheelbri\SigningCertificates`) and the
  signers of WheelBri's own driver catalogs, in Windows' driver store or in the uninstaller's `driver\` folder (the
  install folder's or the package's; `driver\common.ps1` `Get-SigningThumbprints`); never a certificate found by its
  name.

## Licence note

Nothing is copied from GIMX (GPLv3). `tools/uhid` is written from the kernel's `linux/uhid.h` alone, and the
virtual G920's HID descriptor comes from HIDMaestro's builder (MIT, noted in `core/src/HidG920.cpp`). The Logitech
FFB command decoder, if it is ever needed, may borrow from GIMX; if code is copied rather than re-implemented,
this project must become GPLv3.
