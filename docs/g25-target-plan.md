# Plan: the Logitech G25 as a target

Status: plan only, parked (2026-10-13). Nothing of it is built. Pick it up from "Steps"; the open questions come first.

## Why

Today the only target is the G920 (`g920Spec`, `config/g920-*.ini`, the `wheelbri-g920` driver, `wheelbri-uhid`). Some
games know the G25 and not the G920: older titles (rFactor, GTR 2, LFS, Richard Burns Rally, the NFS games before
2015) have G25 presets, button names and force feedback tuned for it, and some only offer a wheel at all when they see
046D:C299. A G25 target lets the rig be that wheel, the same way it is a G920 now.

## What a G25 is (from the research of 2026-10-13)

- One USB device: the pedals and the shifter plug into the wheel base, so everything reports through it.
- Two USB identities: 046D:C294 (Driving Force compatibility, what it starts as: 12 buttons, combined pedals, no
  clutch, only gears 1-2) and 046D:C299 (native G25, after the host sends `F8 10 00 00 00 00 00`).
- Native mode: 4 axes (wheel 14-bit, accelerator, brake, clutch, separate), 1 hat (the shifter's D-pad), 19 buttons:
  2 on the wheel, 2 paddles, 8 on the shifter (4 red, 4 black), gears 1-6 and R. Logitech's own numbering (support
  article) puts the gears at DirectInput buttons 8-14 (0-based). The order of the other 12 is not documented.
- Rotation 40-900 deg (`F8 81` + range). No LEDs (those came with the G27).
- Force feedback: two motors, Logitech's classic protocol (the Driving Force Pro / G27 family): 4 effect slots,
  constant, spring, damper, auto-centre, periodic shapes, ramp, high-resolution spring and damper, friction; range and
  auto-centre as their own commands. Not HID PID, not the G920's HID++. On Windows games reach it through DirectInput
  and Logitech's driver; on Linux `hid-lg4ff` drives it (constant force and auto-centre only).

## Open questions (answer before building)

1. **The real descriptor and button order.** We have no G25. Get a native-mode report descriptor and a joy.cpl button
   list from an owner (a `usbhid-dump` / USBView capture, or a forum post we can read), or buy one second-hand. The
   descriptor decides axis letters (Y / Rz / Slider for the pedals is a guess from the G920) and the 12 button
   positions we cannot fill in yet.
2. **Which games and which way.** List the games the user wants the G25 for, and whether each finds the wheel through
   DirectInput (shim and virtual device both work) or through Logitech's SDK / LGS (needs the real driver stack: out of
   scope, as for the G920).
3. **Force feedback on the virtual G25.** A real G25 has no HID PID. Windows' `pid.dll` gives a HID PID device
   DirectInput force feedback with no Logitech driver, as our G920 does today, so the plan keeps PID. A game that sends
   Logitech's classic commands itself (raw HID writes) would get nothing; check the games from question 2.
4. **Sequential mode.** How the G25 reports the stick in sequential mode (its own two buttons, or the gear buttons) is
   not documented. We need it only if a game binds it; otherwise sequential up/down map onto the paddles.

## Decisions (proposed)

- **Native identity only.** The target is always 046D:C299 with the native layout. No compatibility mode, no mode
  switch to emulate: our devices come up native.
- **Same three ways as the G920**: in-game shim, virtual device (Windows driver), `wheelbri-uhid` (Linux). Each is a
  step of its own below; the shim is the cheapest and goes first.
- **Profiles stay data.** A `g25Spec()` next to `g920Spec()`, registered in `makeProfile("g25")`; nothing in the mapper
  changes. Game profiles keep their `target = g920 | g25` line; a game profile is only applied with its target
  (`profileFitsTarget` exists already).
- **Force feedback stays one model.** Whatever the way, game effects end as `FfbEffect`s through `FfbSafetyLimiter`
  (`FfbController`). The Linux classic-protocol decoder (step 5) produces `FfbEffect`s like `HidPid` does.
- **No Logitech files.** Nothing from Logitech's driver or SDK is shipped or copied.

## Steps (each a reviewable PR, in this order)

