# WheelBri's drivers

Two UMDF 2 drivers (user mode, Windows 11, built with Visual Studio + WDK), both needed for the app's Mode Virtual
only; the in-game shim needs neither.

- **wheelbri-g920** (`driver/`): the virtual wheel, a HID minidriver running in Windows' driver host. One package for
  all four virtual wheels (G25, G27, G920, G923).
- **wheelbri-hide** (`driver/hide/`): an upper filter on the rig's game-controller interfaces, so that only
  WheelBri can open them while it runs and games see only the virtual wheel.

```
rig ─► wheelbri-service / app ─UDP 41329─► wheelbri-g920 driver ─HID─► Windows ─► games
                  ▲                               │
                  └────────UDP 41330──────────────┘  the game's force feedback (HID PID, decoded in the driver)
```

## The virtual wheel: wheelbri-g920

- **Device:** a Windows software device created by the app's Start in Mode Virtual (or `wheelbri-service
  --virtual-g920`) while the session runs, removed when it stops or crashes: `SWD\wheelbri\G25_0`, `G27_0`, `G920_0`
  or `G923_0`, for the Target (the config's `target_profile`). If an earlier one is stuck half-removed (a program
  held it), the service uses a spare identity (`_1` to `_3`) instead of needing a restart.
- **The four wheels** (`VirtualWheel` in `core/include/wheelbri/HidG920.hpp`, one hardware id each in the one INF;
  the driver reads the device's hardware id and answers as that wheel, an unknown one as the G920):

  | Target | Hardware id | VID:PID | Product string | Buttons |
  |---|---|---|---|---|
  | G25 | `wheelbri\G25` | 046D:C299 | G25 Racing Wheel | 19 |
  | G27 | `wheelbri\G27` | 046D:C29B | G27 Racing Wheel | 23 |
  | G920 | `wheelbri\G920` | 046D:C262 | G920 Driving Force Racing Wheel USB | 19 |
  | G923 | `wheelbri\G923` | 046D:C26E | G923 Racing Wheel for Xbox One and PC | 24 |

  All have the same report: axes X, Y, Rz, Slider carrying wheel, throttle, brake and clutch as a real G920 does,
  the buttons (G920 and G25: the wheel's 11, an unused 12th, the Driving Force Shifter's gears 1-6 and reverse at
  13-19; the G27 and G923 declare more in the same report size), a hat and HID PID force feedback. The G923 also has HID++ for G HUB (`reportDescriptorFor`: two vendor collections, reports
  0x11 and 0x12, answered by core `HidPp`), so its HID PID reports are 0x10 higher; see
  [docs/ghub-hidpp-plan.md](../docs/ghub-hidpp-plan.md). Every HID++ request goes to the driver's log.
- **DirectInput:** while the device exists the service writes the DirectInput entry for its id (`OEMName`, `OEMData`
  under `...\MediaProperties\PrivateProperties\Joystick\OEM\VID_046D&PID_<pid>`, HKCU and HKLM) so that it counts as
  a driving wheel, and removes Logitech's foreign force feedback entry (`OEMForceFeedback`) there.
- **Force feedback:** Windows' `pid.dll` turns DirectInput effects into HID PID reports; the driver answers the
  effect-block handshake and decodes them (`core/src/HidPid.cpp`, unit-tested) into the service's `FfbPacket`s,
  where the safety limiter applies as for every other source.
- **Not together with** the in-game shim: both take the wheel's state from UDP 41329.

A driver installed before a wheel was added to the INF has no entry for that wheel's id; Start then fails with "the
installed wheelbri-g920 driver is older than the virtual <wheel>": install it again (below, or the installer).

## Hiding the rig: wheelbri-hide

Our replacement for HidHide: a UMDF 2 **upper filter** on the game-controller interfaces of each device of the active
rig (the Rig tab): exactly the HID interfaces DirectInput lists as game controllers, which is what games open (the
wheel maker's software, such as Moza Pit House, uses the other interfaces).

- **No device ids in the driver or the INF:** the INF's only hardware id, `WHEELBRI_HIDE_FILTER`, belongs to no device,
  so Windows never puts the filter on anything by itself. `install-hide.ps1 -Devices` puts it on exactly the
  interfaces it is given (HID instance ids, or `vid:pid` by hand), taking it from the INF's models for the class like
  Device Manager's "Have disk" (SetupAPI `DiInstallDevice`), even where another driver ranks higher (Windows'
  `input.inf` or the wheel maker's).
- **Who puts it on:** the app at Start, for the rig's devices that do not have it yet (through the WheelBri Helper, or
  itself when it runs as Administrator); the installer runs `wheelbri-service --prepare-hide` for a rig saved before a
  reinstall. A device takes the filter only when Windows restarts it: close the wheel maker's software and games.
- **The virtual wheels are never hidden** (046D:C299, C29B, C262, C26E): `install-hide.ps1` skips them, and the filter
  hides nothing on one even if it ends up there.
- **Who may open a filtered interface:** the process in `HKLM\SOFTWARE\wheelbri\hide` `AllowedPid` (the session's),
  nobody else (so DirectInput, SDL and games do not see the device); 0 or missing lets everyone in. It **fails open**:
  if that process is gone (a crash), or the id now belongs to another program (`AllowedPidStart`, the process's start
  time, differs), or the setting cannot be read, nothing is hidden. Everything else passes straight through.

## Build (Developer PowerShell for VS)

Needs Visual Studio 2026 with *Desktop development with C++*, the Spectre-mitigated libraries, the Windows SDK and
the WDK (same build numbers). In **Developer PowerShell for VS 2026**, in the repo:

```powershell
msbuild driver\wheelbri-g920.vcxproj /p:Configuration=Release /p:Platform=x64
```

```powershell
msbuild driver\hide\wheelbri-hide.vcxproj /p:Configuration=Release /p:Platform=x64
```

The packages land in `driver\x64\Release\wheelbri-g920\` and `driver\hide\x64\Release\wheelbri-hide\` (`.inf`,
`.dll`, `.cat`). From MSYS2, `scripts/make/drivers.sh` builds both (`-Only g920` or `-Only hide`, `-Clean`,
`-Configuration Debug`, `-Install`; see [scripts/make/README.md](../scripts/make/README.md)).

### Signing certificate

By default both drivers are test-signed with the WDK's `WDKTestCert`, which other driver projects on this PC may
share. WheelBri can have its own instead: code signing only, private key not exportable, in `Cert:\CurrentUser\My`,
valid 5 years (an existing valid one is reused). Once, in a normal PowerShell (no Administrator needed):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File driver\new-signing-cert.ps1
```

