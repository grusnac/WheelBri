# Games

Which games WheelBri has been played with, how to set each one up, and what to know about it. WheelBri presents your
rig as a Logitech wheel. Two pickers on the Mapping tab choose which one and how:

- **Target**: G25, G27, G920 or G923 (the arrows go round in that order). Every game below was played with the
  **G920** only; the G25, G27 and G923 are built but not yet tried in any game (`docs/g25-target-plan.md`,
  `docs/g27-target-plan.md`, `docs/g923-target-plan.md`).
- **Mode**:
  - **Virtual**: a virtual wheel on Windows (WheelBri's own driver), seen by every game; your rig is hidden from games
    while a session runs. Windows 11 only, and it needs the WheelBri Helper (installed with WheelBri) or the app
    started as Administrator.
  - **Shim**: a `dinput8.dll` next to the game's program, answering the game as the target. No driver, no hiding,
    no Administrator; the only Mode on Windows 7 and 10.

Tested rig: Moza R3 base and pedals, with the add-on clutch pedal, on Windows 11.

A game that needs something changed has a game profile (`config/games/<id>.ini`, compiled into WheelBri). There are
three: NFS 2015, NFS Payback and NFS Unbound. Each was checked with the G920 and is used **only with Target G920**:
with another target the log says the profile "is for the g920 and the target is ...: not used". The app's **Game
profiles** tab (G) lists them: what each profile changes and why, your own values for it, and for NFS Unbound what its
settings file holds now. While a session runs, the tab opens by itself on a profiled game when it starts.

## How to play a game

1. **Rig tab** (R): name the controls of your wheel, pedals and other devices (Detect, then touch a control). A device
   joins the rig when one of its controls is named.
2. **Mapping tab** (M): pick **Target** (G920 for the games below) and **Mode**. Map the target's axes and buttons to
   your rig's controls and set the **steering rotation**; Save. The steering axis goes through 1:1: set your wheel's
   own software (Pit House for Moza) to the same rotation. WheelBri never reads or sets it.
3. **Mode Shim only**: the Mapping tab shows "Game <name>" and **Games (N)**. Open Games, **Add game**, choose the
   folder that holds the game's `.exe`, and click the game to pick it. Start puts the shim in that folder (or replaces
   an older copy of ours; the 32-bit one for a 32-bit game) and writes the target into its `wheelbri-shim.ini`. Start
   stays greyed out until a game is picked. A `dinput8.dll` of another mod is never touched.
4. **Start**. Start is greyed out until every rig device the mapping uses is connected (the status says what is
   missing). With Mode Virtual the first Start on a PC hides the rig, which can take up to a minute: do not unplug
   anything.
5. **Start the game**. The status next to Stop says "Running 1:23" and, for a profiled game, its name (and the
   rotation to set in the wheel's software when the profile has one); the Game profiles tab opens on it.
6. **Game profiles tab**: change the game's rotation, force feedback gain and, for NFS Unbound, its own settings (see
   below). These values are saved as you change them; the gain and rotation apply from the next Start, the game's own
   settings only when you press **Save to file** with the game closed.
7. **In the game**: pick the wheel (it shows as the target, e.g. "Logitech G920 Driving Force Racing Wheel USB") and
   keep the game's wheel controls at their defaults.
8. **Stop** when done. After a Shim session the app asks whether to delete the shim from the game's folder.

For every game, force feedback goes through the safety limiter (80% cap, slew limit), and a constant push the game
leaves unchanged for 2 s (a loading screen) fades out (`[ffb] frozen_force_s`, 0 = never); any change brings it back.

## Verified

Played on the tested rig with Target G920: wheel, pedals, buttons and force feedback checked in the game.

### NFS 2015 (`NFS16.exe`)

- **Modes:** Virtual and Shim, both played.
- **Works:** steering, throttle, brake, clutch, buttons, D-pad; force feedback (constant, spring, damper, sine,
  square).
