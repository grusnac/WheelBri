# Targets

The **Target** is the Logitech wheel games see. Pick it on the Mapping tab (fixed while running); each Target keeps a
mapping of its own (`my-<target>.ini`). One page per Target:

| Target | Page | Pick it for | Ids | Buttons |
|---|---|---|---|---|
| **G920** (default) | [target-g920.md](target-g920.md) | modern games; the safest choice | 046D:C262 | 19 |
| **G923** | [target-g923.md](target-g923.md) | modern games, TrueForce (ACC, DiRT Rally 2.0) | 046D:C26E | 24 |
| **G25** | [target-g25.md](target-g25.md) | games of 2006-2010 whose presets know only the G25 (Race Driver: GRID) | 046D:C299 | 19 |
| **G27** | [target-g27.md](target-g27.md) | games of 2009-2015 that know the G27 but not the G920 | 046D:C29B | 23 |

All four work in Mode Virtual in Assetto Corsa Competizione and DiRT Rally 2.0 ([games.md](games.md)). No button order
has been checked against a real Logitech wheel.

## Which one?

1. The game has a G920 preset: **G920**. It has only a G923 preset, or you want TrueForce in ACC or DiRT Rally 2.0:
   **G923**.
2. The game is older than the G920 (2015) and lists the G27: **G27**; lists only the G25: **G25**.
3. Not sure: start with G920, then try the newest Logitech wheel the game's controller list names.

## What all four share

- **900 degrees** full lock. The Mapping tab's **Steering rotation** sets what the game sees; set your wheel's own
  software to the same number.
- **The same axes in both Modes and on both systems:**

  | Target | Wheel | Throttle | Brake | Clutch | Slider 0 |
  |---|---|---|---|---|---|
  | G920, G923 | X | Y | Rz | Slider | — |
  | G25, G27 | X | Y | Rz | Slider 1 | combined pedals |

- **The Rig maps the core controls by itself** on every Target: Steering, the pedals, the D-pad, A, B, X, Y and the
  paddles. Every other button you map on the Mapping tab.
- **Button numbers in the app** are the ones the game uses: from 1 (as `joy.cpl` and games count), from 0 in the
  G25's and G27's **Native** order. The INI's `target_N` always counts from 0.
- **Force feedback** through WheelBri's safety limits ([user-guide.md](user-guide.md#force-feedback)). On Windows, a
  Logitech driver plays it when G HUB has installed one, else Windows' HID PID driver:

  | Target | With G HUB installed | Without |
  |---|---|---|
  | G920, G923 | G HUB's force feedback driver (HID++) | Windows' HID PID driver |
  | G25, G27, Order Logitech | G HUB's older-wheel driver (classic Logitech commands) | Windows' HID PID driver |
  | G25, G27, Order Native | Windows' HID PID driver | Windows' HID PID driver |

  Linux: the kernel's HID PID driver, for every Target.

## Changing a Target's layout

Only in code: `g25Spec()`, `g27Spec()`, `g920Spec()`, `g923Spec()` in `core/src/DeviceProfile.cpp` and the matching
`config/*.ini`. Users cannot add Targets.
