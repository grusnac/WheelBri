# Future functionalities

Ideas worked out but not built, kept here until we decide to do them. Nothing in this file is implemented.

## Running WheelBri as a standard user (the helper service plan)

Planned on 2026-10-05; started 2026-10-06 (see Progress). End users are standard users, while development runs as Administrator. Today
`wheelbri.exe` always asks for Administrator (its manifest says `requireAdministrator`), so a standard user cannot run
it at all.

### What needs Administrator today

| What | Where in the code |
|---|---|
| The app always asks for Administrator | `service/app/wheelbri.exe.manifest.in` (`requireAdministrator`) |
| Creating and removing the virtual G920 | `VirtualG920` (`SwDeviceCreate`) |
| Showing the rig only to WheelBri: `HKLM\SOFTWARE\wheelbri\hide` `AllowedPid` / `AllowedPidStart` | `HideFilter` |
| Clearing a hide left behind by a crash | `HideFilter::clearStale`, `--unhide` (HKLM) |
| Putting the hide filter on the rig's devices | `ensureHideFilter` in `main.cpp` (`install-hide.ps1`) |
| Installing the G920 driver when it is missing | `ensureG920Driver` in `main.cpp` (`install-driver.ps1`) |
| DirectInput's G920 entries, and removing Logitech's force feedback entry every 2 s | `VirtualG920` (the HKLM half; the HKCU half works as a user) |
| Letting the drivers write into the user's log folder | `shareLogFolder` (folder permission plus HKLM `LogFolder`) |
| Copying the shim into a protected game folder (e.g. under `Program Files`) | `installShim` |

