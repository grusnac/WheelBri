#include "Privileged.hpp"

#include "HideFilter.hpp"
#include "Platform.hpp"
#include "VirtualWheelDevice.hpp"

#include "wheelbri/FileText.hpp"
#include "wheelbri/Logging.hpp"

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#endif

namespace wheelbri::service::privileged {

namespace {

std::string joined(const std::vector<std::string>& items) {
    std::string text;
    for (const std::string& item : items) text += (text.empty() ? "" : ",") + item;
    return text;
}

}  // namespace

#ifdef _WIN32

namespace {

const logging::Logger& hideLog() {
    static const logging::Logger logger = logging::get("service.hide");
    return logger;
}

const logging::Logger& wheelDriverLog() {
    static const logging::Logger logger = logging::get("service.driver");
    return logger;
}

constexpr wchar_t kPolicyKey[] = L"SOFTWARE\\wheelbri\\hide";

std::uint64_t creationTime(const HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32U) | created.dwLowDateTime;
}

// The filter runs in the driver host as LocalService, which may not look at an elevated process (nor at another
// user's): then it cannot compare AllowedPidStart and must assume the process still runs. Let it query that process
// (name and times only: PROCESS_QUERY_LIMITED_INFORMATION), so a reused process id after a crash is recognised.
void letFilterCheck(const HANDLE process) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    DWORD status = GetSecurityInfo(process, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                                   nullptr, &descriptor);
    if (status != ERROR_SUCCESS) {
        WE_LOG_WARN(hideLog(), "hide: cannot read the process's security (error {}): after a crash a reused process "
                               "id may keep the wheel hidden (wheelbri-service --unhide fixes it)",
                    status);
        return;
    }
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD sidSize = sizeof(sid);
    PACL updated = nullptr;
    if (CreateWellKnownSid(WinLocalServiceSid, nullptr, sid, &sidSize)) {
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = PROCESS_QUERY_LIMITED_INFORMATION;
        access.grfAccessMode = GRANT_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
        status = SetEntriesInAclW(1, &access, dacl, &updated);
        if (status == ERROR_SUCCESS) {
            status = SetSecurityInfo(process, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, updated,
                                     nullptr);
        }
    } else {
        status = GetLastError();
    }
    if (status != ERROR_SUCCESS) {
        WE_LOG_WARN(hideLog(), "hide: cannot let wheelbri-hide check the process (error {}): after a crash a reused "
                               "process id may keep the wheel hidden (wheelbri-service --unhide fixes it)",
                    status);
    }
    if (updated != nullptr) LocalFree(updated);
    LocalFree(descriptor);
}

}  // namespace

std::filesystem::path findDriverScript(const std::filesystem::path& script, const std::filesystem::path& package,
                                       const int levelsAbove) {
    int levels = 0;
    for (std::filesystem::path base = platform::executablePath().parent_path(); !base.empty() && levels <= levelsAbove;
         base = base.parent_path(), ++levels) {
        if (base == base.parent_path()) break;   // a drive's root: anyone may create folders there
        std::error_code ec;
        if (std::filesystem::is_regular_file(base / script, ec) && std::filesystem::is_regular_file(base / package, ec)) {
            return base / script;
        }
    }
    return {};
}

