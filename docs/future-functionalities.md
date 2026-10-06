# Future functionalities

Ideas worked out but not built, kept here until we decide to do them (and, for one built idea, what is still to
check).

## Running WheelBri as a standard user

Built on 2026-10-06 (version 0.7.0): the WheelBri Helper service; README.md, "Running as a standard user (WheelBri
Helper)" and "Security notes", and CLAUDE.md describe it. Still to do: the test as a real standard user (a second
Windows account without Administrator: install, Start / Stop, end the app in Task Manager, the shim in a Steam
folder, uninstall).

## Logitech RS Shifter & Handbrake

A separate Logitech device (its own USB controller on PC; on console only through the Racing Adapter), not part of a
G920: a sequential shifter or a handbrake (switch or analog). A real G920 has no handbrake input, so adding one to the
emulated G920 would make it differ from a real one. Options when we get to it: map a rig handbrake to a G920 button,
or emulate the RS Shifter & Handbrake as a second, separate device.

## G29 target

Not supported now (the G920 is the one target). If it comes back: Logitech's Driving Force Shifter FAQ gives the G29's
shifter as joy.cpl buttons 12-17 for gears 1-6 and 18 for reverse (the G920's are 13-18 and 19).
