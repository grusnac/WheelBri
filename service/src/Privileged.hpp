#pragma once

#include "wheelbri/LogLevel.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wheelbri::service::privileged {

// The steps that need Administrator, in one place (library wheelbri_privileged), used by the app when it runs as
// Administrator (as before) and by the WheelBri Helper (service/helper) for an app running as a standard user. Every
// input is explicit: nothing here reads the user's settings, so the helper can call it with what it checked itself.
// Windows only; elsewhere they do nothing and say so.

using Cancelled = std::function<bool()>;

// What putHideFilter came to (also `wheelbri-service --prepare-hide`'s exit code).
enum class HideSetup {
    Running = 0,      // the filter runs on every interface asked for
    Failed = 1,       // the installer could not be run, or failed (the message says why)
    Waiting = 2,      // the filter is on, but Windows has not restarted a device with it yet (the message says why)
    NothingToDo = 3,  // nothing asked for
};

// A driver install script and its built package (paths relative to a base folder), looked for next to this program
// and up to `levelsAbove` folders above it (build/service/ in the source tree: 2), never in a drive's root folder (any
// user may create C:\driver\...). The helper, a service, looks only next to itself (0). Empty when not found.
[[nodiscard]] std::filesystem::path findDriverScript(const std::filesystem::path& script,
                                                     const std::filesystem::path& package, int levelsAbove);

// Runs a driver install script (Windows PowerShell from System32, no window) and waits for its exit code. Never cut
// off: stopped halfway it can leave a device with a half-switched driver, so `cancelled` only makes it say so, and
// after 2 minutes it is left to finish by itself (then nullopt). `started` false + `error`: it did not run.
[[nodiscard]] std::optional<int> runDriverScript(const std::filesystem::path& script,
                                                 const std::vector<std::string>& arguments, const char* tag,
                                                 const Cancelled& cancelled, bool& started, std::string& error);

// `logFolder` below: where the install scripts write their logs (-LogFolder). Empty: the script's own
// %LOCALAPPDATA%\wheelbri\logs, right when the app runs them as the user's Administrator; the WheelBri Helper runs as
// SYSTEM, whose %LOCALAPPDATA% nobody looks at, and gives the caller's folder (or ProgramData's).

// The virtual wheel's driver (wheelbri-wheel, all four targets) installed when Windows has none
// (driver\install-driver.ps1). True when it is there afterwards (or was already); else `message` says why not.
bool installWheelDriver(int levelsAbove, const std::filesystem::path& logFolder, const Cancelled& cancelled,
                       std::string& message);

// wheelbri-hide put on `install` (HID interface instance ids; install-hide.ps1 -Devices, which skips those that have
// it already), then waits, opening nothing, until every one of `interfaces` runs it (HideFilter::waitUntilStarted:
// Windows restarts each device, or this does after a few seconds). `message`: what to tell the user when it is not
// Running.
[[nodiscard]] HideSetup putHideFilter(const std::vector<std::string>& install, const std::vector<std::string>& interfaces,
                                      int levelsAbove, const std::filesystem::path& logFolder,
                                      const Cancelled& cancelled, std::string& message);

// HKLM\SOFTWARE\wheelbri\hide: AllowedPid (+ AllowedPidStart, the process's creation time, so a reused id is not
// let in). `processStart` 0 with `pid` 0 (everyone again).
bool writeAllowedPid(std::uint32_t pid, std::uint64_t processStart);
// The value now (0: nobody hides the rig).
[[nodiscard]] std::uint32_t allowedPid();

// HKLM\SOFTWARE\wheelbri LogLevel (REG_SZ: trace, debug, info or warn): the app's log level for the drivers (the
// hide filter reads it at each line; the virtual G920's driver when it starts, then from the session's packets).
// False with `message` when it could not be written (not Windows: nothing to write, true).
bool writeLogLevel(LogLevel level, std::string& message);

// Only `process` (a HANDLE opened with PROCESS_QUERY_LIMITED_INFORMATION | READ_CONTROL | WRITE_DAC) may open the
// rig's filtered devices: writes its id and creation time, and lets the filter (LocalService) look at that process so
// it can tell when it is gone. False with `message` when it could not be written.
bool allowOnly(void* process, std::string& message);

}  // namespace wheelbri::service::privileged
