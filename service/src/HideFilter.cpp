#include "HideFilter.hpp"

#ifdef _WIN32
#include "DinputDevice.hpp"
#endif

#include "wheelbri/HidHideCli.hpp"
#include "wheelbri/Logging.hpp"

#ifdef _WIN32

#include <windows.h>
#include <aclapi.h>
#include <cfgmgr32.h>

#include <string>
#include <string_view>
#include <vector>

#ifndef DN_NEED_RESTART
#define DN_NEED_RESTART 0x00000100
#endif

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.hide");
    return logger;
}

constexpr wchar_t kPolicyKey[] = L"SOFTWARE\\wheelbri\\hide";

std::string narrow(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size, nullptr, nullptr);
    return text;
}

// Strings of a REG_MULTI_SZ buffer.
std::vector<std::string> multiSz(const wchar_t* data, std::size_t chars) {
    std::vector<std::string> items;
    std::size_t start = 0;
    for (std::size_t i = 0; i < chars; ++i) {
        if (data[i] == L'\0') {
            if (i == start) break;
            items.push_back(narrow(std::wstring_view(data + start, i - start)));
            start = i + 1;
        }
    }
    return items;
}

// This process's creation time: with the id, it tells the filter whether the process that hid the wheel still
// runs (an id can be reused by another program after a crash).
ULONGLONG processStart() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    return (static_cast<ULONGLONG>(created.dwHighDateTime) << 32U) | created.dwLowDateTime;
}

// The filter runs in the driver host as LocalService, which may not look at an elevated process: then it cannot
// compare AllowedPidStart and must assume the process still runs. Let it query this process (name and times only:
// PROCESS_QUERY_LIMITED_INFORMATION), so a reused process id after a crash is recognised.
void letFilterCheckThisProcess() {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    DWORD status = GetSecurityInfo(GetCurrentProcess(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                   &dacl, nullptr, &descriptor);
    if (status != ERROR_SUCCESS) {
        WE_LOG_WARN(log(), "hide: cannot read this process's security (error {}): after a crash a reused process id "
                           "may keep the wheel hidden (wheelbri-service --unhide fixes it)", status);
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
            status = SetSecurityInfo(GetCurrentProcess(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                     nullptr, updated, nullptr);
        }
    } else {
        status = GetLastError();
    }
    if (status != ERROR_SUCCESS) {
        WE_LOG_WARN(log(), "hide: cannot let wheelbri-hide check this process (error {}): after a crash a reused "
                           "process id may keep the wheel hidden (wheelbri-service --unhide fixes it)", status);
    }
    if (updated != nullptr) LocalFree(updated);
    LocalFree(descriptor);
}

bool writeAllowedPid(const DWORD pid) {
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kPolicyKey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                                     nullptr, &key, nullptr);
    if (status == ERROR_SUCCESS) {
        // The start time first: the filter reads AllowedPid, then checks it against AllowedPidStart.
        const ULONGLONG started = pid == 0 ? 0 : processStart();
        const LSTATUS startStatus =
            RegSetValueExW(key, L"AllowedPidStart", 0, REG_QWORD, reinterpret_cast<const BYTE*>(&started), sizeof(started));
        if (startStatus != ERROR_SUCCESS) {
            WE_LOG_WARN(log(), "hide: cannot write AllowedPidStart (error {}): a reused process id may stay allowed",
                        startStatus);
        }
        status = RegSetValueExW(key, L"AllowedPid", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&pid), sizeof(pid));
        RegCloseKey(key);
    }
    if (status != ERROR_SUCCESS) {
        WE_LOG_ERROR(log(), "hide: cannot write HKLM\\SOFTWARE\\wheelbri\\hide (error {}){}", status,
                     status == ERROR_ACCESS_DENIED ? ": run the service as Administrator" : "");
        return false;
    }
    return true;
}

