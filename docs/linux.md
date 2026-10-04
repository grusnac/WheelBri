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
(wheel X, brake Y, clutch Rz, throttle Slider, 11 buttons, hat, HID PID force feedback). Proton normally reads
joysticks through SDL, which would rename the axes and drop that force feedback; `PROTON_ENABLE_HIDRAW` makes it
read this one raw, so its DirectInput sees exactly what NFS sees on Windows and sends HID PID effects, which
`wheelbri-uhid` turns into the service's force feedback packets. `SDL_GAMECONTROLLER_IGNORE_DEVICES` hides the
R3 itself from the game (Proton applies it to every backend), so the game finds one wheel.

## Steam Deck: install (Desktop Mode, once)

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

4. Plug in the R3 and check its axes (the Linux numbering may differ from Windows):

   ```
   ~/wheelbri/bin/wheelbri-service --config ~/wheelbri/config/g920-virtual-device.ini --ui
   ```

   Turn the wheel and press each pedal; if a pedal moves the wrong bar, fix `axis =` under `[throttle]`,
   `[brake]` or `[clutch]` in `~/wheelbri/config/g920-virtual-device.ini`. Close the window when done.
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
