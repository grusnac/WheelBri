# Linux test plan (plan, 2026-11-07)

Status: **in progress (step 0 done; step 1 done (item 3 steering values open); step 2: ACC G25, G920, G923, G27 done; Game Mode run done with G923; step 3 next).** Goal: prove WheelBri 1.0.1 on Linux beyond the one known result (ACC, Mode Virtual, under
Proton) and close the README's "not yet tried on a Steam Deck" gap.

Rig: Moza R3 base and pedals with the clutch add-on (VID/PID 0x346E/0x0005 for the base).

## What is known today (from the docs)

| What | State |
|---|---|
| ACC, Mode Virtual, Proton | works on this Steam Deck, Target G25 (2026-10-25); other Targets not tried |
| Steam Deck | ACC with a virtual G25 only |
| DiRT Rally 2.0 on Linux | never tried |
| NFS 2015, Heat, Unbound on Linux | never tried |
| NFS Payback on Linux | not expected to work (Proton has no Logitech DLL) |
| Race Driver: GRID on Linux | never tried |
| Mode Shim on Linux (`--shim`) | never tried in a game |
| G27, G923 on Linux | never tried (G25 in ACC only) |

## Step 0: preconditions (check, don't assume)

1. Which machine ACC was tested on (desktop Linux or the Deck): note distro and kernel.
2. On the Deck, Desktop Mode, Konsole:
   - `uname -r`: R3 force feedback needs Linux 6.15 or later (`hid-universal-pidff`). If older, FFB tests are
     blocked: record it and test only inputs.
   - `ldd --version`: the bundle needs glibc 2.39 or newer.
   - SteamOS version (Settings > System).
3. Hardware: the R3 and pedals reach the Deck through a powered USB-C hub or dock; both show in `lsusb`.
4. Install: unpack the 1.0.1 bundle, `install-udev-rules.sh`, log out and in; `id` shows group `wheelbri`;
   `/dev/uhid` is group `wheelbri`.

Stop here if any check fails; fix or record before step 1.

Resume here (2026-11-07):

1. Branch `test-plans` predates the `deck-setup.sh` fix: `git merge main` before setting the container up again.
2. Step 3: DiRT Rally 2.0.
3. Still to answer: G923 steering (rows 3, 5); Stop clean for rows 1, 3, 4; step 1 item 3 steering values.

Step 0 results (2026-11-07, Steam Deck):

| Check | Result | Pass |
|---|---|---|
| SteamOS | 3.8.28, build 20260922.1 | - |
| Kernel | 6.18.50-valve2-1-neptune-618 (>= 6.15) | yes |
| glibc | 2.41 (>= 2.39) | yes |
| lsusb | Moza R3 racing wheel and pedals seen | yes |
| Groups before install | deck steamos-log-submitter wheel (no `wheelbri` yet) | expected |
| `deck-setup.sh` | container glibc 2.44, SteamOS 2.41: pin to the archive did not take | fixed on `main` (1828969, 1f8201f): container glibc 2.41 |
| Install | local build 1.0.1 (28c34f9) via `deck-build.sh`, not the bundle: tests pass, programs load on SteamOS | yes |
| udev rules | `install-udev-rules.sh` rerun: rules in place, `/dev/uhid` root:wheelbri 0660, `id` lists `wheelbri` | yes |
| Earlier Deck runs | logs of 2026-10-25: Mode Virtual, virtual G25 over uhid, 4 game runs; the game was ACC (user) | yes |
| ACC result machine | this Steam Deck (user) | - |
| `--list` | `346e:0005 Gudsen R3 Racing Wheel and Pedals` | yes |

## Step 1: smoke test (Desktop Mode)

1. `~/wheelbri/bin/wheelbri` opens; Rig tab finds base and pedals; name the core controls.
2. Mapping tab: check axes (Linux numbers them differently from Windows); save.
3. `wheelbri-service --list` and `--print`: values move with the wheel and pedals.
4. `wheelbri-service --ffb-test`: the gentle sequence is felt on the R3.
5. Start, then `--unhide` after a forced kill (`kill -9` the service): the rig comes back.
   On Linux `--unhide` does nothing (`HideFilter::clear()` is empty there): the rig is hidden only from the game
   `wheelbri.sh` starts. So on Linux: during a game started by `wheelbri.sh`, `pkill -9 -f wheelbri-service`; no force
   is stuck, and the virtual wheel (wheelbri-uhid, a process of its own) stays with a centred wheel and released
   pedals after 1.5 s (`InputFeed::kStaleMs`) until the game ends.

Step 1 results:

