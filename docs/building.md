# Building WheelBri

CMake 3.24+, a C++20 compiler (GCC; MinGW-w64 on Windows; others untried) and internet on the first configure (CMake
fetches spdlog 1.15.3 and GoogleTest 1.15.2). No other library: DirectInput and Win32 on Windows, evdev and X11 on
Linux. The Windows drivers need Visual Studio.

| CMake option | Default | Builds |
|---|---|---|
| `WE_BUILD_SERVICE` | ON | the app `wheelbri`, `wheelbri-service`, and on Windows `wheelbri-helper` |
| `WE_BUILD_TESTS` | ON | `wheelbri_tests` (skipped when cross-compiling) |
| `WE_BUILD_SHIM` | ON on Windows | `dinput8.dll`, the shim |
| `WE_BUILD_LOGI` | ON on Windows x64 | `wheelbri-logi.dll`, a logging stand-in for Logitech's SDK helper |
| `WE_BUILD_UHID` | ON on Linux | `wheelbri-uhid`, the Linux virtual wheel |
| `WE_BUILD_FUZZ` | OFF | `wheelbri_fuzz` (Clang only) |

Outputs: `build/service/`, `build/shim/dinput8.dll`, `build/logi/`, `build/tools/uhid/`.

## Windows

From an MSYS2 UCRT64 shell, in the repository root. Once:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
```

Configure, build, test:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build
```

```bash
ctest --test-dir build
```

The programs need the compiler's runtime DLLs on `PATH` (`C:\msys64\ucrt64\bin`); the shim is linked statically.

### 32-bit shim

32-bit games need a 32-bit `dinput8.dll`; only the shim is ever 32-bit. Once:

```bash
pacman -S --needed mingw-w64-i686-gcc
```

Configure and build (→ `build-x86/shim/dinput8.dll`):

```bash
PATH="/mingw32/bin:$PATH" cmake -S . -B build-x86 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw32.cmake -DWE_BUILD_SERVICE=OFF -DWE_BUILD_TESTS=OFF -DWE_BUILD_LOGI=OFF
```

```bash
PATH="/mingw32/bin:$PATH" cmake --build build-x86 --target wheelbri_shim
```

### Drivers

Mode Virtual uses two UMDF 2 drivers: `wheelbri-wheel` (the virtual wheel, every Target) and `wheelbri-hide` (hides
the rig while Mode Virtual runs). They need Visual Studio with *Desktop development with C++*, the Spectre-mitigated
libraries, the Windows SDK and the WDK. From a **Developer PowerShell for VS**:

```powershell
msbuild driver\wheelbri-wheel.vcxproj /p:Configuration=Release /p:Platform=x64
```

```powershell
msbuild driver\hide\wheelbri-hide.vcxproj /p:Configuration=Release /p:Platform=x64
```

They are test-signed with the WDK's `WDKTestCert`. To sign with WheelBri's own certificate instead (once, normal
PowerShell, then rebuild both):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File driver\new-signing-cert.ps1
```

Install from an **Administrator PowerShell** (the setup and the app do this themselves; Secure Boot and test mode stay
as they are):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File driver\install-driver.ps1
```

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File driver\hide\install-hide.ps1
```

Uninstall, the filter first:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File driver\hide\uninstall-hide.ps1
```

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File driver\uninstall-driver.ps1 -RemoveCertificate
```

### Windows package

Needs `build`, `build-x86` and both drivers built. Writes `dist\windows\WheelBri-<version>.zip` and
`WheelBri-<version>-setup.exe`, after checking every file, each program's processor and that both drivers have the same
signer:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\make\package.ps1 -BuildDir build
```

## Linux

Once, the compiler, CMake, Ninja and the X11 headers for the window (without them the window is skipped):

```bash
sudo apt install build-essential cmake ninja-build libx11-dev
```

(Fedora: `gcc-c++ cmake ninja-build libX11-devel`; Arch: `base-devel cmake ninja libx11`.) Then the same configure,
build and test commands as on Windows.

### Shims (cross-compiled)

Needs MinGW-w64 (`mingw-w64` on Debian/Ubuntu, `mingw-w64-gcc` on Arch). 64-bit (→ `build-win/shim/dinput8.dll`):

```bash
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake -DCMAKE_BUILD_TYPE=Release -DWE_BUILD_SERVICE=OFF -DWE_BUILD_TESTS=OFF
```

```bash
cmake --build build-win
```

32-bit (→ `build-win-x86/shim/dinput8.dll`):

```bash
cmake -S . -B build-win-x86 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw32.cmake -DCMAKE_BUILD_TYPE=Release -DWE_BUILD_SERVICE=OFF -DWE_BUILD_TESTS=OFF -DWE_BUILD_LOGI=OFF
```

```bash
cmake --build build-win-x86 --target wheelbri_shim
```

### Steam Deck

SteamOS has no compiler. Once (Desktop Mode, Konsole, in the checkout): an Arch Linux distrobox pinned to SteamOS's
glibc. Run it again with `--recreate` when `deck-build.sh` says SteamOS's glibc moved on:

```bash
scripts/linux/deck-setup.sh
```

After every `git pull`: builds in the container, runs the tests and installs into `~/wheelbri`:

```bash
scripts/linux/deck-build.sh
```

### Linux bundle

Writes `dist/wheelbri-linux-x86_64.tar.gz`. Build on a system whose glibc is not newer than the target's (SteamOS's):

```bash
scripts/linux/package.sh --shim build-win/shim/dinput8.dll --shim-x86 build-win-x86/shim/dinput8.dll
```

## Code layout

| Path | What |
|---|---|
| `core/` | portable library: mapping, target profiles, force feedback limiter, config, wire format, telemetry |
| `service/` | the app and `wheelbri-service`; `helper/`: the WheelBri Helper Windows service |
| `shim/` | `dinput8.dll`, a DirectInput proxy that answers as the Target inside one game |
| `driver/` | UMDF drivers `wheelbri-wheel` and `hide/` (`wheelbri-hide`), with install scripts |
| `tools/uhid/` | `wheelbri-uhid`, the Linux virtual wheel (kernel uhid) |
| `logi/` | `wheelbri-logi.dll`, logging stand-in for Logitech's wheel SDK helper |
| `installer/`, `scripts/` | Windows installer, Linux scripts, packaging |
| `config/` | each Target's default config, `wheelbri-shim.ini`, `games/` (game profiles, compiled in) |
| `tests/` | GoogleTest unit tests with fakes |

## Architecture

```
rig ──DirectInput / evdev──► wheelbri-service ──UDP 127.0.0.1──► one consumer, seen by the game as the Target:
          map · encode · FFB safety      41329 state ►         Mode Virtual: wheelbri-wheel driver or wheelbri-uhid
                                         ◄ 41330 game FFB      Mode Shim:    dinput8.dll next to the game's .exe
```

- **Pipeline:** raw axes → `Mapper` → `WheelState` → the Target's `IDeviceProfile::encode` → `StatePacket` (33 bytes,
  magic `WEMU`). Nothing on this loop allocates or blocks.
- **UDP, not shared memory:** under Proton the shim lives in Wine and the service is a Linux process.
- **Force feedback safety:** every force goes through `FfbSafetyLimiter` (slew limit; strength is set in the wheel's
  own app) and a watchdog that stops forces after 500 ms of silence. Never add a way around it.
- **Logging:** spdlog via `WE_LOG_*`, one dotted logger per component (`core/include/wheelbri/Logging.hpp`), to
  `%LOCALAPPDATA%\wheelbri\logs` or `~/.local/state/wheelbri`.

Target details: [targets.md](targets.md).