- **Steps:** Target G920, either Mode; in the game, Settings, Controls: the wheel's controls at their defaults.
- **Caveats:**
  - **Pedal remap, automatic.** NFS 2015 reads a G920's pedals from its own table, which has them on other axes
    than a real G920 reports them: brake from Y, clutch from Rz, throttle from the Slider. Without a fix the gas
    pedal brakes. Its game profile (`config/games/nfs2015.ini`) swaps them while the game runs (Virtual: the service
    routes the pedals; Shim: the shim applies it); the status shows "NFS 2015". Real G920 owners report the same swap
    on EA's forum.
  - **Keep the game's own wheel controls at their defaults.** WheelBri reads the game's wheel bindings
    (`Documents\Need For Speed\settings\PROFILEOPTIONS_profile`). If they were changed to work around the swap above,
    the status says **NFS 2015: RESET ITS WHEEL CONTROLS** in red: in the game, Settings, Controls, wheel, reset to
    default.
  - **Force feedback off at the start of some races.** The game loads its effects and starts none; the status says
    **GAME FFB OFF: ALT-TAB**. Switching to another window and back makes it start them.
  - **Keyboard button prompts.** The game does not treat a G920 as an Xbox device, so it shows keyboard prompts. A
    virtual Xbox pad to get Xbox prompts was tried and removed: the game then drove from the pad.
  - **No rev lights.** The game's Logitech rev light calls (`Escape`) are not passed to the wheel.

### NFS Payback (`NeedForSpeedPayback.exe`)

- **Modes:** Virtual and Shim, both played (the shim on 2026-10-13: the game read the emulated G920 through it).
- **Works:** steering, throttle, brake, handbrake, nitro, buttons, D-pad, Xbox button prompts; force feedback.
- **Steps:** install Logitech G HUB and check the registry entry below (once); Target G920, either Mode; set your
  wheel's own software to **180** degrees for this game (the status shows "NFS Payback, 180 deg").
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

    WheelBri does not do this by itself (not built yet).
  - **Use G HUB's DLL, not the old Logitech Gaming Software's.** Both work, but G HUB's gives the better force
    feedback (the spring follows the game; with the old one it stayed at 1%).
  - **Slow steering is the game's.** With any wheel (G29 owners report the same) the car turns in late and little,
    while the keyboard is instant. WheelBri checked what the SDK hands the game (with its logging stand-in,
    `wheelbri-logi.dll`): the steering arrives exactly and within about 4 ms, with no steering curve, so the delay is
    in the game's own handling. The cars also understeer and have a wide turning circle at speed.
  - **180 degrees of rotation helps; set it in the wheel's software.** A small rotation makes the car respond to less
    turning: at 180 degrees full lock is 90 degrees each way. The game profile (`config/games/nfspayback.ini`) says
    180: while the game runs the status shows "NFS Payback, 180 deg" and the log asks you to set your wheel's own
    software (Pit House: Maximum Steering Angle) to 180 too. WheelBri does not change the steering itself (the axis
    goes through 1:1): the wheel's own setting is what makes it 180. Change the profile's value or turn it off in the
    Game profiles tab. On 2026-10-11, with Pit House at 360, the steering was slow.
  - **DirectX error:** an earlier try with the shim (2026-10-11) ended in a DirectX error (ResizeBuffers) at start; it
    did not come back.
  - **Harmless things in the logs:** the game asks Logitech's DLL for a 900 degree range and its own settings, and for
    rev lights; G HUB's DLL turns these down. Nothing is lost: the rotation is set in the wheel's software, and rev
    lights are not passed to the wheel in any game.

### NFS Heat (`NeedForSpeedHeat.exe`)

- **Modes:** Virtual. The shim was not tried.
- **Works:** steering, throttle, brake, buttons (A, B, X, Y, paddles, Menu, View, RSB, LSB), D-pad with diagonals;
  force feedback (spring, damper, constant, sine, square). No game profile: it reads the G920 as a real one reports
  itself. The clutch was not tried.
