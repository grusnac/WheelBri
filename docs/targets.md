# Targets

The Logitech wheels WheelBri can present, how each one looks to a game, and where that comes from. Using them:
[user-guide.md](user-guide.md). All four pass in Mode Virtual in Assetto Corsa Competizione and DiRT Rally 2.0
([games.md](games.md)); no button order has been checked against a real Logitech wheel.

| Target | Ids | Buttons | Button order from |
|---|---|---|---|
| G25 | 046D:C299 | 19 | several sources (below); Native order too |
| G27 | 046D:C29B | 23 | the G25's, plus the wheel's lower buttons from GIMX; Native order too |
| G920 | 046D:C262 | 19 | the real G920 |
| G923 | 046D:C26E | 24 | 0-18 as the G920, 19-23 (Plus, Minus, the dial) a guess (a real one has 23); unsigned axes, HID++ for G HUB |

All use 900 degrees. Orders below count from 0, as the code and the INI's `target_N` do.

## Axes

| Target | Wheel | Throttle | Brake | Clutch | Slider 0 |
|---|---|---|---|---|---|
| G920, G923 | X | Y | Rz | Slider | — |
| G25, G27 | X | Y | Rz | Slider 1 | combined pedals |

The same in both Modes and on both systems. The G25 and G27 follow what games' presets read (Assetto Corsa
Competizione's G25 and G27 presets: clutch axis 7, Slider 1), not Logitech's driver (`WmJoyHid.inf` in Logitech
Gaming Software 5.10: X wheel, Y combined, Z throttle, Rz brake, Slider clutch). The combined pedals rest in the
middle, go down with the throttle and up with the brake, as Race Driver: GRID's G25 preset needs.

## Button orders (G25, G27)

The Mapping tab's **Order** picks one; each order keeps its own mapping (`my-g25.ini`, `my-g25-native.ini`; the
choice is in `app.ini` and in the file's `[buttons] order` for the command-line service).

**Logitech** (default): as games of that era saw a G25 on Windows with Logitech's driver.

| Buttons | What |
|---|---|
| 0-3 | shifter's red buttons, left to right |
| 4, 5 | right paddle, left paddle |
| 6, 7 | wheel's right and left buttons |
| 8-14 | gears 1-6, reverse (sequential mode presses 8 and 9) |
| 15-18 | shifter's black diamond: top, left, bottom, right |
| 19-22 | G27 only: the wheel's lower buttons |
| hat | D-pad |

The Rig's A, B, X, Y land on the black diamond (bottom, right, left, top): the menu buttons of that era's games.

Sources: Logitech support ("Programming the G25 Shifter positions": gears 8-14), Race Driver: GRID's G25 preset
(paddles, gears, Select 17, Back 18), GIMX's Windows G25 configuration (red 0-3, wheel 6/7, black 15-18), DiRT Rally
2.0 and American Truck Simulator users' G27 reports, a G25 read through Unity (gears, sequential).

**Native**: a G25 or G27 without Logitech's driver (Linux, or Windows without it), as newer games' presets read it
(ACC's bind 0-11 and nothing at the gears).

| Buttons | What |
|---|---|
| 0-3 | black diamond: bottom, left, right, top |
| 4, 5 | paddles |
| 6, 7 | wheel buttons |
| 8-11 | red 2, 3, 4, 1 |
| 12-17 | gears 1-6 |
| 18 / 22 | reverse: G25 / G27 (whose wheel's lower buttons are 18-21) |

Sources: GIMX's G25 and G27 configurations for Windows and Linux side by side (facts only, nothing copied) and a G27's
HID descriptor. ACC binds the native diamond as A, X, B, Y, as its G920 preset binds the G920's A, B, X, Y.

## Button numbers in the app

The app shows the Target's buttons as the game numbers them (`gameButtonNumber`): from 1 with Logitech, as
Logitech's driver, `joy.cpl` and games count (DiRT Rally 2.0's G25 handbrake "Button 7" is 6 above, R Wheel); from 0
with Native. The rig's buttons and `target_N` count from 0.

To change an order: `g25Spec()`, `g27Spec()`, `g923Spec()` in `core/src/DeviceProfile.cpp` and the matching
`config/*.ini`.

## G HUB and HID++ (G923)

With Target G923 in Mode Virtual the virtual wheel answers G HUB's HID++ 2.0 (`core/include/wheelbri/HidPp.hpp`,
written from the public description): Root, FeatureSet, firmware version, name and ForceFeedback (0x8123). G HUB shows
the wheel as active. Every answer comes in the very long report `0x12`, as a real G923 answers even its long (`0x11`)
requests. Rotation it sets is logged, **not applied**. Effects sent over 0x8123 (by G HUB or Logitech's force
feedback driver) are played on the rig, scaled by the gain G HUB sets (`HidPpFfb.hpp`).

