# Targets

The **Target** is the wheel games see (a Logitech or a Thrustmaster one). Pick it on the Mapping tab (fixed while running); each Target keeps a
mapping of its own (`my-<target>.ini`). One page per Target:

| Target | Page | Pick it for | Ids | Buttons |
|---|---|---|---|---|
| **G920** (default) | [target-g920.md](target-g920.md) | modern games; the safest choice | 046D:C262 | 19 |
| **G923** | [target-g923.md](target-g923.md) | modern games, TrueForce (ACC, DiRT Rally 2.0) | 046D:C26E | 24 |
| **G25** | [target-g25.md](target-g25.md) | games of 2006-2010 whose presets know only the G25 (Race Driver: GRID) | 046D:C299 | 19 |
| **G27** | [target-g27.md](target-g27.md) | games of 2009-2015 that know the G27 but not the G920 | 046D:C29B | 23 |
| **RGT** | [target-rgt.md](target-rgt.md) | games with a preset for Thrustmaster's RGT Force Feedback Pro; not yet tested in a game | 044F:B653 | 10 |

The four Logitech Targets work in Mode Virtual in Assetto Corsa Competizione and DiRT Rally 2.0 ([games.md](games.md)).
No button order has been checked against a real wheel.

## Which one?

1. The game has a G920 preset: **G920**. It has only a G923 preset, or you want TrueForce in ACC or DiRT Rally 2.0:
   **G923**.
2. The game is older than the G920 (2015) and lists the G27: **G27**; lists only the G25: **G25**.
3. The game has a preset for the Thrustmaster RGT Force Feedback Pro and none for those: **RGT** (not yet tested in
   a game).
4. Not sure: start with G920, then try the newest Logitech wheel the game's controller list names.

## What they share

- **900 degrees** full lock (the RGT: 270, fixed). The Mapping tab's **Steering rotation** sets what the game sees; set your wheel's own
  software to the same number.
- **The same axes in both Modes and on both systems:**

  | Target | Wheel | Throttle | Brake | Clutch | Slider 0 |
  |---|---|---|---|---|---|
  | G920, G923 | X | Y | Rz | Slider | — |
  | G25, G27 | X | Y | Rz | Slider 1 | combined pedals |
  | RGT | X | Rz | Y | Slider 0 | — |

  The RGT's are its default (5 axes, Clutch); its AXES and PEDAL SET change them
  ([target-rgt.md](target-rgt.md#axes)), and Combined pedals do not apply to it.

  With **Combined pedals** on (Mapping tab, or a game's own), as Logitech's "Report Combined Pedals": the Throttle axis
  carries throttle and brake (middle at rest), the Brake axis rests in the middle, the clutch is unchanged.

- **The Rig maps the core controls by itself** on every Target: Steering, the pedals, the D-pad, A, B, X, Y, the
  paddles and an H-pattern shifter's gears (on a Target with a shifter; Gear 7 has none on the Logitech ones). Every
  other button you map on the Mapping tab, and the RGT's levers too.
- **Button numbers in the app** are the ones the game uses: from 1 (as `joy.cpl` and games count), from 0 in the
  G25's and G27's **Native** order. The INI's `target_N` always counts from 0.
- **Force feedback** through WheelBri's safety limits ([user-guide.md](user-guide.md#force-feedback)). On Windows, a
  Logitech driver plays it when G HUB has installed one, else Windows' HID PID driver:

  | Target | With G HUB installed | Without |
  |---|---|---|
  | G920, G923 | G HUB's force feedback driver (HID++) | Windows' HID PID driver |
  | G25, G27, Order Logitech | G HUB's older-wheel driver (classic Logitech commands) | Windows' HID PID driver |
  | G25, G27, Order Native | Windows' HID PID driver | Windows' HID PID driver |

  The RGT: Windows' HID PID driver; with Thrustmaster's driver installed, not supported yet
  ([target-rgt.md](target-rgt.md#force-feedback)).

  Linux: the kernel's HID PID driver, for every Target (the RGT not yet tested there).

## Changing a Target's layout

Only in code: `g25Spec()`, `g27Spec()`, `g920Spec()`, `g923Spec()`, `rgtSpec()` in `core/src/DeviceProfile.cpp` and the matching
`config/*.ini`. Users cannot add Targets.
