# WheelBri

WheelBri presents your rig (a wheel and its pedals, shifter or button box) to PC games as a Logitech **G25**, **G27**,
**G920** or **G923**, so games that only know Logitech wheels accept yours, with force feedback. Windows and Linux
(Proton, Steam Deck). Built and tested with a **Moza R3**; other wheels should work but are untried.

- [User guide](docs/user-guide.md): set up, map, play, troubleshoot
- [Games](docs/games.md): what was tested and what each game needs
- [Targets](docs/targets.md): each Logitech wheel's axes and button orders
- [Building](docs/building.md): build, drivers, packaging, code layout

## Two Modes

- **Virtual** (recommended): a virtual Logitech wheel every game sees; your rig is hidden from games while it runs.
  Windows 11 and Linux.
- **Shim**: a `dinput8.dll` next to one game's `.exe` answers as the wheel inside that game. For older DirectInput
  games only: modern ones (Assetto Corsa Competizione, DiRT Rally 2.0) and games that read the wheel through
  Logitech's SDK DLL don't work with it.

## Status

| What | State |
|---|---|
| Mode Virtual, Windows 11, all four Targets | **works** in ACC and DiRT Rally 2.0; G920 in four NFS games, G25 in Race Driver: GRID |
| Mode Shim, Windows 11 | **works** in NFS 2015, Heat, Unbound (G920) and Race Driver: GRID (G25) |
| Linux, Mode Virtual | ACC under Proton; **not yet tried on a Steam Deck** |
| G HUB with the virtual G923 | sees the wheel; its rotation and effects are not applied ([Targets](docs/targets.md#g-hub-and-hid-g923)) |
| Logitech's force feedback drivers (Windows) | **work** in ACC: G HUB's for the G920 and G923, its older-wheel driver for the G25 and G27 in Logitech order ([Targets](docs/targets.md#force-feedback-drivers-g25-g27)) |
| TrueForce (G923), Windows 11 | **works** in ACC: force; texture as a vibration, not yet checked on a rig; needs G HUB installed ([Games](docs/games.md#assetto-corsa-competizione)) |
| Windows 7 to 10 (Mode Shim only) | built, **not yet tried** |

## Download

Releases: <https://github.com/grusnac/WheelBri/releases>

| System | File |
|---|---|
| Windows 11, 64-bit | `WheelBri-<version>-setup.exe` (or the `.zip`: run `Setup.cmd`) |
| Windows 7 SP1 to 10, 64-bit | the same; Mode Shim only |
| Linux x86-64, Steam Deck | `wheelbri-linux-x86_64.tar.gz` |

## Install on Windows

1. Run `WheelBri-<version>-setup.exe` and approve the Administrator prompt.
2. Restart Windows when the setup asks (Windows 11).

With Firefox installed, the setup also asks whether to turn off Firefox's game controller support (default no):
Firefox keeps the wheel open, so WheelBri cannot hide it until Firefox is closed. The uninstaller turns it back on.

On Windows 11 it installs the app, the two drivers with their certificate and the **WheelBri Helper**, a service that
does the Administrator work so the app runs as a normal user. On Windows 7 to 10 it installs Mode Shim only. Windows 7
first needs the prerequisites in the package's `win7` folder: run `download-prerequisites.cmd`, then
`install-prerequisites.cmd` as Administrator, and restart.

**Upgrade:** run the new setup; your rig, mappings and settings stay.

**Uninstall:** Start menu > WheelBri > Uninstall WheelBri (or Settings > Apps). It asks what to keep: your rig,
mappings and settings (kept by default) and the logs (removed by default), both in `%LOCALAPPDATA%\wheelbri`. It also
takes the shim out of the games WheelBri put it into, and removes the drivers' certificate (both can be unticked).

Next: [User guide](docs/user-guide.md).

## Install on Linux and the Steam Deck

1. Unpack the bundle into your home folder (→ `~/wheelbri`):

   ```bash
   tar -xzf wheelbri-linux-x86_64.tar.gz -C ~
   ```

   On the Deck you can build it instead ([Building](docs/building.md#steam-deck)).

2. Once per machine, the udev rules, then log out and in. They let your user create the virtual wheel, let games open
   it and let WheelBri read the rig (Steam Deck: set a password with `passwd` first):

   ```bash
   ~/wheelbri/install-udev-rules.sh
   ```

3. Force feedback on the R3 needs the kernel's `hid-universal-pidff` (Linux 6.15 and later). Check it is there:

   ```bash
   modinfo hid-universal-pidff
   ```

Next: [User guide](docs/user-guide.md), then [Linux and the Steam Deck](docs/user-guide.md#linux-and-the-steam-deck)
for the game's Steam launch option.

**Uninstall:** `~/wheelbri/install-udev-rules.sh --remove`, then delete `~/wheelbri`, and for a clean start
`~/.config/wheelbri` and `~/.local/state/wheelbri`.

## Licence

Nothing is copied from GIMX (GPLv3). `tools/uhid` is written from the kernel's `linux/uhid.h`, the HID descriptor
comes from HIDMaestro's builder (MIT, noted in `core/src/HidWheelDevice.cpp`), HID++ from its public description.
WheelBri never ships Logitech's files.
