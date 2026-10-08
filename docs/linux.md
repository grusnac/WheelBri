# Linux and Steam Deck

The Moza R3 as a Logitech G920 for Proton games (NFS 2015) on Linux: the Steam Deck, or any x86-64
distribution. `wheelbri-service` reads the R3 and plays force feedback on it, as on Windows. The game sees the
G920 in one of two ways:

| | uhid (default) | shim |
|---|---|---|
| What | `wheelbri-uhid`: a virtual G920 for the whole system, through the kernel's uhid (Phase 4; the Linux counterpart of `driver/wheelbri-g920`) | `dinput8.dll` in the game folder answers as a G920 (Phases 2 and 3, as on Windows) |
| Game folder | untouched | `dinput8.dll` copied there |
| Launch option | `PROTON_ENABLE_HIDRAW=0x046D/0xC262 SDL_GAMECONTROLLER_IGNORE_DEVICES=0x346E/0x0005 %command%` | `WINEDLLOVERRIDES="dinput8=n,b" %command%` |
| Once | `install-udev-rules.sh` (sudo) | nothing |

`wheelbri.sh` sets the launch options itself, so the only one Steam needs is `wheelbri.sh %command%`.

How the uhid way reaches the game: the virtual G920 has the descriptor the Windows bridge gives HIDMaestro
(wheel X, brake Y, clutch Rz, throttle Slider, 19 buttons with the Driving Force Shifter's, hat, HID PID force feedback). Proton normally reads
joysticks through SDL, which would rename the axes and drop that force feedback; `PROTON_ENABLE_HIDRAW` makes it
read this one raw, so its DirectInput sees exactly what NFS sees on Windows and sends HID PID effects, which
`wheelbri-uhid` turns into the service's force feedback packets. `SDL_GAMECONTROLLER_IGNORE_DEVICES` hides the
R3 itself from the game (Proton applies it to every backend), so the game finds one wheel.

## Steam Deck: build it there from the repository

The way to follow development: get the code with git, build it on the Deck, and after every change only
`git pull` and build again. SteamOS is Arch Linux underneath, but read-only and without a compiler, so the compiler
lives in an Arch Linux distrobox container (SteamOS 3.5 and later have distrobox) and SteamOS itself stays untouched.
The container is pinned to the day Arch still shipped SteamOS's glibc version (the Arch Linux Archive), so what it
builds runs on SteamOS as it is.

Once (Desktop Mode, Konsole):

1. A password for `sudo`, if you never set one: `passwd`.
2. A key for GitHub, so the Deck can fetch the repository: `ssh-keygen -t ed25519` (Enter at each question), then
   add the contents of `~/.ssh/id_ed25519.pub` in GitHub > Settings > SSH and GPG keys.
3. The code, on the branch being worked on:

   ```
   git clone git@github.com:grusnac/WheelBri.git ~/WheelBri
   cd ~/WheelBri
   git switch steam-deck
   ```

4. The build tools: pacman's base-devel, cmake, ninja, git, libx11 and mingw-w64-gcc in the container (several
   minutes and ~1 GB the first time; nothing is installed on SteamOS itself):

   ```
   bash scripts/linux/deck-setup.sh
   ```

5. Build, test and install into `~/wheelbri` (the first build downloads spdlog and GoogleTest and takes a few
   minutes; later ones only rebuild what changed):

   ```
   bash scripts/linux/deck-build.sh
   ```

