# Target G920

The **Logitech G920 Driving Force** (Xbox One and PC). WheelBri's default Target: most modern games with wheel support
have a preset for it. All Targets: [targets.md](targets.md).

| | |
|---|---|
| Ids | 046D:C262, "Logitech G920 Driving Force Racing Wheel USB" |
| Buttons | 19 (18 used), D-pad as a hat |
| Rotation | 900 degrees |
| Modes | Virtual (Windows 11, Linux) and Shim |
| Force feedback | G HUB's driver over HID++ when G HUB is installed, else Windows' HID PID (Windows); HID PID (Linux) |
| Tested | ACC, DiRT Rally 2.0, NFS 2015, Payback, Heat, Unbound ([games.md](games.md)) |

## When to pick it

- The game lists the G920 (or "Logitech G29/G920") in its controls.
- The NFS games: their built-in profiles work with the G920 only.
- Pick the [G923](target-g923.md) instead only for TrueForce, or when a game has a G923 preset and no G920 one.

## Set up

1. Mapping tab: **Target G920**, **Mode** Virtual (or Shim for an older DirectInput game).
2. Check the Rig mapped A, B, X, Y, the paddles and the D-pad; map Menu, View, the stick buttons, Xbox and the
   shifter gears yourself if the game uses them.
3. **Steering rotation**: the game's full lock; the same in your wheel's software.

## Axes

Wheel X, throttle Y, brake Rz, clutch Slider; 16 bits each.

## Buttons

Numbers as the game and the app count them (from 1). `target_N` in the INI is the number minus 1.

| # | Button | From the Rig |
|---|---|---|
| 1-4 | A, B, X, Y | A, B, X, Y |
| 5 | Right paddle | Right paddle |
| 6 | Left paddle | Left paddle |
| 7 | Menu | |
| 8 | View | |
| 9 | RSB (right stick button) | |
| 10 | LSB (left stick button) | |
| 11 | Xbox | |
| 12 | (unused: the real wheel has it, nothing presses it) | |
| 13-18 | Driving Force Shifter: gears 1-6 | |
| 19 | Driving Force Shifter: reverse | |
| hat | D-pad | D-pad |

The order is the G920's in PC mode (`joy.cpl` 1-11), then Logitech's Driving Force Shifter (its FAQ: 13-19).

## Force feedback

- **Windows, G HUB installed** (it need not run, only have seen the wheel once): games' DirectInput effects go through
  G HUB's force feedback driver, which sends them over HID++ (feature 0x8123); WheelBri plays them on your rig.
  WheelBri removes Windows' own HID PID entry for the wheel from HKCU while G HUB's is there, so DirectInput finds
  Logitech's. In G HUB the wheel shows as *Inactive*: that is expected and does not affect force feedback.
- **Windows, no G HUB**: Windows' HID PID driver.
- **Linux**: the kernel's HID PID driver.

## Shadow view

The real G920 drawn from Logitech's pictures, with the Driving Force Shifter's gate. Buttons carry their printed labels
(A B X Y, LSB / RSB, View, Menu, Xbox); the paddles carry none.

## Known

- G HUB sees the virtual G920 but does not configure it (rotation, sensitivity): set those in your wheel's software.
- Built-in game profiles (NFS 2015, Payback, Unbound) apply with this Target only.
