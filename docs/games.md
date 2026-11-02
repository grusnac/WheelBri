# Games

The games WheelBri was tested with and what each needs. Rig: Moza R3 base and pedals with the clutch add-on, Windows 11.
Using the app: [user-guide.md](user-guide.md).

| Game | Target | Mode Virtual | Mode Shim | Built-in profile |
|---|---|---|---|---|
| [Assetto Corsa Competizione](#assetto-corsa-competizione) | all four | works | not supported | none |
| [DiRT Rally 2.0](#dirt-rally-20) | all four | works | not supported | none |
| [NFS 2015](#nfs-2015) | G920 | works | works | pedal remap |
| [NFS Payback](#nfs-payback) | G920 | works; needs G HUB | not recommended | 180 degrees |
| [NFS Heat](#nfs-heat) | G920 | works | works | none |
| [NFS Unbound](#nfs-unbound) | G920 | works | works | gain 3, its hidden settings |
| [Race Driver: GRID](#race-driver-grid) | G25 | works | works (32-bit) | none |

"Works" means wheel, pedals, buttons and force feedback were checked in the game. Built-in profiles apply with Target
G920 only, by themselves while the game runs.

## Assetto Corsa Competizione

- **Set up:** pick the Target's preset in the game's controls. For the G25 and G27 use the Mapping tab's **Order:
  Native**: ACC's presets read the wheel's own button numbers.
- **Mode Shim:** not supported by the game; use Mode Virtual.
- **Linux:** runs under Proton with Mode Virtual.
- **TrueForce (Target G923, Windows):** ACC then plays its TrueForce force (with its damping) on your wheel.
  - In `my-g923.ini`: `[emulator] trueforce = true`, and `[steering] target_rotation_deg` at the rotation you want
    (360 tested).
  - Logitech's force feedback driver must be installed (`C:\Program Files\Logitech\Direct Input Force Feedback`,
    installed with G HUB on the test PC); WheelBri registers it for the wheel.
  - In ACC: Steer Lock **2700**, so the car turns fully at your rotation; Gain 100% felt right.
  - Moza: keep Pit House closed while you play. With it open the wheel nudges as a track loads.
  - The road texture is not played yet ([Targets](targets.md#trueforce-g923)).

## DiRT Rally 2.0

- **Set up:** pick the Target's preset in the game's controls. G25 and G27: **Order: Logitech** (its G25
  handbrake "Button 7" is R Wheel).
- **Mode Shim:** not supported by the game; use Mode Virtual.

## NFS 2015

- **Set up:** nothing; keep the game's wheel controls at their defaults.
- **Profile:** the game reads a G920's pedals from other axes than a real G920 reports them; the profile swaps them.
- **Known:** changed wheel controls show `NFS 2015: RESET ITS WHEEL CONTROLS`. Force feedback is sometimes off at the
  start of a race (`GAME FFB OFF: ALT-TAB`): Alt+Tab out and back. Keyboard button prompts, no rev lights.

## NFS Payback

- **Set up:** install Logitech G HUB, and set your wheel's software to **180** degrees for this game.
- **Profile:** 180 degrees while the game runs (change it in the Games tab).
- **Mode Shim:** the game reads wheels through Logitech's SDK DLL; use Mode Virtual.
- **Known:** the SDK loads the helper DLL named in the registry. If that entry still points at the removed Logitech
  Gaming Software, the game ignores every wheel. Point it at G HUB's DLL from an Administrator PowerShell, game closed
  (use your G HUB's version folder):

  ```powershell
  reg add "HKLM\SOFTWARE\Classes\CLSID\{63BD165D-1584-4E75-AB56-08330350545F}\ServerBinary" /ve /t REG_SZ /d "C:\Program Files\Logi\wheel_sdk\9_1_1\logi_steering_wheel_x64.dll" /f
  ```

  Slow steering is the game's own handling; 180 degrees helps.

## NFS Heat

- **Set up:** nothing; no Logitech software needed. Played at 360 degrees.
- **Target:** G920. As a G923 the D-pad does nothing (the game predates the G923; real G923 owners report the same).
- **Known:** light force feedback (rarely over 30%): raise the game's or the wheel's strength. It can stop until you
  Alt+Tab, as in NFS 2015.

## NFS Unbound

- **Set up:** before the first race, game closed: Games tab, NFS Unbound, **Save to file** (**Locate...** first if
  the file is not under Documents).
- **Profile:** writes force feedback 1.0, spring 0.8 and the steering rotation into the game's hidden settings
  (original kept as `PROFILEOPTIONS.before-wheelbri`).
- **Target:** G920. As a G923 the game falls back to a generic layout (A opens the friend list).
- **Known:** the game quits without stopping its forces; WheelBri stops them half a second later.

## Race Driver: GRID

- **Target:** **G25**, Order Logitech (the game has its own G25 preset). Mode Shim uses the 32-bit shim.
- **Set up:** VSync on (or the frame rate capped): above about 144 Hz the game sends a broken force, which WheelBri
  plays as none. The game sends no centring and turns the wheel's off: set the G25's **Centring spring** on the
  Mapping tab.
- **Buttons:** Instant Replay is Red 3. In a replay the wheel and pedals rewind and fast forward; pause, cameras and
  the Flashback are keys only (F1, F7/F8, F12).
- **Known:** Alt+Tab in fullscreen crashes the game during a race (its own Direct3D 9 bug). Windowed
  (`fullscreen="false"` in `Documents\Codemasters\GRID\hardwaresettings\hardware_settings_config.xml`) may avoid it.
  For 1440p, delete `system\hardware_settings_restrictions.xml` (editing it crashed the game at start).

## Not yet tried

- **The Steam Deck**, and the NFS games on Linux (NFS Payback is not expected to work: Proton has no Logitech DLL).
- **Windows 7 and 10** (Mode Shim only).
- **Other games** that support a Logitech wheel should work in Mode Virtual. One that reads the pedals differently
  needs a small game profile, as NFS 2015 does.

## Reporting a game

Set the Logs tab's **Level** to debug, play, and send that run's files (`wheelbri-service-...`, `telemetry-...`,
`ffb-...`, and `wheelbri-shim-...` or `wheelbri-driver-...`) with the game, Target, Mode, date and what the wheel,
pedals and buttons did.
