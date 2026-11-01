---
name: target-wheel-spike
description: WheelBri only. Use before adding a new target wheel or improving one (G25, G27, G920, G923): pick a reference game that uses the wheel fully, prove the wheel's whole footprint and the game's whole use of it, and block any change until both are known with high certainty.
---

# Target wheel spike

Investigation only. Nothing is implemented or changed (code, config, game profile, driver, `docs/targets.md`)
until the gate (§1) passes and the user confirms. Goal, with proof: everything the **real wheel** is and does
(identity, channels, every message it takes and sends, every API on top, its behaviour on each message, call and
signal), and everything a **reference game** does with it.

## 0. Start

- Root `CLAUDE.md` and the area files touched apply.
- Ask: which wheel; new or improving; what triggered it; anything to skip.
- User has no real Logitech wheel (rig: Moza R3): wheel facts from sources, game and WheelBri facts measured.
  Never a guess as a fact.

## 1. Gate

All true, then the user says it passed:

- Reference game(s) chosen and agreed (§3).
- Wheel footprint (§4) complete with proof, unknowns accepted by the user as risks.
- Wheel behaviour known for every inbound message, call and signal: reply, state change, physical effect, timing,
  errors, re-enumeration.
- Game footprint (§5) captured from the game in every phase.
- Trace matrix (§6) has no empty cell.
- Nothing the reference game uses rests on One source or Guess.

Proof levels: **Measured** (log/capture + line), **Logitech** (official doc, SDK header, INF), **Two sources**
pass; **One source**, **Guess** don't.
Allowed before the gate, with the user's OK only: logging-only diagnostic tooling, off by default, invisible to the
game, committed as such.

## 2. WheelBri today

Note what the target (or, for a new one, the closest target as template) does now. Grep, read only needed parts:
`docs/targets.md`, `docs/games.md`, `config/<target>.ini`, `config/games/*.ini`; `<target>Spec()` in
`core/src/DeviceProfile.cpp`; `VirtualWheel` in `HidWheelDevice.hpp`; `HidPid`, `HidPp`, `HidPpFfb`,
`TrueForce*`, `LogiSdk`, `EmulatedJoystick` (shim) headers; `tools/uhid/UhidWheel.hpp`; `driver/Driver.cpp`.

## 3. Reference game

The game whose support of the wheel is most complete: all axes, buttons, shifter, clutch, game-set rotation, every
FFB effect, LEDs, Logitech SDK, HID++ / G HUB, TrueForce, mode switches.

- Table: wheel capabilities (§4) × candidate games, each mark with its source.
- Candidates: the user's games (`docs/games.md`) first; DiRT Rally 2.0 is the Logitech reference, ACC for
  TrueForce; others only if they cover more.
- No single game covers all: smallest set, saying which proves what. Uncovered: "not exercised", lowest priority.
- User confirms before §5.

## 4. Wheel footprint

Each mode is its own footprint (G920 Xbox/PC, G923 Xbox/PS, G25/G27 compatibility/native).

Sources, in order: Logitech (logitechg.com; G920 = Xbox version, never G29; manuals, FAQs, LGS / G HUB INFs, Steering
Wheel and TrueForce SDK docs and headers; support archive); real-device dumps (linux-hardware.org, `usb.ids`,
`lsusb -v`, `usbhid-dump`, USBPcap); open drivers, **facts only, never copy** (Linux `hid-lg4ff`,
`hid-logitech-hidpp`, `hid-lg`, new-lg4ff GPLv2; Oversteer, GIMX GPLv3; flag before taking any code); games' data;
community last (needs a second source). CAPTCHA: send the user the link. Keep a source list (URL + what it gave).

