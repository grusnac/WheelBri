# The virtual G923 in Logitech G HUB (HID++)

Status: built (2026-10-16) for the G923 target only, unit-tested, **not yet tried with G HUB**. Games are not meant to
notice any of it.

## Why G HUB shows it "Inactive"

G HUB finds a Logitech wheel by its ids (046D:C26E: so the card says G923), then talks to it in Logitech's own protocol,
HID++ 2.0, over vendor reports of its own, not as a joystick. Until 0.9.8 the virtual wheel had only the joystick
and the HID PID force feedback games use, so G HUB got no answer and greyed the card out. (G HUB also keeps cards of
wheels it has seen before, "Inactive" while they are away.)

## What WheelBri does now (three steps in one)

1. **Listen.** The virtual G923's HID descriptor has HID++'s two reports after the joystick's: long (report ID 0x11,
   19 bytes each way) and very long (0x12, 63 bytes), each its own collection on vendor usage page 0xFF43 (the
   reports a G920 has, Linux's `hid-logitech-hidpp` says; the usages 0x0602 / 0x0604 are a guess). Every request G
   HUB sends goes to the driver's log: each kind once at info ("hid++: asked for feature 0x807A: not there"), every
   request and answer in hex at debug (the app's Logs tab, Level debug).
2. **Answer.** `HidPp` (`core/include/wheelbri/HidPp.hpp`, written from HID++'s public description, nothing from GPL
   code) answers the features G HUB needs to know what a device is: Root (0x0000: feature lookup, protocol 4.2),
   FeatureSet (0x0001), DeviceFwVersion (0x0003: one entity, USB, model C26E, firmware "WBR 99.99", high so no update
   looks newer; there is no firmware-update feature, so G HUB cannot start one), DeviceNameType (0x0005: "G923 Racing
   Wheel for Xbox One and PC", type 0x0A steering wheel). Anything else gets HID++'s error and a log line.
3. **Settings.** ForceFeedback (0x8123) is answered: its rotation (aperture) and global gain are kept, read back to G
   HUB and logged ("hid++: rotation set to 540 deg (not applied)"). Effects G HUB or a Logitech SDK game uploads get a
   slot and a log line, **and are not played**: the game's own force feedback (HID PID or the shim) is what reaches the
   R3, through `FfbSafetyLimiter` as always.

To make room, the G923's HID PID reports are 0x10 higher (0x21.. instead of 0x11..): HID++ needs IDs 0x11 and 0x12,
which the PID block used. `routeReport` / `wirePidId` (`HidG920.hpp`) translate them for `HidPid`, which is unchanged;
the driver and `wheelbri-uhid` both use them. The G920's, G25's and G27's descriptors are exactly as before.

## What G HUB did (driver logs and G HUB's own log, 2026-10-16)

- It asked for every feature above, plus **0x4540** (not there; unknown, G HUB carries on without it).
- It set the rotation (900, then 360) and uploaded one effect, `00 86 00 00 00 00 7f ff 19 99 00 00 00 00 19 99 7f ff
  ...`, freed a second later: by its numbers (0x1999 = 20 %) most likely its centring spring at "Centering Spring
  Strength 20".
- G HUB's log: the wheel went BLOCKED (`MISSING_INTERFACE`, for under a second), then **ACTIVE** as `g923_xbox`; the
  Steering Wheel page works and G HUB saves its settings for it (Operating Range, Sensitivity) as for a real one.
- Its wheel dial was half a turn off and wrapped as the wheel crossed the centre. The axes were signed (-32768..32767,
  centre 0); a real Logitech wheel's are unsigned (0..65535, centre 32768), and G HUB reads them so. Since then the
  G923's axes are unsigned (`VirtualWheel::unsignedAxes`, `packInputFor`); games read them through DirectInput, which
  scales either range the same. The other targets keep the signed axes they were tested with.

## The pedals (brake shown as the clutch)

G HUB reads the pedals by HID usage as a real Logitech wheel reports them: throttle Y, brake **Z**, clutch **Rz**. The
virtual wheel reported them as games read a G920 on Windows (brake Rz, clutch Slider), so G HUB showed the brake as the
clutch. On Windows the G923 now reports the real layout (`VirtualWheel::logitechPedals`), and the service writes
DirectInput's per-device axis entries while the wheel exists, so games still get the brake on Rz and the clutch on
Slider: `...\Joystick\OEM\VID_046D&PID_C26E\Axes\<n>`, value `Attributes` (DIOBJECTATTRIBUTES: flags, usage
page, usage), `<n>` the DirectInput axis (5 Rz <- HID Z, 6 Slider <- HID Rz, 2 Z <- HID Slider which the wheel lacks).
That `<n>` is the DirectInput axis is read from a published registry fix that swaps Z and Rz this way; Microsoft
documents the structure, not the numbering. The entries carry a `WheelBri` value: an Axes key without it (Logitech's
software, for a real wheel) is left alone and the log says so; ours is removed at stop (and by uninstall / clean-pc
with the whole key). wheelbri-uhid (Linux) keeps the games' layout: Proton reads no such entries.

Check: joy.cpl (Game Controllers > Properties) should show the brake on "Z Rotation" and the clutch on the slider, as
before; G HUB's pedal page should show each pedal as itself.

## What G HUB's settings can and cannot do

WheelBri passes the steering axis through 1:1: its rotation (`target_rotation_deg`) is what it tells games and shows,
while the R3's real lock is set in Pit House, which WheelBri never reads or sets. So G HUB's Operating Range cannot turn
the R3 to 360 deg either; at most WheelBri can show it. What G HUB could really change is force feedback (its centring
spring, its strength), through `FfbSafetyLimiter` like every force, and only once its effect format is read from more
uploads. Until then, and until the user chooses who wins, nothing G HUB sets is applied.

## What is open

- **Whether the dial and the pedals are right now** in G HUB, and the pedals still right in games (untried).
- **More features:** the real G923 lists 0x807A rev lights, 0x80A3, 0x80D0, 0x8120, 0x8127 besides 0x8123; G HUB went
  active without them.
- **Who wins** for G HUB's force feedback (spring, strength): WheelBri's `[ffb]` or G HUB. Not decided.
- **Effects through HID++** (Logitech SDK games): their format is to be read from the log before anything is played.
- The G920 target: the same can be done for it once the G923 works (it is the same HID++ family).

## How to check

Reinstall the driver (it changed), start WheelBri with Target G923, Mode Virtual, then open G HUB. Afterwards the
driver's log (`wheelbri-driver-<date>-NN.log` in the logs folder) has the "hid++:" lines.
