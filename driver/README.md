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

The service log says `virtual G920 G920_0 ready after … ms` and `hidhide: real wheel hidden from games`;
joy.cpl lists only the G920. While it runs the service hides the R3 from games through HidHide (needed: NFS takes
the R3 when it can see it), keeps its own access, and makes the R3 visible again when it stops. `--no-hide`
leaves the R3 visible; after a crash, `wheelemu-service --unhide` makes it visible again. No Xbox controller for
the game's button prompts yet (the bridge has one). Stop the service with
Ctrl+C (close the game and Steam first). Driver log: `C:\ProgramData\wheelemu\logs\wheelemu-driver-<date>.log`.
