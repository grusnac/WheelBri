# Linux and Steam Deck

Your rig (here the Moza R3) as a Logitech G25, G27, G920 or G923 for Proton games (NFS 2015) on Linux: the Steam
Deck, or any x86-64 distribution. The app's Target picks the wheel (G25, G27, G920, G923; the G920 when none is
chosen), `wheelbri.sh --target` too. `wheelbri-service` reads the rig and plays force feedback on it, as on Windows,
through the same safety limiter. The game sees the wheel in one of two ways (the G920 below; the others after the
table):

| | `--virtual` (uhid, default) | `--shim` |
|---|---|---|
| What | `wheelbri-uhid`: a virtual G920 for the whole system, through the kernel's uhid (Phase 4; the Linux counterpart of `driver/wheelbri-g920`) | `dinput8.dll` in the game folder answers as a G920 (Phases 2 and 3, as on Windows) |
| Game folder | a WheelBri shim left there is taken out | `dinput8.dll` and `wheelbri-shim.ini` put in before the game, taken out after it (`--keep-shim` leaves them); the 32-bit `dinput8.dll` for a 32-bit game, `dinput8Hooked.dll` next to the Ultimate ASI Loader |
| Launch option | `PROTON_ENABLE_HIDRAW=0x046D/0xC262 SDL_GAMECONTROLLER_IGNORE_DEVICES=0xVVVV/0xPPPP %command%` (your rig's ids, comma separated; the R3: `0x346E/0x0005`) | `WINEDLLOVERRIDES="dinput8=n,b" %command%` |
| Once | `install-udev-rules.sh` (sudo) | nothing |

`wheelbri.sh` sets the launch options itself, so the only one Steam needs is `wheelbri.sh %command%`.

How the uhid way reaches the game: the virtual G920 has the Windows device's report (wheel, throttle, brake,
clutch, 19 buttons with the Driving Force Shifter's, hat, HID PID force feedback), with two axes renamed for
Proton: brake Z and clutch Rz, the real G920's own layout. Proton's DirectInput has a hack for the G920's id that
turns Z into Rz and Rz into a Slider, so the game gets what it gets on Windows: wheel X, throttle Y, brake Rz, clutch
Slider (with the Windows names, the axes would shift by one). Proton normally reads
joysticks through SDL, which would rename the axes and drop that force feedback; `PROTON_ENABLE_HIDRAW` makes it
read this one raw, so its DirectInput sees exactly what NFS sees on Windows and sends HID PID effects, which
`wheelbri-uhid` turns into the service's force feedback packets. `SDL_GAMECONTROLLER_IGNORE_DEVICES` hides the
R3 itself from the game, so the game finds one wheel: Proton (9 and 10) checks that list for SDL devices and for
raw (hidraw) ones too, so it also hides wheels Proton would open raw by itself (Fanatec, Simucube, ...).

The G25 (Target G25 in the app, or `wheelbri.sh --target g25`; [g25-target-plan.md](g25-target-plan.md)): the same
in both ways, with `PROTON_ENABLE_HIDRAW=0x046D/0xC299` instead (`wheelbri.sh` sets it from the target). The virtual
G25 keeps the Windows axis names (Proton has no hack for it) and sits on the virtual bus (`0006:046D:C299`): on USB
the kernel's `hid-lg` would claim it and then refuse it, leaving it to no driver. Run `install-udev-rules.sh` again
once, for its hidraw rule. Untried so far.

The G27 ([g27-target-plan.md](g27-target-plan.md)): as the G25, with `--target g27` and
`PROTON_ENABLE_HIDRAW=0x046D/0xC29B` (`0006:046D:C29B`); `install-udev-rules.sh` again once. Untried.

The G923 for Xbox One and PC ([g923-target-plan.md](g923-target-plan.md)): as the G25, with `--target g923` and
`PROTON_ENABLE_HIDRAW=0x046D/0xC26E` (`0006:046D:C26E`; on USB `hid-logitech-hidpp` would match it);
`install-udev-rules.sh` again once. Untried.

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

4. The build tools: pacman's base-devel, cmake, ninja, git, libx11 and mingw-w64-gcc (the shims, 64-bit and
   32-bit) in the container (several minutes and ~1 GB the first time; nothing is installed on SteamOS itself):

   ```
   bash scripts/linux/deck-setup.sh
   ```

