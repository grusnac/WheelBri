---
name: target-wheel-spike
description: WheelBri only. Use before adding a new target wheel (a Logitech wheel WheelBri presents to games) or improving an existing one (G25, G27, G920, G923) - picks a reference game that uses the wheel fully, proves the wheel's whole footprint and the game's whole use of it, and blocks any change until both are known with high certainty.
---

# Target wheel spike

A spike is investigation only. It ends with a report, a gap list and a gate the user signs off. Until then
**nothing is implemented or changed**: no code, config, game profile, driver or `docs/targets.md` edit.

Goal: know, with proof,

1. everything the **real wheel** is and does: identity, every channel, every message it accepts and sends, every
   API or callable interface on top of it, and how it behaves on each message, call and signal;
2. everything a **reference game** does with it: how it finds the wheel, every channel, call and message it uses,
   when, with which values, and what it expects back;

so that WheelBri's target can be built or changed knowing exactly what the game sends, what the real wheel would
do, and what WheelBri does today.

## 0. Before starting

- Root `CLAUDE.md` rules apply; read the area file of every area the spike touches (driver, in-game DLL, tests).
- Ask the user (one short question set): which wheel; new target or improving one; what triggered it; any part to
  skip. Name the report from the answer.
- The user has **no real Logitech wheel** (rig: Moza R3). Facts about the real wheel come from sources; facts about
  the game and WheelBri come from measuring. Never write a guess as a fact.

## 1. The gate (read first, check last)

Implementation may start only when all of these hold and the user has said the gate is passed:

- [ ] A reference game (or a small set) is chosen and the user agreed (section 3).
- [ ] **Wheel footprint** complete (section 4): every item filled with proof, or marked unknown with the user
      accepting it as a known risk.
- [ ] **Wheel behaviour** complete: for every inbound message, call and signal, what the wheel does (reply, state
      change, physical effect, timing, errors, re-enumeration), with proof.
- [ ] **Game footprint** complete (section 5): every channel, call and message the reference game uses, captured
      from the game itself, in every phase (start, menus, driving, pause, car / track change, settings, exit).
- [ ] **Trace matrix** closed (section 6): every game call maps to the wheel messages it causes, the wheel's
      behaviour, and what WheelBri does now. No row empty.
- [ ] No row needed by the reference game rests on **One source** or **Guess**.

Proof levels: **Measured** (log or capture file + line, from the game or a real wheel), **Logitech** (official
doc, SDK header, Logitech INF), **Two sources** (independent). **One source** and **Guess** do not pass the gate
for anything the reference game uses.

The only thing allowed before the gate: **diagnostic capture tooling** (logging-only, off by default, changes
nothing the game sees), and only after the user OKs it. It is not target work and says so in its commit.

## 2. What WheelBri has today (improving a target)

Read, and note per item what the target does now:

- `docs/targets.md`, `docs/games.md`, `config/<target>.ini`, `config/games/*.ini` entries for it.
- `core/src/DeviceProfile.cpp` (`<target>Spec()`), `core/include/wheelbri/HidWheelDevice.hpp` (`VirtualWheel`
  entry), `HidPid.hpp`, `HidPp.hpp`, `HidPpFfb.hpp`, `TrueForce*.hpp`, `LogiSdk.hpp`, `EmulatedJoystick.hpp`
  (shim), `tools/uhid/UhidWheel.hpp` (Linux), `driver/Driver.cpp` for what only the driver answers.
- Grep first, read only the sections needed.

New target: read the closest existing target the same way; it is the template.

## 3. Pick the reference game

The reference game is the one whose support of this wheel is the most complete and best implemented: it should
exercise everything the wheel offers (all axes and buttons, shifter, clutch, rotation set by the game, every FFB
effect it can play, LEDs, Logitech's SDK, HID++ / G HUB paths, TrueForce, mode switches).

- List the wheel's capabilities (from section 4.1-4.4) as rows; list candidate games as columns; mark which game
  uses which capability, with the source (game's preset files, docs, Logitech's supported-games list, captures).
- Candidates: games the user owns first (`docs/games.md`); DiRT Rally 2.0 is the current reference for Logitech
  wheels, ACC for TrueForce. Suggest others only if they cover something these don't.
- One game covering everything: it is the reference. Otherwise the smallest set that does, with which game proves
  which capability. Capabilities no game uses: listed as "not exercised", lowest priority, cannot be validated.
- The user confirms the choice before section 5.

## 4. Wheel footprint (the real wheel)

Separate each **mode** (G920 Xbox / PC mode, G923 Xbox vs PlayStation, G25/G27 compatibility vs native): each mode
is its own footprint.

Sources, in this order: Logitech's own (logitechg.com, for the G920 the Xbox version, never the G29; manuals, FAQs,
INFs in LGS / G HUB installers, Steering Wheel SDK and TrueForce SDK docs and headers; support archive for old
wheels); real-device dumps (linux-hardware.org probes, `usb.ids`, published `lsusb -v`, `usbhid-dump`, USBPcap
captures); open drivers and tools, **facts only, nothing copied** (Linux `hid-lg4ff.c`, `hid-logitech-hidpp.c`,
`hid-lg.c`, new-lg4ff, Oversteer: GPLv2; GIMX: GPLv3; flag to the user before any code would be taken); games'
data; community reports last (need a second source). A page with a CAPTCHA: send the user the link. Keep a source
list (URL + what it gave).

### 4.1 Identity

- USB: VID, PID per mode, `bcdDevice` / firmware, `bcdUSB`, class/subclass/protocol, power, speed; strings
  (manufacturer, product, serial, interfaces, languages); interfaces and order, endpoints (size, interval),
  container id, extra functions (Xbox GIP, audio, HID++ collection, TrueForce `0xFFFD`).