- **Identity.** USB: VID, PID per mode, `bcdDevice`/firmware, `bcdUSB`, class, power, speed, strings, interfaces,
  endpoints, container id, extra functions (GIP, audio, HID++, TrueForce `0xFFFD`). Windows: hardware/compatible ids,
  binding INF, class, friendly name, `OEMName`/`OEMForceFeedback` (`HKCU\...\MediaProperties\PrivateProperties\
  Joystick\OEM\VID_046D&PID_xxxx`), `guidProduct`, instance name, `DIDEVCAPS`, `dwDevType`, WGI `RacingWheel`,
  XInput. Linux: kernel driver, evdev name/id, sysfs (`range`, `alternate_modes`, `combined_pedals`, autocentre),
  hidraw, Proton/Steam view.
- **Outbound (wheel → host).** Every channel; every report descriptor byte by byte, decoded; input layout per mode
  (axis resolution, sign, rest, direction; HID button order; hat; pedals; shifter; dials); rate and when sent;
  feature reports; HID++ features, answers, notifications, errors; vendor streams.
- **Inbound (host → wheel) + behaviour.** Per message/call: bytes, meaning, sender (game via DirectInput, Logitech
  driver, G HUB/LGS, SDK, kernel), and what the wheel does (reply bytes, state, force/centring/LEDs, timing,
  clamping, bad input, re-enumeration/PID change). Covers output/feature reports, FFB (HID PID or Logitech classic:
  slots, autocentre, range `0xF8`, mode switch, LEDs), HID++ (0x8123...), TrueForce, gain, rotation, combined
  pedals, calibration, reset.
- **APIs on top.** Logitech driver (`WmJoyHid.inf` relabelling, button order with/without it, range via `Escape`/
  properties, panel settings); G HUB/LGS (reads, writes, registry, "active" condition); Steering Wheel and TrueForce
  SDKs (every export, what it sends, what it needs back); DirectInput (`EnumEffects`, `DIPROP_*`, honoured
  parameters); Linux `EV_FF`, `ff-memless`, sysfs.

## 5. Game footprint

Captured from the game running against WheelBri's target (closest one for a new target), at least twice, noting
differences. User steps: short, one command per block, filtered output.

- Detection: how it picks the wheel and its preset (name, VID/PID, `guidProduct`, `LogiIsModelConnected`, WGI), and
  what it does when a detail differs.
- Channels: DirectInput, Logitech SDK, TrueForce SDK, raw HID, HID++ via G HUB, XInput, WGI.
- Every call/message with arguments, values, order, rate, expected answer, per phase: start, detection, menus,
  driving, pause, car/track change, settings, exit, unplug/replug.
- Its preset: axes, buttons, FFB mode, rotation, LEDs, where stored.
- Tools: shim `emulate = false` + `logi_log = true` (`wheelbri-shim-*.log`); driver logs (output reports, HID++,
  `trueforce:`); service and `ffb-*` logs; `scripts/telemetry_summary.sh`; Linux `evtest`, hidraw. Missing
  capture → propose diagnostic tooling (§1).

## 6. Trace matrix

`Game call | Phase | Channel | Wheel messages | Real wheel's behaviour | WheelBri now | Proof`, plus a "not
exercised" row per unused capability. Differences = gap list.

## 7. Report

`Claude outputs/spikes/<target>-spike.md`: summary with gate status per item; capability × game table; footprints
(one fact per line, source, proof level); trace matrix; gaps (`Item | Real | WheelBri | Effect on game | Change |
Size`); open questions with how to close each; after the gate only, steps as small commits with the CLAUDE.md
checklists they hit (new target: `DeviceProfileSpec` in `makeProfile()` + `VirtualWheel`, `config/<target>.ini`,
docs, tests); UI gets sketches first.
Send path + summary. Gate open: say what's missing and keep investigating. Passed: wait for the user's pick.

## 8. After the gate

- Check each change in the reference game against the trace matrix; a difference reopens its row.
- Only confirmed facts go into `docs/targets.md`. Never ship or copy Logitech DLLs or GPL code.
- Per step: edit, build, tests separately (errors/failures only), git commands for one small commit.
