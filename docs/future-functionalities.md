# Future functionalities

Ideas worked out but not built, kept here until we decide to do them. Nothing in this file is implemented.

## Running WheelBri as a standard user

End users are standard users, while development runs as Administrator. Today `wheelbri.exe` always asks for
Administrator (its manifest says `requireAdministrator`), so a standard user cannot run it at all.

### What needs Administrator today

| What | Where | Needed for |
|---|---|---|
| The app always asks for Administrator | `service/app/wheelbri.exe.manifest` (`requireAdministrator`) | everything |
| Creating the virtual G920 and the virtual Xbox pad | `VirtualG920`, `VirtualXboxPad` (`SwDeviceCreate`) | virtual device |
| Hiding the rig: writing `HKLM\SOFTWARE\wheelbri\hide AllowedPid` | `HideFilter` | virtual device |
| Putting the hide filter on the rig's devices | `ensureHideFilter` in `main.cpp` (runs `install-hide.ps1`, which installs a driver) | virtual device, first time per device |
| Game controller registry entries (HKLM part) and removing Logitech's force feedback entry | `VirtualG920` (the HKCU part works as a user) | virtual device |
| Letting the drivers write into the user's log folder | `shareLogFolder` (folder permission plus HKLM `LogFolder`) | driver logs |
| Copying the shim into a game folder | `installShim` | shim, only for games in protected folders (e.g. under `Program Files`; Steam libraries are usually writable) |

The shim mode needs no Administrator, apart from the last row.

### The plan: one small helper service, installed once

1. **A "WheelBri Helper" Windows service.** The installer, run once by an Administrator, installs it and it runs as
   SYSTEM. It does a short, fixed list of things on request from the app, over a local named pipe:
   - creates and removes the virtual G920 and the Xbox pad;
   - sets `AllowedPid`, always to the caller's own process id (the helper reads it from the pipe with
     `GetNamedPipeClientProcessId`, so nobody can name another process);
   - puts the hide filter on rig devices: only on game controllers that are plugged in, and only WheelBri's own
     signed filter;
   - writes the game controller registry entries and keeps Logitech's force feedback entry out.

   Every request is checked against strict rules; anything else is refused.
2. **Clean-up comes with the connection.** When the app closes or crashes, the pipe closes. The helper then removes
   the virtual devices and clears `AllowedPid`, so the wheel is never left hidden and no G920 is left behind.
3. **The app runs as the user.** The manifest becomes `asInvoker`: no prompt. Start asks the helper for the privileged
   steps. Without a helper (development builds), the app does them itself when it runs as Administrator, as now.
4. **Driver logs.** Either the installer lets the drivers write to the user's log folder once (simplest), or the
   helper does it per user.
5. **Shim in protected game folders.** The app copies the shim as the user. If Windows refuses, it explains that the
   folder is protected and what to do.
6. **Installer and uninstaller.** They add and remove the helper; `scripts/make/clean-pc.ps1` learns about it.
7. **Tests.** The pipe messages and the request checks go in `core`, unit-tested like the rest. The helper itself can
   only be tested on Windows.

Size: a new service program, the pipe protocol, changes to Start and Stop in the app, and the installer.

### Decisions to make before building it

1. **Approach:** the helper service above, or something lighter: Start shows one Administrator prompt when it needs
   the virtual device, and the shim mode never asks.
2. **Driver logs:** in the user's log folder (the installer grants it once), or in `C:\ProgramData\wheelbri\logs`
   (they are the drivers' logs, not the user's files).
3. **Shim into protected folders:** only a clear message, or let the helper copy it there too (more convenient, but a
   SYSTEM service then writes DLLs into protected folders).

## Logitech RS Shifter & Handbrake

A separate Logitech device (its own USB controller on PC; on console only through the Racing Adapter), not part of a
G920: a sequential shifter or a handbrake (switch or analog). A real G920 has no handbrake input, so adding one to the
emulated G920 would make it differ from a real one. Options when we get to it: map a rig handbrake to a G920 button,
or emulate the RS Shifter & Handbrake as a second, separate device.

## G29 target

Not supported now (the G920 is the one target). If it comes back: Logitech's Driving Force Shifter FAQ gives the G29's
shifter as joy.cpl buttons 12-17 for gears 1-6 and 18 for reverse (the G920's are 13-18 and 19).
