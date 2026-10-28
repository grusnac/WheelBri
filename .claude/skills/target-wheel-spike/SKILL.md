---
name: target-wheel-spike
description: WheelBri only. Use before adding a new target wheel (a Logitech wheel WheelBri presents to games) or improving an existing one (G25, G27, G920, G923) - deep investigation of the real wheel's whole footprint, identity, every inbound and outbound message and every API exposed on top of it, compared with what WheelBri presents today.
---

# Target wheel spike

A spike is investigation only: it ends with a report and a gap list, never with code. Implementation starts only
after the user has read the report and picked what to do.

Goal: know the **real** target wheel well enough that a game, Windows, Linux, G HUB / Logitech Gaming Software or
Logitech's SDK cannot tell WheelBri's target from it, in the parts that matter to games. Every fact carries its
source and how sure it is.

## 0. Before starting

- Root `CLAUDE.md` rules apply; read the area file of every area the spike touches (driver, in-game DLL, tests).
- Ask the user (one short question set): which wheel; new target or improving one; what triggered it (a game that
  misbehaves, a missing feature, a whole new wheel); any part to skip. Name the report file from the answer.
- The user has **no real Logitech wheel** (rig: Moza R3). Facts about the real wheel come from sources (section 2);
  facts about WheelBri's target come from measuring it (section 4). Never write a guess as a fact.

## 1. What WheelBri has today (improving a target)

Read, and note per item what the target does now:

