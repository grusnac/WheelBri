# Changelog

## Unreleased

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