// Device properties (the same Device Manager and Get-PnpDeviceProperty show), defined here rather than through
// <devpkey.h> and INITGUID.
constexpr DEVPROPKEY kUpperFilters{{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 19};   // DEVPKEY_Device_UpperFilters (20 is LowerFilters)
constexpr DEVPROPKEY kDriverDesc{{0xa8b865dd, 0x2e3d, 0x4094, {0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6}}, 4};

// A device's string or string-list property; empty if it has none.
std::vector<std::string> devNodeStrings(const DEVINST node, const DEVPROPKEY& key) {
    wchar_t buffer[2048] = {};
    ULONG bytes = sizeof(buffer) - 2 * sizeof(wchar_t);   // room for the list's closing zeros
    DEVPROPTYPE type = 0;
    if (CM_Get_DevNode_PropertyW(node, &key, &type, reinterpret_cast<PBYTE>(buffer), &bytes, 0) != CR_SUCCESS) return {};
    if (type != DEVPROP_TYPE_STRING && type != DEVPROP_TYPE_STRING_LIST) return {};
    return multiSz(buffer, bytes / sizeof(wchar_t) + 1);
}

std::string joined(const std::vector<std::string>& items) {
    std::string text;
    for (const std::string& item : items) text += (text.empty() ? "" : ", ") + item;
    return text.empty() ? "none" : text;
}

bool listsIgnoreCase(const std::vector<std::string>& items, std::string_view wanted) {
    for (const std::string& item : items) {
        if (hidhide::equalsIgnoreCase(item, wanted)) return true;
    }
    return false;
}

}  // namespace

const char* HideFilter::stateName(const State state) {
    switch (state) {
        case State::Ready: return "hidden while WheelBri runs";
        case State::NotInstalled: return "no filter yet";
        case State::NeedsRestart: return "filter waits for a restart";
        case State::NotConnected: return "not connected";
    }
    return "?";
}

