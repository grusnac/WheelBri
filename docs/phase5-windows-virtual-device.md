# Phase 5 design: a virtual G920 on Windows

Status: **step 0 works** (`tools/vbridge`, 2026-09-30): NFS 2015 without `dinput8.dll` drives the virtual G920,
force feedback feels as with the shim, the R3 is hidden by HidHide. Both step-0 questions are answered yes.

## Goal

Today the G920 exists only inside NFS: `dinput8.dll` in the game folder answers DirectInput calls as a
G920. Phase 5 makes Windows itself show a G920 (in `joy.cpl`, to every game, through every API that reads
HID: DirectInput, raw HID, SDL), with its force feedback, fed by the same `wheelemu-service`.

```
today      Moza R3 ──SDL3──► service ──UDP──► dinput8.dll inside each game ──► game
phase 5    Moza R3 ──SDL3──► service ──UDP──► virtual G920 driver ──HID──► Windows ──► every game
           (R3 hidden from games by HidHide; the service is allowed to see it)
```

Worth it when: more than one game, games that do not load `dinput8.dll`, or not wanting a DLL in game
folders. Not worth it for NFS alone: the shim already works there.

## What has to be true

| Topic | Finding |
|---|---|
| Driver kind | A **UMDF 2 HID minidriver** (user mode). It sits below Microsoft's `MsHidUmdf.sys` pass-through, answers the HID class driver's requests (report descriptor, device attributes, read/write reports, get/set feature). Microsoft's `vhidmini2` sample is the recommended starting point ([docs][umdf-hid]). A crash takes down the driver host process, not Windows. |
| Signing | The kernel parts (`MsHidUmdf.sys`, `WUDFRd.sys`) are Microsoft's, already signed. Our package (INF, DLL, catalog) is signed with a **self-made certificate trusted on this PC** (placed in the machine's Root and TrustedPublisher stores). **No test mode, Secure Boot stays on.** HIDMaestro installs this way ([HIDMaestro][hidmaestro-repo]). *Correction:* an earlier message said test signing with Secure Boot off would be needed; that applies to kernel drivers, not to this one. Still to confirm on your Windows edition (HIDMaestro lists Windows 10/11 IoT LTSC as tested). |
| Identity | The driver chooses VID `046D`, PID `C262`, product string and HID report descriptor, so DirectInput derives the same product GUID and name the shim reports today. DirectInput reports "driving" device type from the registry's joystick OEM entry for `VID_046D&PID_C262` (to verify: installer writes it, as Logitech's software does). |
| Force feedback | A real G920 does FFB through Logitech's driver (HID++), which we cannot use. The virtual G920 declares **HID PID** (usage page 0x0F) instead: Windows' built-in PID driver then offers DirectInput FFB for it and turns the game's effects into PID output and feature reports, which our driver decodes. vJoy and HIDMaestro use the same mechanism. |
| The real R3 | Must be hidden from games, or a game may take the R3 instead of the G920. **HidHide** ([Nefarius][hidhide]), signed and free, hides it from everyone except whitelisted programs: `wheelemu-service.exe` stays on the list. |
| Logitech G HUB | If installed, it may try to talk to anything with Logitech's VID. Check at the first test; exit G HUB if it interferes. |
| Toolchain | Visual Studio 2022 (Community) + Windows SDK + WDK on your PC (roughly 10 GB). Drivers cannot be built with MSYS2/MinGW or in the Linux sandbox; the rest of the project stays on MSYS2. |

## Two ways to build it

| | A. On top of HIDMaestro | B. Our own UMDF driver |
|---|---|---|
| What | MIT-licensed C#/.NET SDK + UMDF driver that creates virtual controllers with any VID/PID and descriptor, HID PID FFB, self-signed install ([site][hidmaestro-site]) | `driver/` in this repo, based on `vhidmini2` |
| Our code | A small C# bridge process: our UDP state → HIDMaestro input; its FFB output → our `FfbPacket` to the service | The driver itself + a PID decoder in `core/` |
| Effort | Small: days | Larger: the driver, the PID protocol, the installer |
| Risk | Young project (tens of stars), .NET 10 needed, tested editions are IoT LTSC, no G920 profile listed (we would supply our descriptor), another process to run | Everything is ours to debug; every build and test happens on your PC |
| Control | Limited to what the SDK exposes | Full; same C++ and logging as the rest |

## Recommended plan

**Step 0 — prove it with HIDMaestro first (A), before writing a driver.** It answers the two questions
that decide everything, cheaply:
1. Does NFS 2015 accept a HID-level G920 (no `dinput8.dll` in the folder)?
2. Does force feedback through Windows' PID driver reach the service with the same quality as the shim?

If both are yes: keep A if it is good enough, or build B to own the code. If NFS rejects it: stop,
the shim stays the answer for NFS, and Phase 5 only matters for other games.

**Steps for B** (each ends with something you can check on your PC):

