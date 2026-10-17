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
emulated G920 would make it differ from a real one. Mapping a rig handbrake onto a target button already works (a
button's secondary sources may be an axis past a threshold: `[buttons] target_N_or`). Not built: emulating the RS
Shifter & Handbrake as a second, separate device, for games that know it.

## Logitech wheel SDK registry entry

Games built on Logitech's wheel SDK (NFS Payback) find Logitech's helper DLL through
`HKCR\CLSID\{63BD165D-1584-4E75-AB56-08330350545F}\ServerBinary`; on the tested PC it named a removed Logitech Gaming
Software DLL and the game ignored every wheel (`docs/games.md`). Not built: WheelBri checking that entry and pointing
it at G HUB's installed DLL. Today the user does it by hand.

## G29 target

Not supported now (the targets are the G25, G27, G920 and G923; the G29 profile was dropped on 2026-10-04: the same
wheel as the G920 with more buttons). If it comes back: Logitech's Driving Force Shifter FAQ gives the G29's shifter
as joy.cpl buttons 12-17 for gears 1-6 and 18 for reverse (the G920's are 13-18 and 19).
