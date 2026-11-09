# WheelBri

WheelBri presents your rig (a wheel and its pedals, shifter or button box) to PC games as a Logitech **G25**, **G27**,
**G920** or **G923**, so games that only know Logitech wheels accept yours, with force feedback. Windows and Linux
(Proton, Steam Deck). Built and tested with a **Moza R3**; other wheels should work but are untried.

## Documentation

| Page | Read it for |
|---|---|
| [Install](docs/install.md) | setup wizard, uninstaller, scripted install, Windows 7 to 10, Linux |
| [User guide](docs/user-guide.md) | set up the rig, map it, play, force feedback, troubleshoot |
| [Capabilities](docs/capabilities.md) | everything WheelBri can do, on one page |
| [Targets](docs/targets.md) | which Logitech wheel to pick: [G920](docs/target-g920.md), [G923](docs/target-g923.md), [G25](docs/target-g25.md), [G27](docs/target-g27.md) |
| [Games](docs/games.md) | what was tested and what each game needs |
| [Building](docs/building.md) | build, drivers, packaging, code layout |
| [Changelog](CHANGELOG.md) | what changed in each release |

## How it works

1. **Rig tab**: touch each control and say what it is (Steering, Throttle, A, Left paddle...).
2. **Mapping tab**: pick the **Target** (the Logitech wheel games see) and the **Mode**, check the mapping.
3. **Start**, then start the game and pick the Logitech wheel.

Two Modes:

- **Virtual** (recommended): a virtual Logitech wheel every game sees; your rig is hidden from games while it runs.
  Windows 11 and Linux.
- **Shim**: a `dinput8.dll` next to one game's `.exe` answers as the wheel inside that game. For older DirectInput
  games only: modern ones (ACC, DiRT Rally 2.0) and games that read the wheel through Logitech's SDK DLL don't take
  it. The only Mode on Windows 7, 8, 8.1 and 10.

## Status

| What | State |
|---|---|
| Mode Virtual, Windows 11, all four Targets | **works** in ACC and DiRT Rally 2.0; G920 in four NFS games, G25 in Race Driver: GRID |
| Mode Shim, Windows 11 | **works** in NFS 2015, Heat, Unbound (G920) and Race Driver: GRID (G25) |
| Logitech's force feedback drivers (Windows) | **work** in ACC: G HUB's for the G920 and G923, its older-wheel driver for the G25 and G27 in Logitech order |
| TrueForce (G923, Windows 11) | **works** in ACC and DiRT Rally 2.0; needs G HUB installed; road texture not yet felt on a rig |
| G HUB with the virtual G923 | sees it and its effects play; the rotation G HUB sets is not applied |
| Steam Deck (SteamOS 3.8, Proton), Mode Virtual | **works** in ACC (all four Targets), NFS 2015 (G920) and Race Driver: GRID (G25), Game Mode too |
| Steam Deck (SteamOS 3.8, Proton), Mode Shim | **works** in NFS 2015 (G920) and Race Driver: GRID (G25, 32-bit), Game Mode too |
| Linux, not tried | DiRT Rally 2.0, NFS Heat, Unbound, Payback |
| Windows 7, 8, 8.1 and 10 (Mode Shim only) | built, **not yet tried** |
| Button orders | from public sources; **not yet checked on real Logitech wheels** |

## What's new in 1.0.2

- **Steam Deck tested**: ACC (all four Targets), NFS 2015 and Race Driver: GRID, in Game Mode too.
- **Linux clutch on the G25 and G27** works under Proton.
- **`wheelbri.sh`**: no more stalls in Steam's launch option; waits for the wheel and says when it is missing;
  `--order` picks the G25's or G27's button order.

## What's new in 1.0.1

- **Safer force feedback settings**: a config can only make the safety guards gentler, never looser.
- **Mapping tab**: the Target, Order and TrueForce rows line up.
- **Docs**: brought up to date with 1.0.0, with a command-line reference.

## What's new in 1.0.0

The first stable release. Since 0.9.9:

- **Windows setup wizard and Uninstaller**: a graphical installer with a checklist and log, a Start menu folder and
  restart choice; an Uninstaller where every box follows one rule (ticked is kept) for settings, logs, the shim in
  games and the drivers' certificate.
- **Logitech's own force feedback drivers**: G HUB's for the G920 and G923, its older-wheel driver for the G25 and G27
  (Logitech order), as on the real wheels.
