WheelBri
========

Makes your rig (a wheel and its pedals, shifter or button box) look like a Logitech wheel a game supports, so
games that only know Logitech wheels, such as Need for Speed (2015), accept it, with force feedback. The wheel it
presents is the Target you pick in the app: a Logitech G25, G27, G920 (the default) or G923. On Windows 11 it does
that with its own drivers: a virtual wheel (the Target) and a filter that hides your real wheel and its accessories
from games while WheelBri runs (otherwise games would see two wheels).

Needs
-----
- Windows 11, 64-bit (build 22000 or later), for everything. Nothing else has to be installed first: the programs
  carry their own runtime, and the drivers use what Windows already has (UMDF 2).
- Windows 7 SP1 or Windows 10, 64-bit: the in-game shim way only (Mode Shim: a dinput8.dll next to each game's .exe
  presents the Target to that game). The virtual wheel and hiding your rig need Windows 11, so the installer puts no
  drivers and no WheelBri Helper there, and asks for no restart. Windows 7 needs two updates first:
  - KB2999226, the Universal C Runtime (Windows Update installs it; the installer says when it is missing);
  - Windows Management Framework 5.1 (KB3191566, needs .NET Framework 4.5.2 or newer), for PowerShell 5.1, which
    the installer runs in (the setup says when it is missing).
  The folder win7 in the unpacked package (WheelBri-<version>.zip, next to Setup.cmd) has scripts for them (cmd and
  Windows PowerShell only): download-prerequisites.cmd downloads all three (the two updates and .NET Framework
  4.5.2) from Microsoft into that folder (double-click it; on a Windows 10 or 11 PC if Windows 7 cannot reach
  Microsoft's servers), and install-prerequisites.cmd, on the Windows 7 PC, installs what is missing: right-click it
  > Run as administrator, restart Windows, then run WheelBri-<version>-setup.exe.
- Windows 8 and 8.1 (64-bit) are not tested. The setup runs there when PowerShell 5.1 (Windows Management Framework
  5.1) and the Universal C Runtime (KB2999226) are installed, and installs the shim way only, as on Windows 10; the
  win7 scripts are for Windows 7 only.
- An account that can approve an Administrator prompt, once, to install (the drivers and the WheelBri Helper are
  installed system-wide). Playing needs no Administrator: any user of the PC can run WheelBri.
- Your wheel and accessories, connected by USB. They can also be connected after installing.

Install
-------
Run WheelBri-<version>-setup.exe and approve the Administrator prompt: everything is installed with its defaults.
(Setup.cmd in the unpacked package WheelBri-<version>.zip does the same.) On Windows 11, at the end, after
"WheelBri is installed", it asks whether to restart Windows now (Enter = yes). Restart: that makes sure every driver
is installed properly (the filter that hides your wheel from games often starts only after a restart). Save your work
in other programs first; Windows restarts 2 seconds after the answer. On success the window closes at once. If
something fails, the window stays open with the reason in red.

Everything the installer did is in %LOCALAPPDATA%\wheelbri\logs\wheelbri-install-<date>.log, with every other log.
Running the setup again upgrades in place; an edited config in the install folder is kept (the new default is
written next to it as <name>.ini.new). An install from before the rename (when WheelBri was called wheelemu) is
replaced, settings included: its drivers, devices and Settings > Apps entry go, and its rigs and configs move to
%LOCALAPPDATA%\wheelbri the first time the app starts.

What it installs:
- the program in C:\Program Files\WheelBri, with the in-game shim (shim\dinput8.dll, shim\x86\dinput8.dll) and the
  default configs (config\);
- on Windows 11, two drivers: the virtual wheel (wheelbri-g920, which is the G25, G27, G920 or G923, whichever the
  Target is) and the filter that hides the real wheel (wheelbri-hide). The drivers are signed with WheelBri's own
  certificate, not by Microsoft, so the installer adds that certificate to this PC's trusted certificates. Secure
  Boot and test mode are not changed;
- on Windows 11, the WheelBri Helper, a Windows service that starts with Windows: it creates the virtual wheel and
  hides your rig for the app, so the app runs as an ordinary user (Services: "WheelBri Helper");
- a Start menu folder "WheelBri" (the app, and "Uninstall WheelBri"), a desktop shortcut and an entry in
  Settings > Apps.

Play
----
The top row of the Mapping tab (M) has two pickers, each with < and >:
- Target: the wheel games see, G25, G27, G920 (the default) or G923. Pick the one your game supports.
- Mode: Virtual (the default on Windows 11: WheelBri's driver creates the Target as a virtual wheel that every game
  sees, and your rig is hidden) or Shim (dinput8.dll next to the game's .exe presents the Target to that game only;
  nothing is created or hidden; the only Mode before Windows 11).

With Mode Virtual:
1. Start "WheelBri" from the Start menu and press Start (no Administrator prompt). From then on games see the
   virtual wheel, and not your real one: WheelBri hides it every time it starts.
2. Start the game and pick the Logitech wheel (the Target) in its controller settings.
3. Press Stop, or close the window, when done: the virtual devices go away and your rig is ordinary again.

What is hidden is your rig: every device set up in the Rig tab (R) (wheel, pedals, shifter, button box...).
Set it up before the first Start; with no rig nothing is hidden and the window says so. The first Start puts
WheelBri's filter on those devices (they restart once; close the wheel maker's software and games first; Windows may
want a restart). If the wheel maker's software later puts its own driver back, the next Start puts the filter on
again. The window says "rig hidden (N of M devices)", or lists in red the devices it could not hide.

