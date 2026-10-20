# Games

The games WheelBri was tested with, and what each needs. Rig: Moza R3 base and pedals with the clutch add-on, Windows
11, Target **G920** (Race Driver: GRID: **G25**). How to use the app: [user-guide.md](user-guide.md).

| Game | Mode | Status | Game profile |
|---|---|---|---|
| [NFS 2015](#nfs-2015) | Virtual, Shim | works | pedal remap |
| [NFS Payback](#nfs-payback) | Virtual, Shim | works; needs Logitech G HUB | 180 degrees |
| [NFS Heat](#nfs-heat) | Virtual | works | none |
| [NFS Unbound](#nfs-unbound) | Virtual, Shim | works; force feedback being tuned | gain 3, its hidden settings |
| [Race Driver: GRID](#race-driver-grid) | Virtual, Shim | works as a **G25**; crashes on Alt+Tab in fullscreen | none |

"Works" means wheel, pedals, buttons and force feedback were checked in the game. Profiles apply with Target G920 only
and are applied by themselves while the game runs.

## NFS 2015

- **Set up:** nothing. Keep the game's wheel controls at their defaults.
- **Profile:** the game reads a G920's pedals from other axes than a real G920 reports them (the gas pedal would brake);
  the profile swaps them.
- **Known:** if the game's wheel controls were changed, the status says `NFS 2015: RESET ITS WHEEL CONTROLS`. Force
  feedback is sometimes off at the start of a race (`GAME FFB OFF: ALT-TAB`): Alt+Tab out and back. Keyboard button
  prompts and no rev lights.

## NFS Payback

- **Set up:** install Logitech G HUB, and set your wheel's software to **180** degrees for this game.
- **Profile:** 180 degrees while the game runs (change it in the Games tab).
- **Known:** the game reads wheels through Logitech's SDK, which loads the helper DLL named in the registry. On some PCs
  that entry still points at the old, removed Logitech Gaming Software, and the game then ignores every wheel. Point it
  at G HUB's DLL from an Administrator PowerShell with the game closed (use the version folder you have):

  ```powershell
  reg add "HKLM\SOFTWARE\Classes\CLSID\{63BD165D-1584-4E75-AB56-08330350545F}\ServerBinary" /ve /t REG_SZ /d "C:\Program Files\Logi\wheel_sdk\9_1_1\logi_steering_wheel_x64.dll" /f
  ```

  The slow steering is the game's own handling (other wheels report the same); 180 degrees helps. An earlier try with
  the shim (2026-10-11) ended in a DirectX error (ResizeBuffers) at start; it did not come back.

## NFS Heat

- **Set up:** nothing; no Logitech software needed. Played at 360 degrees.
- **Known:** its force feedback is light (rarely over 30%): raise the game's or the wheel's strength. It can stop until
  you Alt+Tab, as in NFS 2015. Mode Shim and the clutch are untried.

## NFS Unbound

- **Set up:** before the first race, with the game closed: Games tab, NFS Unbound, **Save to file** (**Locate
  file** first if the file is not under Documents).
- **Profile:** the game's wheel settings are hidden in its settings file and very light by default; the profile writes
  force feedback 1.0, spring 0.8 and the steering rotation there (the original is kept as
  `PROFILEOPTIONS.before-wheelbri`), and multiplies its steering force by 3.
- **Known:** the game quits without stopping its forces; WheelBri stops them half a second later.
- **Target:** use the G920. As a G923 the game falls back to a layout for wheels it does not know (A opens the friend
  list); WheelBri says so on the Games tab and in the bar next to Start.

## Race Driver: GRID

- **Target:** **G25** (the game has its own G25 preset: wheel X, throttle Y, brake Rz, its combined pedals on Slider 0).
  Both modes work; Mode Shim needs the 32-bit shim (the game is 32-bit).
- **Set up:** VSync on in the game (or the frame rate capped): with VSync off at 144 Hz it sends a broken force (no
  force feedback). The game sends no centring of its own and turns the wheel's off: set the G25's **Centring spring**
  on the Mapping tab (Logitech's software had the same setting; untried yet).
- **Buttons (its G25 preset):** Instant Replay is Red 3; in the replay the wheel and pedals rewind and fast forward,
  and pause, cameras and Jump In (the Flashback) are keys only (F1, F7/F8, F12).
- **Known:** Alt+Tab in fullscreen crashes the game during a race (its own Direct3D 9 code writes to a graphics
  buffer it has lost, seen in a crash dump; not WheelBri's). Do not Alt+Tab while racing; windowed
  (`fullscreen="false"` in `Documents\Codemasters\GRID\hardwaresettings\hardware_settings_config.xml`) may avoid it
  (untried in a race).
  Native 1440p: edits inside `system\hardware_settings_restrictions.xml` (even PCGamingWiki's `<res mem="270" />`)
  made this copy crash at start; deleting the whole file let it run at 2560x1440. GridMTFix's patched game crashed at
  start too.

## Not yet tried

- **The G27 and G923 targets** in any game.
- **Linux and the Steam Deck.** NFS Payback is not expected to work there (Logitech's DLL is missing in Proton).
- **Windows 7 and 10** (Mode Shim only).
- **Other games** that support a G920 should work in Mode Virtual. A game that reads the pedals differently needs a
  small game profile, as NFS 2015 does.

## Reporting a game

Set the Logs tab's **Level** to debug, play, and send that run's files from `%LOCALAPPDATA%\wheelbri\logs`
(`wheelbri-service-...`, `telemetry-...`, `ffb-...`, and `wheelbri-shim-...` or `wheelbri-driver-...`), with the
game, Target, Mode, date and what the wheel, pedals and buttons did.