std::vector<HideFilter::Check> HideFilter::check(const std::vector<Target>& targets, const bool quiet) {
    std::vector<Check> checks;
    for (const Target& target : targets) checks.push_back({target, State::NotConnected, {}, "not connected", {}});

    // The game-controller interfaces are exactly what DirectInput lists (what games see), whatever HID usage the
    // device reports: the Moza R3, a multi-axis controller, has no compatible id that says "game controller" and the
    // registry does not give compatible ids here. A vendor tool's other interfaces are never listed.
    for (const std::string& path : dinput::gameControllerPaths()) {
        const std::string id = hidhide::instanceIdOfHidPath(path);
        const auto usb = hidhide::hidDeviceIdOf(id);
        if (!usb) continue;
        Check* entry = nullptr;
        for (Check& c : checks) {
            if (c.target.id == *usb) entry = &c;
        }
        if (entry == nullptr) continue;
        DEVINST node = 0;
        const std::wstring wide(id.begin(), id.end());   // device instance ids are ASCII
        if (CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(wide.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
            entry->otherInterfaces.push_back(id + " (DirectInput lists it, but Windows does not find its device)");
            continue;
        }
        entry->interfaces.push_back(id);
        State state = State::Ready;
        std::string detail;
        // Ours: the reflector in its upper filters (any user-mode filter shows as WUDFRd) and our INF's driver
        // description ("... (wheelbri: hidden from games ...)"; "wheelemu" before the rename).
        const std::vector<std::string> description = devNodeStrings(node, kDriverDesc);
        const std::vector<std::string> filters = devNodeStrings(node, kUpperFilters);
        const bool ourDriver = !description.empty() && (hidhide::containsIgnoreCase(description.front(), "wheelbri") ||
                                                        hidhide::containsIgnoreCase(description.front(), "wheelemu"));
        if (!listsIgnoreCase(filters, "WUDFRd") || !ourDriver) {
            state = State::NotInstalled;
            detail = "no wheelbri-hide on " + id + ": driver '" + joined(description) + "', upper filters " + joined(filters);
        } else {
            // Listed in the registry is not running: after the driver is switched, Windows may load the new stack
            // only when the interface restarts (something still had it open). Until then nothing is hidden.
            ULONG status = 0;
            ULONG problem = 0;
            if (CM_Get_DevNode_Status(&status, &problem, node, 0) == CR_SUCCESS &&
                ((status & DN_NEED_RESTART) != 0 || (status & DN_HAS_PROBLEM) != 0)) {
                state = State::NeedsRestart;
                detail = "wheelbri-hide is set on " + id + " but not running yet (" +
                         ((status & DN_NEED_RESTART) != 0 ? std::string("Windows wants a restart")
                                                          : "problem code " + std::to_string(problem)) +
                         ")";
            }
        }
        // The device counts as its worst interface.
        const auto rank = [](const State s) {
            return s == State::NotInstalled ? 3 : s == State::NeedsRestart ? 2 : s == State::Ready ? 1 : 0;
        };
        if (entry->interfaces.size() == 1 || rank(state) > rank(entry->state)) {
            entry->state = state;
            entry->detail = detail;
        }
    }
    if (!quiet) {
        for (const Check& c : checks) {
            const std::string what = "'" + c.target.name + "' (" + hidhide::deviceIdText(c.target.id) + ")";
            if (c.state == State::Ready) {
                std::string list;
                for (const std::string& i : c.interfaces) list += (list.empty() ? "" : ", ") + i;
                WE_LOG_INFO(log(), "hide: wheelbri-hide is on {}: {}", what, list);
            } else {
                WE_LOG_WARN(log(), "hide: {}: {}{}", what, stateName(c.state), c.detail.empty() ? "" : " (" + c.detail + ")");
            }
            if (c.interfaces.empty() && !c.otherInterfaces.empty()) {
                std::string list;
                for (const std::string& i : c.otherInterfaces) list += (list.empty() ? "" : ", ") + i;
                WE_LOG_WARN(log(), "hide: {}: {}", what, list);
            }
        }
    }
    return checks;
}

bool HideFilter::hide() {
    letFilterCheckThisProcess();
    hidden_ = writeAllowedPid(GetCurrentProcessId());
    if (hidden_) {
        WE_LOG_INFO(log(), "hide: the rig is hidden from games (only this process, {}, may open its devices)",
                    GetCurrentProcessId());
    }
    return hidden_;
}

void HideFilter::restore() {
    if (hidden_ && writeAllowedPid(0)) {
        WE_LOG_INFO(log(), "hide: the rig is visible to games again");
    }
    hidden_ = false;
}

void HideFilter::clear() {
    if (writeAllowedPid(0)) {
        WE_LOG_INFO(log(), "hide: wheelbri-hide lets every program open the rig's devices again");
    }
}

void HideFilter::clearStale() {
    DWORD pid = 0;
    DWORD size = sizeof(pid);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kPolicyKey, L"AllowedPid", RRF_RT_REG_DWORD, nullptr, &pid, &size) !=
            ERROR_SUCCESS ||
        pid == 0 || pid == GetCurrentProcessId()) {
        return;
    }
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    const bool alive = process != nullptr && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    if (process != nullptr) CloseHandle(process);
    if (alive) {
        WE_LOG_WARN(log(), "hide: process {} hides the wheel (another wheelbri-service?); this one may not see it", pid);
        return;
    }
    WE_LOG_WARN(log(), "hide: the wheel was still hidden for process {}, which is gone (crash?): clearing", pid);
    if (!writeAllowedPid(0)) {
        WE_LOG_ERROR(log(), "hide: the wheel stays hidden: run wheelbri-service --unhide as Administrator");
    }
}

HideFilter::~HideFilter() { restore(); }

}  // namespace wheelbri::service

#else  // not Windows

namespace wheelbri::service {

std::vector<HideFilter::Check> HideFilter::check(const std::vector<Target>& targets, bool) {
    std::vector<Check> checks;
    for (const Target& target : targets) checks.push_back({target, State::NotConnected, {}, "not on Windows", {}});
    return checks;
}
const char* HideFilter::stateName(const State state) {
    return state == State::Ready ? "hidden while WheelBri runs" : "not hidden";
}
bool HideFilter::hide() { return false; }
void HideFilter::restore() {}
void HideFilter::clear() {}
void HideFilter::clearStale() {}
HideFilter::~HideFilter() = default;

}  // namespace wheelbri::service

#endif
