---
paths:
  - "tools/uhid/**"
  - "scripts/linux/**"
---

# Linux, Proton and the Steam Deck

## Proton

- Proton reads each HID axis by usage (`HidP_GetUsageValue`): two axes with the same usage both read the first. The
  uhid descriptor for Proton has no shared usages; the clutch is Usage Dial (`protonReportDescriptor`).
- Wine lists keyboards and mice that have a joystick HID part (e.g. Keychron "System Control") as controllers, and
  games take the first that is not an Xbox pad. `wheelbri.sh` hides them in both Modes
  (`SDL_GAMECONTROLLER_IGNORE_DEVICES`); keep that.
- Wine 11 / Proton 11 hand Unix variables to Windows code as `WINE_HOST_<name>` (`HOME`, XDG): the shim reads both.
- Old games may miss Steam's first-run installers in the prefix (GRID: OpenAL); the fix per game is in `docs/games.md`.

## `wheelbri.sh` (Steam launch option)

- Steam preloads its overlay (`LD_PRELOAD`) into everything a launch option starts and counts each process as the
  game. The preload goes to the game only (WheelBri's own commands stall with it), and no game starts without the
  service and the virtual wheel running.
- Mode Virtual starts the service with `--virtual-wheel-outside`, so game profiles apply without a shim.

## Steam Deck

- R3 force feedback needs kernel 6.15+ (`hid-universal-pidff`). Builds run in the `deck-setup.sh` container, pinned to
  SteamOS's glibc.
- "No input in the game", check in this order: kernel log (the rig drops on a USB hub and may not come back),
  `wheelbri-service --list`, telemetry `out_*` (is the service sending?), then the game's own controller choice
  (secondary vs primary, Steam's pad, a keyboard). Only then suspect WheelBri.