1. **G25 profile in core.**
   - `g25Spec()` in `DeviceProfile.cpp`: short name "G25", 046D:C299, "G25 Racing Wheel", rotation 900, the axis ranges
     of the descriptor, the 19 button names (gears at 8-14; the rest from question 1, else placeholders "Button 0" ...
     marked in a comment as unverified).
   - `makeProfile("g25")`; `DeviceProfileTest` for the names, ranges and encode.
   - `[emulator] target_profile = g25` is accepted by `loadEmulatorConfig` (and `EmulatorConfigTest`).
2. **The shim as a G25.**
   - `EmulatedJoystick` takes its identity, objects and button count from the profile instead of G920 constants
     (check every `kG920*` used there and in `shim/`).
   - `config/g25-in-game-shim.ini` (built-in config: WheelBri's default mapping onto the G25, with gears and the
     8 shifter buttons left to the user where the order is unknown).
   - The Mapping tab's target picker (`ConfigPicker`) gets "G25 in-game shim"; the Mapping tab already draws the target
     profile's own buttons and axes.
   - Test in one G25 game (question 2) with `[shim]` logs: the game sees "G25 Racing Wheel", buttons and FFB arrive.
3. **The virtual device as a G25 (Windows).**
   - Generalise `HidG920` into a per-target descriptor + input report (`HidTarget`, keeping the HID PID block shared),
     with a G25 descriptor built from the real one (question 1) plus the PID block.
   - The `wheelbri-g920` driver picks its descriptor and hardware id from a device property the service sets
     (`VirtualG920` -> `VirtualWheel`), so one driver serves both; its INF gets the 046D:C299 hardware id. The rename
     of files and the service class is its own small PR before this one.
   - `config/g25-virtual-device.ini`; "G25 virtual device" in `ConfigPicker`.
   - The hide filter, the "driving" registry entry and the XUSB pad stay as they are; check that Logitech's FFB entry
     (kept out for the G920) is not needed or wanted for C299 either.
4. **Linux (`wheelbri-uhid`) as a G25, input only.**
   - `UhidG920` -> per-target, with the G25 descriptor; `PROTON_ENABLE_HIDRAW=0x046D/0xC299` in the launch wrapper and
     `docs/linux.md`.
   - Note: the kernel binds `hid-lg4ff` to C299 and talks the classic protocol, not PID, so force feedback needs step 5.
5. **Classic Logitech force feedback decoder (Linux, and any game that writes it raw).**
   - core `LogiClassicFfb`: 7-byte output reports (slot commands for constant, spring, damper, periodic, auto-centre,
     range) -> `FfbPacket`s, unit-tested from captured `hid-lg4ff` traffic (or from the protocol document). This is
     our own implementation from the protocol; nothing copied from GPL code (`hid-lg4ff` is GPL: read for behaviour,
     not copied).
   - `wheelbri-uhid` feeds it the output reports when the target is the G25.
6. **Game profiles for G25 games** as they come (`config/games/<id>.ini`, `target = g25`), and the docs: README target
   table, CLAUDE.md status list, `docs/games.md`.

## Risks

- The button order and axis letters are guesses until question 1 is answered: a game's built-in G25 preset would bind
  the wrong controls. Step 1 must not ship placeholders as if they were known.
- Games that detect the G25 through Logitech's software (LGS/G HUB, the Steering Wheel SDK) will not see a virtual one,
  as with the G920.
- Two virtual wheels at once (G920 and G25) are not planned: one target per session, as now.
- FFB feel: the G25's own effects are weaker and coarser than a direct drive wheel; the limiter's defaults stay as they
  are, and a game's G25-tuned forces may need a game profile gain.

## Test plan

- Unit: `DeviceProfileTest` (G25 spec), `EmulatorConfigTest` (`target_profile = g25`), the descriptor and report tests
  for `HidTarget` (G25), `UhidG920Test` per target, `LogiClassicFfbTest` (step 5).
- Windows: joy.cpl shows "G25 Racing Wheel", 4 axes, 19 buttons and the hat moving with the rig; the FFB test in joy.cpl
  plays through the limiter; one G25 game end to end with telemetry and `ffb-<date>.csv`.
- Linux: `evtest` on the uhid G25, `fftest` (constant force) once step 5 is in.