std::optional<int> runDriverScript(const std::filesystem::path& script, const std::vector<std::string>& arguments,
                                   const char* const tag, const Cancelled& cancelled, bool& started, std::string& error) {
    const std::filesystem::path powershell =
        platform::systemFolder() / "WindowsPowerShell" / "v1.0" / "powershell.exe";
    std::vector<std::string> command{powershell.string(), "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                     script.string()};
    command.insert(command.end(), arguments.begin(), arguments.end());
    const auto child = platform::ChildProcess::start(command, error);
    started = child != nullptr;
    if (child == nullptr) return std::nullopt;
    child->keepRunningAfterExit();   // not even this process ending (a closed console) may cut it off
    std::optional<int> exit;
    bool saidWaiting = false;
    for (int waited = 0; !(exit = child->poll()) && waited < 120'000; waited += 200) {
        if (cancelled && cancelled() && !saidWaiting) {
            saidWaiting = true;
            WE_LOG_INFO(hideLog(), "{}: stop requested; waiting for the installer to finish first (it must not be cut "
                                   "off while it changes a device's driver)",
                        tag);
        }
        platform::sleepMs(200);
    }
    if (!exit) child->release();
    return exit;
}

namespace {

// The script's arguments, with -LogFolder when one is given (never an empty argument).
std::vector<std::string> withLogFolder(std::vector<std::string> arguments, const std::filesystem::path& logFolder) {
    if (!logFolder.empty()) {
        arguments.push_back("-LogFolder");
        arguments.push_back(pathToUtf8(logFolder));   // UTF-8, as ChildProcess expects (a user name may not be ASCII)
    }
    return arguments;
}

}  // namespace

bool installWheelDriver(const int levelsAbove, const std::filesystem::path& logFolder, const Cancelled& cancelled,
                       std::string& message) {
    if (VirtualWheelDevice::driverState() != VirtualWheelDevice::DriverState::Missing) return true;
    const std::filesystem::path script = findDriverScript(
        std::filesystem::path("driver") / "install-driver.ps1",
        std::filesystem::path("driver") / "x64" / "Release" / "wheelbri-wheel" / "wheelbri-wheel.inf", levelsAbove);
    if (script.empty()) {
        message = "the virtual wheel's driver (wheelbri-wheel) is not installed and no built package was found next to "
                  "the program (build and install it once: scripts/make/drivers.sh -Install)";
        WE_LOG_ERROR(wheelDriverLog(), "wheel driver: {}", message);
        return false;
    }
    WE_LOG_INFO(wheelDriverLog(), "wheel driver: the virtual wheel's driver (wheelbri-wheel) is not installed: "
                                  "installing it now ({})",
                script.string());
    bool started = false;
    std::string error;
    const std::optional<int> exit = runDriverScript(script, withLogFolder({"-Configuration", "Release"}, logFolder), "g920",
                                                    cancelled, started, error);
    if (!started) {
        message = "cannot run the driver installer: " + error;
        WE_LOG_ERROR(wheelDriverLog(), "wheel driver: {}", message);
        return false;
    }
    if (cancelled && cancelled()) {
        message = "stopped";
        return false;
    }
    if (!exit || *exit != 0) {
        message = "the driver installer " +
                  (!exit ? std::string("took longer than 2 minutes (left to finish by itself)")
                         : "failed (exit code " + std::to_string(*exit) + ")") +
                  " (its log: wheelbri-driver-install-<date>.log in the logs folder; or run " + script.string() +
                  " yourself as Administrator)";
        WE_LOG_ERROR(wheelDriverLog(), "wheel driver: {}", message);
        return false;
    }
    if (VirtualWheelDevice::driverState() == VirtualWheelDevice::DriverState::Missing) {
        message = "the driver installer ended without error, but Windows still does not list wheelbri-wheel (its log: "
                  "wheelbri-driver-install-<date>.log in the logs folder)";
        WE_LOG_ERROR(wheelDriverLog(), "wheel driver: {}", message);
        return false;
    }
    WE_LOG_INFO(wheelDriverLog(), "wheel driver: the virtual wheel's driver (wheelbri-wheel) is installed");
    return true;
}

HideSetup putHideFilter(const std::vector<std::string>& install, const std::vector<std::string>& interfaces,
                        const int levelsAbove, const std::filesystem::path& logFolder, const Cancelled& cancelled,
                        std::string& message) {
    if (interfaces.empty()) return HideSetup::NothingToDo;
    int restartAfterMs = 6'000;   // a filter put on earlier: Windows may still restart the device by itself
    // Our driver there already (a Stop took the filter out): back in the stack, no installer.
    std::vector<std::string> toInstall;
    std::vector<std::string> relisted;
    for (const std::string& id : install) {
        if (!HideFilter::hasOurDriver(id)) {
            toInstall.push_back(id);
            continue;
        }
        bool changed = false;
        if (const std::string problem = HideFilter::setFilterListed(id, true, changed); !problem.empty()) {
            message = problem;
            WE_LOG_ERROR(hideLog(), "hide: {}", message);
            return HideSetup::Failed;
        }
        if (changed) relisted.push_back(id);
    }
    if (!toInstall.empty()) {
        const std::string devices = joined(toInstall);
        const std::filesystem::path script = findDriverScript(
            std::filesystem::path("driver") / "hide" / "install-hide.ps1",
            std::filesystem::path("driver") / "hide" / "x64" / "Release" / "wheelbri-hide" / "wheelbri-hide.inf",
            levelsAbove);
        if (script.empty()) {
            message = "wheelbri-hide is not on " + devices +
                      " and no built package was found next to the program (build it once: scripts/make/drivers.sh "
                      "-Only hide)";
            WE_LOG_ERROR(hideLog(), "hide: {}", message);
            return HideSetup::Failed;
        }
        WE_LOG_INFO(hideLog(), "hide: putting wheelbri-hide on the rig's devices {} ({}); each reconnects once, now only",
                    devices, script.string());
        bool started = false;
        std::string error;
        const std::optional<int> exit =
            runDriverScript(script,
                            withLogFolder({"-Configuration", "Release", "-Devices", devices, "-NoRestart"}, logFolder),
                            "hide", cancelled, started, error);
        if (!started) {
            message = "cannot run the installer: " + error;
            WE_LOG_ERROR(hideLog(), "hide: {}", message);
            return HideSetup::Failed;
        }
        if (cancelled && cancelled()) {
            message = "stopped";
            return HideSetup::Waiting;
        }
        // 2: put on, but not started yet (-NoRestart: the device waits for a restart): restarted below at once.
        if (exit && *exit == 2) restartAfterMs = 0;
        if (!exit || (*exit != 0 && *exit != 2)) {
            message = "the installer " +
                      (!exit ? std::string("took longer than 2 minutes (left to finish by itself)")
                             : "failed (exit code " + std::to_string(*exit) + ")") +
                      " (its log: wheelbri-hide-install-<date>.log in the logs folder; or run " + script.string() +
                      " -Devices " + devices + " yourself as Administrator)";
            WE_LOG_ERROR(hideLog(), "hide: {}", message);
            return HideSetup::Failed;
        }
    }
    // Windows loads the filter when it restarts the device: a few seconds, with nothing of ours holding it.
    std::string why;
    // A program that has the wheel open and will not let go when Windows asks (2026-11-02: Firefox, for its gamepad
    // support) would make Start fail. At Start the rig is hidden already (runSession hides it before putting the filter
    // on), so such a program is cut off by reconnecting the wheel (HideFilter::reconnect: as if it were unplugged), and
    // it cannot open the wheel again as it comes back. Only then: never while the rig is not hidden (the installer's
    // --prepare-hide), where the program would just open it again, and never when Windows refused for another reason.
    const bool mayReconnect = allowedPid() != 0;
    std::vector<std::string> heldOpen;
    const auto takeBack = [&]() {
        if (!mayReconnect || heldOpen.empty() || (cancelled && cancelled())) return false;
        std::string failed;
        if (HideFilter::reconnect(heldOpen, 20'000, cancelled, failed)) return true;
        why += "; reconnecting the wheel did not help either: " + failed;
        return false;
    };
    if (!relisted.empty()) {
        WE_LOG_INFO(hideLog(), "hide: restarting {} with wheelbri-hide", joined(relisted));
        if (!HideFilter::restartNow(relisted, 15'000, cancelled, why, mayReconnect ? &heldOpen : nullptr) &&
            !takeBack()) {
            if (cancelled && cancelled()) {
                message = "stopped";
                return HideSetup::Waiting;
            }
            message = "the filter is on, but Windows has not restarted the device with it yet: " + why +
                      ". Close what uses the wheel (the program named there; else its maker's software, Steam, a web "
                      "browser, the Game Controllers window) or unplug it and plug it in again, then press Start again";
            WE_LOG_ERROR(hideLog(), "hide: {}", message);
            return HideSetup::Waiting;
        }
    }
    if (HideFilter::waitUntilStarted(interfaces, 25'000, cancelled, why, restartAfterMs,
                                     mayReconnect ? &heldOpen : nullptr) ||
        (takeBack() && HideFilter::waitUntilStarted(interfaces, 10'000, cancelled, why, 0))) {
        WE_LOG_INFO(hideLog(), "hide: wheelbri-hide runs on every connected device of the rig");
        return HideSetup::Running;
    }
    if (cancelled && cancelled()) {
        message = "stopped";
        return HideSetup::Waiting;
    }
    message = "the filter is on, but Windows has not restarted the device with it yet: " + why +
              ". Close what uses the wheel (the program named there; else its maker's software, Steam, a web browser, "
              "the Game Controllers window) or unplug it and plug it in again, then press Start again";
    WE_LOG_ERROR(hideLog(), "hide: {}", message);
    return HideSetup::Waiting;
}

HideSetup takeHideFilterOff(const std::vector<std::string>& interfaces, const Cancelled& cancelled,
                            std::string& message) {
    std::vector<std::string> changedIds;
    for (const std::string& id : interfaces) {
        if (!HideFilter::hasOurDriver(id)) continue;   // unplugged, or never had it: nothing to take off
        bool changed = false;
        if (const std::string problem = HideFilter::setFilterListed(id, false, changed); !problem.empty()) {
            message = problem;
            WE_LOG_ERROR(hideLog(), "hide: {}", message);
            return HideSetup::Failed;
        }
        if (changed) changedIds.push_back(id);
    }
    if (changedIds.empty()) return HideSetup::NothingToDo;
    std::string why;
    if (HideFilter::restartNow(changedIds, 10'000, cancelled, why)) {
        WE_LOG_INFO(hideLog(), "hide: wheelbri-hide is out of the rig's devices {} until the next Start",
                    joined(changedIds));
        return HideSetup::Running;
    }
    message = "the hide filter is off, but Windows has not restarted the wheel without it yet: " + why +
              ". It keeps costing CPU until the wheel restarts: close what uses it, or unplug it and plug it in again";
    WE_LOG_WARN(hideLog(), "hide: {}", message);
    return HideSetup::Waiting;
}

bool writeAllowedPid(const std::uint32_t pid, const std::uint64_t processStart) {
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kPolicyKey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                                     nullptr, &key, nullptr);
    if (status == ERROR_SUCCESS) {
        // The start time first: the filter reads AllowedPid, then checks it against AllowedPidStart.
        const ULONGLONG started = pid == 0 ? 0 : processStart;
        const LSTATUS startStatus = RegSetValueExW(key, L"AllowedPidStart", 0, REG_QWORD,
                                                   reinterpret_cast<const BYTE*>(&started), sizeof(started));
        if (startStatus != ERROR_SUCCESS) {
            WE_LOG_WARN(hideLog(), "hide: cannot write AllowedPidStart (error {}): a reused process id may stay allowed",
                        startStatus);
        }
        const DWORD value = pid;
        status = RegSetValueExW(key, L"AllowedPid", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(key);
    }
    if (status != ERROR_SUCCESS) {
        WE_LOG_ERROR(hideLog(), "hide: cannot write HKLM\\SOFTWARE\\wheelbri\\hide (error {}){}", status,
                     status == ERROR_ACCESS_DENIED ? ": run the service as Administrator" : "");
        return false;
    }
    return true;
}

bool writeLogLevel(const LogLevel level, std::string& message) {
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\wheelbri", 0, nullptr, REG_OPTION_NON_VOLATILE,
                                     KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status == ERROR_SUCCESS) {
        const std::string name = logLevelName(clampToApp(level));
        const std::wstring value(name.begin(), name.end());   // ASCII
        status = RegSetValueExW(key, L"LogLevel", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
    if (status != ERROR_SUCCESS) {
        message = "cannot write HKLM\\SOFTWARE\\wheelbri LogLevel (error " + std::to_string(status) + ")";
        return false;
    }
    return true;
}

std::uint32_t allowedPid() {
    DWORD pid = 0;
    DWORD size = sizeof(pid);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kPolicyKey, L"AllowedPid", RRF_RT_REG_DWORD, nullptr, &pid, &size) !=
        ERROR_SUCCESS) {
        return 0;
    }
    return pid;
}

bool allowOnly(void* const process, std::string& message) {
    const HANDLE handle = static_cast<HANDLE>(process);
    const DWORD pid = GetProcessId(handle);
    const std::uint64_t started = creationTime(handle);
    if (pid == 0 || started == 0) {
        message = "cannot read the process's id and start time (error " + std::to_string(GetLastError()) + ")";
        WE_LOG_ERROR(hideLog(), "hide: {}", message);
        return false;
    }
    letFilterCheck(handle);
    if (!writeAllowedPid(pid, started)) {
        message = "cannot write HKLM\\SOFTWARE\\wheelbri\\hide";
        return false;
    }
    return true;
}

#else  // not Windows

std::filesystem::path findDriverScript(const std::filesystem::path&, const std::filesystem::path&, int) {
    return {};
}

std::optional<int> runDriverScript(const std::filesystem::path&, const std::vector<std::string>&, const char*,
                                   const Cancelled&, bool& started, std::string& error) {
    started = false;
    error = "driver scripts run on Windows only";
    return std::nullopt;
}

bool installWheelDriver(int, const std::filesystem::path&, const Cancelled&, std::string& message) {
    message = "the virtual wheel's driver is for Windows only";
    return false;
}

HideSetup putHideFilter(const std::vector<std::string>&, const std::vector<std::string>& interfaces, int,
                        const std::filesystem::path&, const Cancelled&, std::string& message) {
    if (interfaces.empty()) return HideSetup::NothingToDo;
    message = "the hide filter is for Windows only (" + joined(interfaces) + ")";
    return HideSetup::Failed;
}

HideSetup takeHideFilterOff(const std::vector<std::string>& interfaces, const Cancelled&, std::string& message) {
    if (interfaces.empty()) return HideSetup::NothingToDo;
    message = "the hide filter is for Windows only (" + joined(interfaces) + ")";
    return HideSetup::Failed;
}

bool writeAllowedPid(std::uint32_t, std::uint64_t) { return false; }
bool writeLogLevel(LogLevel, std::string&) { return true; }
std::uint32_t allowedPid() { return 0; }

bool allowOnly(void*, std::string& message) {
    message = "the hide filter is for Windows only";
    return false;
}

#endif

}  // namespace wheelbri::service::privileged
