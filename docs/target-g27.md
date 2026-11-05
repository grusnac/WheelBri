# Target G27

The **Logitech G27 Racing Wheel** (2009): the [G25](target-g25.md) with four more buttons on the wheel. For games of
its era that list the G27, and newer presets that still use it. All Targets: [targets.md](targets.md).

| | |
|---|---|
| Ids | 046D:C29B, "Logitech G27 Racing Wheel USB" |
| Buttons | 23 (shifter included), D-pad as a hat |
| Rotation | 900 degrees |
| Modes | Virtual (Windows 11, Linux) and Shim |
| Extras | two button orders (Logitech, Native), centring spring |
| Force feedback | G HUB's older-wheel driver (Order Logitech, Windows), else Windows' or Linux's HID PID |
| Tested | ACC, DiRT Rally 2.0 (Mode Virtual) ([games.md](games.md)) |

## When to pick it

- The game lists the G27 but not the G920 or G923.
- The game knows only the G25: use the [G25](target-g25.md).

## Set up

As the [G25](target-g25.md#set-up): **Target G27**, a **Mode**, an **Order** (Logitech for games of its era, Native
for newer presets such as ACC's; each with its own mapping, `my-g27.ini` and `my-g27-native.ini`), and the
**Centring spring** for games that send no centring. The Rig puts A, B, X, Y on the black diamond.

## Axes

As the G25: wheel X, throttle Y, brake Rz, clutch Slider 1, combined pedals on Slider 0.

## Buttons, Order Logitech

Numbers as the game and the app count them (from 1).

| # | Button | From the Rig |
|---|---|---|
| 1-4 | Shifter's red buttons, left to right | |
| 5 | Right paddle | Right paddle |
| 6 | Left paddle | Left paddle |
| 7 | Wheel's right button 1 (top) | |
| 8 | Wheel's left button 1 (top) | |
| 9-14 | Gears 1-6 | |
| 15 | Reverse | |
| 16 | Black diamond top | Y |
| 17 | Black diamond left | X |
| 18 | Black diamond bottom | A |
| 19 | Black diamond right | B |
| 20 | Wheel's right button 2 | |
| 21 | Wheel's left button 2 | |
| 22 | Wheel's right button 3 | |
| 23 | Wheel's left button 3 | |
| hat | D-pad | D-pad |

1-19 are the G25's. 20-23, the wheel's lower buttons, come from GIMX (facts only) and users' reports; which of each
side's lower buttons is which is not confirmed.

## Buttons, Order Native

Numbers from 0, as the app shows them in this order.

| # | Button |
|---|---|
| 0-3 | Black diamond bottom, left, right, top |
| 4, 5 | Right paddle, left paddle |
| 6, 7 | Wheel's right button 1, left button 1 |
| 8-11 | Red 2, 3, 4, 1 |
| 12-17 | Gears 1-6 |
| 18, 19 | Wheel's right buttons 2 and 3 |
| 20, 21 | Wheel's left buttons 2 and 3 |
| 22 | Reverse |

## Force feedback

As the [G25](target-g25.md#force-feedback): with Order Logitech and G HUB's older-wheel driver installed, Logitech's
classic driver (checked in ACC); otherwise Windows' or Linux's HID PID driver.

## Shadow view

The real G27 with its shifter and button panel; the knob sits in the gear the game sees.

## Known

- The G27's rev lights are not emulated (games drive them through Logitech's SDK).
