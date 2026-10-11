# Build scripts

Run them from an **MSYS2 UCRT64** shell, from any folder (they find the repo themselves). The CMake ones also
work on Linux.

| Script | When |
|---|---|
| `scripts/make/rebuild.sh` | Full rebuild of the CMake parts: deletes `build/`, configures and builds from scratch. After changing CMake files or options, or when a build acts up |
| `scripts/make/ui.sh` | Build the service and open its mapping window (`--ui`) with `config/moza-r3-as-g920.ini`, or the config given. The check after a mapping or config change |
| `scripts/make/app.sh` | After any change: builds everything that changed (core, app, service, helper, shim, tests; configures the first time) and starts the app (`wheelbri.exe`: Start, Stop) as Administrator (prompt), or `--as-user` as you (needs `wheelbri-helper.exe --console` running from an Administrator shell). `--test` runs the whole unit test suite before starting (nothing starts when a test fails); `--no-start` only builds (and tests, with `--test`) |
| `scripts/make/shim.sh <game folder>` | Build `dinput8.dll` and copy it next to the game (plus `wheelbri-shim.ini` if there is none). `shim.sh --off <game folder>` takes it out, needed before using the virtual G920 |
| `scripts/make/logi.sh` | Build `wheelbri-logi.dll` (the logging stand-in for Logitech's wheel SDK helper, for NFS Payback) and print the Administrator PowerShell commands that point the SDK's registry entry at it, and back at G HUB's library |
| `scripts/make/drivers.sh` | Build the two UMDF drivers (Visual Studio + WDK). `-Install` installs them (Administrator prompt), `-Only g920`, `-Clean`, `-Configuration Debug` |
| `scripts/make/package.sh` | The installer for another PC: builds app, service, shim and drivers (Release), then `package.ps1` makes `dist/windows/WheelBri-<version>/`, its zip and `WheelBri-<version>-setup.exe`. `--no-build` packages what is built; `-NoSetupExe` skips Setup.exe |
| `scripts/make/clean-pc.sh` | Before installing from scratch: removes everything WheelBri put on the PC (running copies, virtual devices, its drivers (and HidHide / HIDMaestro, which earlier versions used), every device the hide filter was on put back on Windows' own driver, their certificate, the install folder (also under Program Files (x86)), shortcuts, the installer's copies in `%TEMP%`, Settings > Apps entry, registry entries (UMDF registrations included), `%LOCALAPPDATA%\wheelbri` and `C:\ProgramData\wheelbri`). Lists what it found and asks first (Administrator prompt). `-KeepData` keeps rigs, configs and logs; `-GameFolder <folder>` also takes the shim out of a game; `-KeepThirdParty` keeps HidHide / HIDMaestro; `-Yes` asks nothing. Never touches the repo |
| `scripts/make/measure.sh` | Measure what WheelBri costs the PC, from outside the app (nothing added to it): every second, CPU per process (app or service, helper, the driver hosts, named by the WheelBri drivers they carry at that moment, `WUDFHost:g920-driver+hide-filter` when one host has both; optionally the game: `-Game NFS16`) and per thread, memory, handles, GDI objects and the timer resolution. Opens its own Administrator PowerShell window; **M** starts a named phase ("closed", "stopped", "shim", "virtual"; a name used again gets -2, -3...), **Q** stops. Everything goes to one log, `%LOCALAPPDATA%\wheelbri\logs\perf-<date>-<time>.log` (`[perf]` process lines, `[perf.thread]` thread lines, `[perf.phase]`, and the summary per phase as `[perf.summary]`, also printed) |
| `scripts/make/all.sh` | Everything above that builds: CMake parts and drivers. `--clean` for a full rebuild of everything |

The drivers are signed with a certificate made on this PC, once, from an Administrator PowerShell:
`powershell -ExecutionPolicy Bypass -File driver\new-signing-cert.ps1` (it writes `driver\signing-certificate.props`, which git ignores and the driver projects
read; `-Remove` deletes it again). See `driver/README.md`.

`drivers.ps1`, `package.ps1`, `clean-pc.ps1` and `measure.ps1` are the PowerShell scripts behind `drivers.sh`,
`package.sh`, `clean-pc.sh` and `measure.sh`; they can also be run on their own from PowerShell.

Settings through the environment: `WE_BUILD_TYPE=Debug` (default `Release`), `WE_BUILD_DIR` (default `build`),
`WE_CMAKE_ARGS` (extra configure arguments), for example `WE_BUILD_TYPE=Debug scripts/make/app.sh`.

To play, build and start the app (`scripts/make/app.sh`); before a commit, `scripts/make/app.sh --test`.
