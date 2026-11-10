# Plan: games set the rotation ("Allow Game To Adjust Settings")

Status: put aside (2026-11-10). Investigation first, no code until the questions below have answers.

## What it is

Logitech Profiler and Logitech Gaming Software had "Allow Game To Adjust Settings" (global and per game): a game could
set the wheel's rotation (and force feedback values, out of scope here) through Logitech's driver.

## What WheelBri does now

- The rotation the target reports comes from the mapping (`target_rotation_deg`) or the running game's
  (`[game_rotation]`, `GameRotation.hpp`, `GameWatch::rotationNow`).
- Logitech's SDK DLL: `LogiSetOperatingRange` / `LogiSetOperatingRangeDInput` are logged only (`core/src/LogiSdk.cpp:86-93`,
  `logi_log`).
- G923 TrueForce interface: command `0e` (operating range) is logged only (`TrueForce.hpp:128`).
- Shim: `IDirectInputDevice8::Escape` is logged and not forwarded (`shim/src/EmulatedDevice.hpp:640`).

## Questions

1. How did DirectInput games of the G25 era ask Logitech's driver for a rotation: `Escape`, a driver property, the SDK
   DLL, or not at all? Which games did it (GRID, DiRT 2/3, rFactor, Carbon/ProStreet?)
2. Does any game WheelBri already runs ask for a rotation? Read existing shim and logi logs for `Escape` and
   `OperatingRange` lines.
3. What should WheelBri do with a request: only report it, or change the rotation the target reports? The rig's own
   rotation stays the wheel's app's (WheelBri never sets it), so following the game means the two can differ.

## Steps

1. Grep the logs in `%LOCALAPPDATA%\wheelbri\logs` and `win7-logs/` for `Escape #` and `OperatingRange`.
2. Run GRID, DiRT Rally 2.0, Carbon with `logi_log = true` and the shim at debug; note every request.
3. Search Logitech's SDK docs and driver notes for the Escape command codes of the G25/G27 driver.
4. Write the findings here; decide whether it is worth a feature. If yes: plan, tests, docs, as any feature.

## Done when

Each question has an answer backed by a log line or a source link.
