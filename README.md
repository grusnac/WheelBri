# WheelBri

WheelBri presents your rig (a wheel and its pedals, shifter or button box) to PC games as a Logitech wheel they
support: **G25**, **G27**, **G920** or **G923**. The game's force feedback goes back to your wheel through a safety
limiter. It runs on Windows and on Linux (Proton, including the Steam Deck). Built and tested with a **Moza R3**.

- **Using it:** [docs/user-guide.md](docs/user-guide.md)
- **Games it was tested with:** [docs/games.md](docs/games.md)

## Download

Releases: <https://github.com/grusnac/WheelBri/releases>

| System | File | What you get |
|---|---|---|
| Windows 11, 64-bit | `WheelBri-<version>-setup.exe` (or the `.zip`, run `Setup.cmd`) | Mode Virtual (a virtual wheel every game sees) and Mode Shim |
| Windows 7 SP1 to 10, 64-bit | the same | Mode Shim only (**untested** on a real Windows 7 or 10) |
| Linux x86-64, Steam Deck | `wheelbri-linux-x86_64.tar.gz` | both, under Proton (**not yet tried** on a Deck) |

## Status

| Part | State |
|---|---|
| Target G920 on Windows 11, Mode Virtual and Mode Shim | **working** in four Need for Speed games ([docs/games.md](docs/games.md)) |
| Target G25 on Windows 11, Mode Virtual and Mode Shim | **working** in Race Driver: GRID ([docs/games.md](docs/games.md)); its centring spring untried |
| Targets G27, G923 | built and unit-tested, **not yet tried in a game**; button orders partly guessed ([Targets](#targets)) |
| G HUB with the virtual G923 | tried once; later changes untried ([G HUB and HID++](#g-hub-and-hid)) |
| Linux (`wheelbri-uhid`, the shim under Proton) | built and unit-tested, **not yet tried on a Steam Deck** |
| Windows 7 to 10 | built, **not yet tried** |

## Building

You need CMake 3.24 or newer, a C++20 compiler and internet on the first configure (CMake fetches spdlog 1.15.3 and
GoogleTest 1.15.2). Nothing else: the program uses DirectInput and Win32 on Windows, evdev and X11 on Linux. It is
built with GCC (MinGW-w64 on Windows); other compilers are untried. The drivers need Visual Studio (see
[Drivers](#drivers)).

| CMake option | Default | Builds |
|---|---|---|
| `WE_BUILD_SERVICE` | ON | the app (`wheelbri`), `wheelbri-service` and, on Windows, `wheelbri-helper` |
| `WE_BUILD_TESTS` | ON | the unit tests (`wheelbri_tests`); skipped when cross-compiling |
| `WE_BUILD_SHIM` | ON for Windows | `dinput8.dll`, the shim |
| `WE_BUILD_LOGI` | ON for Windows | `wheelbri-logi.dll` (a diagnostic stand-in for Logitech's SDK helper, 64-bit only) |
| `WE_BUILD_UHID` | ON on Linux | `wheelbri-uhid`, the virtual wheel for Linux |

### Configure, build, test

The same on Windows and Linux, from the repository root. Pick any generator with `-G` (Ninja, Makefiles, ...).

Configure:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

Build:

```bash
cmake --build build --config Release
```

Run the tests:

```bash
ctest --test-dir build -C Release
```

Outputs are in `build/service/` (`wheelbri`, `wheelbri-service`, `wheelbri-helper`), `build/shim/dinput8.dll`,
`build/logi/` and `build/tools/uhid/`. On Windows with MinGW the programs need the compiler's runtime DLLs on `PATH`
(for MSYS2 UCRT64: `C:\msys64\ucrt64\bin`); the shim is linked statically.

On Linux the window needs the X11 headers (`libx11-dev` or `libX11-devel`); without them it is skipped with a
warning.

### The 32-bit shim

32-bit games need a 32-bit `dinput8.dll`. Only the shim is 32-bit. With a 32-bit MinGW-w64 compiler (MSYS2:
`mingw-w64-i686-gcc`, with `/mingw32/bin` first on `PATH`), configure:

```bash
cmake -S . -B build-x86 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw32.cmake -DCMAKE_BUILD_TYPE=Release -DWE_BUILD_SERVICE=OFF -DWE_BUILD_TESTS=OFF -DWE_BUILD_LOGI=OFF
```

Build (→ `build-x86/shim/dinput8.dll`):

```bash
cmake --build build-x86 --target wheelbri_shim
```

### The shim from Linux

Both shims cross-compile with MinGW-w64. The 64-bit one (→ `build-win/shim/dinput8.dll`):

```bash
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake -DCMAKE_BUILD_TYPE=Release -DWE_BUILD_SERVICE=OFF -DWE_BUILD_TESTS=OFF
```

```bash
cmake --build build-win
```

The 32-bit one is the [32-bit shim](#the-32-bit-shim) commands with `build-win-x86` as the folder.

### Drivers

Mode Virtual on Windows 11 uses two UMDF 2 drivers: `wheelbri-wheel` (the virtual wheel, all four targets) and
`wheelbri-hide` (hides the rig from games while WheelBri runs). They need Visual Studio with *Desktop development with
C++*, the Spectre-mitigated libraries, the Windows SDK and the WDK. From a **Developer PowerShell for VS**:

```powershell
msbuild driver\wheelbri-wheel.vcxproj /p:Configuration=Release /p:Platform=x64
```

```powershell
msbuild driver\hide\wheelbri-hide.vcxproj /p:Configuration=Release /p:Platform=x64
```

They are test-signed with the WDK's `WDKTestCert`. For WheelBri's own certificate instead (once, normal PowerShell;
rebuild both drivers afterwards):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File driver\new-signing-cert.ps1
```

Install from an **Administrator PowerShell** (the installer and the app do this themselves). The install trusts the
signing certificate; Secure Boot and test mode stay as they are:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File driver\install-driver.ps1
```

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File driver\hide\install-hide.ps1
```

Uninstall, the filter first:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File driver\hide\uninstall-hide.ps1
```

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File driver\uninstall-driver.ps1 -RemoveCertificate
```

### Packaging

The Windows installer, from what is built in `build`, `build-x86` and the drivers' folders (PowerShell; it checks every
file, the processor of each program and that both drivers have the same signer). It writes
`dist\windows\WheelBri-<version>.zip` and `WheelBri-<version>-setup.exe`:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\make\package.ps1 -BuildDir build
```

The Linux bundle (`dist/wheelbri-linux-x86_64.tar.gz`; build it on a system whose glibc is not newer than the
target's, e.g. SteamOS's):

```bash
scripts/linux/package.sh --shim build-win/shim/dinput8.dll --shim-x86 build-win-x86/shim/dinput8.dll
```

## Linux and the Steam Deck

### Installing

Unpack the bundle in your home folder (→ `~/wheelbri`):

```bash
tar -xzf wheelbri-linux-x86_64.tar.gz -C ~
```

On the Deck you can build instead: SteamOS has no compiler, so `scripts/linux/deck-setup.sh` makes an Arch Linux
distrobox with SteamOS's glibc and `scripts/linux/deck-build.sh` builds, tests and installs into `~/wheelbri` from it.

Once per machine, the udev rules (needs `sudo`; log out and in afterwards). They let your user create the virtual
wheel, let games open it and let the service read the rig:

```bash
~/wheelbri/install-udev-rules.sh
```

Force feedback on the R3 needs the kernel's `hid-universal-pidff` (Linux 6.15 and later). Check it:

```bash
modinfo hid-universal-pidff
```

### Playing

Open the app (`~/wheelbri/bin/wheelbri`) and set up the rig as on Windows ([user guide](docs/user-guide.md)). Linux
numbers the axes differently from Windows, so check the mapping. Then set the game's Steam launch option
(Properties > Launch Options) to one of:

| Way | Launch option |
|---|---|
| Virtual wheel (default) | `/home/deck/wheelbri/wheelbri.sh %command%` |
| Shim in the game folder | `/home/deck/wheelbri/wheelbri.sh --shim %command%` |

`wheelbri.sh` starts the service (and the virtual wheel, or puts the shim in) before the game and stops everything when
it ends; it works in Game Mode. Its options, before `%command%`: `--target g25|g27|g920|g923`, `--config FILE`,
`--hide VID/PID,...`, `--game-folder DIR`, `--keep-shim`, `--ui`/`--no-ui`; `--help` explains them.

### Logs and troubleshooting

Logs are in `~/.local/state/wheelbri/` (the app's Logs tab shows them).

- `wheelbri-uhid` stops at once with `Permission denied` on `/dev/uhid`: run `install-udev-rules.sh` and log out and
  in. `No such file`: `sudo modprobe uhid`.
- The game sees no wheel: add `PROTON_LOG=1 WINEDEBUG=+hid,+dinput` before the launch option and look for `046d:c262`
  in `~/steam-<appid>.log`.
- Two wheels in the game: save the rig in the Rig tab (only its devices are hidden), or pass `--hide 0x346E/0x0005`.
- `--shim` refuses ("belongs to another mod"): the game folder has another mod's `dinput8.dll`; use the virtual wheel.
- No force feedback but the wheel works: with the game and WheelBri closed, run
  `~/wheelbri/bin/wheelbri-service --ffb-test`.

## For developers

### Layout

| Path | What |
|---|---|
| `core/` | portable library: mapping, target profiles, force feedback limiter, config, wire format, telemetry (no OS headers) |
| `service/` | the app and `wheelbri-service` (reads the rig, plays force feedback, the window); `helper/`: the WheelBri Helper service |
| `shim/` | `dinput8.dll`, a DirectInput proxy that answers as the target inside one game |
| `driver/` | the UMDF drivers `wheelbri-wheel` and `hide/` (`wheelbri-hide`) with their install scripts |
| `tools/uhid/` | `wheelbri-uhid`, the virtual wheel for Linux (kernel uhid) |
| `logi/` | `wheelbri-logi.dll`, a logging stand-in for Logitech's wheel SDK helper |
| `installer/`, `scripts/` | the Windows installer, Linux scripts, packaging |
| `config/` | each target's default config, `wheelbri-shim.ini`, `games/` (game profiles, compiled in) |
| `tests/` | GoogleTest unit tests with fakes of the interfaces |

### Architecture

```
rig ──DirectInput / evdev──► wheelbri-service ──UDP 127.0.0.1──► one consumer, which the game sees as the target:
          map · encode · FFB safety      41329 state ►         Mode Virtual: wheelbri-wheel driver or wheelbri-uhid
                                         ◄ 41330 game FFB      Mode Shim:    dinput8.dll next to the game's .exe
```

- **Pipeline:** raw axes → `Mapper` → `WheelState` → the target's `IDeviceProfile::encode` → `StatePacket` (33 bytes,
  magic `WEMU`). Code on this loop allocates nothing and never blocks.
- **UDP loopback, not shared memory:** under Proton the shim lives in Wine and the service is a Linux process.
- **Force feedback safety:** every force to the wheel goes through `FfbSafetyLimiter` (slew limit; no cap of its
  own: the strength is set in the wheel's own app, such as Moza Pit House) and a watchdog that stops forces when the
  game goes quiet for 500 ms. Never add a way around it.
- **Logging:** spdlog through the `WE_LOG_*` macros, one dotted logger per component (rules in
  `core/include/wheelbri/Logging.hpp`). Logs and telemetry go to `%LOCALAPPDATA%\wheelbri\logs` or
  `~/.local/state/wheelbri`.

### Targets

All four use 900 degrees and the G920's DirectInput layout (wheel X, throttle Y, brake Rz, clutch Slider), except
the G25 and G27 on Windows (both modes): wheel X, throttle Y, brake Rz, their combined pedals on Slider 0 (in the
middle at rest, down with the throttle, up with the brake) and the clutch on Slider 1, as Assetto Corsa Competizione's
G25 and G27 presets read them (clutch: axis 7, Slider 1). Race Driver: GRID's own G25 preset reads Slider 0 as combined
pedals (its lower half throttle, its upper half brake): with the clutch there, at rest at the top, the brake was held
down (the car reversed by itself); with nothing there (0, the bottom), the throttle was (the car could not back up).
(Logitech's own driver, in Logitech Gaming Software 5.10's `WmJoyHid.inf`, names the G25's axes X wheel, Y combined
pedals, Z accelerator, Rz brake, Slider clutch, 19 buttons: ours follows what games' presets read instead.) Until
2026-10-21 the G25's clutch was on Z and the G27's on Slider 0.

Race Driver: GRID's force feedback needs VSync on (or the frame rate capped). With VSync off at 144 Hz it passed a
constant force of -2147483648 (a broken number, not a force) for the whole race: no force feedback, and a full
push before WheelBri treated such values as none. With VSync on it sends real forces, about 90 updates a second.
The shim warns once when a game passes such a value. In Mode Virtual the number never reaches WheelBri: Windows' HID
PID driver clamps it to a full push first (2026-10-21, VSync off at 300 Hz: nothing but 0 and -100% for minutes, the
wheel pulled to one side and let go every few seconds). So a constant force at exactly full scale while the game has
sent no force between none and full for 2 s plays as none, in both modes (`[ffb] ignore_lone_full_push`, on by
default; the service warns when it starts). A full push among real forces still plays: with VSync on, GRID's 604 full
pushes in 40 minutes all came within a moment of others.

The G25's buttons are in the order games of its time saw on Windows: the shifter's red buttons 0-3 (left to right),
right paddle 4, left paddle 5, the wheel's right and left buttons 6 and 7, gears 1-6 and reverse 8-14 (sequential mode
presses 8 and 9), the shifter's black diamond 15-18 (top, left, bottom, right), the D-pad a hat. Sources: Logitech
support ("Programming the G25 Shifter positions": gears 8-14), Race Driver: GRID's G25 preset (paddles 4/5, gears,
menu Select 17 and Back 18), GIMX's G25 configuration for Windows (red 0-3, wheel buttons 6/7, black 15-18), G27
reports from DiRT Rally 2.0 and American Truck Simulator users (the same order), a G25 read through Unity (gears,
sequential). The default mapping (`config/g25.ini`) puts an Xbox-style rim's A, B, X, Y on the black diamond
(bottom, right, left, top), the menu buttons of that era's games. The G27 has the same and its wheel's lower buttons
19-22.

A real G25 or G27 without Logitech's driver (Linux, or Windows without it) numbers its buttons in its own HID order
instead, which newer games' presets read (Assetto Corsa Competizione's G25 and G27 presets bind 0-11 and nothing at the
gears): the black diamond 0-3 (bottom, left, right, top), paddles 4/5, wheel buttons 6/7, red 8-11 (red 2, 3, 4, 1),
gears 12-17, reverse 18 (G25) or 22 (G27, its wheel's lower buttons 18-21). The Mapping tab's **Button order**
(`[buttons] order = driver | native`, G25 and G27 only) picks which one the game gets; the Mapping tab names the
buttons in the driver's order either way. Sources: GIMX's G25 and G27 configurations for Windows and for Linux side
by side (facts only; GIMX is GPLv3 and nothing is copied) and a G27's HID descriptor; ACC's presets bind the native
diamond as A, X, B, Y, as its G920 preset binds the G920's A, B, X, Y. Neither order checked on a real wheel.

| Target | Ids | Buttons | Button order |
|---|---|---|---|
| G25 | 046D:C299 | 19 | **from several sources** (above), native order too; not checked on a real G25 |
| G27 | 046D:C29B | 23 | the G25's, its wheel's lower buttons 19-22 **from GIMX**; native order too |
| G920 | 046D:C262 | 19 | **tested** |
| G923 | 046D:C26E | 24 | 0-18 as the G920, 19-23 **a guess**; unsigned axes and HID++ for G HUB |

The orders above count from 0, as the code and the INI's `target_N` do. The app shows the Target's buttons by the
numbers the game sees (`gameButtonNumber`): from 1 in the driver's order, as Logitech's driver, joy.cpl and games
count them (DiRT Rally 2.0's G25 preset asks for "Button 7" for the handbrake: R Wheel, 6 above), from 0 in the native
order.

To fix an order: `g25Spec()`, `g27Spec()`, `g923Spec()` in `core/src/DeviceProfile.cpp` and the matching
`config/*.ini`.

### G HUB and HID++

With Target G923 and Mode Virtual the virtual wheel answers G HUB's HID++ 2.0 (`core/include/wheelbri/HidPp.hpp`,
written from the public description): Root, FeatureSet, firmware version, name and ForceFeedback (0x8123). Rotation
and gain are logged, **not applied**; effects G HUB uploads are **not played**. Tried once (2026-10-16): G HUB showed
the wheel as active. The unsigned axes added since are untried. The pedals are in the games' layout (throttle Y, brake
Rz, clutch Slider, as the G920's), not a real G923's (brake Z, clutch Rz), so G HUB shows them in the wrong places:
with a real G923's layout DirectInput relabelled them for games (in ACC gas was the clutch, brake the throttle, clutch
the brake), and the registry entries meant to undo that did not.

## Licence

Nothing is copied from GIMX (GPLv3); `tools/uhid` is written from the kernel's `linux/uhid.h`, the HID descriptor
comes from HIDMaestro's builder (MIT, noted in `core/src/HidWheelDevice.cpp`) and HID++ from its public description.
WheelBri never ships Logitech's files.
