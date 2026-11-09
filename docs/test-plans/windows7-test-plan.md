# Windows 7 test plan (plan, 2026-11-07)

Status: **not started.** Goal: prove WheelBri 1.0.1 on Windows 7 SP1 64-bit, where only Mode Shim exists
(README: "built, not yet tried").

Rig: Moza R3 base and pedals with the clutch add-on.

## Which games can be tested at all

Mode Shim only, so ACC, DiRT Rally 2.0 (no shim support) and NFS Payback (Logitech SDK DLL) are out by design.

| Game | Mode Shim on Win11 | On Windows 7 | Test? |
|---|---|---|---|
| Race Driver: GRID | works (G25, 32-bit) | game is of the Win7 era | **yes, reference** |
| NFS 2015 | works (G920) | check the game's published minimum OS | yes, if it runs |
| NFS Heat | works (G920) | check minimum OS (reported as Windows 10) | only if it runs |
| NFS Unbound | works (G920) | check minimum OS (reported as Windows 10) | only if it runs |
| ACC, DiRT Rally 2.0, NFS Payback | n/a | n/a | no |

## Step 0: preconditions (check, don't assume)

1. **Machine:** a real Windows 7 SP1 x64 install (spare disk or dual boot). A VM is a fallback only: USB
   pass-through of the R3 may not carry force feedback reliably; note which one was used.
2. **Launchers:** Steam ended Windows 7 support on 1 January 2024, and the EA app reportedly does not install on
   Windows 7. For each game, check whether it can start at all: Steam client still runs or not, offline mode,
   game folder copied from Windows 11. Record the result; don't work around DRM.
3. No prerequisites: `WheelBri-<version>-setup.exe` installs the Universal C Runtime (KB2999226) itself when it is
   missing. Check it is there afterwards (`C:\Windows\System32\ucrtbase.dll`) and read the install log.
4. Moza Pit House (or the R3 without it): the wheel works in `joy.cpl` with force feedback before WheelBri.

Stop here if no game can start; then only steps 1 and 2 are possible.

## Step 1: install and app behaviour

1. Setup wizard: installs Mode Shim only, no drivers, no Helper, no restart asked.
2. App starts (if not: missing UCRT, see user-guide).
3. Mapping tab: Virtual button absent; Mode Shim shown locked; hover tooltip says only Mode Shim is available on
   Windows 7.
4. Rig tab finds base and pedals; `wheelbri-service --list`, `--print`, `--ffb-test` work.
5. Uninstaller: every box follows "ticked is kept".

## Step 2: shim handling without a game running

- Games tab: add each game folder; shim put in (32 or 64-bit, as the `.exe`); **Chain mod** with a Widescreen Fix
  or ReShade `dinput8.dll`; **Remove shim** gives the other mod back.

## Step 3: Race Driver: GRID (Target G25, Mode Shim)

- Order Logitech (the game's G25 preset), Centring spring set, VSync on.
- With GridMTFix / Widescreen Fix: chained, both work.
- Check the 144 Hz broken-force guard still holds.

## Step 4: NFS 2015 (Target G920, Mode Shim)

- Default wheel controls; profile swaps the pedals; note the FFB-off-at-start and Alt+Tab behaviour.

## Step 5: NFS Heat and NFS Unbound (Target G920, Mode Shim), only if they start

- Heat: light FFB expected. Unbound: Games tab Save to file first.

## Optional: Windows 8 / 8.1 / 10 and Vista

- Same Shim-only path on 8.1 and 10 (and 8.1/later's Virtual choice per your Windows-support decision).
- Vista: installer must say straight away it can't be installed.

## Per-run checklist (every game x Target)

1. Game sees the Logitech wheel by name / preset.
2. Steering direction and rotation.
3. Throttle, brake, clutch full range.
4. Core buttons and the button order for the Target.
5. Force feedback felt; Live tab shows asked and played.
6. Game quit: forces stop; shim state as expected.
7. Logs at debug: `wheelbri-service-...`, `telemetry-...`, `ffb-...`, `wheelbri-shim-...`.

## Results

| # | Machine (real/VM) | Game | Target | Launches | Inputs | Buttons | FFB | Notes |
|---|---|---|---|---|---|---|---|---|
| | | | | | | | | |

## Done when

Every testable game has a row; README Status and games.md updated (or the "not yet tried" line changed to what was
found, including launcher blockers).
