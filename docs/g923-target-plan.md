# The Logitech G923 (Xbox One and PC) as a target

Status: built (2026-10-15) for the shim, the Windows virtual device and Linux `wheelbri-uhid`, unit-tested; **not yet
tried in a game**, nor under Proton. Since then the virtual G923 also answers G HUB's HID++ (2026-10-16), reports
unsigned axes (2026-10-16) and, on Windows, its pedals in Logitech's HID layout with DirectInput axis entries that keep
games reading brake Rz and clutch Slider (2026-10-17): `docs/ghub-hidpp-plan.md`. G HUB saw it once (2026-10-16,
before the axis and pedal changes); none of the later changes has been tried. Pick **Target G923** on the Mapping tab
(the arrows go round G25, G27, G920, G923); Mode Virtual or Shim works as for the other targets. Open: the order of
Plus, Minus and the dial, a first game, the pedals in games and G HUB. Details under "Unverified".

## Why

Games from 2020 on know the G923 by name and offer a preset for it (buttons named, force feedback tuned) when they see
its ids. The Xbox/PC edition is the G920's successor: the same family (HID++), with the controls the G920 lacked.

## What a G923 for Xbox One and PC is (research of 2026-10-15)

- Two USB identities (Linux kernel patches, 2021 and 2023):
  - **Xbox mode, 046D:C26D**, the one it starts in: an Xbox One controller (`xpad`: "Logitech G923 Wheel (Xbox
    Mode)"). Only the face buttons, D-pad, paddles and one pedal work there.
  - **PC mode, 046D:C26E**, after a mode-switch message (the G920's: `0f 00 01 01 42`) from Logitech's driver,
    `xpad` or `usb_modeswitch`: a HID joystick plus HID++ (`hid-logitech-hidpp` drives it with the G920's quirks).
    USB product string "G923 Racing Wheel for Xbox One and PC".
  WheelBri presents only PC mode, as it presents the G920's PC mode (C262), never its Xbox mode (C261).
- Rotation 900 deg, pedals throttle, brake, clutch, as the G920.
- Controls: the G920's (A, B, X, Y, paddles, Menu, View, RSB, LSB, Xbox, D-pad, the Driving Force Shifter's gears)
  plus **Plus and Minus**, a **dial** (turns both ways) with a button in its centre, and rev lights (5 LEDs, HID++).
- Force feedback: HID++ (feature 0x8123, as the G920) and TrueForce. WheelBri speaks neither: the virtual device
  exposes HID PID and the shim forwards DirectInput effects to the service, as for every target. TrueForce (Logitech's
  SDK, audio-driven) is not emulated.

### Button order (0-based, as DirectInput numbers them)

Logitech does not document it, and no capture of a real G923 Xbox/PC in PC mode was found (a G923 Linux project asks
for one; users say its numbers differ from the PlayStation edition's). What `g923Spec()` uses:

| Buttons | What | Source |
|---|---|---|
| 0-10 | A, B, X, Y, R Paddle, L Paddle, Menu, View, RSB, LSB, Xbox | the G920's (same family, same quirks in Linux) |
| 11 | nothing | the G920's |
| 12-17, 18 | gears 1-6, reverse (Driving Force Shifter) | the G920's (Logitech's shifter FAQ) |
| 19, 20 | Plus, Minus | guess: after the G920's, in the G29's order |
| 21, 22, 23 | dial right, dial left, dial centre (Enter) | guess: the G29's order |

24 buttons: the virtual device's button block declares 24 and no padding bits (the input report keeps its size).

## How WheelBri presents it

The same machinery as the G27 (docs/g27-target-plan.md):

- **Profile**: `g923Spec()` (`core/src/DeviceProfile.cpp`, `makeProfile("g923")`, `target_profile = g923`): the
  G920's (axes, ranges, first 19 buttons) with 046D:C26E, "Logitech G923 Racing Wheel for Xbox One and PC" and 24
  buttons (`kG923Buttons`); since 2026-10-16 its virtual device's axes are unsigned (`VirtualWheel::unsignedAxes`, for
  G HUB; DirectInput scales either range the same). The Mapping tab groups them: FACE, PADDLES, PLUS MINUS on the
  left; STICKS, SYSTEM, DIAL on the right; the SHIFTER below.
- **Config**: `config/g923.ini`, both modes: the G920's default mapping; Plus, Minus and the dial unmapped.
- **Mapping tab**: Target `< G920 | G25 | G27 | G923 >` when built, the arrows going round; now in the order G25,
  G27, G920, G923.
- **Shim**: unchanged; it presents whatever `target_profile` says.
- **Virtual device (Windows)**: the same driver; its INF binds `wheelbri\G923` too (reinstall the driver), and
  `VirtualWheel` has the G923's identity. The descriptor is the G920's with 24 buttons; the 6 bytes of the padding's
  Input item become global items that change nothing, so the descriptor keeps its size. Since 2026-10-16 it also
  carries HID++'s reports 0x11 and 0x12 for G HUB, with its HID PID reports 0x10 higher; since 2026-10-17, on Windows,
  its pedals are reported as a real Logitech wheel's (brake Z, clutch Rz) and the service writes DirectInput's
  `OEM\VID_046D&PID_C26E\Axes` entries while it runs so games still read brake Rz and clutch Slider; the uninstaller
  and clean-pc remove only the entries WheelBri marked (`docs/ghub-hidpp-plan.md`).
- **Virtual device (Linux)**: `wheelbri-uhid --target g923`, on the virtual bus like the G25 and G27
  (`0006:046D:C26E`): Proton's axis hack is for the G920's ids only, and on the virtual bus `hid-logitech-hidpp`
  (which matches C26E on USB) does not look at it. Proton games with `PROTON_ENABLE_HIDRAW=0x046D/0xC26E`
  (`wheelbri.sh --target g923` sets it).

## Unverified

- The order of Plus, Minus and the dial (19-23). Check: a game with G923 button names (F1, Assetto Corsa Competizione,
  Forza Horizon on PC) shows which G923 button each rig button presses; or a capture from a real G923 Xbox/PC in PC
  mode (`joy.cpl` button numbers).
- That games accept the virtual G923 as a G923 and offer their preset; some may look for Logitech's own software or
  TrueForce and treat it as a plain wheel.
- Its force feedback in a G923-aware game (through HID PID / the shim).
- With Mode Virtual on Windows: that games still read brake and clutch right through the DirectInput axis entries
  (joy.cpl: brake on Z Rotation, clutch on the slider), and that G HUB shows the dial and pedals right.

## Sources

- [hid-logitech-hidpp: add G923 Xbox (2023)](https://lkml.iu.edu/hypermail/linux/kernel/2301.1/04882.html)
- [xpad: add Logitech G923 Xbox variant (2021)](https://lkml.iu.edu/hypermail/linux/kernel/2105.3/05344.html)
- [logitech-hidpp: add G923 (2021)](https://lkml.iu.edu/hypermail/linux/kernel/2105.3/05298.html)
- [G923 Xbox version current state (dmesg, product string)](https://github.com/ZRtmWrJqXcjbqBLIMBYMCeUw/Logitech-G923-Linux-Kernel-Driver/issues/5)
- [Support the Xbox G923 and G920 (HID++)](https://github.com/Emperor-Z/g923-claude/issues/12)
- [Logitech G29 vs G923 (controls)](https://simracingsetup.com/?p=4826704)