| Item | Result | Pass |
|---|---|---|
| 1 App, Rig tab | opens; Rig `My rig`: R3 base and pedals, steering axis 0, throttle 2, brake 5, clutch 6 (none inverted), A/B/X/Y, D-pad, paddles 12/13 | yes |
| 2 Mapping tab | the six saved configs (G25, G27 each in both orders; G920; G923) map every button as the Target docs say; pedals and paddles from the Rig; buttons and pedals live in item 3 | yes |
| 3 `--list` / `--print` | `--list` finds the R3; `--print`: buttons and pedals move as they should (user); steering's full locks and centre not reported | yes (steering open) |
| 4 `--ffb-test` | spring, push left, push right, damper all felt on the R3; left is left (no `invert`) | yes |
| 5 Forced kill | service killed during ACC: force stopped at once; the G25 stays in the game and takes no input (as designed) | yes |

## Step 2: ACC on the Deck, all four Targets (Mode Virtual)

Re-proves the known-good game on new hardware first.

- Desktop Mode, terminal: `wheelbri.sh --target <t>` with `%command%` in ACC's launch options.
- Order: G920, G923, G25 (Order Native), G27 (Order Native).
- Then once in **Game Mode** with the launch option only (G920; run with G923): WheelBri starts with the game and
  stops after.

## Step 3: DiRT Rally 2.0, all four Targets (Mode Virtual)

- G920, G923, G25, G27 (Order Logitech; G25 handbrake "Button 7" is R Wheel).
- Note: the game must run under Proton first (check before mapping).

## Step 4: NFS games (Target G920, Mode Virtual)

- First check each game launches under Proton at all (account login, launcher). Record blockers, don't debug them.
- NFS 2015 (built-in profile swaps pedals), NFS Heat, NFS Unbound (Games tab Save to file: check the Linux path of
  `PROFILEOPTIONS`).
- NFS Payback: one short run only, to confirm the expected failure and its log line.

## Step 5: Race Driver: GRID (Target G25)

- Mode Virtual, Order Logitech; Centring spring set; VSync on.
- Mode Shim: `wheelbri.sh --target g25 --shim %command%` (32-bit shim). With GridMTFix/other `dinput8.dll` present:
  check it is chained and given back after.

## Step 6: Mode Shim on Linux, other games

- NFS 2015, Heat, Unbound with `--shim` (G920). Check the shim is put in before and taken out after; `--keep-shim`
  leaves it.

## Step 7: Game Mode behaviour

- Launch option start and stop in Game Mode for each game that passed.
- Game crash or quit: rig unhidden, forces stopped.
- Steam's own pad is never hidden; Deck controls still work in menus.

## Per-run checklist (every game x Target x Mode)

1. Game sees the Logitech wheel by name / preset.
2. Steering direction and rotation match the wheel.
3. Throttle, brake, clutch full range, no inversion.
4. Core buttons: D-pad, A/B/X/Y, paddles; button order right for the Target.
5. Force feedback felt; Live tab shows asked (blue) and played (green).
6. Stop or quit: rig back, no stuck force.
7. Logs: Logs tab Level debug; keep `~/.local/state/wheelbri/` files of the run.

## Results

| # | Machine | Game | Target | Mode | Inputs | Buttons | FFB | Stop clean | Notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | Deck | ACC | G25 (both Orders) | Virtual | yes (clutch after the fix) | | yes, felt like on Windows (user); the logged menu run asked only 0 | | ACC binds the clutch to axis 7 (Slider 1) and shows the combined pedals there (rests half, throttle up, brake down); the clutch pedal binds to nothing. WheelBri sends it (telemetry: `out_clutch` 0..65535). Proton log: ACC reads the virtual G25 over hidraw and dinput maps Slider 0 and 1 right, but reads each axis by usage (`HidP_GetUsageValue`), so both Sliders read the first. Fixed: for Proton the clutch is Usage Dial (`protonReportDescriptor`); retested with Order Native: clutch works. Launch stalled at times until Stop, then the game started with no G25: Steam's overlay `LD_PRELOAD` in wheelbri.sh's own commands, and Stop killed the service and wheelbri-uhid (Steam counted them as the game's). Fixed in wheelbri.sh (preload for the game only; no game without service and wheel); retested: starts at once, stops at once |
| 2 | Deck | ACC | G920 | Virtual | yes (wheel, throttle, brake, clutch) | yes (menus: A enters, B back; as on Windows) | yes, as on Windows (user) | yes (quits fast) | |
| 3 | Deck | ACC | G923 | Virtual | yes (pedals; wheel not reported) | yes | yes (user) | | |
| 4 | Deck | ACC | G27 (Order Native) | Virtual | yes (pedals as expected, clutch included; a lap driven) | yes | yes, centring and vibrations as on Windows (user) | | |
| 5 | Deck, Game Mode | ACC | G923 | Virtual | works (user) | works (user) | works (user) | yes (`pgrep -a wheelbri` empty after quitting, exit code 1) | Launch option only, started from Game Mode: WheelBri started with the game and the G923 worked (user) |

## Done when

Every row above has a result; README Status and games.md updated with what was proven (with the Deck, kernel and
SteamOS version noted).