- **Steps:** Target G920, Mode Virtual; nothing else.
- **Caveats:**
  - **No Logitech software needed.** Unlike NFS Payback, it reads the wheel through DirectInput itself: while it ran,
    no process had Logitech's wheel SDK helper loaded (`tasklist /m logi_steering_wheel_x64.dll` listed none).
  - **Any rotation, kept the same in both places.** Played at 360 degrees (the Mapping tab's steering rotation and
    Pit House's Maximum Steering Angle both 360). The game's full lock is the wheel's end stop.
  - **Light force feedback.** The game asks for little: constant force under 10% most of the time and never over 30%,
    spring and damper around 25-50%; nothing came near the 0.8 cap. For stronger forces, raise the game's own force
    feedback setting if it has one, or the wheel's in Pit House.
  - **Force feedback can stop until you alt-tab.** Once the game stopped every effect and sent none while driving on
    for 3.5 minutes (the app's log warns: "the game loaded its force feedback ... but has started none of them"). It
    came back after switching to another window and back, as in NFS 2015.

### NFS Unbound (`NeedForSpeedUnbound.exe`)

- **Modes:** Shim; Virtual (force feedback being tuned, 2026-10-12).
- **Works:** it reads the pedals as a real G920 reports them (throttle Y, brake Rz, clutch Slider), which is why
  that is WheelBri's default for every game.
- **Steps:** Target G920, either Mode. Before the first race, with the game closed: Game profiles tab, NFS Unbound,
  Game config, **Save to file** (**Locate file** first when the file is not under Documents). Set Pit House to the
  steering rotation the tab shows.
- **Its wheel settings are hidden.** Its menus have none, but its settings file (`Documents\Need For Speed(TM)
  Unbound\settings\PROFILEOPTIONS`) has `WheelFFBMagnitude` (all of it), `WheelTireForcesMagnitude` (the steering
  force), `WheelSpringMagnitude` (the centring spring), `WheelDamperMagnitude`, `WheelSurfaceMagnitude`,
  `WheelCollisionMagnitude` (0.5 each by default), `WheelMaxRotation` (270) and `WheelDeadzone`. At the defaults its
  force feedback is very light (steering force 5% of full scale on average, the centring spring 0 most of the time;
  real G920 owners say the same). With force feedback 1.0 and spring 0.8 it centres and has weight (2026-10-12).
  The game profile (`config/games/nfsunbound.ini`) has these values: force feedback 1.0, spring 0.8, and
  `WheelMaxRotation` = the steering rotation it is played with (its rotation row in the Game profiles tab, else the
  Mapping tab's). Change them in the Game profiles tab (NFS Unbound): the rotation in its rotation row
  (`[game_rotation] nfsunbound = 360`; Off = the Mapping tab's), the others under Game config
  (`[game_settings] nfsunbound.spring = 0.8`). They are written only when you press **Save to file**, with the game
  closed (refused while it runs). The tab shows what the file holds now next to the values. The file as it was is
  kept as `PROFILEOPTIONS.before-wheelbri`. The game reads the file when it starts and writes its own values back
  when it ends, so edits made while it runs are lost.
- **Steering force made stronger.** The profile also multiplies its pushing forces by 3 (`[ffb] gain`; the 80% cap
  and the slew limit still apply; its springs and damper stay as they are). Change it in the Game profiles tab, force
  feedback gain (`[game_ffb_gain] nfsunbound = 3`, 0.25 to 4), from the next Start.
- **Closing the game leaves its forces on.** It quits without stopping its effects. With Mode Virtual the driver stops
  them 0.5 s after the game's stream of updates ends while any pushing force plays (before 2026-10-12: 10 s, or never
  for a force under 2%, and the wheel crept to full lock). Springs and dampers alone are kept: a game at rest may stop
  sending while it holds only those.

## Not yet tried

What is in place and what is not known.

### The G25, G27 and G923 targets

Built for both Modes (and for Linux), not yet tried in any game. Their button order is partly a guess (see each
target's plan). The three game profiles are for the G920 only: NFS 2015 played as another target would get no pedal
remap, NFS Payback no rotation advice and NFS Unbound no gain.

### NFS 2015 and other games on Linux / Steam Deck (Proton)

- **Ways:** the virtual wheel for Linux (`wheelbri-uhid`, read by Proton with `PROTON_ENABLE_HIDRAW=0x046D/0xC262`
  for the G920), or the shim under Proton (`WINEDLLOVERRIDES="dinput8=n,b"`).
- **Status:** built and unit-tested, not yet tried on a Steam Deck. The NFS 2015 pedal profile applies there too.
  Guide: [linux.md](linux.md).
- **Not expected:** NFS Payback on Proton. It needs Logitech's helper DLL and its registry entry inside the game's
  Wine prefix, which nothing sets up yet.

### Windows 7 and 10

Mode Shim only (the virtual device needs Windows 11). Built, not yet tried on Windows 7 (README, "Windows 7 to 10").

### Other DirectInput games that support a G920

- **Modes:** Virtual (every game sees it), or Shim for games that load `dinput8.dll`.
- **Status:** a game that reads a G920 the way a real one reports itself needs nothing (as NFS Heat and NFS Unbound);
  one that reads it differently needs a small game profile, as NFS 2015 does. Games built on Logitech's wheel SDK
  need G HUB, as NFS Payback does.

## Reporting a game

Play with the Logs tab's **Level** at debug, then send the files of that run from the logs folder
(`%LOCALAPPDATA%\wheelbri\logs`): `wheelbri-service-<date>-<NN>.log`, `telemetry-<date>-<NN>.log` and
`ffb-<date>-<NN>.log`, plus `wheelbri-shim-<date>-<NN>.log` with Mode Shim or `wheelbri-driver-<date>-<NN>.log` with
Mode Virtual. Say which Target and Mode, and what the wheel, pedals and buttons did in the game.
