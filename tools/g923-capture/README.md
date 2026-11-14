# G923 capture kit

This kit captures a real Logitech G923 (Xbox One and PC, `046D:C26E`) and decodes what WheelBri's virtual G923
needs:

- the TrueForce parameter answers (0x06 above all);
- the status report's layout and full scale;
- HID++ answers and the feature list;
- the USB and HID descriptors.

The first capture (2026-11-01) is behind the real G923's values in `core/src/TrueForce.cpp`; its raw files stay out of
the repository (`captures/` is git-ignored: they carry the wheel's serial number).

Everything runs from the MSYS2 UCRT64 shell at the repository root, through `scripts/make/g923-capture.sh`.
Sessions open an Administrator window (USB capture needs it). Do the steps in that window, then come back.

## Before the session (once)

1. Install **Wireshark** with **USBPcap** ticked in the installer, then **restart Windows**.
2. Run the kit's own tests (no wheel needed): `scripts/make/g923-capture.sh test`. This builds `wheelbri_tests` and
   runs the `G923Cap*` tests. The decoder, `wheelbri-g923cap`, is built by each command that uses it: nothing else to
   install.
3. Optional: `scripts/make/g923-capture.sh check` with things as they are now. It lists in red what to change before the session.

## The session (about an hour)

| # | Do | Command | You do |
|---|---|---|---|
| 0 | WheelBri: **Stop**, quit the app. Unplug the **R3**. Quit the **R3's own software**. Plug in the **G923**. | `scripts/make/g923-capture.sh check` | Fix anything red, run it again until it ends green. It notes the wheel's USB port: keep using that port. |
| 1 | Plug-in + G HUB | `… session S1` | Unplug the wheel when asked. Plug it back into the same port. Then the G HUB clicks it lists. |
| 2 | ACC with TrueForce | `… session S2` | ACC starts by itself. Then: menu → Hot Lap → hold the wheel at the 4 angles asked (5 s each, the window counts down) → sweep → 1-2 min of driving → quit. |
| 3 | Probe, G HUB open | `… probe` | Nothing. Read-only requests, about 30 s. |
| 4 | Probe, G HUB closed | quit G HUB from the tray, then `… probe` | Nothing. |
| 5 | ACC, G HUB closed | `… session S3` | It closes G HUB, then answers whether there is force, before and after G HUB comes back. |
| 6 | DiRT Rally 2.0 | `… session S4` | Skips itself if the game has no TrueForce SDK. Otherwise: one short stage. |
| 7 | NFS Heat | `… session S5` | 2 min of driving. |
| 8 | Decode everything | `… decode` | Read `captures/g923/<date>/report.md`. |
| 9 | Put things back | `… restore` | Say whether to remove USBPcap. Then plug in the R3, start WheelBri, and run `… check --wheelbri`. |

`…` = `scripts/make/g923-capture.sh`.

Each session ends with PASS/FAIL lines and a `summary.md`. If a session fails, run it again: a new folder `-2` is
made, and nothing is overwritten. Press `s` instead of Enter to skip a step that doesn't apply (for example a G HUB
switch that isn't there).

## What lands where

`captures/g923/<date>/` (git-ignored):

- `S0-check/`
  - `check.md`
  - `registry-HKCU|HKLM-before.reg`, and after `restore`, `-after.reg`
- `S1-…` to `S5-…`, each with:
  - `capture.pcap`: only the G923's frames. The raw captures are deleted once filtered, because other devices on
    the same hub (a keyboard) must not stay in them.
  - `notes.txt`: every step with its time; the holds the decoder uses.
  - `transcript.log`
  - `wheelbri-shim-*.log`: the game's SDK calls (S2-S4).
  - `summary.md`, `stream.tsv`
- `S6-probe/probe-ghub-running.md`, `probe-ghub-closed.md`
- `descriptors-VVVV-PPPP.txt`: the wheel's descriptors, read once and reused.
- `report.md`: all sessions, with the parameters across sessions next to WheelBri's current values (differences in
  bold).
- `state.txt`, `shim-state.txt`: what to put back.

Open any `capture.pcap` in Wireshark to look by hand. Frame numbers in the summaries are Wireshark's.

## Safety

- **WheelBri's `trueforce` setting:** don't switch it off while the real wheel is plugged in. WheelBri would remove
  Logitech's `OEMForceFeedback` entry for `C26E`, and the real wheel would lose force feedback in DirectInput games.
  The kit only exports that registry key; it never changes it.
- **The probe only reads.** On TrueForce it sends only type `0x05`. On HID++ it sends only Root, FeatureSet,
  DeviceFwVersion and DeviceNameType reads. Both lists are fixed (`kSafeTrueForceTypes` in
  `include/g923cap/Probe.hpp`, `safeHidpp()` in `src/Probe.cpp`), checked before every write, and pinned by tests
  (`G923Cap.ProbesEveryParameterReadOnly`, `G923Cap.ProbesHidppReadOnly`).
- **The shim** goes into ACC and DiRT through `shim.sh`, set to `emulate = false` and `logi_log = true`. `restore`
  puts the earlier `wheelbri-shim.ini` back, and takes the shim out where there was none before.

## Files

| File | What |
|---|---|
| `g923-capture.ps1` | Checks, guided sessions, restore (PowerShell 5.1, elevates itself) |
| `src/main.cpp` | `wheelbri-g923cap decode\|locate\|keep\|probe ...` |
| `src/Pcap.cpp` | USBPcap pcap/pcapng reader, transfers |
| `src/UsbDescriptors.cpp` | USB configuration and HID report descriptors |
| `src/Protocol.cpp` | TrueForce packets and status fields (as `wheelbri/TrueForce.hpp` has them), HID++ pairing and names |
| `src/Analysis.cpp` | Decoding a session |
| `src/Reports.cpp` | The checks, `summary.md`, `report.md` (WheelBri's values from `TrueForceDevice::parameterBits()`); `locate`, `keep` |
| `src/Probe.cpp`, `src/ProbeWindows.cpp` | The read-only probe; Windows HID (`hid.dll`, `setupapi.dll` loaded at run time) |
| `tests/G923CapTest.cpp` (repo root) | A fake G923 that answers with the real wheel's values, and the tests on it |
