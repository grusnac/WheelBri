# Games

Which games WheelBri has been played with, and what to know about each. WheelBri presents your rig as a
**Logitech G920**, in one of two ways (the Mapping tab's target):

- **G920 virtual device**: a virtual G920 on Windows (WheelBri's own driver), seen by every game;
- **G920 in-game shim**: a `dinput8.dll` next to the game's program, answering the game as a G920.

The lists below say which way was used. Tested rig: Moza R3 base and pedals, with the add-on clutch pedal, on
Windows 11.

## Verified

Played on the tested rig: wheel, pedals, buttons and force feedback checked in the game.

### NFS 2015 (`NFS16.exe`)

- **Ways:** G920 virtual device and G920 in-game shim, both played.
- **Works:** steering, throttle, brake, clutch, buttons, D-pad; force feedback (constant, spring, damper, sine,
  square) through the safety limiter.
- **Caveats:**
  - **Pedal remap, automatic.** NFS 2015 reads a G920's pedals from its own table, which has them on other axes
    than a real G920 reports them: brake from Y, clutch from Rz, throttle from the Slider. Without a fix the gas
    pedal brakes. WheelBri's built-in game profile (`config/games/nfs2015.ini`) swaps them while the game runs; the
    status next to Start shows "NFS 2015". Real G920 owners report the same swap on EA's forum.
  - **Keep the game's own wheel controls at their defaults.** If they were changed to work around the swap above,
    the status says **NFS 2015: RESET ITS WHEEL CONTROLS**: in the game, Settings, Controls, wheel, reset to
    default.
  - **Keyboard button prompts.** The game does not treat a G920 as an Xbox device, so it shows keyboard prompts. A
    virtual Xbox pad to get Xbox prompts was tried and removed: the game then drove from the pad.
  - **No rev lights.** The game's Logitech rev light calls are not passed to the wheel.

### NFS Payback (`NeedForSpeedPayback.exe`)

- **Ways:** G920 virtual device only. With the in-game shim the game showed a DirectX error (ResizeBuffers) and did
  not start.
- **Works:** steering, throttle, brake, handbrake, nitro, buttons, D-pad, Xbox button prompts; force feedback
  through the safety limiter.
- **Caveats:**
  - **Needs Logitech G HUB installed.** Payback does not read wheels itself: it asks Logitech's wheel SDK, built
    into the game, which loads Logitech's helper DLL from the path in the registry
    (`HKCR\CLSID\{63BD165D-1584-4E75-AB56-08330350545F}\ServerBinary`, default value). G HUB installs that DLL
    (`C:\Program Files\Logi\wheel_sdk\<version>\logi_steering_wheel_x64.dll`). WheelBri never ships Logitech's DLLs.
  - **The registry entry may point at a missing DLL.** On the tested PC it still named the old, removed Logitech
    Gaming Software, so the game ignored every wheel. Point it at G HUB's DLL (Administrator PowerShell, game
    closed; use the version folder you have):

    ```powershell
    reg add "HKLM\SOFTWARE\Classes\CLSID\{63BD165D-1584-4E75-AB56-08330350545F}\ServerBinary" /ve /t REG_SZ /d "C:\Program Files\Logi\wheel_sdk\9_1_1\logi_steering_wheel_x64.dll" /f
    ```

    WheelBri doing this by itself is planned.
  - **Use G HUB's DLL, not the old Logitech Gaming Software's.** Both work, but G HUB's gives the better force
    feedback (the spring follows the game; with the old one it stayed at 1%).
  - **Slow steering is the game's.** With any wheel (G29 owners report the same) the car turns in late and little,
    while the keyboard is instant. WheelBri checked what the SDK hands the game: the steering arrives exactly and
    within about 4 ms, with no steering curve, so the delay is in the game's own handling. The cars also understeer
    and have a wide turning circle at speed.
  - **180° of rotation, set automatically; set Pit House to match.** A small rotation makes the car respond to less
    turning: at 180° full lock is 90° each way. WheelBri's game profile (`config/games/nfspayback.ini`) switches the
    steering to 180/180 while the game runs (the status shows "NFS Payback, 180 deg") and back afterwards; change it
    or turn it off in the Mapping tab (PER GAME). Set your wheel's own software (Pit House: Maximum Steering Angle)
    to **180** for this game too: WheelBri cannot set it. With Pit House at 360 the steering is 1:1 and slow (full
    lock at the wheel's end stop); with the wheel wider than the game's full lock (360 wheel, 180 game) it turns on
    past full lock with the car not steering and the force feedback slack, as seen on 2026-10-11.
  - **Harmless things in the logs:** the game asks Logitech's DLL for a 900° range and its own settings, and for rev
    lights; G HUB's DLL turns these down. Nothing is lost: the rotation is WheelBri's, and rev lights are not passed
    to the wheel in any game.

### NFS Heat (`NeedForSpeedHeat.exe`)

- **Ways:** G920 virtual device. The in-game shim was not tried.
- **Works:** steering, throttle, brake, buttons (A, B, X, Y, paddles, Menu, View, RSB, LSB), D-pad with diagonals;
  force feedback (spring, damper, constant, sine, square) through the safety limiter. No game profile: it reads the
  G920 as a real one reports itself. The clutch was not tried.
- **Caveats:**
  - **No Logitech software needed.** Unlike NFS Payback, it reads the wheel through DirectInput itself: while it ran,
    no process had Logitech's wheel SDK helper loaded (`tasklist /m logi_steering_wheel_x64.dll` listed none).
  - **Any rotation, kept the same in both places.** Played at 360 degrees (Mapping tab: game full lock and wheel
    rotation 360; Pit House: Maximum Steering Angle 360). The game's full lock is the wheel's end stop.
  - **Light force feedback.** The game asks for little: constant force under 10% most of the time and never over 30%,
    spring and damper around 25-50%; nothing came near the 0.8 cap. For stronger forces, raise the game's own force
    feedback setting if it has one, or the wheel's in Pit House.
  - **Force feedback can stop until you alt-tab.** Once the game stopped every effect and sent none while driving on
    for 3.5 minutes (the app's log warns: "the game loaded its force feedback ... but has started none of them"). It
    came back after switching to another window and back, as in NFS 2015.

### NFS Unbound (`NeedForSpeedUnbound.exe`)

- **Ways:** G920 in-game shim; G920 virtual device (force feedback being tuned, 2026-10-12).
- **Works:** it reads the pedals as a real G920 reports them (throttle Y, brake Rz, clutch Slider), which is why
  that is WheelBri's default for every game.
- **Its wheel settings are hidden.** Its menus have none, but its settings file (`Documents\Need For Speed(TM)
  Unbound\settings\PROFILEOPTIONS`) has `WheelFFBMagnitude` (all of it), `WheelTireForcesMagnitude` (the steering
  force), `WheelSpringMagnitude` (the centring spring), `WheelDamperMagnitude`, `WheelSurfaceMagnitude`,
  `WheelCollisionMagnitude` (0.5 each by default), `WheelMaxRotation` (270) and `WheelDeadzone`. At the defaults its
  force feedback is very light (steering force 5% of full scale on average, the centring spring 0 most of the time;
  real G920 owners say the same). With force feedback 1.0 and spring 0.8 it centres and has weight (2026-10-12).
  WheelBri's game profile (`config/games/nfsunbound.ini`) sets these while the game is closed: force feedback 1.0,
  spring 0.8, and `WheelMaxRotation` = the steering rotation it is played with. Change them in the Mapping tab, PER
  GAME: the rotation in Unbound's rotation row as for NFS Payback (`[game_rotation] nfsunbound = 360`; Off = the
  mapping's own), the others below it (`[game_settings] nfsunbound.spring = 0.8`); they are written at the next
  Start, or when the game closes. Set the wheel's own software (Pit House) to the same rotation. The
  file as it was is kept as `PROFILEOPTIONS.before-wheelbri`. The game reads the file when it starts and writes its
  own values back when it ends, so edits made while it runs are lost.
- **Steering force made stronger.** The profile also multiplies its pushing forces by 3 (`[ffb] gain`; the 80% cap
  and the slew limit still apply; its springs and damper stay as they are). Change it in the Mapping tab, PER GAME,
  ffb gain (`[game_ffb_gain] nfsunbound = 3`, 0.25 to 4), from the next Start.
- **Closing the game leaves its forces on.** It quits without stopping its effects; the virtual G920's driver now
  stops them 0.5 s after the game's stream of updates ends (before: 10 s, or never for a force under 2%, and the
  wheel crept to full lock).

## Expected to work

Not played yet; what is in place and what is not known.

### NFS 2015 and other games on Linux / Steam Deck (Proton)

- **Ways:** the virtual G920 for Linux (`wheelbri-uhid`, read by Proton with
  `PROTON_ENABLE_HIDRAW=0x046D/0xC262`), or the in-game shim under Proton (`WINEDLLOVERRIDES="dinput8=n,b"`).
- **Status:** built and unit-tested, not yet tried on a Steam Deck. The NFS 2015 pedal profile applies there too.
  Guide: [linux.md](linux.md).
- **Not expected:** NFS Payback on Proton. It needs Logitech's helper DLL and its registry entry inside the game's
  Wine prefix, which nothing sets up yet.

### Other DirectInput games that support a G920

- **Ways:** G920 virtual device (every game sees it), or the in-game shim for games that load `dinput8.dll`.
- **Status:** a game that reads a G920 the way a real one reports itself needs nothing (as NFS Heat and NFS Unbound);
  one that reads it differently needs a small game profile, as NFS 2015 does. Games built on Logitech's wheel SDK
  need G HUB, as NFS Payback does.

## Reporting a game

Play with **Logs** at debug (Logs tab, Level), then send the day's files from the logs folder
(`%LOCALAPPDATA%\wheelbri\logs`): `wheelbri-service-<date>-<NN>.log`, `telemetry-<date>-<NN>.log` and
`ffb-<date>-<NN>.log`, plus the shim's log if the shim was used. Say what the wheel, pedals and buttons did in the
game.