The pedals are in games' layout (throttle Y, brake Rz, clutch Slider, 16 bits), not a real G923's (throttle Y, brake Z,
clutch Rz, 8 bits, released at the top), so G HUB shows them in the wrong places. With a real G923's layout
DirectInput relabelled them for games (in ACC gas became the clutch), and the registry entries meant to undo that did
not.

## TrueForce (G923)

`[emulator] trueforce = true` (off by default; `trueforce_probe` until 2026-10-28) gives the virtual G923 its
TrueForce interface: a second device in the wheel's container, usage page `0xFFFD`, report `0x01`
(`core/include/wheelbri/TrueForce.hpp`). It answers the game's set-up as a real wheel and reports the steering
(state on 127.0.0.1:41331) over `target_rotation_deg`, the wheel's own rotation, whatever range the game sets. Its
parameters, count range (904.5 degrees, 268101.8 counts a turn) and full scale are a real G923's, measured with USB
captures (`tools/g923-capture`); it answers the operating range (`0x0e`) with a plain status, as a real G923. All 64
parameters are the G923's, `0x36` too (NaN). Driver log lines start `trueforce:` (each packet and the 10 s summaries at
log level debug).

The force of a game in TrueForce mode (ACC: `logiTrueForceSetTorqueKF`) comes on that interface: Logitech's SDK
streams it as type `0x01` packets (1000 a second to a real G923), the motor torque target the mean of the words at
bytes 6..7 and 8..9. It needs the full-scale torque WheelBri reports in its status (0.65 Nm, a G923's): with 0 there
the SDK streams none. The driver plays it as a constant force in the service's slot 7
(`core/include/wheelbri/TrueForceFfb.hpp`), through the safety limiter and the game profile's gain like any other
effect. The damping the game sets (`0x0c`, `logiTrueForceSetDamping`) plays alongside as a damper in slot 6: without
it a direct-drive wheel swings from lock to lock. To a real G923 the SDK sends no `0x0c` at all.

The texture (the packets' sample window: 4 new samples a millisecond, which a real G923 adds to its motor torque)
plays as a vibration in slot 5: a sine whose size and frequency follow each 40 ms of samples (in ACC about 2% of full
scale at about 240 Hz). The rig takes a new force at most every 4 ms, too slow for the samples themselves, so its own
firmware makes the vibration. Not yet felt on a rig.

The SDK the game loads is G HUB's (`C:\Program Files\Logi\trueforce\<version>\trueforce_sdk_x64.dll`): without G
HUB installed no game can use TrueForce; it need not run. At Start the service log says whether it found the SDK.

DirectInput effects on the virtual G923 go to Windows' HID PID driver, `trueforce` on or off, so a game that drives
the wheel through DirectInput alone gets its effects one by one, as on a G920. WheelBri removes the `OEMForceFeedback`
entry Logitech's software writes for a G923 (checked every 2 s; G HUB writes it back for a real G923 plugged in too),
which named Logitech's driver (`hidpp_forcefeedback_x64.dll` -> HID++ 0x8123). Neither game tested needs that driver:

- ACC: its force and texture come through the SDK, and it sets the range on the TrueForce interface (full lock both
  ways, 2026-11-03).
- DiRT Rally 2.0 has no TrueForce of its own, but on a G923 with the TrueForce interface it opens that interface
  itself and sends nothing through DirectInput (2026-11-03): its effects come mixed into the one TrueForce force,
  with a texture up to the 25% cap. With `trueforce` off it sends its effects one by one (steering force and
  friction).

To see what a game asks Logitech's SDK (ACC in TrueForce mode reads the wheel through `trueforce_sdk_x64.dll`), put
the 64-bit shim in its folder with `emulate = false` and `logi_log = true` in `wheelbri-shim.ini`: every SDK call, its
arguments and results go to `wheelbri-shim-*.log` (`shim/src/LogiHook.hpp`).
