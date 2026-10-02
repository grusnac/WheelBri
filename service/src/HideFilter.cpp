#include "HideFilter.hpp"

#include "wheelemu/HidHideCli.hpp"
#include "wheelemu/Logging.hpp"

#ifdef _WIN32

#include <windows.h>
#include <cfgmgr32.h>

#include <string_view>
#include <vector>

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

bool writeAllowedPid(const DWORD pid) {
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kPolicyKey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                                     nullptr, &key, nullptr);
    if (status == ERROR_SUCCESS) {
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

std::unique_ptr<HideFilter> HideFilter::find(const std::string& vendorId) {
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
        for (const std::string& filter : multiSz(filters, bytes / sizeof(wchar_t))) {
            if (hidhide::equalsIgnoreCase(filter, "WUDFRd")) {
                WE_LOG_INFO(log(), "hide: wheelemu-hide is on {}", id);
                return std::unique_ptr<HideFilter>(new HideFilter(id));
            }
        }
    }
    return nullptr;
}

bool HideFilter::hide() {
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
