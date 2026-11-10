# What WheelBri can do

A one-page list. How to use each: [user-guide.md](user-guide.md).

## Your rig as a Logitech wheel

- **Four Targets**: G920 (default), G923, G25, G27, each on its own page ([targets.md](targets.md)).
- **Any rig**: a wheel base plus up to 3 accessories (pedals, shifter, handbrake, button box). Built and tested with a
  Moza R3; other wheels should work but are untried.
- **Two Modes**:
  - **Virtual**: a virtual Logitech wheel every game sees, with your rig hidden from games while it runs (Windows 11:
    WheelBri's own drivers; Linux: `/dev/uhid`). Works with modern games.
  - **Shim**: a `dinput8.dll` next to one game's `.exe`, for older DirectInput games; 32 and 64-bit; chains another
    mod's `dinput8.dll` (ReShade, Widescreen Fix) instead of overwriting it; works next to the Ultimate ASI Loader.
    The only Mode on Windows 7, 8, 8.1 and 10.

## Set up and mapping

- **Rig tab**: touch a control to name it and say what it is (Steering, Throttle, A, Left paddle...). That maps it on
  every Target at once.
- **Mapping tab**: every axis, D-pad direction and button of the Target, with a source picker that finds a control by
  moving it or by typing. Invert per pedal, steering rotation with presets, up to 8 extra controls per button
  (a handbrake lever past a set point, a second button).
- **G25 and G27**: Logitech or Native button order, each with its own mapping; centring spring 0-100%.
- **G923**: TrueForce on or off.
- **Live editing**: every change is saved and used at once, even while a game runs. **Undo** one change at a time;
  **Default** back to WheelBri's mapping.
- **Side by side**: the editor and the Live tab in one window.

## While you play

- **Live tab**: the **Shadow view** (the Target drawn as the real wheel, every control lit as the game gets it) or the
  **List** (every rig axis next to the Target's, last button pressed and what drives it).
- **Force feedback monitor**: per kind of effect, what the game asks and what reaches the wheel.
- **Status and notices**: what blocks Start, which devices are missing, a lost wheel, a game that ignores the wheel.
- **Game detection**: a game with a profile opens the Games tab on it when it starts.

## Per-game values (Games tab)

- Steering rotation, pedal axes and Normal / Inverted, combined pedals, and button changes for one game only, applied
  while it runs.
- Built-in profiles for NFS 2015, Payback and Unbound (Target G920), NFS Underground, Underground 2 and Most Wanted
  (combined pedals, every Target), and NFS Unbound's hidden wheel settings written to its file.
- Combined pedals (Logitech's "Report Combined Pedals") for every game too, on the Mapping tab.
- Shim status per game folder: shim in, ASI Loader, chained mod, **Chain mod**, **Remove shim**; hot swap of the shim
  when the Target changes.

## Force feedback

- Everything the game sends, passed on through safety limits: no sudden jumps, a push held 2 s unchanged fades, forces
  stop when the game goes quiet, a cap on all effects together, a watchdog per sender.
- Logitech's own force feedback drivers on Windows when G HUB has installed them (G920, G923; G25 and G27 in Logitech
  order), else Windows' HID PID driver.
- **TrueForce** for the G923 (Windows, Mode Virtual): the torque stream, damping and road texture.
- Strength is set in your wheel's own software, once for all games.

## Windows

- **Setup wizard** and **Uninstaller** with a checklist and log ([install.md](install.md)); scripted installs too.
- The **WheelBri Helper** service does the Administrator work, so the app runs as a normal user.
- Hides the rig even when another program holds it open: Start reconnects its USB port.
- Optional: turn off Firefox's game controller support, which holds wheels open.

## Linux and the Steam Deck

- Mode Virtual through `/dev/uhid`, read by Proton games raw.
- `wheelbri.sh` as a Steam launch option starts WheelBri with the game and stops it after, in Game Mode too; options
  for Target, Mode, button order, mapping file, devices to hide ([user-guide.md](user-guide.md#linux-and-the-steam-deck)).
- Mode Shim per game, put in before the game and taken out after.

## Diagnostics

- **Logs tab** with levels, one file per run, kept 2 days.
- `wheelbri-service --ffb-test` plays a gentle force sequence; `--unhide` brings a rig back after a crash; `--list`,
  `--print` for raw values.
