WheelBri
========

Makes your rig (a wheel and its pedals, shifter or button box) look like a Logitech G920, so
games that only know Logitech wheels, such as Need for Speed (2015), accept it, with force feedback. It does that
with its own drivers: a virtual G920 and a filter that hides your real wheel and its accessories from games
while WheelBri runs (otherwise games would see two wheels).

Needs
-----
- Windows 11, 64-bit (build 22000 or later). Nothing else has to be installed first: the programs carry their own
  runtime, and the drivers use what Windows already has (UMDF 2).
- An account that can approve an Administrator prompt (the drivers are installed system-wide).
- Your wheel and accessories, connected by USB. They can also be connected after installing.

Install
-------
Run Setup.exe and approve the Administrator prompt: everything is installed with its defaults. At the end, after
"WheelBri is installed", it asks whether to restart Windows now (Enter = yes). Restart: that makes sure every driver
is installed properly (the filter that hides your wheel from games often starts only after a restart). Save your work
in other programs first; Windows restarts 15 seconds after the answer. If something fails, the window stays open
with the reason in red.

Everything the installer did is in %LOCALAPPDATA%\wheelbri\logs\wheelbri-install-<date>.log, with every other log.
Running Setup.exe again upgrades in place; an edited config is kept (the new default is written next to it as
<name>.ini.new). An install from before the rename (when WheelBri was called wheelemu) is replaced, settings included: its
drivers, devices and Settings > Apps entry go, and its rigs and configs move to %LOCALAPPDATA%\wheelbri the first time
the app starts.

What it installs:
- the program in C:\Program Files\WheelBri;
- two drivers: the virtual G920 (wheelbri-g920) and the filter that hides the real wheel (wheelbri-hide).
  The drivers are signed with WheelBri's own certificate, not by Microsoft, so
  the installer adds that certificate to this PC's trusted certificates. Secure Boot and test mode are not changed;
- a Start menu folder "WheelBri" (the app, and "Uninstall WheelBri"), a desktop shortcut and an entry in
  Settings > Apps.

Play
----
1. Start "WheelBri" from the Start menu (it asks for Administrator) and press Start. From then on games see the
   virtual G920, and not your real wheel: WheelBri hides it every time it starts.
2. Start the game and pick the Logitech G920 in its controller settings.
3. Press Stop, or close the window, when done: the virtual devices go away and your rig is ordinary again.

What is hidden is your rig: every device set up in the Rig tab (R) (wheel, pedals, shifter, button box...).
Set it up before the first Start; with no rig nothing is hidden and the window says so. The first Start puts
WheelBri's filter on those devices (they restart once; close the wheel maker's software and games first; Windows may want a
restart). If the wheel maker's software later puts its own driver back, the next Start puts the filter on again. The window
says "rig hidden (N of M devices)", or lists in red the devices it could not hide.

The Mapping tab (M) maps your rig onto the G920 by itself, from what you set up in the Rig tab. Its top row says
how the G920 reaches the game: "virtual device" (the default, every game) or "in-game shim" (dinput8.dll from the
shim folder next to the game's .exe). Change a mapping there and press Save to keep your own; "Rig mapping" goes
back to the rig's. Each Start uses exactly what the tab shows.

Uninstall
---------
Any of these (they all run the same uninstaller and ask for Administrator):
- Start menu > WheelBri > Uninstall WheelBri;
- Settings > Apps > Installed apps > WheelBri > Uninstall;
- Uninstall.cmd in C:\Program Files\WheelBri.
It asks to confirm, whether to keep your settings (copied to C:\ProgramData\wheelbri\config-backup) and whether to
remove the drivers' certificate, then closes a running WheelBri and removes the drivers, shortcuts, the
Settings > Apps entry and the program folder. Logs stay in %LOCALAPPDATA%\wheelbri\logs.

If the program folder was deleted by hand or is damaged, Uninstall.cmd in the unpacked package (from the
WheelBri-<version>.zip next to Setup.exe) removes what is left: drivers, certificate, shortcuts and the
Settings > Apps entry.
Options (after Uninstall.cmd or uninstall.ps1): -KeepConfig, -RemoveCertificate, -Quiet (no questions),
-InstallDir D:\WheelBri.

When something does not work
----------------------------
- Logs: all in %LOCALAPPDATA%\wheelbri\logs: the app, telemetry, both drivers, the installer and
  wheelbri-hide-install-<date>.log for hiding the rig. In the app, the Logs tab (press O) opens it.
- The game shows no wheel: check that the app says "running", then joy.cpl (Win+R, joy.cpl) should list the
  G920.
- After a crash the rig is not hidden any more on its own; "wheelbri-service.exe --unhide" (as Administrator)
  clears the setting by hand.

Extras
------
shim\ holds dinput8.dll, an older way that needs no drivers (copied next to one game's .exe). It is not used by
the app and is not needed for the steps above.