- Windows: hardware and compatible ids, the binding INF, class, friendly name, `OEMName` / `OEMForceFeedback` under
  `HKCU\System\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\VID_046D&PID_xxxx`,
  DirectInput `guidProduct`, instance name, `DIDEVCAPS`, `dwDevType`, `Windows.Gaming.Input.RacingWheel`, XInput.
- Linux: binding kernel driver, evdev name and `id`, sysfs attributes (`range`, `alternate_modes`,
  `combined_pedals`, autocentre), hidraw nodes, what Proton and Steam report.

### 4.2 Channels and outbound messages (wheel → host)

- Every channel: HID interfaces, HID++, vendor streams (TrueForce), Xbox GIP, anything else.
- Full report descriptor of every interface, byte for byte, decoded.
- Input report layout per mode (axes: resolution, sign, rest, direction; buttons in HID order; hat; pedals separate
  or combined; shifter; dials), report rate, on-change vs continuous.
- Feature reports answered; HID++ features, indexes, answers, notifications, errors; vendor stream packets.

### 4.3 Inbound messages, calls and signals (host → wheel), with behaviour

One row each: bytes / call, meaning, who sends it (game via DirectInput, Logitech's driver, G HUB / LGS, SDK,
kernel), **what the wheel does**: reply (bytes), state change, physical effect (force, centring, LEDs), timing,
limits and clamping, error or ignore on bad input, re-enumeration or PID change. Covers: output and feature
reports, FFB (HID PID effects, block load, pool, device control; or Logitech's classic commands: slots,
autocentre, range `0xF8`, mode switch, LEDs), HID++ (0x8123 ...), TrueForce, gain, rotation set / get, combined
pedals, calibration, reset.

### 4.4 APIs and callable interfaces on top

- Logitech's Windows driver: axis relabelling (`WmJoyHid.inf`), button order with / without it, range via
  DirectInput `Escape` / properties, control panel settings that change what games see.
- G HUB / LGS: what they read and set, registry entries they own, what makes them show the wheel as active.
- Logitech Steering Wheel SDK and TrueForce SDK: every exported function, what each sends to the wheel, what it
  needs back (e.g. full-scale torque in status, else no TrueForce stream).
- DirectInput: `EnumEffects`, `DIPROP_*` answered, effect parameters honoured. Linux: `EV_FF` caps, `ff-memless`,
  sysfs controls.

## 5. Game footprint (the reference game)

Captured from the game running, against WheelBri's target (the closest existing one for a new target), never from
reading about it alone. Give the user short step-by-step instructions, one command per code block; filter output,
never dump whole logs.

- **Detection:** how it finds and picks the wheel (name string, VID/PID, `guidProduct`, `LogiIsModelConnected`,
  WGI), what makes it choose its preset, what it does when a detail differs.
- **Channels used:** DirectInput, Logitech SDK, TrueForce SDK, raw HID / hidraw, HID++ through G HUB, XInput, WGI.
- **Every call and message**, with arguments, values, order, rate, and the answer the game expects, per phase:
  start, wheel detection, menus, driving, pause, car / track change, settings (rotation, FFB strength), exit,
  wheel unplug / replug.
- **Its preset:** axis bindings, button numbers, FFB mode, rotation, LED use, and where it is stored.
- Tools: shim with `emulate = false`, `logi_log = true` (`wheelbri-shim-*.log`: every SDK call); driver logs
  (output reports, HID++, `trueforce:`); service and `ffb-*` logs in `%LOCALAPPDATA%\wheelbri\logs`;
  `scripts/telemetry_summary.sh`; Linux `evtest` and hidraw. Missing a capture: propose diagnostic tooling (gate
  rule above).
- Repeat a capture at least twice; note what changed between runs.

## 6. Trace matrix

One row per thing the game does:
`Game call / message | Phase | Channel | Wheel message(s) it causes | Real wheel's behaviour | WheelBri now | Proof`.
Plus a row per wheel capability the game does not use ("not exercised"). Differences become the gap list.

## 7. The report

Write `Claude outputs/spikes/<target>-spike.md` (working notes, not shipped docs):

1. Summary: target, modes, reference game(s), the findings that matter most, **gate status** (each box with why).
2. Reference-game choice: the capability × game table.
3. Wheel footprint (4.1-4.4) and game footprint (5): one fact per line, with source and proof level.
4. Trace matrix.
5. Gap table: `Item | Real wheel | WheelBri now | Effect on the game | Proposed change | Size`.
6. Open questions, each with how to close it (which capture, which game, which page).
7. Proposed steps (only once the gate passes), each a small commit, in order, with the CLAUDE.md checklists they
   touch (new target = `DeviceProfileSpec` in `makeProfile()` + `VirtualWheel` entry; `config/<target>.ini`;
   `StatePacket` / config key / helper / fuzz target checklists when hit; `docs/targets.md`; tests). UI changes
   get sketches first.

Send the user the report path and the summary. Gate not passed: say what is missing and the next capture or source
to get it, then keep investigating. Gate passed: wait for the user to confirm and pick the steps.

## 8. After the gate

- Every change is checked in the reference game against the trace matrix: same calls, same answers, same
  behaviour as the real wheel. A difference reopens that row.
- Only confirmed facts go into `docs/targets.md` (short, current, with sources as its tables do).
- Nothing from Logitech's DLLs is shipped or copied; nothing from GPL code is copied.
- Each step: edit in the repo, build, then tests as a separate step (errors and failures only), then ready-to-run
  git commands for one small commit.
