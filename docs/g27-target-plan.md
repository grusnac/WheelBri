# The Logitech G27 as a target

Status: built (2026-10-15), not yet tried in a game. Pick **Target < G27 >** on the Mapping tab; Mode Virtual or Shim
works as for the G920 and the G25. What is still unverified, and how to check it, is under "Unverified".

## Why

Games of the G27's era (2009-2015) know it by name: rFactor 2, Assetto Corsa, Project CARS, DiRT, GRID, F1 2010-2015,
the NFS games of those years. Some offer a wheel preset (buttons named, force feedback tuned) only when they see
046D:C29B. A G27 target lets the rig be that wheel, as it can be a G920 or a G25.

## What a G27 is (research of 2026-10-15)

- One USB device: the pedals and the shifter plug into the wheel base. Native mode 046D:C29B, after Logitech's driver
  (or Linux's `hid-lg4ff`) switches it; it can also present itself as a Driving Force EX, Driving Force Pro or G25
  (`hid-lg4ff`'s multimode table). WheelBri presents only the native G27.
- Rotation 40-900 deg (`hid-lg4ff`: the same range command as the G25).
- Pedals: accelerator, brake, clutch, separate axes, as on the G25.
- Buttons (Wikipedia): 6 on the wheel, 2 paddles, 8 on the shifter (4 red, 4 black) and its D-pad (a hat), gears
  1-6 and reverse. No sequential mode (the G25's shifter had one). 23 buttons in all.
- Rev lights (LEDs) on the wheel: driven by games through Logitech's SDK or driver calls. Not emulated.
- Force feedback: Logitech's classic protocol, like the G25. WheelBri never speaks it: the virtual device exposes HID
  PID, and the shim forwards DirectInput effects to the service, as for every target.

### Button order (0-based, as DirectInput numbers them)

Logitech documents only the gears (at 8-14, as on the G25). The rest comes from users' reports of what games and
Logitech's Profiler show (forum threads: Steam GRID Autosport, Overtake F1 2011, GTPlanet GT5), and is what
`g27Spec()` uses:

| Buttons | What | Source |
|---|---|---|
| 0-3 | the shifter's red buttons, left to right | "the red buttons on the H-shifter are 1, 2, 3, 4"; "the 3rd Red gearbox button (Button 3)" |
| 4, 5 | right and left paddle | "button 6 - LEFT Paddle" (1-based) |
| 6, 7 | the wheel's top right and top left buttons | "top left button ... in the Profiler is shown as Button 8" (1-based) |
| 8-13, 14 | gears 1-6, reverse | Logitech (as the G25) |
| 15-18 | the shifter's black buttons | the shifter buttons left over; "Button 17 (on Gearbox Module)" |
| 19, 21 | the wheel's right buttons below the top one, top to bottom | "In the right you have 6, 19 and 21" |
| 20, 22 | the wheel's left buttons below the top one, top to bottom | "in the left side from up to down, you have the button 7, 20 and 22"; "19 to 22 weren't there in the G25" |

The same reports suggest the G25 may number its red and black buttons the other way round from WheelBri's G25 guess
(red 0-3, black 15-18). That is left alone until a real G25 or G27 is captured.

## How WheelBri presents it

The same machinery as the G25 (docs/g25-target-plan.md):

- **Profile**: `g27Spec()` (`core/src/DeviceProfile.cpp`, `makeProfile("g27")`, `target_profile = g27`): 046D:C29B,
  "Logitech G27 Racing Wheel USB", 900 deg, 23 buttons (`kG27Buttons`, gears at `kG27FirstGear` = 8), pedals released
  at the top of the range, the G920's DirectInput layout (wheel X, throttle Y, brake Rz, clutch Slider). The Mapping
  tab groups its buttons: WHEEL (left and right, top to bottom) and PADDLES, RED and BLACK, the SHIFTER below.
- **Config**: `config/g27.ini`, both modes (WheelBri's default mapping: the G25's,
  with red and black in the G27's places; the 4 lower wheel buttons unmapped).
- **Mapping tab**: Target `< G920 | G25 | G27 >`, the arrows going round.
- **Shim**: unchanged; it presents whatever `target_profile` says.
- **Virtual device (Windows)**: the same driver; its INF binds `wheelbri\G27` too, and `VirtualWheel` has the G27's
  identity. The report descriptor is the G920's with the button block declaring 23 buttons and 1 padding bit instead
  of 19 and 5 (`g920ReportDescriptor(kG27Buttons)`): the input report keeps its size (3 button bytes).
- **Virtual device (Linux)**: `wheelbri-uhid --target g27`, on the virtual bus like the G25 (`0006:046D:C29B`);
  Proton games with `PROTON_ENABLE_HIDRAW=0x046D/0xC29B` (`wheelbri.sh --target g27` sets it).

## Unverified

- The button order above (except the gears). Check: a game with G27 button names (F1, GRID, Assetto Corsa) shows which
  G27 button each rig button presses; or a capture from a real G27 (`joy.cpl` button numbers).
- That games accept the virtual G27 as a G27 (name, ids) and offer their G27 preset.
- Its force feedback in a G27-aware game (through HID PID / the shim, as for the G920).

## Steps after this

- Try the shim in a game with a G27 preset, then the virtual device.
- Fix the button order from what the game shows; the G25's too if it turns out the same.
