# wheelbri-g920 driver (Phase 5, plan B)

Our own virtual Logitech G920: a UMDF 2 HID minidriver running in Windows' driver host, replacing
the HIDMaestro bridge of step 0 (removed). Design: [docs/phase5-windows-virtual-device.md](../docs/phase5-windows-virtual-device.md).

```
Moza R3 ─► wheelbri-service ─UDP 41329─► wheelbri-g920 driver ─HID─► Windows ─► games
                  ▲                              │
                  └────────UDP 41330─────────────┘  the game's force feedback (HID PID, decoded in the driver)
```

- **Device:** created by `wheelbri-service --virtual-g920` while it runs (a Windows software device,
  `SWD\wheelbri\G920_0`), removed when it stops or crashes. If an earlier one is stuck half-removed (a
  program held it), the service uses a spare identity (`G920_1` … `G920_3`) instead of needing a restart.
- **Identity:** VID 046D PID C262, "G920 Driving Force Racing Wheel USB", the same HID report descriptor the
  HIDMaestro bridge used (wheel X, brake Y, clutch Rz, throttle Slider, 11 buttons, hat, HID PID force feedback). The
  service writes the DirectInput "driving" entry and removes Logitech's foreign force feedback entry.
- **Force feedback:** Windows' `pid.dll` turns DirectInput effects into HID PID reports; the driver answers
  the effect-block handshake and decodes them (`core/src/HidPid.cpp`, unit-tested) into the service's
  `FfbPacket`s. The service does not change.
- **Not together with** the shim: both use UDP 41329.

## Build (once per change)

Needs Visual Studio 2026 with *Desktop development with C++*, the Spectre-mitigated libraries, the Windows
SDK and the WDK (same build numbers). In **Developer PowerShell for VS 2026**, in the repo:

```
msbuild driver\wheelbri-g920.vcxproj /p:Configuration=Release /p:Platform=x64
```

The package lands in `driver\x64\Release\wheelbri-g920\` (`.inf`, `.dll`, `.cat`), signed with a
certificate the WDK made for this PC.

## Install (Administrator PowerShell)

```
driver\install-driver.ps1
```

Trusts that certificate on this PC (Root and TrustedPublisher stores) and installs the package with
`pnputil`. No test mode, Secure Boot stays on. Remove: `driver\uninstall-driver.ps1` (add
`-RemoveCertificate` to also remove the certificate).

## Use

Take the shim out of the game (`dinput8.dll` → `dinput8.dll.off`), do not run the bridge, then as
Administrator:

```
build\service\wheelbri-service.exe --config config\g920-virtual-device.ini --ui --virtual-g920
```

The service log says `virtual G920 G920_0 ready after … ms` and `hide: the rig is hidden from games`; joy.cpl
lists only the G920. While it runs the service hides every device of the active rig (Wheel setup: wheel, pedals,
shifter, button box…) from games (needed: NFS takes the real wheel when it can see it), keeps its own access, and
makes them visible again when it stops. `--no-hide` leaves the rig visible; after a crash, `wheelbri-service
--unhide` makes it visible again (the next start also clears it by itself).

## Hiding the rig: wheelbri-hide (`driver/hide`)

Our replacement for HidHide: a UMDF 2 **upper filter** on the game-controller interfaces of each device of the
active rig: exactly the HID interfaces DirectInput lists as game controllers (what games see; Moza Pit House uses
the other interfaces). No device ids are in the driver or the scripts: the INF's only hardware id,
`WHEELBRI_HIDE_FILTER`, belongs to no device, so Windows never puts it on anything by itself (when it matched game
controllers' compatible ids, Windows put it on the virtual G920), and `install-hide.ps1 -Devices` puts it on exactly
the interfaces it is given (HID instance ids, or `vid:pid` by hand), taking it from the INF's models for the class
like Device Manager's "Have disk" (SetupAPI `DiInstallDevice`).
The app passes the rig's devices at Start; the virtual G920 (046d:c262) is never touched, and the filter never
hides it even if it ends up on it. When a program opens a filtered interface, the filter checks
`HKLM\SOFTWARE\wheelbri\hide` `AllowedPid`: while the service runs it holds the service's process id and everyone
else is refused (so DirectInput, SDL and games do not see the device); 0 or missing lets everyone in. It fails
open: if that process is gone (the service crashed or was killed) or the id now belongs to another program
(`AllowedPidStart`, the process's start time, differs), or the setting cannot be read, nothing is hidden.
Everything else passes straight through. Same signing as the G920 driver; user mode, no test mode.

The install and uninstall scripts stop at the first failed step with a red line and exit code 1; a yellow line
says when Windows wants a restart. `install-hide.ps1` with no `-Devices` only adds the package (what the installer
does); with `-Devices` it exits 0 when the filter runs on every connected one, 2 when it is set but Windows starts
it only after a restart, 1 on failure (devices not connected are listed and skipped). `uninstall-hide.ps1` takes it
off whatever devices have it. `uninstall-driver.ps1 -RemoveCertificate` refuses while wheelbri-hide is installed:
it uses the same certificate.

Build and install (Developer PowerShell, then Administrator PowerShell; close Pit House and games, the devices
restart once):

```
msbuild driver\hide\wheelbri-hide.vcxproj /p:Configuration=Release /p:Platform=x64
```

```
powershell -ExecutionPolicy Bypass -File driver\hide\install-hide.ps1 -Devices 346e:0005
```

Remove: `driver\hide\uninstall-hide.ps1` (those devices go back to Windows' own setup). Logs (in
`%LOCALAPPDATA%\wheelbri\logs`, with every other log): `wheelbri-hide-<date>.log` (which programs were refused) and
`wheelbri-hide-install-<date>.log`. The window lists rig devices the filter is not running on.  Stop the service with
Ctrl+C (close the game and Steam first). Driver log: `wheelbri-driver-<date>.log` in the same folder.

Both drivers run as LocalService. The app (as Administrator) lets LocalService write in the user's
`%LOCALAPPDATA%\wheelbri\logs` and names it in `HKLM\SOFTWARE\wheelbri` `LogFolder`, which the drivers read for every
line; the installer does the same. Until then (and if that folder cannot be written) they write to
`C:\ProgramData\wheelbri\logs`, which the app moves into the user's folder at its next start.

