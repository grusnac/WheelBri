# Install, upgrade, uninstall

Short version in the [README](../README.md#install). Next, once installed: [user-guide.md](user-guide.md).

| System | Download | What you get |
|---|---|---|
| Windows 11, 64-bit | `WheelBri-<version>-setup.exe` (or the `.zip`) | everything: Mode Virtual and Mode Shim |
| Windows 7 SP1 to 10, 64-bit | the same | Mode Shim only (no drivers, no restart) |
| Linux x86-64 (glibc 2.39 or newer), Steam Deck | `wheelbri-linux-x86_64.tar.gz` | Mode Virtual and Mode Shim |

Releases: <https://github.com/grusnac/WheelBri/releases>

## Windows: the setup wizard

Close WheelBri and any game, run `WheelBri-<version>-setup.exe` and approve the Administrator prompt (the drivers need
it). The wizard:

1. **Welcome**: what setup found. Your Windows and what it gets (full install, or Mode Shim only), an earlier WheelBri
   it will upgrade, and that it runs as Administrator.

   ![Setup: Welcome](images/installer-welcome.png)

2. **Install folder**: `C:\Program Files\WheelBri` by default. A new or empty folder, or an earlier WheelBri's; a folder
   holding other files is refused, so the uninstaller never deletes your files. Outside Program Files setup locks the
   folder so only Administrators can change it.

   ![Setup: Install folder](images/installer-folder.png)

3. **Options**:
   - **Start menu folder** (default `WheelBri`): WheelBri, Uninstaller and WheelBri logs go in it.
   - **Desktop shortcut** (on).
   - **Turn off game controllers in Firefox** (off; shown only with Firefox installed). Firefox holds every game
     controller open, so WheelBri cannot hide the wheel while it runs. The uninstaller turns it back on.
   - **Open WheelBri when setup finishes** (on).

   ![Setup: Options](images/installer-options.png)

4. **Ready**: your choices in one list; **Back** changes them, **Install** starts.

   ![Setup: Ready](images/installer-ready.png)

5. **Installing**: a checklist (check this PC, close a running WheelBri, copy files, install the virtual wheel driver
   and the hiding filter driver, start the WheelBri Helper, shortcuts, Settings > Apps entry, check the installation)
   with the full log below. **Copy log** and **Open log folder** help when something fails.

   ![Setup: Installing](images/installer-installing.png)

6. **Finish**: **Restart Windows now** (default) or later. Windows 11 needs the restart so the hiding filter takes over
   your rig's devices.

   ![Setup: Finish](images/installer-finish.png)

What it puts on the PC (all removed by the uninstaller):

- the program in the install folder, the Start menu folder, the desktop shortcut and the Settings > Apps entry;
- two drivers: `wheelbri-wheel` (the virtual wheel, all four Targets) and `wheelbri-hide` (hides your rig while Mode
  Virtual runs);
- the **WheelBri Helper** service (starts with Windows): it does the Administrator work, so the app runs as a normal
  user;
- the drivers' certificate in this PC's trusted stores. The drivers are signed with WheelBri's own certificate, not
  Microsoft's; each release has its own, its private key deleted once the drivers are signed. Secure Boot and test
  mode stay as they are.

Your rig, mappings and settings live in `%LOCALAPPDATA%\wheelbri`, the logs in `%LOCALAPPDATA%\wheelbri\logs`
(the install's own log too: `wheelbri-install-<date>.log`).

### Upgrade

Run the new setup. The Welcome page names the version it upgrades; your rig, mappings and settings stay.

### Windows 7 SP1 to 10

The same setup installs Mode Shim only: no drivers, no Helper, no restart. Windows 7 first needs PowerShell 5.1 and the
Universal C Runtime: from the package's `win7` folder run `download-prerequisites.cmd`, then
`install-prerequisites.cmd` as Administrator, and restart.

### Without the wizard

From the unpacked `.zip`, `Setup.cmd` runs the same install (`install.ps1`) in a console window. Options, for scripted
installs:

| Option | What it does |
|---|---|
| `-InstallDir D:\WheelBri` | another install folder |
| `-NoDesktopShortcut` | no desktop shortcut |
| `-StartMenuFolder "Games\WheelBri"` | the Start menu folder's name |
| `-StopBrowserGamepads` | turn off Firefox's game controllers without asking |
| `-Restart Now\|Later\|Ask` | at the end (default Ask) |
| `-Quiet` | no questions, no pause, no restart (restart yourself) |

Exit code 0: installed (warnings listed at the end); 1: failed (the red line says why).

## Windows: the uninstaller

Start menu > WheelBri > **Uninstaller** (or Settings > Apps > WheelBri, or `Uninstaller.exe` in the install folder).

1. **What to keep**: the program, drivers, Helper, shortcuts and Settings > Apps entry always go. One rule for every
   box: **ticked is kept, unticked is removed**.

   | Box | Default | What it is |
   |---|---|---|
   | Rig, mappings and settings | kept | rigs, configs, per-game values, device names, `app.ini`; with its size |
   | Logs | removed | app, service, drivers, telemetry, force feedback, install, dumps |
   | The shim in game folders | removed | lists the games; another mod's DLL WheelBri chained gets its name back |
   | The drivers' certificate | removed | only WheelBri's drivers used it |

   Firefox's game controllers are turned back on if setup turned them off.

   ![Uninstaller: What to keep](images/uninstaller-keep.png)

2. **Removing**: a checklist and log, as when installing.

   ![Uninstaller: Removing](images/uninstaller-removing.png)

3. **Finish**: what was removed and what was kept (**Open folder** shows it). If Windows still holds a driver, it
   finishes removing it at the next restart. Install again any time: kept settings are picked up where they were.

   ![Uninstaller: Finish](images/uninstaller-finish.png)

Scripted: `Uninstall.cmd` in the install folder runs `uninstall.ps1`. Its defaults keep more than the Uninstaller's
boxes do, so a script never deletes data it was not told to:

| Option | Without it |
|---|---|
| `-RemoveSettings` | rig, mappings and settings kept |
| `-RemoveLogs` | logs kept |
| `-KeepShims` | the shim taken out of the game folders |
| `-RemoveCertificate` | the drivers' certificate kept |
| `-KeepConfig` | configs edited in the install folder (`config\*.ini`) asked about, or with `-Quiet` moved to `%LOCALAPPDATA%\wheelbri\config-backup-<date>` |
| `-Quiet` | questions asked |
| `-InstallDir DIR` | the folder Settings > Apps records |

`Uninstall.cmd` from an unpacked package also cleans up an install whose folder is damaged or gone.

## Linux and the Steam Deck

1. Unpack into your home folder (→ `~/wheelbri`):

   ```bash
   tar -xzf wheelbri-linux-x86_64.tar.gz -C ~
   ```

   The bundle needs glibc 2.39 or newer (`ldd --version` shows yours); on an older system, or on the Deck if you
   like, build it instead ([building.md](building.md#steam-deck)).

2. Once per machine, the udev rules (Steam Deck: set a password with `passwd` first), then log out and in:

   ```bash
   ~/wheelbri/install-udev-rules.sh
   ```

   They add you to the group `wheelbri`, which alone may use `/dev/uhid` to create the virtual wheel, let games open
   it and let WheelBri read the rig.

3. Force feedback on the R3 needs the kernel's `hid-universal-pidff` (Linux 6.15 and later):

   ```bash
   modinfo hid-universal-pidff
   ```

4. Set up the rig in the app (`~/wheelbri/bin/wheelbri`), then add the Steam launch option
   ([user-guide.md](user-guide.md#linux-and-the-steam-deck)).

Settings live in `~/.config/wheelbri`, logs in `~/.local/state/wheelbri` (or under `$XDG_CONFIG_HOME` and
`$XDG_STATE_HOME` when those are set).

**Upgrade:** unpack the new bundle over `~/wheelbri`. Your settings stay.

**Uninstall:**

```bash
~/wheelbri/install-udev-rules.sh --remove
```

Then delete `~/wheelbri`, and for a clean start `~/.config/wheelbri` and `~/.local/state/wheelbri`.

## Troubleshooting the install

- **Setup failed**: the red line and the log say why; **Copy log** and send it with your Windows version.
- **`HELPER NOT INSTALLED`** in the app: Win+R, `services.msc`, start "WheelBri Helper", or run the setup again.
- **The rig stays hidden after a crash**: [user-guide.md](user-guide.md#windows).
