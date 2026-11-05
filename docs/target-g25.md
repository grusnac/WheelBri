# Target G25

The **Logitech G25 Racing Wheel** (2006), with its six-speed H-shifter and button panel. For older games, and newer
ones whose presets still know only the G25. All Targets: [targets.md](targets.md).

| | |
|---|---|
| Ids | 046D:C299, "Logitech G25 Racing Wheel USB" |
| Buttons | 19 (shifter included), D-pad as a hat |
| Rotation | 900 degrees |
| Modes | Virtual (Windows 11, Linux) and Shim (32 and 64-bit games) |
| Extras | two button orders (Logitech, Native), centring spring |
| Force feedback | G HUB's older-wheel driver (Order Logitech, Windows), else Windows' or Linux's HID PID |
| Tested | Race Driver: GRID (both Modes), ACC and DiRT Rally 2.0 (Mode Virtual) ([games.md](games.md)) |

## When to pick it

- The game has a G25 preset and none for a newer Logitech wheel (Race Driver: GRID).
- The game knows the G27 too: the [G27](target-g27.md) gives four more buttons.

## Set up

1. Mapping tab: **Target G25**, **Mode** Virtual (or Shim for a DirectInput game such as GRID).
2. **Order**:
   - **Logitech** (default): games of the G25's era, which saw it through Logitech's driver (GRID, DiRT Rally 2.0).
   - **Native**: newer games' presets that read the wheel's own numbers (ACC).

   Each order keeps its own mapping (`my-g25.ini`, `my-g25-native.ini`). The first Native mapping starts as a copy of
   the Logitech one with its buttons marked amber (their numbers changed): check them against the game, then
   **Got it**.
3. **Centring spring** (0-100%, off by default): the G25's own spring to the centre, on top of the game's forces. For
   games that send no centring (GRID). Applies at the next Start.
4. The Rig puts A, B, X, Y on the shifter's black diamond (bottom, right, left, top), the menu buttons of that era.
   Map the red buttons, wheel buttons and gears yourself.

## Axes

Wheel X, throttle Y, brake Rz, clutch Slider 1, and on Slider 0 the **combined pedals**: resting in the middle, down
with the throttle, up with the brake (GRID's G25 preset reads these). This follows games' presets, not Logitech's
driver.

## Buttons, Order Logitech

Numbers as the game and the app count them (from 1).

| # | Button | From the Rig |
|---|---|---|
| 1-4 | Shifter's red buttons, left to right | |
| 5 | Right paddle | Right paddle |
| 6 | Left paddle | Left paddle |
| 7 | Wheel's right button | |
| 8 | Wheel's left button | |
| 9-14 | Gears 1-6 (sequential mode presses 9 and 10) | |
| 15 | Reverse | |
| 16 | Black diamond top | Y |
| 17 | Black diamond left | X |
| 18 | Black diamond bottom | A |
| 19 | Black diamond right | B |
| hat | D-pad | D-pad |

Sources: Logitech support (gears), GRID's G25 preset, GIMX's Windows G25 configuration (facts only), DiRT Rally 2.0 and
American Truck Simulator users' reports.

## Buttons, Order Native

The G25 without Logitech's driver (Linux, or Windows without it), as newer presets read it. Numbers from 0, as the app
shows them in this order.

| # | Button |
|---|---|
| 0 | Black diamond bottom |
| 1 | Black diamond left |
| 2 | Black diamond right |
| 3 | Black diamond top |
| 4, 5 | Right paddle, left paddle |
| 6, 7 | Wheel's right button, left button |
| 8-11 | Red 2, 3, 4, 1 |
| 12-17 | Gears 1-6 |
| 18 | Reverse |

ACC binds the native diamond as A, X, B, Y, as its G920 preset binds the G920's A, B, X, Y.

## Force feedback

| Order | G HUB's older-wheel driver installed (Windows) | Force feedback through |
|---|---|---|
| Logitech | yes | Logitech's driver, as a G25 with Logitech's software had |
| Logitech | no | Windows' HID PID driver |
| Native | either | Windows' HID PID driver (Linux's on Linux) |

With Logitech's driver the wheel takes Logitech's classic commands: constant and variable forces, springs, dampers,
friction. Periodic forces and ramps are logged, not played (no tested game uses them). The driver updates the force
about 110 times a second in 256 steps, so road texture is coarser than on the G920, as on a real G25. G HUB does not
support the G25, so WheelBri writes the driver's registry entry while the wheel runs and removes it at Stop (and the
uninstaller removes one a crash left).

## Shadow view

The real G25 with its shifter and button panel; the knob sits in the gear the game sees. Button numbers follow the
Order.

## Known

- GRID above about 144 frames a second sends a broken force: WheelBri plays it as none
  (`[ffb] ignore_lone_full_push`). Cap the frame rate.
- Rotation and range commands from games are ignored: the rotation is the Mapping tab's.
