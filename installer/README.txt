WheelBri
========

Makes a Moza R3 wheel look like a Logitech G920, so
games that only know Logitech wheels, such as Need for Speed (2015), accept it, with force feedback. It does that
with its own drivers: a virtual G920 and a filter that hides the real R3 from games
while WheelBri runs (otherwise games would see two wheels).

Needs
-----
- Windows 11, 64-bit (build 22000 or later). Nothing else has to be installed first: the programs carry their own
  runtime, and the drivers use what Windows already has (UMDF 2).
- An account that can approve an Administrator prompt (the drivers are installed system-wide).
- The Moza R3, connected by USB. It can also be connected after installing.

Install
-------
Run Setup.exe and approve the Administrator prompt: everything is installed with its defaults. At the end, after
"WheelBri is installed", it asks whether to restart Windows now (Enter = yes). Restart: that makes sure every driver
is installed properly (the filter that hides the R3 from games often starts only after a restart). Save your work
in other programs first; Windows restarts 15 seconds after the answer. If something fails, the window stays open
with the reason in red.

Everything the installer did is in C:\ProgramData\wheelbri\logs\wheelbri-install-<date>.log.
Running Setup.exe again upgrades in place; an edited config is kept (the new default is written next to it as
<name>.ini.new). An install from before the rename (when WheelBri was called wheelemu) is replaced, settings included: its
drivers, devices and Settings > Apps entry go, and its rigs and configs move to %LOCALAPPDATA%\wheelbri the first time
the app starts.

What it installs:
- the program in C:\Program Files\WheelBri;
- two drivers: the virtual G920 (wheelbri-g920) and the filter that hides the real R3 (wheelbri-hide). The virtual
  Xbox controller of earlier versions is removed. The drivers are signed with WheelBri's own certificate, not by Microsoft, so
  the installer adds that certificate to this PC's trusted certificates. Secure Boot and test mode are not changed;
- a Start menu folder "WheelBri" (the app, and "Uninstall WheelBri"), a desktop shortcut and an entry in
  Settings > Apps.

Play
----
1. Start "WheelBri" from the Start menu (it asks for Administrator) and press Start. From then on games see the
   virtual G920, and not the R3: WheelBri hides it every time it starts.
2. Start the game and pick the Logitech G920 in its controller settings.
3. Press Stop, or close the window, when done: the virtual devices go away and the R3 is an ordinary wheel again.

Hiding the R3 needs WheelBri's filter on the R3. The installer puts it there when the R3 is connected; otherwise
the first Start does (the R3 restarts once; close Moza Pit House and games first). If Moza's software later puts
its own driver back on the R3, the next Start puts the filter on again. The window says "R3 NOT HIDDEN" in red when
it could not.

The settings are in C:\Program Files\WheelBri\config\moza-r3-as-g920.ini (moza-r3-as-g29.ini presents a G29).
Editing them needs an editor started as Administrator; each Start reads them again.

Uninstall
---------
Any of these (they all run the same uninstaller and ask for Administrator):
- Start menu > WheelBri > Uninstall WheelBri;
- Settings > Apps > Installed apps > WheelBri > Uninstall;
- Uninstall.cmd in C:\Program Files\WheelBri.
It asks to confirm, whether to keep your settings (copied to C:\ProgramData\wheelbri\config-backup) and whether to
remove the drivers' certificate, then closes a running WheelBri and removes the drivers, shortcuts, the
Settings > Apps entry and the program folder. Logs stay in %LOCALAPPDATA%\wheelbri\logs and
C:\ProgramData\wheelbri\logs.

If the program folder was deleted by hand or is damaged, Uninstall.cmd in the unpacked package (from the
WheelBri-<version>.zip next to Setup.exe) removes what is left: drivers, certificate, shortcuts and the
Settings > Apps entry.
Options (after Uninstall.cmd or uninstall.ps1): -KeepConfig, -RemoveCertificate, -Quiet (no questions),
-InstallDir D:\WheelBri.

When something does not work
----------------------------
- Logs: %LOCALAPPDATA%\wheelbri\logs (app/service, telemetry) and C:\ProgramData\wheelbri\logs (drivers,
  installer, wheelbri-hide-install-<date>.log for hiding the R3). In the app, the Logs tab (press O) opens the first one.
- The game shows no wheel: check that the app says "running", then joy.cpl (Win+R, joy.cpl) should list the
  G920.
- After a crash the R3 is not hidden any more on its own; "wheelbri-service.exe --unhide" (as Administrator)
  clears the setting by hand.

Extras
------
shim\ holds dinput8.dll, an older way that needs no drivers (copied next to one game's .exe). It is not used by
the app and is not needed for the steps above.