5. Build, test and install into `~/wheelbri` (the first build downloads spdlog and GoogleTest and takes a few
   minutes; later ones only rebuild what changed). It builds the app, the service, `wheelbri-uhid` and both shims
   (`shim/dinput8.dll`, and `shim/x86/dinput8.dll` for 32-bit games), and runs the tests side by side:

   ```
   bash scripts/linux/deck-build.sh
   ```

6. The udev rules, once: `~/wheelbri/install-udev-rules.sh`. Then go on with steps 4 and 5 below (check the axes,
   force feedback) and [Every day](#every-day).

After every change: close WheelBri and the game, then

```
cd ~/WheelBri && git pull && bash scripts/linux/deck-build.sh
```

`deck-build.sh` options: `--no-tests` (faster), `--no-shim` (neither `dinput8.dll`), `--clean` (from scratch),
`--prefix DIR`. A config you edited in `~/wheelbri/config` is kept; the new version goes next to it as `<name>.new`.
`deck-setup.sh --recreate` makes the container again if it ever breaks, or when `deck-build.sh` says a SteamOS
update brought a newer glibc (your files are not in it).

## Steam Deck: install from the bundle (Desktop Mode, once)

1. Copy `wheelbri-linux-x86_64.tar.gz` to the Deck and unpack it in your home folder (Konsole):

   ```
   tar -xzf wheelbri-linux-x86_64.tar.gz -C ~
   ```

   That gives `~/wheelbri` with `bin/` (`wheelbri`, the app; `wheelbri-service`; `wheelbri-uhid`), `config/`,
   `shim/dinput8.dll` and `shim/x86/dinput8.dll` (the shim way: 64-bit and 32-bit games), `wheelbri.sh`,
   `install-udev-rules.sh` and this file.
2. `sudo` needs a password; if you never set one: `passwd`.
3. Let your user create the virtual wheel, games open it, and the service use the R3:

   ```
   ~/wheelbri/install-udev-rules.sh
   ```

4. Plug in the R3 and map it in the app (`~/wheelbri/bin/wheelbri`): the Rig tab names your wheel, pedals and
   their controls; then, in the Mapping tab, pick what drives each G920 input and **Save**. WheelBri's default
   mapping was made on Windows, and Linux numbers the R3's axes in another order (a pedal can land on the wrong
   target). Press Start and check that each pedal moves its own bar, then Stop and close the app. `wheelbri.sh` (Steam's launch option) uses the
   mapping you saved (`~/.config/wheelbri/configs/my-g920.ini`).
5. Force feedback on the R3 needs the kernel's driver for it: `hid-universal-pidff` (Linux 6.15 and later; it
   lists the R3). Check with `modinfo hid-universal-pidff`; `wheelbri-service --ffb-test` plays a short, gentle
   sequence on the wheel.

## Every day

In Steam, NFS > Properties > Launch Options:

```
/home/deck/wheelbri/wheelbri.sh %command%
```

Start the game as usual, also in Game Mode: the script starts the service and the virtual G920 before the game
and stops both when it ends (`--virtual` says the same explicitly). For the shim way instead:

```
/home/deck/wheelbri/wheelbri.sh --shim %command%
```

`--shim` puts `dinput8.dll` and `wheelbri-shim.ini` into the game's folder before the game starts and takes them
out when it ends, as the app's Games panel does: a 32-bit game (its largest `.exe` is 32-bit, e.g. NFS Carbon) gets
the 32-bit `shim/x86/dinput8.dll`, and next to the Ultimate ASI Loader (a `dinput8.dll` other mods use) the shim goes
in as `dinput8Hooked.dll`, which the loader then loads; the loader itself is never touched.
The folder is the one of the `.exe` at the end of `%command%`; a game Steam starts through a
launcher has none (NFS 2015 starts as `steam2ea://launchgame/1262540...`, through the EA app), so then it is the
game's Steam install folder, found from its app id (`AppId=` in `%command%`) in Steam's libraries. `--game-folder
DIR` names another. With the EA app, Proton may count the game as running until the EA app closes too, so the shim
can stay in the folder a little longer than the game. For
testing, `--keep-shim` leaves them there; the next `--virtual` start takes them out again, so the game then finds
only the virtual G920. Only WheelBri's files are touched: a `dinput8.dll` of another mod (not ours, and no
`wheelbri-shim.ini` next to it) makes `--shim` stop before starting anything, and a `wheelbri-shim.ini` with your
own settings is used and kept. If Steam kills the game hard, the shim stays until the next `--virtual` start (or
`--shim` start, which takes it out at the end).

