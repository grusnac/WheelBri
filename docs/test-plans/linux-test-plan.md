# Linux test plan (plan, 2026-11-07)

Status: **in progress (step 0).** Goal: prove WheelBri 1.0.1 on Linux beyond the one known result (ACC, Mode Virtual, under
Proton) and close the README's "not yet tried on a Steam Deck" gap.

Rig: Moza R3 base and pedals with the clutch add-on (VID/PID 0x346E/0x0005 for the base).

## What is known today (from the docs)

| What | State |
|---|---|
| ACC, Mode Virtual, Proton | works (machine not recorded in the docs: note it in step 0) |
| Steam Deck | never tried |
| DiRT Rally 2.0 on Linux | never tried |
| NFS 2015, Heat, Unbound on Linux | never tried |
| NFS Payback on Linux | not expected to work (Proton has no Logitech DLL) |
| Race Driver: GRID on Linux | never tried |
| Mode Shim on Linux (`--shim`) | never tried in a game |
| G25, G27, G923 on Linux | only ACC, if at all (check the old logs) |

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

Step 0 results (2026-11-07, Steam Deck):

| Check | Result | Pass |
|---|---|---|
| SteamOS | 3.8.28, build 20260922.1 | - |
| Kernel | 6.18.50-valve2-1-neptune-618 (>= 6.15) | yes |
| glibc | 2.41 (>= 2.39) | yes |
| lsusb | Moza R3 racing wheel and pedals seen | yes |
| Groups before install | deck steamos-log-submitter wheel (no `wheelbri` yet) | expected |
| `deck-setup.sh` | container glibc 2.44, SteamOS 2.41: pin to the archive did not take | **no, blocks the local build** |

## Step 1: smoke test (Desktop Mode)

1. `~/wheelbri/bin/wheelbri` opens; Rig tab finds base and pedals; name the core controls.
2. Mapping tab: check axes (Linux numbers them differently from Windows); save.
3. `wheelbri-service --list` and `--print`: values move with the wheel and pedals.
4. `wheelbri-service --ffb-test`: the gentle sequence is felt on the R3.
5. Start, then `--unhide` after a forced kill (`kill -9` the service): the rig comes back.

## Step 2: ACC on the Deck, all four Targets (Mode Virtual)

Re-proves the known-good game on new hardware first.

- Desktop Mode, terminal: `wheelbri.sh --target <t>` with `%command%` in ACC's launch options.
- Order: G920, G923, G25 (Order Native), G27 (Order Native).
- Then once in **Game Mode** with the launch option only (G920): WheelBri starts with the game and stops after.

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
| | | | | | | | | | |

## Done when

Every row above has a result; README Status and games.md updated with what was proven (with the Deck, kernel and
SteamOS version noted).