It prints the thumbprint and writes `driver\signing-certificate.props` (this PC only, git-ignored), which both
`.vcxproj` files import when it exists (it sets their `TestCertificate`). Then rebuild both drivers and install them
again. Without that file the build signs with `WDKTestCert` as before. To go back (or if a build fails to sign):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File driver\new-signing-cert.ps1 -Remove
```

That deletes only the `.props` file; the certificate stays in your store (`certmgr.msc` removes it). Both drivers must
be signed by the same certificate (`package.ps1` checks it): rebuild both after a switch.

## Install and uninstall (Administrator PowerShell)

The end user's installer does all of this. By hand, from the repo (or the package folder, which has the same layout),
the virtual wheel:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File driver\install-driver.ps1
```

It trusts the certificate that signed the package (the machine's Root and TrustedPublisher stores), records its
thumbprint in `HKLM\SOFTWARE\wheelbri\SigningCertificates` and installs the package with `pnputil`. No test mode,
Secure Boot stays on. `-Configuration Debug` takes the Debug build. The app also runs it by itself at Start when
Windows has no `wheelbri-g920` package (after `clean-pc`, say) and says so in its log.

The hiding filter (close the wheel maker's software and games: each device restarts once):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File driver\hide\install-hide.ps1 -Devices 346e:0005
```

`-Devices` takes `vid:pid` (all of that device's game-controller interfaces; several comma-separated) or HID instance
ids (`HID\VID_346E&PID_0005&MI_02\...`, what the app passes). Without `-Devices` it only adds the package to Windows
and takes any older copy off every device (what the installer does; the app puts it back at the next Start).
`-NoRestart` (the app) leaves restarting the devices to the caller; `-LogFolder <folder>` writes its log there (the
WheelBri Helper passes the user's). Exit codes: 0 the filter runs on every given device that is plugged in
(unplugged ones are listed and skipped), 2 it is set but starts only after a restart (unplugging and replugging the
device is enough when Windows is not waiting for a restart), 1 failed (a red line says why).

Uninstall, the filter first (its devices go back to Windows' own driver), then the virtual wheel and its devices:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File driver\hide\uninstall-hide.ps1
```

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File driver\uninstall-driver.ps1 -RemoveCertificate
```

Leave out `-RemoveCertificate` to keep the certificate. It refuses while wheelbri-hide is installed (same certificate,
and the filter sits on the rig's devices), and removes only the certificates that signed our packages, found by
thumbprint (the recorded ones, and the signers of the installed and packaged `.cat` files), never another
`WDKTestCert`. Stop WheelBri first. Every script stops at the first failed step with a red line and exit code 1; a
yellow line says when Windows wants a restart. Each relaunches itself in 64-bit PowerShell when started from a 32-bit
one.

## Use without the app

The app's Start in Mode Virtual does everything below. From a command line, take the shim out of the game first
(`scripts/make/shim.sh --off <game folder>`), then, as Administrator (the WheelBri Helper serves only `wheelbri.exe`
from its own folder):

```powershell
build\service\wheelbri-service.exe --config config\g920.ini --ui --virtual-g920
```

`config\g25.ini`, `g27.ini` or `g923.ini` give the other wheels. With the log level at info (the config's
`[logging] level`), the service log says `virtual G920 G920_0 ready after … ms` and `hide: the rig is hidden from
games`; joy.cpl lists the virtual wheel and not the rig. While it runs the service hides every device of the active
rig (Rig tab: wheel, pedals, shifter, button box…) from games (NFS takes the real wheel when it can see it), keeps its
own access, and makes them visible again when it stops. `--no-hide` leaves the rig visible; after a crash,
`wheelbri-service --unhide` (Administrator, no session running) clears the setting (the next start also clears a
stale one by itself). Stop with Ctrl+C or by closing the window.

## Logs

Both drivers run as LocalService and write one file per run into the user's `%LOCALAPPDATA%\wheelbri\logs`, with every
other WheelBri log: `wheelbri-driver-<date>-<NN>.log` (the virtual wheel: start, the game's effects, a summary of the
input once a minute) and `wheelbri-hide-<date>-<NN>.log` (which programs opened a hidden device or were refused). The
install scripts write `wheelbri-driver-install-<date>.log` and `wheelbri-hide-install-<date>.log` there.

The app (as Administrator, or through the WheelBri Helper) lets LocalService write in that folder and names it in
`HKLM\SOFTWARE\wheelbri` `LogFolder`, which the drivers read for every line; the installer and `install-hide.ps1` do
the same. Until then, and when that folder cannot be used (it, or a folder above it, is a junction or symbolic link, or
it does not exist), they write to `C:\ProgramData\wheelbri\logs`, which the app moves into the user's folder later.
They never write to a log file that is a link: the user owns that folder and must not be able to point LocalService's
writes elsewhere.

How much they write is the app's Level button (Logs tab): `LogLevel`, which the app writes at each Start and on a
click while a session runs, and the drivers read at most once a second. Warnings and errors are always written. Each
line says its level: `[2026-10-08 21:14:05.120] [debug] pid: start block 1 (constant, slot 0)`. The game's effect
events (`pid:` lines) are `debug`; the `PidTraceLines` trace is `trace`.

## Registry values

All under `HKLM\SOFTWARE\wheelbri` (Administrator to write):

| Value | Type | Written by | Meaning |
|---|---|---|---|
| `LogFolder` | REG_SZ | app, helper, installer, `install-hide.ps1` | the drivers' log folder (missing: `C:\ProgramData\wheelbri\logs`) |
| `LogLevel` | REG_SZ | app (Start, Level button) | `warn` (missing), `info`, `debug` or `trace` |
| `PidTraceLines` | REG_DWORD | by hand | trace the next N force feedback reports in the driver's log, read at each start of the device (0 or missing: off) |
| `hide\AllowedPid`, `hide\AllowedPidStart` | REG_DWORD, REG_QWORD | the session | the one process that may open the rig, and its start time |
| `SigningCertificates\<thumbprint>` | REG_SZ | install scripts | the certificates the install trusted, for the uninstall |

By hand, from an Administrator PowerShell, for example:

```powershell
reg add HKLM\SOFTWARE\wheelbri /v LogLevel /t REG_SZ /d debug /f
```

```powershell
reg add HKLM\SOFTWARE\wheelbri /v PidTraceLines /t REG_DWORD /d 1000 /f
```

## When it does not work

- **Start fails with "the installed wheelbri-g920 driver is older than the virtual G25"** (or G27, G923): the
  installed package predates that wheel. Install it again: `scripts/make/drivers.sh -Only g920 -Install`, or the
  installer.
- **No virtual wheel in joy.cpl:** the app's Logs tab (driver line) and `wheelbri-driver-install-<date>.log` say
  whether the package is installed; `pnputil /enum-drivers` lists `wheelbri-g920.inf`. A device stuck half-removed
  makes the service try `_1` to `_3`; if all four are stuck, restart Windows.
- **The rig is not hidden** ("NOT HIDDEN: <device>" in red, or the game sees two wheels): `install-hide.ps1` exited 2
  (the filter is set but waits for a restart of the device): close the wheel maker's software and games, unplug and
  replug the device, or restart Windows. `wheelbri-hide-install-<date>.log` and the service log (`hide:` lines) say
  which program kept the device open.
- **The rig stays hidden after a crash:** the filter fails open, so this only happens while some process with the
  recorded id still runs. Stop WheelBri, then `wheelbri-service --unhide` as Administrator; `uninstall-hide.ps1`
  removes the filter entirely.
- **Driver logs missing from `%LOCALAPPDATA%\wheelbri\logs`:** look in `C:\ProgramData\wheelbri\logs`; the
  `LogFolder` value is missing or points through a link.
- **No force feedback:** set the Level to debug and look for `pid:` lines in the driver's log: none means the game
  sent no effects to the virtual wheel; for the exact reports, set `PidTraceLines` and restart the session.
