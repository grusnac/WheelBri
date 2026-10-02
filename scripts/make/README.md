# Build scripts

Run them from an **MSYS2 UCRT64** shell, from any folder (they find the repo themselves). The CMake ones also
work on Linux.

| Script | When |
|---|---|
| `scripts/make/build.sh` | After any change: builds what changed (core, service, shim, tests). Configures the first time. Optional target: `build.sh wheelemu-service` |
| `scripts/make/rebuild.sh` | Full rebuild of the CMake parts: deletes `build/`, configures and builds from scratch. After changing CMake files or options, or when a build acts up |
| `scripts/make/test.sh` | Build, then all unit tests. `test.sh Mapper` runs only the matching ones |
| `scripts/make/check.sh` | Before a commit: build, all unit tests, the bridge's wire self-test, then `git status` |
| `scripts/make/ui.sh` | Build the service and open its mapping window (`--ui`) with `config/moza-r3-as-g920.ini`, or the config given. The check after a mapping or config change |
| `scripts/make/app.sh` | Build the app (`wheelemu.exe`: Start, Stop, Open logs) and start it (Administrator prompt). `--no-start` only builds |
| `scripts/make/shim.sh <game folder>` | Build `dinput8.dll` and copy it next to the game (plus `wheelemu-shim.ini` if there is none). `shim.sh --off <game folder>` takes it out, needed before using the virtual G920 |
| `scripts/make/bridge.sh` | Build the C# bridge and probes in `tools/vbridge` (.NET 10). `--clean` rebuilds |
| `scripts/make/drivers.sh` | Build the three UMDF drivers (Visual Studio + WDK). `-Install` installs them (Administrator prompt), `-Only g920`, `-Clean`, `-Configuration Debug` |
| `scripts/make/all.sh` | Everything above that builds: CMake parts, bridge, drivers. `--clean` for a full rebuild of everything |

`drivers.ps1` is the PowerShell script behind `drivers.sh`; it can also be run on its own from PowerShell.

Settings through the environment: `WE_BUILD_TYPE=Debug` (default `Release`), `WE_BUILD_DIR` (default `build`),
`WE_CMAKE_ARGS` (extra configure arguments), for example `WE_BUILD_TYPE=Debug scripts/make/build.sh`.

To play, `scripts/wheelemu.sh` starts the service and the bridge (`--build` builds them first).
