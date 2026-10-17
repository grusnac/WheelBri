# Build scripts

Shortcuts for building, testing, packaging and cleaning up. Run them from an **MSYS2 UCRT64** shell on Windows, from
any folder (they find the repository themselves; a relative game folder is taken from where you started). `app.sh`,
`rebuild.sh` and `ui.sh` also work on Linux; `shim.sh --off` works anywhere; the rest are Windows only and say so.
The `.ps1` scripts are run as `powershell -NoProfile -ExecutionPolicy Bypass -File <script.ps1> [arguments]`.

Each script prints what it runs. When a step is written down for someone (CLAUDE.md), it comes in two forms: first
the full command(s) the script runs, then the script as the shortcut. A workflow that keeps needing the same commands
gets a script here (sourcing `common.sh`) and a row below.

## The scripts

| Script | What it does | Options |
|---|---|---|
| `app.sh` | Builds everything that changed (core, app, service, helper, shim, tests, and on Windows `wheelbri-logi.dll` and the 32-bit shim when MSYS2's 32-bit compiler is there; configures the first time), then starts the app (`wheelbri.exe`: tabs, Start / Stop; O opens the logs folder) as Administrator (one prompt), so it does the privileged steps itself. The one to use after any change | `--test` runs every unit test after the build and starts nothing when one fails; `--no-start` only builds (and tests); `--as-user` starts the app as you, as an installed WheelBri runs (then `wheelbri-helper.exe --console` must run from an Administrator shell); other arguments go to the app (e.g. `--config my.ini`) |
| `rebuild.sh` | Full rebuild of the CMake parts: deletes the build tree and `<build tree>-x86`, configures and builds from scratch (and the 32-bit shim). After changing compilers, CMake options or toolchains, or when a build acts up. Refuses to delete a build tree that holds the repository or a system folder | none |
| `ui.sh [config]` | Builds `wheelbri-service` and opens its window (`--ui`) with `config/g920.ini` or the config given. The quick check after a mapping or config change; close the window to stop | arguments after the config go to the service (e.g. `--print`, `--virtual-g920` as Administrator) |
| `shim.sh <game folder>` | Builds the shim and puts it into the game's folder as the app's Games panel does: the 32-bit `dinput8.dll` (`<build tree>-x86/shim/`, through `shim-x86.sh`) when the folder's largest `.exe` is 32-bit, else the 64-bit one; as `dinput8Hooked.dll` next to the Ultimate ASI Loader (never touched); never over another mod's `dinput8.dll`; plus `config/wheelbri-shim.ini` when the folder has none | `--off <game folder>` renames WheelBri's shim there to `.off` (`dinput8.dll.off` or `dinput8Hooked.dll.off`, `-2`, `-3`... when taken; never the loader or another mod's DLL; `wheelbri-shim.ini` stays): needed before using the virtual wheel with that game |
| `shim-x86.sh` | Builds only the 32-bit `dinput8.dll` (for 32-bit games such as NFS Carbon) into `<build tree>-x86/shim/`. `app.sh`, `rebuild.sh`, `all.sh` and `package.sh` run it too (without the 32-bit compiler they warn and build the rest; `package.sh` stops). Needs MSYS2's 32-bit compiler once: `pacman -S mingw-w64-i686-gcc` | none |
| `logi.sh` | Builds `wheelbri-logi.dll` (the logging stand-in for Logitech's wheel SDK helper, for NFS Payback), writes `wheelbri-logi.ini` next to it (`log_level = debug`) when there is none, and prints the Administrator commands that point the SDK's registry entry at it and back at G HUB's library | none |
| `drivers.sh` | Builds the two UMDF drivers (`wheelbri-g920`, `wheelbri-hide`) with MSBuild (Visual Studio + WDK, found with vswhere; `driver/README.md`). Runs `drivers.ps1` | `-Only g920` or `-Only hide`; `-Install` then installs them (one Administrator window per driver, which closes when done; on a failure the end of the installer's log is shown here); `-Clean` rebuilds from scratch; `-Configuration Debug` |
| `all.sh` | Everything that builds on Windows: `app.sh --no-start`, then `drivers.sh` | `--clean`: `rebuild.sh` and `drivers.sh -Clean` instead |
| `package.sh` | The installer for another PC: builds app, service, helper, both shims and the drivers in Release, then `package.ps1` makes `dist/windows/WheelBri-<version>/`, `WheelBri-<version>.zip` and `WheelBri-<version>-setup.exe` (IExpress). Release only (refuses `WE_BUILD_TYPE=Debug`) | `--no-build` packages what is built; arguments starting with `-` go to `package.ps1` (`-NoSetupExe`: no Setup.exe; `-OutDir <folder>`, default `dist\windows`) |
| `clean-pc.sh` | Before installing from scratch: removes everything WheelBri put on the PC: the WheelBri Helper service, running copies, the virtual devices, both drivers (and HidHide / HIDMaestro, which earlier versions used), every device the hide filter was on put back on Windows' own driver, the UMDF registrations, the drivers' certificates (by thumbprint), the install folder (also under Program Files (x86)), shortcuts, the installer's copies in `%TEMP%` and `%SystemRoot%\Temp`, the Settings > Apps entry, registry entries, DirectInput's entries for the virtual wheels, the shim in the games the app lists, `%LOCALAPPDATA%\wheelbri` and `C:\ProgramData\wheelbri`; the same under the old name wheelemu. Lists what it found and asks once (Administrator prompt; the Administrator window closes when done and this one shows its log). Never touches the repository | `-KeepData` keeps rigs, configs, app settings and logs; `-GameFolder <folder>[,<folder>...]` also takes the shim out of game folders the app does not list; `-KeepThirdParty` keeps HidHide and HIDMaestro; `-Yes` asks nothing |
| `measure.sh` | Measures what WheelBri costs the PC from outside the app (nothing added to it): every second, CPU per process (app or service, helper, the driver hosts named by the WheelBri drivers they carry, e.g. `WUDFHost:g920-driver+hide-filter`; optionally the game) and per thread, memory, handles, GDI and USER objects, and the timer resolution. Opens its own Administrator PowerShell window: **M** starts a named phase (a name used again gets -2, -3...), **Q** stops. Writes `%LOCALAPPDATA%\wheelbri\logs\perf-<date>-<time>.log` (`[perf]`, `[perf.thread]`, `[perf.phase]` and `[perf.summary]` lines) and prints the summary here. Record rounds in `docs/cpu-measurements.md` | `-Phase <name>` names the first phase (default `start`); `-Game NFS16` also measures the game (process name without `.exe`; several comma-separated); `-Seconds N` stops by itself; through `measure.ps1` also `-IntervalMs` (default 1000), `-Top` (threads per phase in the summary, default 10), `-OutDir` (default the logs folder) |

`common.sh` is not run on its own: the scripts source it for the shared steps (configure when there is no build tree
or it was made for another build type, incremental build, the 32-bit shim, coloured messages).

`drivers.ps1`, `package.ps1`, `clean-pc.ps1` and `measure.ps1` are the PowerShell scripts behind `drivers.sh`,
`package.sh`, `clean-pc.sh` and `measure.sh`. They can also be run on their own from PowerShell, for example:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\make\package.ps1 -BuildDir build -NoSetupExe
```

`package.ps1` packages what is already built (`-BuildDir`, default `build`; the 32-bit shim from `<BuildDir>-x86`) and
checks it: every file the installer needs, each program for the right processor and needing no MSYS2 DLL, both
drivers signed by the same certificate, every `.ps1` parsing.

## Settings through the environment

`WE_BUILD_TYPE=Debug` (default `Release`), `WE_BUILD_DIR` (default `build`; the 32-bit shim goes to `<it>-x86`),
`WE_CMAKE_ARGS` (extra configure arguments), for example:

```bash
WE_BUILD_TYPE=Debug scripts/make/app.sh
```

## Signing the drivers

The drivers are signed with the WDK's test certificate, or with WheelBri's own one made on this PC, once, from a
normal PowerShell (it writes `driver\signing-certificate.props`, which git ignores and the driver projects read;
`-Remove` deletes that file again). See `driver/README.md`.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File driver\new-signing-cert.ps1
```

## Typical use

To play with a development build: `scripts/make/app.sh`. Before a commit: `scripts/make/app.sh --test`. After a
driver change: `scripts/make/drivers.sh -Install`. For another PC: `scripts/make/package.sh`. For a test of the
installer on a clean PC: `scripts/make/clean-pc.sh`, then the setup from `dist/windows/`.