6. The udev rules, once: `~/wheelbri/install-udev-rules.sh`. Then go on with steps 4 and 5 below (check the axes,
   force feedback) and [Every day](#every-day).

After every change: close WheelBri and the game, then

```
cd ~/WheelBri && git pull && bash scripts/linux/deck-build.sh
```

`deck-build.sh` options: `--no-tests` (faster), `--no-shim` (no `dinput8.dll`), `--clean` (from scratch),
`--prefix DIR`. A config you edited in `~/wheelbri/config` is kept; the new version goes next to it as `<name>.new`.
`deck-setup.sh --recreate` makes the container again if it ever breaks, or when `deck-build.sh` says a SteamOS
update brought a newer glibc (your files are not in it).

## Steam Deck: install from the bundle (Desktop Mode, once)

1. Copy `wheelbri-linux-x86_64.tar.gz` to the Deck and unpack it in your home folder (Konsole):

   ```
   tar -xzf wheelbri-linux-x86_64.tar.gz -C ~
   ```

   That gives `~/wheelbri` with `bin/`, `config/`, `wheelbri.sh`, `install-udev-rules.sh`, this file
   and `dinput8.dll` (for the shim way).
2. `sudo` needs a password; if you never set one: `passwd`.
3. Let your user create the virtual G920, games open it, and the service use the R3:

   ```
   ~/wheelbri/install-udev-rules.sh
   ```

4. Plug in the R3 and map it in the app (`~/wheelbri/bin/wheelbri`): the Rig tab names your wheel, pedals and
   their roles; then, in the Mapping tab, **Auto-map** and **Save**. WheelBri's default mapping was made on Windows,
   and Linux numbers the R3's axes in another order (a pedal can land on the wrong target). Press Start and check
   that each pedal moves its own bar, then Stop and close the app. `wheelbri.sh` (Steam's launch option) uses the
   mapping you saved (`~/.config/wheelbri/configs/my-g920-virtual-device.ini`).
5. Force feedback on the R3 needs the kernel's driver for it: `hid-universal-pidff` (Linux 6.15 and later; it
   lists the R3). Check with `modinfo hid-universal-pidff`; `wheelbri-service --ffb-test` plays a short, gentle
   sequence on the wheel.

## Every day

In Steam, NFS > Properties > Launch Options:

```
/home/deck/wheelbri/wheelbri.sh %command%
```

Start the game as usual, also in Game Mode: the script starts the service and the virtual G920 before the game
and stops both when it ends. For the shim way instead (copy `~/wheelbri/dinput8.dll` into the NFS folder first):

```
/home/deck/wheelbri/wheelbri.sh --shim %command%
```

Without Steam, from Konsole: `~/wheelbri/wheelbri.sh` starts both (the service with its window) and stops them
when you press Enter; start the game with the launch option it prints.

Logs: `~/.local/state/wheelbri/` (service, `wheelbri-uhid`, `wheelbri.sh`, telemetry).

## When it does not work

- `wheelbri-uhid` stops at once: its log says why. `Permission denied` on `/dev/uhid`: run
  `install-udev-rules.sh` (and log out and in once). `No such file`: `sudo modprobe uhid`.
- The game sees no wheel: `ls -l /dev/hidraw*` while it runs; the virtual G920's node must be readable by you
  (`getfacl /dev/hidrawN`). Proton's own log shows what it found: launch option
  `PROTON_LOG=1 WINEDEBUG=+hid,+dinput /home/deck/wheelbri/wheelbri.sh %command%` writes `~/steam-<appid>.log`
  (look for `046d:c262`).
- Two wheels in the game, or the R3 instead of the G920: the R3 has a different id; pass it with
  `--hide 0x346E/0xNNNN` (`lsusb` shows it).
- No force feedback but the wheel works: check the R3 first (`wheelbri-service --ffb-test`), then the
  `wheelbri-uhid` log (`force feedback reports from the game`).

## Building from source

On any x86-64 Linux with CMake, a C++20 compiler and the X11 headers (`libx11-dev` / `libX11-devel`, for the
`--ui` window; on the Deck itself, in a `distrobox`). The service reads the wheel through evdev itself, no SDL:

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

The bundle above comes from `scripts/linux/package.sh --shim dinput8.dll` (the C++ runtime and spdlog linked
in; it needs only libX11 and the kernel, so it runs on SteamOS as is).