With Mode Shim, the Games button next to Start lists your games. Add game: choose the game's folder (the one with
its .exe) and WheelBri puts the shim there. Pick the game, press Start, then start the game. When you press Stop,
WheelBri asks whether to delete the shim from the game's folder.

The Mapping tab also shows which control of your rig drives each of the Target's axes, buttons and D-pad
directions. WheelBri starts from its default mapping for the Target; change a source and press Save to keep your
own (one for both Modes), "Default" goes back to WheelBri's. Each Start uses exactly what the tab shows.

Uninstall
---------
Any of these (they all run the same uninstaller and ask for Administrator):
- Start menu > WheelBri > Uninstall WheelBri;
- Settings > Apps > Installed apps > WheelBri > Uninstall;
- Uninstall.cmd in C:\Program Files\WheelBri.
It asks to confirm, whether to keep the configs in the install folder (copied to
%LOCALAPPDATA%\wheelbri\config-backup) and whether to remove the drivers' certificate, then closes a running
WheelBri and removes the WheelBri Helper service, the drivers, the virtual devices, shortcuts, the Settings > Apps
entry, WheelBri's registry entries and the program folder. Run with -Quiet (no questions, as Settings > Apps' quiet
uninstall does), it moves the configs in the install folder that were changed after install to
%LOCALAPPDATA%\wheelbri\config-backup-<date>-<time> instead of deleting them.
Your own settings in %LOCALAPPDATA%\wheelbri (your mappings, rigs, app settings, logs and telemetry) are never
removed by the uninstaller.

If the program folder was deleted by hand or is damaged, Uninstall.cmd in the unpacked package
(WheelBri-<version>.zip) removes what is left: drivers, certificate, shortcuts and the Settings > Apps entry.
Options (after Uninstall.cmd or uninstall.ps1): -KeepConfig, -RemoveCertificate, -Quiet (no questions),
-InstallDir D:\WheelBri.

A folder outside Program Files (-InstallDir D:\WheelBri) gets Program Files' permissions at install: only
Administrators may change it, users read and run it (the WheelBri Helper runs as SYSTEM from there).

When something does not work
----------------------------
- Logs: all in %LOCALAPPDATA%\wheelbri\logs: the app, telemetry, both drivers, the installer and
  wheelbri-hide-install-<date>.log for hiding the rig. In the app, the Logs tab (press O) opens it. The drivers
  write to C:\ProgramData\wheelbri\logs instead when they cannot use that folder (for example when it is a link to
  another folder).
- The game shows no wheel: check that the app says "running", then joy.cpl (Win+R, joy.cpl) should list the
  Target (the G920 by default).
- The window says HELPER NOT INSTALLED: the WheelBri Helper service is not running. Start it (Win+R, services.msc,
  "WheelBri Helper", Start) or run the setup again.
- After a crash the rig is not hidden any more on its own (the WheelBri Helper shows it again when the app goes);
  "wheelbri-service.exe --unhide" (as Administrator) clears the setting by hand.

Extras
------
shim\dinput8.dll (64-bit games) and shim\x86\dinput8.dll (32-bit games) are the in-game shim that Mode Shim uses:
the app copies the one that matches the game into the game's folder (Games > Add game), with config\wheelbri-shim.ini
when the game has none yet, and offers to delete the shim at Stop. There is no need to copy them by hand.
