#include "HideFilter.hpp"

#include "wheelemu/HidHideCli.hpp"
#include "wheelemu/Logging.hpp"

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

namespace wheelemu::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.hide");
    return logger;
}

constexpr wchar_t kPolicyKey[] = L"SOFTWARE\\wheelemu\\hide";

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
                           "may keep the wheel hidden (wheelemu-service --unhide fixes it)", status);
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
        WE_LOG_WARN(log(), "hide: cannot let wheelemu-hide check this process (error {}): after a crash a reused "
                           "process id may keep the wheel hidden (wheelemu-service --unhide fixes it)", status);
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
        WE_LOG_ERROR(log(), "hide: cannot write HKLM\\SOFTWARE\\wheelemu\\hide (error {}){}", status,
                     status == ERROR_ACCESS_DENIED ? ": run the service as Administrator" : "");
        return false;
    }
    return true;
}

}  // namespace

std::unique_ptr<HideFilter> HideFilter::find(const std::string& vendorId, const bool quiet) {
    ULONG size = 0;
    const ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
    if (CM_Get_Device_ID_List_SizeW(&size, L"HID", flags) != CR_SUCCESS || size == 0) return nullptr;
    std::vector<wchar_t> ids(size);
    if (CM_Get_Device_ID_ListW(L"HID", ids.data(), size, flags) != CR_SUCCESS) return nullptr;

    const std::string prefix = "HID\\VID_" + vendorId + "&";
    for (const std::string& id : multiSz(ids.data(), ids.size())) {
        if (id.size() < prefix.size() || !hidhide::equalsIgnoreCase(std::string_view(id).substr(0, prefix.size()), prefix)) {
            continue;
        }
        DEVINST node = 0;
        const std::wstring wide(id.begin(), id.end());   // device instance ids are ASCII
        if (CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(wide.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) continue;
        wchar_t filters[512] = {};
        ULONG bytes = sizeof(filters);
        ULONG type = 0;
        if (CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_UPPERFILTERS, &type, filters, &bytes, 0) != CR_SUCCESS) continue;
        bool listed = false;
        for (const std::string& filter : multiSz(filters, bytes / sizeof(wchar_t))) {
            listed = listed || hidhide::equalsIgnoreCase(filter, "WUDFRd");
        }
        if (!listed) continue;
        // Listed in the registry is not running: after the driver is switched, Windows may load the new stack only
        // when the interface restarts (something still had it open). Until then nothing is hidden.
        ULONG status = 0;
        ULONG problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, node, 0) == CR_SUCCESS &&
            ((status & DN_NEED_RESTART) != 0 || (status & DN_HAS_PROBLEM) != 0)) {
            if (!quiet) {
                WE_LOG_WARN(log(), "hide: wheelemu-hide is set on {} but not running yet ({}): Windows loads it when the "
                                   "R3 restarts (unplug it and plug it in again, or restart Windows)",
                            id, (status & DN_NEED_RESTART) != 0 ? std::string("Windows wants a restart")
                                                                 : "problem code " + std::to_string(problem));
            }
            continue;
        }
        WE_LOG_INFO(log(), "hide: wheelemu-hide is on {}", id);
        return std::unique_ptr<HideFilter>(new HideFilter(id));
    }
    return nullptr;
}

bool HideFilter::hide() {
    letFilterCheckThisProcess();
    hidden_ = writeAllowedPid(GetCurrentProcessId());
    if (hidden_) {
        WE_LOG_INFO(log(), "hide: real wheel hidden from games (only this process, {}, may open {})",
                    GetCurrentProcessId(), instanceId_);
    }
    return hidden_;
}

void HideFilter::restore() {
    if (hidden_ && writeAllowedPid(0)) {
        WE_LOG_INFO(log(), "hide: real wheel visible to games again");
    }
    hidden_ = false;
}

void HideFilter::clear() {
    if (writeAllowedPid(0)) {
        WE_LOG_INFO(log(), "hide: wheelemu-hide lets every program open the wheel again");
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
        WE_LOG_WARN(log(), "hide: process {} hides the wheel (another wheelemu-service?); this one may not see it", pid);
        return;
    }
    WE_LOG_WARN(log(), "hide: the wheel was still hidden for process {}, which is gone (crash?): clearing", pid);
    if (!writeAllowedPid(0)) {
        WE_LOG_ERROR(log(), "hide: the wheel stays hidden: run wheelemu-service --unhide as Administrator");
    }
}

HideFilter::HideFilter(std::string instanceId) : instanceId_(std::move(instanceId)) {}
HideFilter::~HideFilter() { restore(); }

}  // namespace wheelemu::service

#else  // not Windows

namespace wheelemu::service {

std::unique_ptr<HideFilter> HideFilter::find(const std::string&) { return nullptr; }
bool HideFilter::hide() { return false; }
void HideFilter::restore() {}
void HideFilter::clear() {}
void HideFilter::clearStale() {}
HideFilter::HideFilter(std::string instanceId) : instanceId_(std::move(instanceId)) {}
HideFilter::~HideFilter() = default;

}  // namespace wheelemu::service

#endif
