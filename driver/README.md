# wheelemu-g920 driver (Phase 5, plan B)

Our own virtual Logitech G920: a UMDF 2 HID minidriver running in Windows' driver host, replacing
HIDMaestro and `tools/vbridge`. Design: [docs/phase5-windows-virtual-device.md](../docs/phase5-windows-virtual-device.md).

```
Moza R3 ─► wheelemu-service ─UDP 41329─► wheelemu-g920 driver ─HID─► Windows ─► games
                  ▲                              │
                  └────────UDP 41330─────────────┘  the game's force feedback (HID PID, decoded in the driver)
```

- **Device:** created by `wheelemu-service --virtual-g920` while it runs (a Windows software device,
  `SWD\wheelemu\G920_0`), removed when it stops or crashes. If an earlier one is stuck half-removed (a
  program held it), the service uses a spare identity (`G920_1` … `G920_3`) instead of needing a restart.
- **Identity:** VID 046D PID C262, "G920 Driving Force Racing Wheel USB", the same HID report descriptor the
  bridge used (wheel X, brake Y, clutch Rz, throttle Slider, 11 buttons, hat, HID PID force feedback). The
  service writes the DirectInput "driving" entry and removes Logitech's foreign force feedback entry.
- **Force feedback:** Windows' `pid.dll` turns DirectInput effects into HID PID reports; the driver answers
  the effect-block handshake and decodes them (`core/src/HidPid.cpp`, unit-tested) into the service's
  `FfbPacket`s. The service does not change.
- **Not together with** the shim or the bridge: all three use UDP 41329.

## Build (once per change)

Needs Visual Studio 2026 with *Desktop development with C++*, the Spectre-mitigated libraries, the Windows
SDK and the WDK (same build numbers). In **Developer PowerShell for VS 2026**, in the repo:

```
msbuild driver\wheelemu-g920.vcxproj /p:Configuration=Release /p:Platform=x64
```

The package lands in `driver\x64\Release\wheelemu-g920\` (`.inf`, `.dll`, `.cat`), signed with a
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
build\service\wheelemu-service.exe --config config\moza-r3-as-g920.ini --ui --virtual-g920
```

The service log says `virtual G920 G920_0 ready after … ms` and `hide: real wheel hidden from games`; joy.cpl
lists only the G920. While it runs the service hides the R3 from games (needed: NFS takes the R3 when it can see
it), keeps its own access, and makes the R3 visible again when it stops. `--no-hide` leaves the R3 visible; after
a crash, `wheelemu-service --unhide` makes it visible again (the next start also clears it by itself).

## Hiding the R3: wheelemu-hide (`driver/hide`)

Our replacement for HidHide: a UMDF 2 **upper filter** on the R3's game-controller interface
(`HID\VID_346E&PID_0005&MI_02`; Moza Pit House uses the other interfaces). When a program opens that interface,
the filter checks `HKLM\SOFTWARE\wheelemu\hide` `AllowedPid`: while the service runs it holds the service's
process id and everyone else is refused (so DirectInput, SDL and games do not see the R3); 0 or missing lets
everyone in. Everything else passes straight through. Same signing as the G920 driver; user mode, no test mode.

Build and install (Developer PowerShell, then Administrator PowerShell; close Pit House and games, the R3
restarts once):

```
msbuild driver\hide\wheelemu-hide.vcxproj /p:Configuration=Release /p:Platform=x64
```

```
powershell -ExecutionPolicy Bypass -File driver\hide\install-hide.ps1
```

Remove: `driver\hide\uninstall-hide.ps1` (the R3 goes back to Windows' own setup). Log:
`C:\ProgramData\wheelemu\logs\wheelemu-hide-<date>.log` (which programs were refused). The service uses it when
it is on the R3, else HidHide if installed.  Stop the service with
Ctrl+C (close the game and Steam first). Driver log: `C:\ProgramData\wheelemu\logs\wheelemu-driver-<date>.log`.

## The Xbox controller: wheelemu-xusb (`driver/xusb`)

For games that show keyboard prompts with only a wheel (NFS 2015): a virtual Xbox 360 controller next to the
G920, for XInput only (no HID, so DirectInput does not see a second controller). The service creates it with
`--virtual-g920` (`--xbox-pad buttons`, the default: the wheel's A/B/X/Y/Menu/View/D-pad press it in menus, with
the bridge's rules, now in `core/XboxPad.hpp`; `idle`: there, never pressed; `off`: none) and feeds it over UDP
41332. Its sticks and triggers never move. The XUSB replies follow HIDMaestro's companion driver (MIT).

```
msbuild driver\xusb\wheelemu-xusb.vcxproj /p:Configuration=Release /p:Platform=x64
```

```
powershell -ExecutionPolicy Bypass -File driver\xusb\install-xusb.ps1
```

The service log says `virtual Xbox 360 controller XUSB_0 ready`; the driver logs to
`C:\ProgramData\wheelemu\logs\wheelemu-xusb-<date>.log`. Without the driver the service runs without it.
