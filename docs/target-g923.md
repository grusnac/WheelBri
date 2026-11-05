# Target G923

The **Logitech G923 for Xbox One and PC**: the [G920](target-g920.md) plus five buttons and **TrueForce**, Logitech's
high-rate force stream. All Targets: [targets.md](targets.md).

| | |
|---|---|
| Ids | 046D:C26E, "Logitech G923 Racing Wheel for Xbox One and PC" |
| Buttons | 24 (the G920's 19, then Plus, Minus and the dial), D-pad as a hat |
| Rotation | 900 degrees |
| Modes | Virtual (Windows 11, Linux) and Shim; TrueForce in Mode Virtual on Windows only |
| Force feedback | TrueForce (games that use it), else as the G920: G HUB's driver over HID++ or Windows' HID PID |
| Tested | ACC (TrueForce on and off), DiRT Rally 2.0 ([games.md](games.md)) |

## When to pick it

- **ACC on Windows**: with TrueForce on, ACC sends all its force through TrueForce, as to a real G923. This is the path
  closest to the real wheel.
- DiRT Rally 2.0: it uses TrueForce on a G923 by itself.
- Not for NFS Heat or NFS Unbound: they predate the G923 (the D-pad does nothing in Heat; Unbound falls back to a
  generic layout). Use the [G920](target-g920.md) there.

## Set up

1. Install **Logitech G HUB** (Windows; it need not run). Games load Logitech's TrueForce SDK from its folder.
2. Mapping tab: **Target G923**, **Mode Virtual**.
3. **TrueForce**: **On** to give games TrueForce, **Off** for a G923 that gets effects one by one, as a G920. Greyed
   (with the reason) when G HUB's TrueForce SDK is not installed. Takes effect at the next Start; the service log says
   at Start whether it found the SDK.
4. Map Plus, Minus and the dial yourself if the game uses them; the rest as on the G920.
5. **Steering rotation**: the rotation you want (360 tested in ACC); set your wheel's software to the same.

## Axes

As the G920: wheel X, throttle Y, brake Rz, clutch Slider; 16 bits, unsigned as a real G923's. The pedals use games'
layout, not a real G923's (throttle Y, brake Z, clutch Rz, 8 bits): with the real layout DirectInput relabelled them
(in ACC gas became the clutch). G HUB therefore shows them in the wrong places; games read them right.

## Buttons

1-19 as the [G920](target-g920.md#buttons). Then:

| # | Button |
|---|---|
| 20 | Plus |
| 21 | Minus |
| 22 | Dial right |
| 23 | Dial left |
| 24 | Dial enter |

Logitech does not document 20-24: their order follows the G29's and is unchecked on a real G923.

## TrueForce

With TrueForce on, the virtual G923 has a second interface in the wheel's container, as a real one, and answers
Logitech's TrueForce SDK with a real G923's measured parameters.

| What the game sends | What reaches your wheel | Live tab |
|---|---|---|
| Motor torque stream (ACC: `logiTrueForceSetTorqueKF`) | a constant force | Steering force |
| Damping (`logiTrueForceSetDamping`) | a damper, so a direct-drive wheel does not swing lock to lock | Damper |
| Road texture (4 samples a millisecond) | a vibration, its size and frequency following each 40 ms | Vibration |
| DirectInput effects of a game without TrueForce | mixed into the one TrueForce force | as sent |

Everything goes through the same safety limits as any other force. The texture as a vibration is not yet felt on a
rig. Details for developers: `core/include/wheelbri/TrueForce.hpp`, `TrueForceFfb.hpp`.

## G HUB

The virtual G923 answers G HUB's HID++ 2.0 (Root, FeatureSet, firmware, name, ForceFeedback 0x8123): G HUB shows it as
active. Effects G HUB or Logitech's driver send over HID++ are played on your rig, scaled by the gain G HUB sets. The
rotation G HUB sets is logged, **not applied**: set rotation on the Mapping tab and in your wheel's software.

## Shadow view

The real G923 (Xbox and PC), with the Driving Force Shifter's gate; printed labels as the G920's plus + and -; the dial
lights its left or right teeth as it turns.

## Known

- TrueForce needs Windows and Mode Virtual. On Linux the G923 works without it.
- To see what a game asks Logitech's SDK: put the 64-bit shim in its folder with `emulate = false` and
  `logi_log = true` in `wheelbri-shim.ini`; every SDK call goes to `wheelbri-shim-*.log`.