| Step | Work | Check |
|---|---|---|
| B1 | Toolchain; `driver/` project from `vhidmini2` (UMDF 2); self-signed install script | Driver installs with Secure Boot on; device appears in Device Manager |
| B2 | G920 identity and input: VID/PID/name, HID descriptor with the layout NFS already reads (wheel X, brake Y, clutch Rz, throttle Slider, 11 buttons, hat); the driver listens to the service's state on 127.0.0.1:41329 exactly like the shim | `joy.cpl` shows "Logitech G920 Driving Force Racing Wheel USB"; every axis and button moves |
| B3 | Hide the R3 with HidHide, whitelist the service; installer writes the joystick OEM entry | NFS with **no** `dinput8.dll` sees only the G920 and drives |
| B4 | HID PID descriptor and reports (pool, create/load/free effect, set effect/constant/condition/periodic/envelope, effect operation, device control and gain); decoded into `FfbEffect` and sent as `FfbPacket` to 41330: **the service does not change** | An FFB test tool, then NFS: forces as with the shim, telemetry identical in shape |
| B5 | Logging (daily file in `C:\ProgramData\wheelemu\logs`, cleaned by the service's `[retention]`), uninstall script, README | Clean install and uninstall; logs show up in the `--ui` panel |

## What step 0 taught us

Things that were not in the plan and cost time; B must handle them too.

| Finding | Handling |
|---|---|
| Logitech's software registers its own force feedback driver for VID 046D PID C262 under `HKCU\...\Joystick\OEM\VID_046D&PID_C262\OEMForceFeedback` (CLSID `{4DD075CB-CCF0-4A26-B970-2ACD092662DB}`). It works only with a real G920: every DirectInput effect call returns `E_NOTIMPL`. | The bridge deletes that entry at start and every 2 s; DirectInput then uses Windows' PID driver (`{EEC6993A-B3FD-11D2-A916-00C04FB98638}`). `FfbProbe` and `--test-id` (1209:0001) tell the two apart. |
| Windows' `pid.dll` sends the type-specific reports (constant, periodic, condition) with effect block index 0, before the Set Effect naming the real block; the condition's parameter-block nibble is `0xF`. | `PidForwarder` keeps EBI-0 parameters pending for the next Set Effect of the same kind, or applies them to the last block of that kind (magnitude updates). |
| Steam Input holds the virtual device open and vetoes its removal; the next device then got no driver. HidHide's install needs a reboot. | Disable Steam Input for the game; quit Steam before stopping the bridge; reboot after installing HidHide. |
| An old HidGuardian entry was listed as a HID class filter. | Removed from `UpperFilters` (HidHide replaces it). |
| After a reboot HidHide's gaming list may not show the R3 at first. | The bridge logs what HidHide lists; start the service, then the bridge. |

## Design of B

- **Reuse the wire format.** The driver speaks the shim's protocol in both directions (`StatePacket` in,
  `FfbPacket` out), so the service, its safety limits, watchdog, telemetry and UI stay as they are.
  Only one of the two runs for a game: the shim or the driver.
- **Keep the logic portable.** New `core/` code, unit-tested on Linux like the rest:
  - `HidG920Descriptor`: builds the report descriptor and packs a `TargetReport` into an input report;
  - `HidPid`: a PID effect-block table that takes the game's output/feature reports and produces
    `FfbEffect` slots (the PID counterpart of `EmulatedEffect` for DirectInput).
  The Windows-only driver stays a thin shell: WDF queues, IOCTLs, a UDP thread.
- **Latency.** The driver completes pending HID read requests as soon as a state packet arrives (the service
  sends 1000 per second), so input latency stays at the shim's level.
- **Safety.** Same rule as the shim: no state from the service for 1.5 s → centred wheel, released pedals;
  the service's FFB watchdog still stops every force when the driver goes quiet.

## Risks and open questions

1. NFS may insist on something only the real Logitech stack provides (it sends Logitech-specific
   `Escape` calls today, which the shim refuses without harm). Step 0 answers this.
2. Windows' PID driver translates DirectInput effects into PID reports its own way (timing, envelopes,
   effect updates): FFB may feel different from the shim path. Step 0 answers this too.
3. Self-signed install on regular Windows 10/11 Home/Pro (not IoT LTSC): expected to work for UMDF, to confirm in B1 / Step 0.
4. Kernel anti-cheat in some other games can see virtual devices; NFS 2015 is not affected as far as known.

## What you would install or change

- Step 0: .NET 10 SDK, HIDMaestro, HidHide. No security settings change. If HIDMaestro's releases do not
  offer a prebuilt `HIDMaestro.Core.dll`, building it needs Visual Studio 2022 with the WDK, the same
  toolchain B needs. The bridge and its test steps: [tools/vbridge/README.md](../tools/vbridge/README.md).
- B: additionally Visual Studio 2022 Community with the WDK.

[umdf-hid]: https://github.com/MicrosoftDocs/windows-driver-docs/blob/staging/windows-driver-docs-pr/wdf/creating-umdf-hid-minidrivers.md
[hidmaestro-repo]: https://github.com/hifihedgehog/HIDMaestro
[hidmaestro-site]: https://hidmaestro.org/
[hidhide]: https://github.com/nefarius/HidHide
