# Plan: G25 shifter, H-pattern and sequential

Status: put aside (2026-11-10) until an H-pattern shifter is on the rig.

## What it is

The real G25's shifter has a physical switch: H-pattern (six gears and reverse, buttons 9-15 in the driver order) or
sequential (push and pull press buttons 9 and 10, `docs/target-g25.md:53`). It is on the shifter, not in Logitech's
software. The G27 has no sequential mode.

## What WheelBri does now

- The G25 and G27 targets have the gear buttons (`kG25FirstGear`, `kG27FirstGear`, `DeviceProfile.hpp`); the user maps
  rig buttons to them.
- No notion of a shifter mode.

## Questions (answer with the shifter on the rig)

1. How does the rig's H-pattern shifter report: buttons per gear, or axes? Neutral = no button?
2. Do sequential-only games (and the old NFS games) read the G25's gears 1/2 as up/down when it is in sequential mode,
   and does anything else change in the report (none expected)?
3. Is a mode needed at all, or does mapping the rig's sequential shifter to buttons 9/10 already do it?

## Steps

1. Capture the rig shifter with `wheelbri-service --list` and the Live tab; note what each gear sends.
2. Test GRID and Carbon with H-pattern gears mapped to 9-15; then with paddles/sequential mapped to 9/10.
3. Only if a mapping cannot do it: a per-target or per-game shifter mode, with tests and docs.

## Done when

Gears work in GRID and Carbon with the rig's H-pattern shifter, and `docs/target-g25.md` says how to map it.