- `docs/targets.md` (the target's rows and sections), `config/<target>.ini`, game profiles in `config/games/*.ini`
  that mention it.
- `core/src/DeviceProfile.cpp` (`<target>Spec()`: identity, axes, ranges, buttons, native order, core buttons).
- `core/include/wheelbri/HidWheelDevice.hpp` (`VirtualWheel` entry: report descriptor, ids, strings, HID++, FFB),
  `HidPid.hpp`, `HidPp.hpp`, `HidPpFfb.hpp`, `TrueForce*.hpp`, `LogiSdk.hpp`, `EmulatedJoystick.hpp` (shim's view),
  `tools/uhid/UhidWheel.hpp` (Linux), `driver/Driver.cpp` for anything only the driver answers.
- Grep first, read only the sections needed (`grep -n`), to keep it small.

New target: read the closest existing target the same way; it is the template.

## 2. Sources for the real wheel, in this order

1. Logitech's own: logitechg.com product and support pages (for the G920 the Xbox version, never the G29), manuals,
   FAQs, driver INFs inside Logitech's installers (LGS, G HUB), Steering Wheel SDK and TrueForce SDK docs and headers.
   A wheel no longer listed there: Logitech's support archive.
2. Descriptor dumps and probes of the real device: linux-hardware.org probes, `usb.ids`, published `lsusb -v`,
   `usbhid-dump` and USBPcap captures (issue trackers, forums).
3. Open drivers and tools, **facts only, nothing copied** (licences: Linux kernel GPLv2 `hid-lg4ff.c`,
   `hid-logitech-hidpp.c`, `hid-lg.c`; new-lg4ff; Oversteer; GIMX GPLv3; SDL's controller database). Flag to the user
   before any code would be taken from them.
4. Games' own data: wheel presets and button maps shipped with games (ACC, DiRT Rally 2.0, GRID, NFS), Proton/Steam
   input mappings.
5. Community reports (forums, Reddit, Steam). Lowest weight; need a second source.

Web pages with a CAPTCHA: send the user the link; they get past it. Keep a source list (URL + what it gave).

## 3. The footprint to cover

Fill every item, or mark it "unknown" with what would settle it. Separate each **mode** the wheel has (G920 Xbox /
PC mode, G923 Xbox vs PlayStation models, G25/G27 compatibility "Driving Force EX" mode vs native mode): each mode
is its own footprint.

### 3.1 Identity

- USB: VID, PID per mode, `bcdDevice` / firmware versions seen, `bcdUSB`, class/subclass/protocol, power, speed.
- Strings: manufacturer, product, serial (present or not), interface strings, string language ids.
- Composite layout: interfaces and their order, endpoints (in/out, size, interval), container id, extra functions
  (Xbox GIP, audio, HID++ collection, TrueForce `0xFFFD`).
- Windows: hardware ids and compatible ids, the INF that binds (Logitech's or `input.inf`), device class, friendly
  name, `OEMName` / `OEMForceFeedback` under
  `HKCU\System\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\VID_046D&PID_xxxx`,
  DirectInput `guidProduct` (PIDVID) and instance name, `DIDEVCAPS` (axes, buttons, POVs, `DIDC_FORCEFEEDBACK`),
  `dwDevType` (wheel/driving subtype), `Windows.Gaming.Input.RacingWheel` (max angle, clutch, handbrake, shifter,
  motor), whether XInput sees it.
- Linux: the kernel driver that binds, evdev name, `id` (bus/vendor/product/version), sysfs attributes (`range`,
  `alternate_modes`, `combined_pedals`, autocentre), the hidraw nodes, what Proton (`PROTON_ENABLE_HIDRAW`) and
  Steam report.

### 3.2 Outbound (wheel → host)

- Full HID report descriptor of **every** interface, byte for byte, decoded: report ids, usages, logical/physical
  ranges, units, bit layout.
- Input report layout per mode: axes (resolution, signed/unsigned, rest value, direction), buttons in HID order,
  hat, pedals (separate / combined), shifter, dials, LEDs state if reported.
- Report rate and when reports are sent (on change, continuous, idle).
- Feature reports the wheel answers, and their contents.
- HID++: features and indexes (Root, FeatureSet, firmware, name, 0x8123 FFB, ...), answers, unsolicited
  notifications, errors for unknown features.
- TrueForce or any vendor stream: packet types, rates, status reports.

### 3.3 Inbound (host → wheel)

- Output reports and their ids.
- FFB protocol: HID PID (which effects, block load, pool, device control) or Logitech's own commands (classic
  FFB slots, autocentre, range `0xF8` commands, mode switch, LEDs), HID++ 0x8123, TrueForce. For each: bytes,
  meaning, who sends it (game via DirectInput, Logitech's driver, G HUB / LGS, SDK, kernel).
- Range / rotation set and get, gain, autocentre, combined pedals switch, mode switch, LED / RPM commands, any
  calibration or reset.
- What happens after each one (does the wheel re-enumerate, change PID, reply).

### 3.4 APIs exposed on top

- Logitech's Windows driver behaviour: axis relabelling (`WmJoyHid.inf`), button order with vs without the driver,
  range via DirectInput `Escape` / properties, Logitech control panel settings that change what games see.
- G HUB / LGS: what they read and set, registry entries they own, what makes them show the wheel as active.
- Logitech Steering Wheel SDK (`LogitechSteeringWheelEnginesWrapper.dll`) and TrueForce SDK
  (`trueforce_sdk_x64.dll`): every exported function games use (`LogiSteeringInitialize`, `LogiGetState`,
  `LogiIsConnected`, `LogiGetOperatingRange`, `LogiPlayLeds`, `logiTrueForce*`, ...), what each asks the device, and
  what the SDK needs back (e.g. full-scale torque in status, else no TrueForce stream).
- DirectInput properties and effects: `EnumEffects` list, `DIPROP_*` answered, effect parameters honoured.
- Linux: `ff-memless` or native FFB effects, `EV_FF` capabilities, sysfs controls.

### 3.5 How games find and use it

- How the games the user cares about detect it (name string, VID/PID, `guidProduct`, SDK `LogiIsModelConnected`).
- Their preset for it: axis bindings, button numbers, FFB mode (DirectInput, SDK, TrueForce), rotation.
- Known breakages when one detail differs (relabelled axes, wrong button order, missing HID++).

## 4. Measure WheelBri's current target (improving a target)

Evidence, not reading code alone. Give the user short step-by-step instructions for anything run on their PC; one
command per code block; filter output (grep, counts), never dump whole logs.

- Mode Virtual: `Get-PnpDevice` / `pnputil /enum-devices` for ids and properties, the `OEM` registry key,
  `joy.cpl`, driver log lines (`trueforce:`, HID++), service log in `%LOCALAPPDATA%\wheelbri\logs`.
- What games ask of the target: shim with `emulate = false`, `logi_log = true` (`wheelbri-shim-*.log`, SDK calls),
  driver logs of HID++ / output reports, `scripts/telemetry_summary.sh` for axes.
- Linux: `wheelbri-uhid`, `evtest`, the hidraw descriptor.
- Unit-level: the target's tests (`DeviceProfileTest`, `HidWheelDevice*`, `HidPp*`) show what is already pinned.

## 5. The report

Write `Claude outputs/spikes/<target>-spike.md` (working notes, not shipped docs). Sections:

1. Summary: what the target is, modes, the 3-5 findings that matter most.
2. Footprint: sections 3.1-3.5, one fact per line: fact, source (link or file), confidence:
   **Measured** (seen on the real wheel or in a game against WheelBri), **Logitech** (official),
   **Two sources**, **One source**, **Guess**.
3. Gap table: `Item | Real wheel | WheelBri now | Effect on games | Proposed change | Size`. New target: the
   WheelBri column is "none" and the table becomes the build list.
4. Open questions and how to answer each (which capture, which game, which page).
5. Proposed steps, each a small commit, in order, with the CLAUDE.md checklists they touch (new target =
   `DeviceProfileSpec` in `makeProfile()` + `VirtualWheel` entry; `config/<target>.ini`; `StatePacket` / config key /
   helper / fuzz target checklists when hit; docs/targets.md; tests). UI changes get sketches first.

Then stop, send the user the report path and the summary, and wait for their pick.

## 6. After the user decides

- Only confirmed facts go into `docs/targets.md` (short, current, with "from" sources as its tables do); guesses
  stay marked as guesses there or stay out.
- Nothing from Logitech's DLLs is shipped or copied; nothing from GPL code is copied.
- Each step: edit in the repo, build, then tests as a separate step (errors and failures only), then ready-to-run
  git commands for one small commit.
