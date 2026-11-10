# Changelog

## Not released yet

- Every Start begins new log files with the next number (the app's log, telemetry-*, ffb-*), so each session has
  files of its own; before, a run of the app wrote all its sessions into one.

- Combined pedals, as Logitech's "Report Combined Pedals", on every Target: for every game on the Mapping tab, or per
  game on the Games tab. Built-in profiles turn them on for NFS Underground, Underground 2 and Most Wanted (2005),
  whose menus scrolled by themselves with separate pedals. The state packet to the shim and the drivers is version 2:
  update the shim and the drivers together with the app.

- Windows: the installer needs no PowerShell any more and starts on a bare Windows 7 SP1, 8, 8.1, 10 or 11. The setup
  wizard does the install and the uninstall itself (only the Windows 11 drivers' own steps stay PowerShell scripts),
  and on Windows 7, 8 and 8.1 installs Microsoft's Universal C Runtime (KB2999226), which `Setup.exe` now carries,
  when it is missing. The package's `win7` prerequisite scripts, `Setup.cmd`, `Uninstall.cmd`, `install.ps1` and
  `uninstall.ps1` are gone: scripted installs use `wheelbri-setup.exe --quiet` and `Uninstaller.exe --uninstall
  --quiet` ([install.md](docs/install.md#without-the-wizard)). Windows 8 and 8.1 are supported (Mode Shim only).
- Windows: the install's log is one file per run (`wheelbri-install-<date>-<NN>.log`) in WheelBri's log format.

## 1.0.2 (2026-11-07)

- Linux: `wheelbri.sh --order logitech|native` picks the G25's or G27's button order; by default the app's Mapping tab
  Order, as the app's Start uses it (before, only the mapping file's).
- Linux: the G25's and G27's clutch works under Proton. Proton's DirectInput read both Sliders from the first one, so
  the clutch showed the combined pedals (in ACC: half at rest, up with the throttle, down with the brake). The virtual
  wheel now gives Proton the clutch as a Dial, which games still see as Slider 1.
- Linux: a game started with `wheelbri.sh` in Steam's launch option no longer stalls until Stop, and Stop no longer
  ends the virtual wheel before the game starts without it: Steam's overlay preload now goes to the game only, and
  `wheelbri.sh` does not start the game when the service or the virtual wheel has ended.
- Linux: `wheelbri.sh` does not start the game when the rig's wheel is not connected (after waiting 10 s for it)
  and says so on screen, in Game Mode too.

## 1.0.1 (2026-11-06)

Changes since 1.0.0.

- Mapping tab: the Target, Order and TrueForce rows' buttons line up after the longest label ("TrueForce" no longer
  runs into Off).
- Force feedback: `[ffb] max_slew_per_second` (1 to 50) and `frozen_force_s` (0.5 to 60 s) can only make the safety
  guards gentler; a value that would loosen them is reported and the default kept.
- Windows package: the README's and the notices' links to the licence open `LICENSE.txt`.
- CI: ShellCheck also checks the Linux scripts users run (`scripts/linux/*.sh`).
- Docs: corrected and brought up to date with 1.0.0; a command-line reference in the user guide; `SECURITY.md`;
  a licence line (SPDX) in every source file.

## 1.0.0 (2026-11-05)

The first stable release. Changes since 0.9.9.

### Install and uninstall (Windows)

- **Setup wizard** (`wheelbri-setup.exe`, started by `WheelBri-<version>-setup.exe`): Welcome, Install folder, Options,
  Ready, Installing (checklist and log, Copy log, Open log folder), Finish (restart now or later).
- **Uninstaller** (`Uninstaller.exe`, Start menu, Settings > Apps): what to keep, one rule for every box (ticked is
  kept): rig, mappings and settings; logs; the shim in game folders (chained mods get their names back); the drivers'
  certificate.
- `install.ps1`: `-StartMenuFolder`, `-Restart Now|Later|Ask`, `-Progress`. Elevated, it installs from a copy only
  Administrators may change.
- Each release signs its drivers with its own code-signing certificate, the private key deleted after signing; the
  installer trusts only the one the package names and removes earlier ones once unused.
- The uninstaller and `clean-pc` remove a G25/G27 force feedback entry a crash left behind.

### Targets

- **G25**: real axes and buttons for Race Driver: GRID, combined pedals on Slider 0, no longer a "car controller" to
  DirectInput; works in GRID in both Modes.
- **G25 and G27**: **Logitech** and **Native** button orders, each with its own mapping; **Centring spring** 0-100%;
  clutch on Slider 1 on every system.
- Every Target in the axis layout games' presets read.
- Button numbers shown as the game counts them (from 1 in Logitech order, from 0 in Native).

### Force feedback

- **Logitech's drivers on Windows**: G HUB's HID++ force feedback driver for the G920 and G923; G HUB's older-wheel
  driver for the G25 and G27 in Logitech order, playing Logitech's classic commands. Each copy of the driver keeps its
  own effect slots.
- **TrueForce for the G923** (Mode Virtual, Windows): the SDK's torque stream as a constant force, its damping as a
  damper, the road texture as a vibration. A **TrueForce** switch on the Mapping tab, greyed without G HUB's
  TrueForce SDK.
- **G HUB's HID++** for the virtual G923: G HUB sees it, every answer in report 0x12 as a real G923.
- Safety: slew on periodic offsets and magnitudes, a cap on all effects together, slots and a watchdog per sender, a
  silent game's small held force stopped, a push to zero or across centre always delivered, a lone full push treated
  as none (GRID).
- Per-game and master force settings retired: strength lives in the wheel's own software.

### App

- **Live tab** with the **Shadow view**: each Target drawn as the real wheel, with shifter, pedals and the G923's
  dial; **List** shows mapped buttons and the last one pressed.
- **Side by side**: the editor and Live in one window; every mapping change used at once while running.
- **Rig roles map the core controls** (Steering, pedals, D-pad, A, B, X, Y, paddles) on every Target.
- Source and secondary pickers with a find box, a scrolling list and rig touches; a control used elsewhere moves in
  one click; up to 8 secondaries per button, levers with a 0-100% threshold.
- Games tab: per-game button changes and secondaries; NFS Heat recognised (a warning when the Target is the G923).
- Live tab's force feedback monitor: per kind of effect, what the game asks and what reaches the wheel.
- One Start / Stop button; a notice above the tabs says what blocks Start; Start ends on the Live tab.
- Mode Shim alone and locked where Mode Virtual cannot run, its hover text naming the Windows.
- A wheel another program holds open (Firefox) is hidden anyway: Start reconnects its USB port.

### Mode Shim

- Another mod's `dinput8.dll` is chained, not overwritten, and given back on **Remove shim**; **Chain mod** on the
  Games tab; Linux `--shim` does the same.
- The hide filter is in the device stack only while Mode Virtual runs.

### Linux

- `/dev/uhid` for the group `wheelbri` only, not every program of the user at the seat.
- Reproducible bundle tarball.

### Quality and security

- CI builds and tests on Windows and Linux, builds the Windows installer and the Linux bundle, and publishes a release
  for a `v<version>` tag; clang-tidy, sanitizers, ShellCheck on the build scripts, and the fuzz targets' fixed inputs
  (`RobustnessTest`); tests run in parallel.
- Helper: driver scripts never write in a folder a user controls; its log only to an accepted caller. System DLLs and
  programs loaded from fixed places only.
- Many fixes found by an audit of the code (bounds-checked byte order, handle leaks, threads ended before exit).

### Docs

- A page per Target, an install page and a capabilities page.

## Earlier

0.2 to 0.9.9 (from 2026-09-30): see the `v0.*` tags.
