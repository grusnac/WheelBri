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

## Everyday use

1. Take the shim out of the game once: rename `dinput8.dll` in the NFS folder to `dinput8.dll.off`.
2. Double-click `tools\vbridge\start-g920.cmd` (asks for Administrator). It starts `wheelemu-service` with
   its window if it is not running yet, then the bridge in its own window. Options of `start-g920.ps1`:
   `-NoUi`, `-StopService` (stop the service with the bridge), `-Config path`, `-XboxPad buttons|idle`
   (see [Xbox button prompts](#xbox-button-prompts)).
   From an MSYS2 UCRT64 shell instead: `scripts/wheelemu.sh` starts both (service with its window, bridge
   with the Xbox controller in `buttons` mode, in its own Administrator window) and stops both cleanly when you
   press Enter (or Ctrl+C) there; `--build` builds both first, `--no-ui`, `--xbox-pad idle|off`,
   `--config FILE`. Both programs take `--stop-file PATH`: they stop as with Ctrl+C when that file appears.
3. The bridge logs `game controller ready` once Windows lists the G920 (joy.cpl: "Logitech G920 Driving
   Force Racing Wheel USB", not the R3).
4. Start Steam and NFS; drive.
5. Stop: Ctrl+C in the bridge window. Best close NFS and Steam first; if a program still has the G920
   open, the bridge says which one and waits (`waiting: NFS16.exe has the virtual G920 open`) and finishes by
   itself once it is closed. Ctrl+C a second time removes it anyway. Closing the bridge window instead of
   Ctrl+C leaves a held G920 in place (no time to wait); the next start replaces it.
6. If a G920 was still removed while held (Ctrl+C twice, an older bridge, a crash), Windows keeps it
   half-removed until it restarts and a new G920 there gets no driver. The bridge notices at start and uses a
   spare identity instead (`trying identity wheelemu-g920-spare-1 instead, no restart needed`); it remembers
   the working one in `%LOCALAPPDATA%\wheelemu\bridge-identity.txt`.

The manual way: start `wheelemu-service`, then
`tools\vbridge\bin\Release\net10.0-windows10.0.26100.0\wheelemu-bridge.exe` as Administrator (the first run
installs HIDMaestro's user-mode driver with a self-made certificate). Started first, the bridge waits up to
60 s for the service (it hides the R3 only once the service runs, which stays allowed to read it). Only one
bridge runs at a time.

Options: `--no-hide` (leave HidHide alone), `--no-driving-type`, `--axis-names` (experimental), `--service-exe PATH`, `--hidhide-cli PATH`,
`--wheel-vid 346E`, `--xbox-pad [buttons|idle]`. After a crash that left the R3 hidden: `wheelemu-bridge --unhide`.

Logs: `%LOCALAPPDATA%\wheelemu\logs\wheelemu-bridge-<date>.log` (the first PID report of each kind is
written in full, to check the decoding) next to the service's log and telemetry.

## Xbox button prompts

With only a wheel, NFS 2015 shows keyboard keys in its menus. `--xbox-pad` (`start-g920.ps1 -XboxPad ...`)
adds a virtual Xbox 360 controller next to the G920, so the game can show Xbox buttons. Its sticks and
triggers never move: it cannot steer or drive.

- `idle`: connected, never pressed. Try this first: enough if the game looks only at which controllers exist.
- `buttons`: the wheel's A, B, X, Y, Menu, View, stick clicks and D-pad also press the controller's, so a game
  that shows the prompts of the controller used last sees an Xbox controller. Only while no pedal is pressed and
  for 2 s after (in menus the pedals rest; while driving a wheel button must not also fire the game's Xbox
  controller binding). The paddles and the Xbox button are never passed on. In menus the D-pad goes to the
  controller only, not to the G920: NFS also navigates with the G920's D-pad, and that put the keyboard prompts
  back after every step. Turning the wheel did the same, so in a menu the G920's steering stays where it was.
  For the steering "in a menu" is stricter: the game plays its force feedback effects at zero for 1.5 s (NFS
  does that in menus; while driving its spring and damper alone are 0.4 to 0.9) AND no pedal for 2 s. Any
  force or pedal gives the steering back at once; the bridge logs both changes. Pressing a pedal in a menu
  still brings the keyboard prompts back (a pedal means driving).

If a menu moves two steps per press in `buttons` mode, the game reads both controllers: use `idle`. The bridge logs `virtual Xbox 360 controller created` and, in
`buttons` mode, the first button press it passed on.

## Force feedback without a game

`FfbProbe` plays forces on the virtual G920 through DirectInput, as a game does: pull right, pull left,
vibration, spring. With the service and the bridge running and the game closed:

```
dotnet run --project tools/vbridge/FfbProbe
```

It reports whether DirectInput offers force feedback for the G920 and whether each effect is created and
played, on screen and in `%LOCALAPPDATA%\wheelemu\logs\ffbprobe-<time>.txt`. If the wheel moves here but not
in a game, the game is not sending force feedback to the device; if not, Windows' path to the bridge is broken.

## Test clutch

No clutch pedal yet? `ClutchProbe` sends a clutch position to `wheelemu-service`, which uses it instead of
the mapped clutch (its `--ui` window and telemetry show it too, and it reaches the game through the bridge or
the shim), and reads the G920's clutch (Rz) back through DirectInput. The service must be running; the game may
run too.

```
dotnet run --project tools/vbridge/ClutchProbe
```

Modes: `steps` (default: 0/25/50/75/100 %, each checked), `sweep [times]` (watch the bar in `joy.cpl`),
`hold <percent> [seconds]` (try the clutch in a game), e.g. `dotnet run --project tools/vbridge/ClutchProbe -- hold 60 45`.
While it runs the mapped clutch is ignored; 1.5 s after it stops the pedal's clutch is used again. Port:
UDP 127.0.0.1:41331. Report: `%LOCALAPPDATA%\wheelemu\logs\clutchprobe-<time>.txt`.

## Undo

Stop the bridge (it un-hides the R3 itself), rename `dinput8.dll.off` back. To remove
HIDMaestro's driver too: Device Manager → *Human Interface Devices* / *Software devices* → HIDMaestro entries
→ Uninstall device (tick "delete the driver").