Everything else already works as a standard user: reading the rig, force feedback, the UDP ports, the windows, every
setting and log, the single-instance lock (a `Global\` mutex needs no privilege), and the shim in an ordinary game
folder (Steam libraries are usually writable).

### How it works

```
 wheelbri.exe (the user, no prompt) --named pipe--> WheelBri Helper (Windows service, SYSTEM)
   reads the rig, maps, plays FFB                      creates / removes the virtual G920
   asks the helper for the privileged steps            sets / clears AllowedPid (always the caller's own process)
                                                       puts the hide filter on rig devices
                                                       installs the G920 driver when it is missing
                                                       DirectInput G920 registry entries
   pipe closes (Stop, crash, logoff) --------------->  helper removes the G920 and shows the rig again
```

Safety rules for the helper:

- A fixed list of requests, each one checked; anything else is refused.
- It never trusts what comes from the user: it reads none of the user's files and runs no path the app sends. Its
  scripts come only from its own folder under Program Files.
- `AllowedPid` is always the caller's own process (`GetNamedPipeClientProcessId`, with its start time), so nobody
  can name another process.
- The hide filter goes only on present HID interfaces of game controllers: never keyboards, mice or system controls
  (HID usages 1/6, 1/2, 1/0x80), never the virtual G920 (046D:C262).
- The pipe lets in the logged-on (interactive) user only, and the helper checks that the caller is `wheelbri.exe`
  from the install folder.
- Clean-up comes with the connection: when the pipe closes, the helper removes the devices and clears `AllowedPid`.
  A crash never leaves anything behind (as the hide filter already promises).

### Phases (small commits, each one switched off until the last)

1. **Protocol in core**: the messages, their encoding and the request checks, with no Windows calls; unit-tested on
   Linux like the rest.
2. **`wheelbri-helper.exe`**: a Windows service (start and stop through the service manager), the pipe with its
   permissions, caller checks, its own log.
3. **The privileged code moves**: `VirtualG920`, the `HideFilter` registry writes, `ensureHideFilter`,
   `ensureG920Driver` and the log-folder grant go into a library shared by the app and the helper.
4. **App side**: one interface for the privileged steps with two versions: through the helper (standard user), or
   directly (Administrator, as now, so development builds keep working without a helper). The status next to Start
   says "HELPER NOT INSTALLED" in red when the virtual-device target cannot run.
5. **The app runs as the user**: the manifest becomes `asInvoker` (no prompt). The shim target needs no helper. A shim
   copy into a protected game folder gives a clear message instead.
6. **Installer, uninstaller, clean-pc, package**: install the helper service (restart on failure) and remove it; the
   new exe goes into `package.ps1` and the required list in `install.ps1`; `clean-pc.ps1` removes it too.
7. **Docs**: CLAUDE.md's security rules (`isElevated` checks), README, `installer/README.txt`; this section is then
   removed from this file.
8. **Test as a real standard user**: a second Windows account without admin rights: install, Start / Stop, kill the
   app (the rig must come back), the shim in a Steam folder, uninstall.

Size: a new service program, about 6 to 8 commits. The biggest risk is phase 3, which moves code that works today.

### Decisions (taken 2026-10-06)

1. **Driver logs** stay in the user's log folder: the helper grants the drivers access when the user connects,
   through a handle so that a link cannot redirect it.
2. **The helper's own log** goes into the connected user's log folder too (`wheelbri-helper-<date>.log`, with every
   other log; the user's choice: all logs in one place). The helper writes it while impersonating the pipe's client,
   so it can only write where that user could, and a link in the folder gains nothing. Before any user connects (at
   boot) it keeps its lines in memory and writes them at the first connection.
3. **Development builds** keep running as Administrator: the helper accepts only `wheelbri.exe` from its install
   folder, and the app does the privileged steps itself when elevated.
4. **Protected game folders**: a clear message only; the helper never copies DLLs.

### Progress

- Phase 1 done (2026-10-06): `core/include/wheelbri/HelperProtocol.hpp` (requests, replies, rules,
  `isInstalledApp`), `HelperProtocolTest`. Not used by anything yet.
- Phase 2 done (2026-10-06): `service/helper/` builds `wheelbri-helper.exe` (service `WheelBriHelper`, or
  `--console`; `--hello` asks a running one). The pipe, its permissions, the caller check and the log in the user's
  folder work; every request but Hello is answered "cannot do this yet". Not installed by the installer yet.
- Phase 3 done (2026-10-06): the privileged steps are in `service/src/Privileged.hpp` (library
  `wheelbri_privileged`), used by the app as before and by the helper's `HelperSession`, which now carries out every
  request and undoes them when the connection closes. The app does not use the helper yet (phase 4); its user's HKCU
  DirectInput entries stay with the app then.
- Phase 4 done (2026-10-06): `PrivilegedSteps` (direct when elevated, else the helper). The helper also accepts
  `wheelbri-service.exe` from its folder, so a session can be tried as a standard user from a console before the app's
  manifest changes (phase 5). A Stop during a long request closes the connection; the helper notices (its request's
  `cancelled` checks the pipe) and undoes it.
- Phase 5 done (2026-10-06): `wheelbri.exe` is `asInvoker`. With the virtual-device target and no Administrator,
  the status says "HELPER NOT INSTALLED" (red, Start greyed out) while no helper listens
  (`PrivilegedSteps::available`, checked once a second without connecting). A shim copy refused by a protected game
  folder says what to do. `scripts/make/app.sh` starts development builds as Administrator, `--as-user` as the user.
- Phase 6 done (2026-10-06): the package carries `wheelbri-helper.exe`; `install.ps1` stops an earlier helper before
  copying, then registers and starts the `WheelBriHelper` service (LocalSystem, automatic, restarted after a crash;
  `Install-HelperService` in `driver/common.ps1`) and starts it again if the install fails after stopping it;
  `uninstall.ps1` and `clean-pc.ps1` stop and delete it first (`Remove-HelperService`).

## Logitech RS Shifter & Handbrake

A separate Logitech device (its own USB controller on PC; on console only through the Racing Adapter), not part of a
G920: a sequential shifter or a handbrake (switch or analog). A real G920 has no handbrake input, so adding one to the
emulated G920 would make it differ from a real one. Options when we get to it: map a rig handbrake to a G920 button,
or emulate the RS Shifter & Handbrake as a second, separate device.

## G29 target

Not supported now (the G920 is the one target). If it comes back: Logitech's Driving Force Shifter FAQ gives the G29's
shifter as joy.cpl buttons 12-17 for gears 1-6 and 18 for reverse (the G920's are 13-18 and 19).