Without Steam, from Konsole: `~/wheelbri/wheelbri.sh` starts both (the service with its window) and stops them
when you press Enter; start the game with the launch option it prints.

From the app (`~/wheelbri/bin/wheelbri`): Start with Mode Virtual (next to Target, Mapping tab) starts
`wheelbri-uhid` for the target too and Stop removes it; the game still needs the launch options above (or
`wheelbri.sh %command%`, which then uses the app's session instead of starting its own, and the app's Target). With
Mode Shim, the Games button next to Start puts the shim into a game's folder (KDE's or GNOME's folder window), the
same way as `--shim`.

Logs: `~/.local/state/wheelbri/` (service, `wheelbri-uhid`, `wheelbri.sh`, telemetry, and the shim: under Proton it
writes there too, not into the game's Wine prefix).

## Steam's virtual pad

Proton always gives the game Steam Input's virtual Xbox pad (`28de:11ff`): the Deck's own controls, and any pad
Steam Input takes over. WheelBri never hides it: it is the only thing that controls the Steam Deck (Steam, its
menus, the game's menus), so it must always stay. (No launch option could hide it anyway: Proton lets it through
before it looks at `SDL_GAMECONTROLLER_IGNORE_DEVICES`.) WheelBri only leaves it out of its own device list.
On Windows, NFS drove from an Xbox pad that was there when it started instead of the wheel; whether it does the same
on the Deck is not tested yet. Signs: the app's status says GAME NOT USING THE WHEEL, and the `wheelbri-uhid` log
counts no force feedback reports from the game. Then pick the wheel in NFS's controls settings; do not turn Steam
Input off.

## When it does not work

- `wheelbri-uhid` stops at once: its log says why. `Permission denied` on `/dev/uhid`: run
  `install-udev-rules.sh` (and log out and in once). `No such file`: `sudo modprobe uhid`.
- The game sees no wheel: `ls -l /dev/hidraw*` while it runs; the virtual G920's node must be readable by you
  (`getfacl /dev/hidrawN`). Proton's own log shows what it found: launch option
  `PROTON_LOG=1 WINEDEBUG=+hid,+dinput /home/deck/wheelbri/wheelbri.sh %command%` writes `~/steam-<appid>.log`
  (look for `046d:c262`).
- Two wheels in the game, or the R3 instead of the G920: the R3 has a different id; pass it with
  `--hide 0x346E/0xNNNN` (`lsusb` shows it).
- `dmesg` says `logitech-hidpp-device ... not enough values in hidpp report 17` when the virtual G920 appears: that is
  expected. The kernel's Logitech HID++ driver claims every G920 id, sees that ours is no HID++ device and leaves it
  to the generic HID driver (`UhidG920Test` keeps it that way).
- No force feedback but the wheel works: check the R3 first (`wheelbri-service --ffb-test`), then the
  `wheelbri-uhid` log (`force feedback reports from the game`).

## Building from source

On any x86-64 Linux with CMake, a C++20 compiler and the X11 headers (`libx11-dev` / `libX11-devel`, for the
`--ui` window; on the Deck itself, in a `distrobox`). The service reads the wheel through evdev itself, no SDL:

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build -j "$(nproc)"
```

The shims are Windows DLLs, built with MinGW-w64 (`cmake/toolchain-mingw64.cmake` for 64-bit games,
`cmake/toolchain-mingw32.cmake` for 32-bit ones, each with `-DWE_BUILD_SERVICE=OFF -DWE_BUILD_TESTS=OFF`; the 32-bit
one also `-DWE_BUILD_LOGI=OFF`). The bundle above comes from
`scripts/linux/package.sh --shim build-win/shim/dinput8.dll --shim-x86 build-win-x86/shim/dinput8.dll` (the C++
runtime and spdlog linked in; it needs only libX11 and the kernel, so it runs on SteamOS as is).