- **TrueForce for the G923**: games' torque, damping and road texture on your wheel; a switch on the Mapping tab.
- **G25 and G27 for real games**: Logitech and Native button orders with a mapping each, combined pedals, centring
  spring; the G25 works in Race Driver: GRID.
- **Live tab**: the Shadow view draws every Target as the real wheel; Side by side editing while a game runs.
- **Easier mapping**: the Rig's roles map the core controls on every Target; a searchable source picker; up to 8
  extra controls per button.
- **Mode Shim chains other mods' `dinput8.dll`** (ReShade, Widescreen Fix) instead of overwriting them, also on Linux.
- **Safer**: force feedback limits per sender, signed drivers with a throwaway certificate per release, the Helper
  and installer locked to Administrators, `/dev/uhid` for the group `wheelbri` only on Linux.

Full list: [CHANGELOG.md](CHANGELOG.md).

## Install

Releases: <https://github.com/grusnac/WheelBri/releases>. Full details: [docs/install.md](docs/install.md).

| System | File |
|---|---|
| Windows 11, 64-bit | `WheelBri-<version>-setup.exe` (or the `.zip`: run `wheelbri-setup.exe`) |
| Windows 7 SP1, 8, 8.1, 10, 64-bit | the same; Mode Shim only |
| Linux x86-64 (glibc 2.39 or newer), Steam Deck | `wheelbri-linux-x86_64.tar.gz` |

### Windows

1. Close WheelBri and any game. Run `WheelBri-<version>-setup.exe` and approve the Administrator prompt.
2. Follow the wizard: install folder (default `C:\Program Files\WheelBri`), options (Start menu folder, desktop
   shortcut, Firefox's game controllers off, open WheelBri at the end), **Install**.
3. Restart Windows when the Finish page asks (Windows 11).

It installs the app, the virtual wheel and hiding filter drivers with their certificate, and the **WheelBri Helper**
service, so the app runs as a normal user. Windows 7, 8, 8.1 and 10 get Mode Shim only, with nothing to install
first: setup brings the Universal C Runtime they may lack ([install.md](docs/install.md#windows-7-sp1-8-81-and-10)).

**Upgrade:** run the new setup; your rig, mappings and settings stay.

**Uninstall:** Start menu > WheelBri > **Uninstaller** (or Settings > Apps). Tick what to keep: rig, mappings and
settings (ticked by default), logs, the shim in game folders, the drivers' certificate. Everything unticked goes.

### Linux and the Steam Deck

```bash
tar -xzf wheelbri-linux-x86_64.tar.gz -C ~
```

```bash
~/wheelbri/install-udev-rules.sh
```

Log out and in, then set up the rig in `~/wheelbri/bin/wheelbri` and add the game's Steam launch option
([user-guide.md](docs/user-guide.md#linux-and-the-steam-deck)). Force feedback on the R3 needs Linux 6.15 or later
(`hid-universal-pidff`). Uninstall: [install.md](docs/install.md#linux-and-the-steam-deck).

Next: [User guide](docs/user-guide.md).

## Licence

Copyright (C) 2026 Grigore Rusnac.

WheelBri is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License
as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
It is distributed in the hope that it will be useful, but **without any warranty**; without even the implied warranty
of merchantability or fitness for a particular purpose. See [LICENSE](LICENSE) for the full text.

Parts come from other projects under their own licences: [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
Security problems: please report them privately ([security policy](https://github.com/grusnac/WheelBri/security/policy)).
Nothing is copied from GIMX (GPLv3). `tools/uhid` is written from the kernel's `linux/uhid.h`, the HID descriptor
comes from HIDMaestro's builder (MIT), HID++ from its public description. WheelBri never ships Logitech's files.

### Trademarks

WheelBri is an independent project, **not affiliated with, endorsed or sponsored by Logitech**. Logitech, G HUB, G25,
G27, G920, G923 and TrueForce are trademarks of Logitech; Moza is a trademark of its owner. The names are used only to
say which wheels WheelBri presents your rig as, and which it was tested with.

### Safety

Force feedback moves a motor that can hurt hands, wrists and thumbs. WheelBri limits what games send, but you use it
at your own risk: keep your wheel's own torque limit set, hold the rim with open thumbs, and stop a session (or switch
the wheel off) if anything behaves oddly.
