# wheelemu-bridge (Phase 5, step 0)

A Windows-level virtual "Logitech G920", made with [HIDMaestro](https://github.com/hifihedgehog/HIDMaestro),
fed by `wheelemu-service`. It answers the two questions of
[docs/phase5-windows-virtual-device.md](../../docs/phase5-windows-virtual-device.md): does NFS accept it
without `dinput8.dll`, and does force feedback through Windows' own path feel like the shim's?

```
Moza R3 ─► wheelemu-service ─UDP 41329─► wheelemu-bridge ─► virtual G920 (HIDMaestro) ─► NFS
                  ▲                              │
                  └────────UDP 41330─────────────┘  the game's force feedback (HID PID reports)
```

The service does not change: the bridge speaks the shim's protocol. **Use the bridge or the shim, never
both** (both listen on port 41329).

## What the virtual G920 looks like

- VID `046D`, PID `C262`, DirectInput name "Logitech G920 Driving Force Racing Wheel USB", device type
  "driving" (the same identity the shim reports). `joy.cpl` names the axes itself for a driving device (its
  "Brake" bar is our clutch, "Accelerator" our throttle; the brake is the box's up/down): games read the
  axes by position, as with the shim. `--axis-names` writes our own names, but is experimental: written that
  way they made the G920 disappear from DirectInput.
- Axes as NFS reads them from the shim: wheel X, brake Y, clutch Rz, throttle Slider (pedals released at
  the top of the range), 11 buttons, a hat.
- Force feedback: HID PID. Windows' `pid.dll` turns the game's DirectInput effects into PID reports; the
  bridge decodes them (constant, ramp, periodic, spring, damper, inertia, friction; durations, loops,
  device gain) into the service's 8 slots. Envelopes are ignored, as in the shim. Logitech's software
  registers its own force feedback driver for every G920 (it works only with a real one: every effect fails
  with "not implemented"); the bridge removes that entry at start and whenever it comes back.

## Install (once)

1. **.NET 10 SDK** (x64): https://dotnet.microsoft.com/download/dotnet/10.0
2. **HIDMaestro.Core.dll**, into `third_party\hidmaestro\` in this repo (the folder is git-ignored):
   - if the [HIDMaestro releases](https://github.com/hifihedgehog/HIDMaestro/releases) page offers
     `HIDMaestro.Core.dll`, download it;
   - otherwise build HIDMaestro: Visual Studio 2022 with "Desktop development with C++" plus the
     **MSVC x64 and ARM64** build tools (its script builds both), the Windows SDK and WDK 10.0.26100, then in a
     *Developer Command Prompt*: `git clone https://github.com/hifihedgehog/HIDMaestro`, `cd HIDMaestro`,
     `scripts\build_all.cmd`. Copy `sdk\HIDMaestro.Core\bin\Debug\net10.0-windows10.0.26100.0\HIDMaestro.Core.dll`.
3. **HidHide** (hides the real R3 from games): https://github.com/nefarius/HidHide/releases, reboot when
   asked. Nothing to configure: while the bridge runs it adds the running `wheelemu-service.exe` to HidHide's
   allowed applications, hides the R3's game-controller interface (other interfaces, used by Moza Pit House,
   stay visible) and switches hiding on; on exit it puts everything back as it was.
4. Build the bridge (MSYS2 or any shell with `dotnet`):
   ```
   dotnet build tools/vbridge/wheelemu-bridge.csproj -c Release
   ```
   Optional check of its protocol code: `dotnet run --project tools/vbridge/SelfTest`.

## Test

1. Take the shim out of the game: rename `dinput8.dll` in the NFS folder to `dinput8.dll.off`.
2. Start `wheelemu-service` as usual. The bridge hides the R3 only once the service runs (the service must
   stay allowed to read it); started first, the bridge waits up to 60 s for the service.
3. Start `tools\vbridge\bin\Release\net10.0-windows10.0.26100.0\wheelemu-bridge.exe` (it asks for
   Administrator; the first run installs HIDMaestro's user-mode driver with a self-made certificate).
4. `joy.cpl` (Win+R; reopen it after starting the bridge): "Logitech G920 Driving Force Racing Wheel USB" is
   listed, the R3 is not. *Properties*: the box moves with the wheel (left/right) and brake (up/down), the
   bars with throttle and clutch; buttons and the hat move with the R3.
5. NFS: controls work, then drive and feel the force feedback.
6. Stop the bridge with Ctrl+C or by closing its window: the virtual G920 disappears and the R3 is visible
   again.

Options: `--no-hide` (leave HidHide alone), `--no-driving-type`, `--axis-names` (experimental), `--service-exe PATH`, `--hidhide-cli PATH`,
`--wheel-vid 346E`. After a crash that left the R3 hidden: `wheelemu-bridge --unhide`.

Logs: `%LOCALAPPDATA%\wheelemu\logs\wheelemu-bridge-<date>.log` (the first PID report of each kind is
written in full, to check the decoding) next to the service's log and telemetry.

## Force feedback without a game

`FfbProbe` plays forces on the virtual G920 through DirectInput, as a game does: pull right, pull left,
vibration, spring. With the service and the bridge running and the game closed:

```
dotnet run --project tools/vbridge/FfbProbe
```

It reports whether DirectInput offers force feedback for the G920 and whether each effect is created and
played, on screen and in `%LOCALAPPDATA%\wheelemu\logs\ffbprobe-<time>.txt`. If the wheel moves here but not
in a game, the game is not sending force feedback to the device; if not, Windows' path to the bridge is broken.

## Undo

Stop the bridge (it un-hides the R3 itself), rename `dinput8.dll.off` back. To remove
HIDMaestro's driver too: Device Manager → *Human Interface Devices* / *Software devices* → HIDMaestro entries
→ Uninstall device (tick "delete the driver").
