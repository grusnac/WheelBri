# Games

The games WheelBri was tested with and what each needs. Rig: Moza R3 base and pedals with the clutch add-on, on Windows 11
and on a Steam Deck (SteamOS 3.8.28, kernel 6.18, Proton 11).
Using the app: [user-guide.md](user-guide.md); the Targets: [targets.md](targets.md).

| Game | Target | Mode Virtual | Mode Shim | Built-in profile |
|---|---|---|---|---|
| [Assetto Corsa Competizione](#assetto-corsa-competizione) | all four | works | not supported | none |
| [DiRT Rally 2.0](#dirt-rally-20) | all four | works | not supported | none |
| [NFS 2015](#nfs-2015) | G920 | works | works | pedal remap |
| [NFS Payback](#nfs-payback) | G920 | works; needs G HUB | not recommended | 180 degrees |
| [NFS Heat](#nfs-heat) | G920 | works | works | detection only (warns as a G923) |
| [NFS Unbound](#nfs-unbound) | G920 | works | works | its hidden settings: force 1.0, spring 0.8, rotation |
| [Race Driver: GRID](#race-driver-grid) | G25 | works | works (32-bit) | none |

"Works" means wheel, pedals, buttons and force feedback were checked in the game. Built-in profiles apply with Target
G920 only, by themselves while the game runs.

## Assetto Corsa Competizione

- **Set up:** pick the Target's preset in the game's controls. For the G25 and G27 use the Mapping tab's **Order:
  Native**: ACC's presets read the wheel's own button numbers.
- **Mode Shim:** not supported by the game; use Mode Virtual.
- **Linux:** runs under Proton with Mode Virtual, all four Targets (Steam Deck, Game Mode too).
- **TrueForce (Target G923, Windows):** ACC then plays its TrueForce force on your wheel. It sends no damping, as to
  a real G923.
  - The Mapping tab's **TrueForce: On** (under Target), and in `my-g923.ini` `[steering] target_rotation_deg` at the
    rotation you want (360 tested).
  - Logitech G HUB must be installed (it need not run): ACC loads Logitech's TrueForce SDK from its folder
    (`C:\Program Files\Logi\trueforce\<version>`). At Start the service log says whether it is there.
  - The Live tab shows ACC's force as Steering force and its road texture as Vibration.
  - In ACC: Steer Lock **2700**, so the car turns fully at your rotation; Gain 100% felt right.
  - Moza: keep Pit House closed while you play. With it open the wheel nudges as a track loads.
  - The road texture plays as a vibration ([G923](target-g923.md#trueforce)); not yet checked on a rig.

## DiRT Rally 2.0

- **Set up:** pick the Target's preset in the game's controls. G25 and G27: **Order: Logitech** (its G25
  handbrake "Button 7" is R Wheel).
- **Mode Shim:** not supported by the game; use Mode Virtual.
- **Target G923 with TrueForce on:** the game opens the TrueForce interface itself, so its force comes as one
  TrueForce force with a texture (Live tab: Steering force and Vibration). With TrueForce off, or as a G920, it sends
  its effects one by one: steering force and friction.

## NFS 2015

- **Set up:** nothing; keep the game's wheel controls at their defaults.
- **Profile:** the game reads a G920's pedals from other axes than a real G920 reports them; the profile swaps them.
- **Known:** changed wheel controls show `NFS 2015: RESET ITS WHEEL CONTROLS`. Force feedback is sometimes off at the
  start of a race (`GAME FFB OFF: ALT-TAB`): Alt+Tab out and back. Keyboard button prompts, no rev lights.
- **Linux:** both Modes under Proton (Steam Deck, Game Mode too).

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
- **Profile:** no values of its own; it is recognised when it starts, and the Games tab warns when the Target is the
  G923.
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
- **Linux:** both Modes under Proton (Steam Deck, Game Mode too). First run OpenAL's installer in the game's prefix:
  without it the game does not start (`OpenAL32.dll` missing).
- **Steam Deck, Game Mode:** if the wheel does nothing, switch the game's controls from the secondary controller to
  the primary, where the wheel is listed.
- **Buttons:** Instant Replay is Red 3. In a replay the wheel and pedals rewind and fast forward; pause, cameras and
  the Flashback are keys only (F1, F7/F8, F12).
- **Known:** Alt+Tab in fullscreen crashes the game during a race (its own Direct3D 9 bug). Windowed
  (`fullscreen="false"` in `Documents\Codemasters\GRID\hardwaresettings\hardware_settings_config.xml`) may avoid it.
  For 1440p, delete `system\hardware_settings_restrictions.xml` (editing it crashed the game at start).

## NFS Underground, Underground 2, Most Wanted (2005)

- **Set up:** nothing; set the wheel controls in the game's own menu.
- **Profile:** combined pedals (every Target): the games read throttle and brake from one axis, and with separate
  pedals their menus scroll by themselves. Underground and Most Wanted are both `speed.exe`: the Games tab lists them
  apart (**NFS U1**, **NFS MW**). A `speed.exe` with `dimap.dll` in its folder is Underground, any other Most Wanted.
- **Known:** Windows 7, Mode Shim, G25: separate pedals scrolled the menus in Underground and Most Wanted; combined pedals
  work in Most Wanted (Underground not yet tried).
- **Steering dead zone:** the games ignore the first part of the turn each way (Most Wanted checked: Windows 7, Mode
  Shim, G25; the Widescreen Fix's `LeftStickDeadzone = 0` and taking the Fix out changed nothing). Set
  **Anti-dead zone** for the game (Games tab) to just under where the car starts to turn.

## Widescreen Fixes (ThirteenAG)

- Mode Shim works next to them: Start puts the shim in as `dinput8Hooked.dll` beside their Ultimate ASI Loader.
- **Windows 7:** the current Fixes need Windows 8 or newer (error "GetSystemTimePreciseAsFileTime could not be
  located"). Use the July 2023 builds, the last for Windows 7: their loader is older, and Start renames it to
  `d3d9.dll` (**Remove shim** gives it its name back). Checked in NFS Carbon and NFS Most Wanted (32-bit, G25).

## Not yet tried

- **On Linux:** DiRT Rally 2.0, NFS Heat, Unbound and Payback (Payback is not expected to work: Proton has no
  Logitech DLL).
- **Windows 7, 8, 8.1 and 10** (Mode Shim only).
- **Other games** that support a Logitech wheel should work in Mode Virtual. One that reads the pedals differently
  needs a small game profile, as NFS 2015 does.

## Reporting a game

Set the Logs tab's **Level** to debug, play, and send that run's files (`wheelbri-service-...`, `telemetry-...`,
`ffb-...`, and `wheelbri-shim-...` or `wheelbri-driver-...`) with the game, Target, Mode, date and what the wheel,
pedals and buttons did.
