# The Logitech G25 as a target

Status: built (2026-10-13) for the shim, the Windows virtual device and Linux `wheelbri-uhid`, unit-tested; **not yet
tried in a game**, nor under Proton. Pick **Target G25** on the Mapping tab (the arrows go round G25, G27, G920, G923);
Mode Virtual or Shim works as for the G920. Open: the button order other than the gears and the axis letters (a
guess), a first game, Linux. Details under "Unverified".

## Why

Some games know the G25 and not the G920: older titles (rFactor, GTR 2, LFS, Richard Burns Rally, the NFS games before
2015, NFS Undercover among them) have G25 presets, button names and force feedback tuned for it, and some only offer a
wheel at all when they see 046D:C299. A G25 target lets the rig be that wheel, the same way it is a G920.

## What a G25 is (from the research of 2026-10-13)

- One USB device: the pedals and the shifter plug into the wheel base, so everything reports through it.
- Two USB identities: 046D:C294 (Driving Force compatibility, what it starts as: 12 buttons, combined pedals, no
  clutch, only gears 1-2) and 046D:C299 (native G25, after Logitech's driver switches it).
- Native mode: 4 axes (wheel 14-bit, accelerator, brake, clutch, separate), 1 hat (the shifter's D-pad), 19 buttons:
  2 on the wheel, 2 paddles, 8 on the shifter (4 red, 4 black), gears 1-6 and R. Logitech's own numbering (support
  article) puts the gears at DirectInput buttons 8-14 (0-based). The order of the other 12 is not documented.
- Rotation 40-900 deg. No LEDs (those came with the G27).
- Force feedback: Logitech's classic protocol (7-byte output reports), not HID PID. On Windows games reach it through
  DirectInput and Logitech's driver; on Linux `hid-lg4ff` drives it.

## How WheelBri presents it

One target among two when it was built (four now: G25, G27, G920, G923), with the same machinery as the G920 (a
target is a profile, its configs and an identity):

- **Profile**: `g25Spec()` (`core/src/DeviceProfile.cpp`, `makeProfile("g25")`, `target_profile = g25`): 046D:C299,
  "Logitech G25 Racing Wheel USB", 900 deg, 19 buttons (`kG25Buttons`, gears at `kG25FirstGear` = 8), pedals released
  at the top of the range. Its DirectInput layout is the G920's: wheel X, throttle Y, brake Rz, clutch Slider.
- **Config**: `config/g25.ini`, both modes (WheelBri's default mapping onto the G25); the user's own as
  `my-g25.ini`, as for the G920.
- **Mapping tab**: Target `< G920 | G25 >` when built; now `G25, G27, G920, G923` in a ring (`ConfigPicker`,
  `[app] target` in app.ini).
- **Shim**: the shim already presents whatever `target_profile` says; at Start the app writes the target into the
  game's `wheelbri-shim.ini` (`setShimTarget`), keeping the user's other lines.
- **Virtual device (Windows)**: the same driver (`wheelbri-g920`, its INF binds `wheelbri\G920` and `wheelbri\G25`) and
  the same HID descriptor and input report (four 16-bit axes, 19 buttons, hat, HID PID force feedback through
  Windows' `pid.dll`); `VirtualWheel` (`core/include/wheelbri/HidG920.hpp`) holds each one's identity: hardware id,
  instance names, ids, product string, DirectInput name. The service creates the software device with the G25's
  hardware id; the driver reads it and answers as 046D:C299. The WheelBri Helper takes the wheel's name in
  `CreateVirtualG920` ("g25"; none = the G920, as an older app asks). A driver installed before the G25 has no entry
  for `wheelbri\G25`: Start says to install the new one.
- **Linux (`wheelbri-uhid --target g25`)**: the Windows descriptor as it is (Proton's DirectInput has a hack for the
  G920's own layout, none for the G25), on the virtual bus (`BUS_VIRTUAL`): the kernel's `hid-lg` claims USB
  046D:C299 and refuses any device that is not really on USB, which would leave the device to no driver at all; on the
  virtual bus `hid-generic` takes it. Proton games: `PROTON_ENABLE_HIDRAW=0x046D/0xC299` (`scripts/linux/wheelbri.sh`
  sets it from the target); force feedback is HID PID, as for the G920.
- **Force feedback** stays one model: game effects arrive as HID PID (driver, uhid) or DirectInput parameters (shim)
  and become `FfbEffect`s through `FfbSafetyLimiter`.

### Changed from the plan

- No `HidTarget` / driver rename: the G25 uses the G920's descriptor and report, so only the identity differs.
- No classic Logitech force feedback decoder (the plan's step 5): it was for `hid-lg4ff` on Linux, which never binds
  the virtual-bus device. Windows games use DirectInput (HID PID through `pid.dll`); a game that wrote classic
  commands to a real G25 through raw HID would get no force feedback (none known).

## Unverified (check before relying on it)

1. **Button order** (other than the gears) and **axis letters**: a guess, the same layout as the G920's. A game's
   built-in G25 preset may bind the wrong controls. Check with a real G25 (joy.cpl's button numbers, or a
   `usbhid-dump` of its descriptor) and fix `g25Spec()` and the G25 configs.
2. **Games**: none tried yet. NFS Undercover and NFS 2015 are the first candidates (shim first, then the virtual
   device). NFS 2015's game profile (`config/games/nfs2015.ini`) is for the G920 only; if NFS 2015 swaps the G25's
   pedals too, the telemetry will show it and a G25 profile can follow.
3. **Linux**: the virtual-bus G25 under Proton is untried (as the uhid G920 is on a Steam Deck).
4. **Sequential mode**: how the G25 reports its stick in sequential mode is not documented; the paddles cover it.

## How to test

- Unit: `DeviceProfileTest` (G25 spec), `EmulatorConfigTest` (`target_profile = g25`), `VirtualWheelTest`,
  `ShimGamesTest` (target in the shim's settings), `HelperProtocolTest` (`CreateVirtualG920` with a wheel),
  `UhidG920Test` / `UhidLoopTest` (the G25 on uhid), `MappingEditTest` (the G25 configs use each control once).
- Windows: joy.cpl shows "Logitech G25 Racing Wheel USB", 4 axes, 19 buttons and the hat moving with the rig; the FFB
  test in joy.cpl plays through the limiter; one G25 game end to end with telemetry and `ffb-<date>-<NN>.log`.
- Linux: `evtest` on the uhid G25 (046D:C299 on bus 0006); a Proton game with `PROTON_ENABLE_HIDRAW=0x046D/0xC299`.

## Risks

- Games that detect the G25 through Logitech's software (LGS/G HUB, the Steering Wheel SDK) will not see a virtual one,
  as with the G920.
- Two virtual wheels at once are not possible: one target per session.
- FFB feel: a game's G25-tuned forces were made for a weak geared wheel; the limiter's defaults stay as they are, and a
  game profile's gain can raise them (today's three profiles are for the G920 only: `profileFitsTarget`).
