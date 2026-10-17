WheelBri
========

WheelBri makes your rig (a wheel and its pedals, shifter or button box) look like a Logitech wheel
that games know, with force feedback: a G25, G27, G920 (the default) or G923. That wheel is the
Target, picked in the app. Games that only know Logitech wheels, such as NFS 2015, then accept your
rig.

A game sees the Target in one of two ways, the Mode, also picked in the app:
- Virtual (Windows 11 only): WheelBri's driver creates the Target as a virtual wheel that every game
  sees, and its filter hides your real rig from games while WheelBri runs (otherwise games would see
  two wheels).
- Shim (any Windows): a file named dinput8.dll (the "shim") next to the game's .exe shows the Target
  to that one game. Nothing is created or hidden.


Needs
-----
- Windows 11, 64-bit (build 22000 or later): both Modes. Nothing has to be installed first: the
  programs carry their own runtime, and the drivers use what Windows already has.
- Windows 10, 64-bit: Mode Shim only. The setup installs no drivers and no WheelBri Helper there, and
  asks for no restart.
- Windows 7 SP1, 64-bit: Mode Shim only, after three downloads from Microsoft (see "Install on
  Windows 7 to 10" below).
- Windows 8 and 8.1 are not tested. The setup runs there once PowerShell 5.1 (Windows Management
  Framework 5.1) and the Universal C Runtime (KB2999226) are installed, and installs Mode Shim only.
- An account that can approve an Administrator prompt, once, to install. Playing needs no
  Administrator: every user of the PC can run WheelBri.
- Your wheel and accessories on USB. They can also be plugged in after installing.


Install on Windows 11
---------------------
1. Run WheelBri-<version>-setup.exe and approve the Administrator prompt. (Setup.cmd in the unpacked
   package, WheelBri-<version>.zip, does the same.)
2. The setup closes a running WheelBri, copies the program to C:\Program Files\WheelBri and installs
   the two drivers, the WheelBri Helper, a Start menu folder, a desktop shortcut and an entry in
   Settings > Apps. Each step is shown in the window.
3. At the end it says "WheelBri <version> is installed" and asks: Restart Windows now? [Y/n]
   Enter (yes) restarts Windows 2 seconds later: save your work in other programs first. Do restart:
   the filter that hides your rig often starts only after a restart.
4. If something fails, the window stays open with the reason in red. Press Enter to close it.

What it puts on the PC:
- the program in C:\Program Files\WheelBri: wheelbri.exe (the app), wheelbri-service.exe (the same
  for a command line), wheelbri-helper.exe, the shims (shim\dinput8.dll for 64-bit games,
  shim\x86\dinput8.dll for 32-bit games) and the default settings (config\);
- two drivers: wheelbri-g920, the virtual wheel (it becomes the G25, G27, G920 or G923, whichever the
  Target is), and wheelbri-hide, the filter that hides your rig from games while WheelBri runs;
- the drivers' certificate: they are signed with WheelBri's own certificate, not by Microsoft, so the
  setup adds that certificate to the certificates this PC trusts. Secure Boot and test mode are not
  changed. The uninstaller can remove the certificate again;
- the WheelBri Helper, a Windows service ("WheelBri Helper" in services.msc) that starts with
  Windows. Creating the virtual wheel and hiding the rig need Administrator: the Helper does those
  things for the app, so the app runs as an ordinary user and asks for no Administrator prompt;
- a Start menu folder "WheelBri" (the app, and "Uninstall WheelBri"), a desktop shortcut and an entry
  in Settings > Apps.

Options of Setup.cmd (in the unpacked package): -InstallDir D:\WheelBri (another folder; it gets
Program Files' permissions: only Administrators may change it), -NoDesktopShortcut, -Quiet (no
questions and no restart: restart Windows yourself afterwards).


Install on Windows 7 to 10
--------------------------
Windows 10: run WheelBri-<version>-setup.exe and approve the Administrator prompt. It installs the
program, the shims, the shortcuts and the Settings > Apps entry. No drivers, no Helper, no restart.

Windows 7 SP1 needs three things from Microsoft first: the Universal C Runtime (KB2999226), .NET
Framework 4.5.2 and Windows Management Framework 5.1 (KB3191566, which brings the PowerShell 5.1 the
setup runs in). The folder win7 in the unpacked package (WheelBri-<version>.zip, next to Setup.cmd)
has the scripts for them:
1. download-prerequisites.cmd: double-click it on any PC with internet (a Windows 10 or 11 PC if the
   Windows 7 PC cannot reach Microsoft). It downloads the three files into the win7 folder and checks
   that Microsoft signed them.
2. Copy the win7 folder to the Windows 7 PC if needed. Right-click install-prerequisites.cmd > Run as
   administrator. It installs what is missing, in the right order, and skips what is there.
3. Restart Windows, then run WheelBri-<version>-setup.exe.
The setup says so when PowerShell 5.1 or the Universal C Runtime is missing.


First start
-----------
Open "WheelBri" from the Start menu. The window has tabs along the bottom (click one, or press its
letter): Rig (R), Mapping (M), Game profiles (G) and Logs (L). At the right of the tabs: a short
status, then Start and Stop.

1. Rig tab: tell WheelBri what your rig is. Every connected game controller is shown. Click a control,
   or turn Detect on and touch it (turn the wheel, press a pedal all the way, press a button), and
   give it a name. A device belongs to the rig as soon as one of its controls has a name. Press Save.
2. Mapping tab, top row: two pickers, each with < and >:
   - Target: the wheel games see: G25, G27, G920 or G923. Pick one your game supports.
   - Mode: Virtual or Shim (on Windows 7 to 10 only Shim).
   Below them: which control of your rig drives each of the Target's axes, buttons and D-pad
   directions. WheelBri starts from its own default mapping for the Target. Change what you need and
   press Save: that is your own mapping for this Target, used in both Modes. Revert drops unsaved
   changes, Default goes back to WheelBri's mapping. Start uses the mapping as saved.
   The steering rotation is set there too. Set the same rotation in your wheel's own software:
   WheelBri never changes your wheel's own settings.
3. Press Start. Stop (or closing the window) ends the session.

When Start is greyed out, the status says why:
- NO RIG / NO WHEEL IN THE RIG: name your devices' controls in the Rig tab and Save.
- NO MAPPING: none of the Target's controls comes from your rig: map them in the Mapping tab.
- NOT CONNECTED: <names>: switch on or plug in the devices your mapping uses.
- HELPER NOT INSTALLED: Mode Virtual needs the WheelBri Helper (see "When something does not work").
While a session runs the status says "Running" and the time, and the game when WheelBri knows it.


Playing with Mode Virtual (Windows 11)
--------------------------------------
1. Close the wheel maker's software if it holds the wheel, then press Start (no Administrator prompt).
   The first Start after installing puts WheelBri's filter on your rig's devices: a window says so,
   it can take up to a minute and the devices reconnect once. Do not unplug them meanwhile.
2. Start the game and pick the Logitech wheel (the Target) in its controller settings.
3. Press Stop, or close the window, when done: the virtual wheel goes away and your rig is visible to
   games again.

What is hidden is your rig: every device of the Rig tab. Other devices stay visible. If the wheel
maker's software later puts its own driver back on a device, the next Start puts the filter on again.
The Logs tab says which devices are hidden, or which could not be.


Playing with Mode Shim
----------------------
With Mode Shim, "Game" and a Games button appear next to Start.
1. Games > Add game: choose the game's folder, the one with its .exe. WheelBri puts the shim there:
   the 32-bit one when the game is 32-bit (older games such as NFS Carbon), else the 64-bit one, and
   wheelbri-shim.ini (its settings) when the folder has none.
   If the folder has the Ultimate ASI Loader (a dinput8.dll many mods use), the shim goes in as
   dinput8Hooked.dll, which the loader loads; the loader stays as it is. Any other dinput8.dll that is
   not WheelBri's belongs to another mod: WheelBri leaves it alone and puts nothing in.
2. Pick the game in the list, press Start, then start the game. Start also writes the Target into the
   game's wheelbri-shim.ini.
3. When you press Stop, WheelBri asks whether to delete the shim from the game's folder. Keep it if you
   play that game again soon. In the Games panel, Remove shim deletes it later; Forget only takes the
   game off the list.
Do not use both Modes for one game: take the shim out before you play that game in Mode Virtual.

The shim replaces the wheel whose USB vendor id is in wheelbri-shim.ini (source_vid; 346e, Moza, by
default). For another make of wheel, open wheelbri-shim.ini in the game's folder with Notepad and set
source_vid to your wheel's vendor id: the first 4 characters of its vid:pid, which the Rig tab shows.


Logs
----
Every log is in %LOCALAPPDATA%\wheelbri\logs (press O in the app to open the folder):
- wheelbri-service-<date>-<NN>.log: the app and its sessions (one file per run);
- wheelbri-shim-...: the shim inside a game; telemetry-... and ffb-...: what the rig and the game did;
- wheelbri-driver-... and wheelbri-hide-...: the two drivers; wheelbri-helper-...: the Helper;
- wheelbri-install-<date>.log, wheelbri-driver-install-<date>.log, wheelbri-hide-install-<date>.log:
  the setup and the driver installers.
The Logs tab shows them live. Its Level button (or V) sets how much is written (warn at first); set
debug before you repeat a problem you want to report. Logs older than 2 days are deleted. The drivers
write to C:\ProgramData\wheelbri\logs instead when they cannot use that folder (for example when it
is a link to another folder); WheelBri moves those files over at a later Start.


Upgrading
---------
Run the new setup. It closes a running WheelBri and replaces the program in place. Your own mappings,
rig and settings are in %LOCALAPPDATA%\wheelbri and stay as they are. A config in the install
folder's config\ that you edited is kept, and the new default is written next to it as
<name>.ini.new. On Windows 11 the setup puts the hiding filter on your saved rig again when its
devices are plugged in; otherwise the next Start does it. Restart Windows when it asks.
An install from before the rename (when WheelBri was called wheelemu) is replaced: its drivers,
devices and Settings > Apps entry go, and its rigs and configs move to %LOCALAPPDATA%\wheelbri the
first time the app starts.


Uninstalling
------------
Take the shim out of your games first (Mode Shim: Games, Remove shim): the uninstaller does not
touch game folders.
Then use any of these (they all run the same uninstaller, which asks for Administrator):
- Start menu > WheelBri > Uninstall WheelBri;
- Settings > Apps > Installed apps > WheelBri > Uninstall;
- Uninstall.cmd in C:\Program Files\WheelBri.
It asks to confirm, whether to keep the configs of the install folder (copied to
%LOCALAPPDATA%\wheelbri\config-backup) and, on Windows 11, whether to remove the drivers' certificate.
Then it closes a running WheelBri and removes the WheelBri Helper, both drivers (your rig's devices go
back to Windows' own driver), the virtual devices, shortcuts, the Settings > Apps entry, WheelBri's
registry entries and the program files.
With -Quiet (no questions, as Settings > Apps' quiet uninstall) it moves the configs of the install
folder that were changed after install to %LOCALAPPDATA%\wheelbri\config-backup-<date>-<time>
instead of deleting them, and keeps the certificate unless -RemoveCertificate is given too.
Always kept: %LOCALAPPDATA%\wheelbri (your mappings, rig, app settings, logs, telemetry). Delete that
folder by hand to remove everything.

If the program folder was deleted by hand or is damaged, Uninstall.cmd in the unpacked package
(WheelBri-<version>.zip) removes what is left: drivers, certificate, shortcuts and the Settings > Apps
entry. Options (after Uninstall.cmd or uninstall.ps1): -KeepConfig, -RemoveCertificate, -Quiet,
-InstallDir D:\WheelBri.


When something does not work
----------------------------
- The game sees no wheel:
  Mode Virtual: check that the status says Running, then press Win+R and run joy.cpl: it should list
  the Target (for example the Logitech G920) and not your rig. Then pick that wheel in the game's
  controller settings.
  Mode Shim: press Start before you start the game, check in the Games panel that the game has the
  shim ("shim in place"), and check source_vid in the game's wheelbri-shim.ini (see "Playing with
  Mode Shim"). A game folder under C:\Program Files may be protected: WheelBri then says it cannot
  write there; install the game in a folder you own.
- The game sees your rig and the Target (two wheels): the Logs tab names the devices that are not
  hidden. Close the wheel maker's software and games, unplug the device and plug it in again (or
  restart Windows), then press Start again.
- GAME NOT USING WHEEL: the game drives from another controller (a gamepad or the keyboard). Pick the
  Logitech wheel in the game's controller settings; unplug a gamepad before you start the game.
- No force feedback but the wheel works: GAME FFB OFF: ALT-TAB means the game loaded its effects but
  did not start them: switch away from the game and back (Alt+Tab). Otherwise check that force
  feedback is on in the game's own settings and, with Mode Shim, that you pressed Start before the
  game and that the game's wheelbri-shim.ini says force_feedback = service.
- HELPER NOT INSTALLED: the WheelBri Helper service is not running. Start it (Win+R, services.msc,
  "WheelBri Helper", Start) or run the setup again. Or start WheelBri as Administrator (right-click >
  Run as administrator): it then does the Helper's work itself.
- Your rig stays hidden from games after WheelBri crashed or was ended: normally the filter hides
  nothing once WheelBri is gone, and the next Start clears what is left. If a game still cannot see
  your rig with WheelBri closed, start WheelBri and press Start and Stop once, or open a Command
  Prompt as Administrator and run:
      "C:\Program Files\WheelBri\wheelbri-service.exe" --unhide
- Start failed (Logs): the Logs tab says why, in red.
