#include "DeviceHolders.hpp"

#ifdef _WIN32

#include "wheelbri/Logging.hpp"

#include <windows.h>
#include <cfgmgr32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.hide");
    return logger;
}

// DEVPKEY_Device_PDOName: the name of the device object ("\Device\000000e3"), which is what a program's handle to the
// device is named. Spelt out like HideFilter.cpp's keys, without <devpkey.h> and INITGUID.
constexpr DEVPROPKEY kPdoName{{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 16};

// Device names are ASCII; anything else becomes '?' (it is only compared with device names).
std::string narrow(const wchar_t* const text, const std::size_t length) {
    std::string result;
    result.reserve(length);
    for (std::size_t i = 0; i < length && text[i] != L'\0'; ++i) {
        result.push_back(text[i] < 0x80 ? static_cast<char>(text[i]) : '?');
    }
    return result;
}

// The device object names of `node` and of every node below it (a program may hold a child open: the veto names the
// node that was asked to go).
void collectDeviceNames(const DEVINST node, std::vector<std::string>& names, const int depth) {
    std::array<wchar_t, 256> buffer{};
    ULONG bytes = static_cast<ULONG>((buffer.size() - 1) * sizeof(wchar_t));
    DEVPROPTYPE type = 0;
    if (CM_Get_DevNode_PropertyW(node, &kPdoName, &type, reinterpret_cast<PBYTE>(buffer.data()), &bytes, 0) ==
            CR_SUCCESS &&
        type == DEVPROP_TYPE_STRING) {
        names.push_back(narrow(buffer.data(), bytes / sizeof(wchar_t)));
    }
    if (depth >= 8) return;
    DEVINST child = 0;
    if (CM_Get_Child(&child, node, 0) != CR_SUCCESS) return;
    do {
        collectDeviceNames(child, names, depth + 1);
    } while (CM_Get_Sibling(&child, child, 0) == CR_SUCCESS);
}

// SeDebugPrivilege on while it lives (an Administrator has it, switched off), as before afterwards: without it a
// service's handles (another user's process) cannot be looked at.
class DebugPrivilege {
public:
    DebugPrivilege() {
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token_)) {
            token_ = nullptr;
            return;
        }
        TOKEN_PRIVILEGES wanted{};
        wanted.PrivilegeCount = 1;
        wanted.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (!LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &wanted.Privileges[0].Luid)) return;
        DWORD size = sizeof(previous_);
        adjusted_ = AdjustTokenPrivileges(token_, FALSE, &wanted, sizeof(previous_), &previous_, &size) != FALSE &&
                    GetLastError() == ERROR_SUCCESS;
    }
    ~DebugPrivilege() {
        if (adjusted_) AdjustTokenPrivileges(token_, FALSE, &previous_, 0, nullptr, nullptr);
        if (token_ != nullptr) CloseHandle(token_);
    }
    DebugPrivilege(const DebugPrivilege&) = delete;
    DebugPrivilege& operator=(const DebugPrivilege&) = delete;

private:
    HANDLE token_ = nullptr;
    TOKEN_PRIVILEGES previous_{};
    bool adjusted_ = false;
};

// One entry of the computer's handle list (SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX), after a count and a reserved word.
struct HandleEntry {
    PVOID object;
    ULONG_PTR pid;
    ULONG_PTR handle;
    ULONG grantedAccess;
    USHORT creatorBackTraceIndex;
    USHORT objectTypeIndex;
    ULONG handleAttributes;
    ULONG reserved;
};
struct HandleListHeader {
    ULONG_PTR count;
    ULONG_PTR reserved;
};

template <typename Function>
Function fromNtdll(const char* const name) {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) return nullptr;
    return reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(ntdll, name)));
}

// Every handle of every process (NtQuerySystemInformation(SystemExtendedHandleInformation)); empty if Windows will
// not say. Tens of thousands on a usual PC.
std::vector<HandleEntry> handleList() {
    using Query = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
    static const auto query = fromNtdll<Query>("NtQuerySystemInformation");
    if (query == nullptr) return {};
    constexpr ULONG kSystemExtendedHandleInformation = 64;
    constexpr LONG kInfoLengthMismatch = static_cast<LONG>(0xC0000004L);
    constexpr std::size_t kMaxBytes = 256U * 1024U * 1024U;
    std::vector<std::uint8_t> buffer(4U * 1024U * 1024U);
    LONG status = kInfoLengthMismatch;
    while (status == kInfoLengthMismatch) {
        ULONG needed = 0;
        status = query(kSystemExtendedHandleInformation, buffer.data(), static_cast<ULONG>(buffer.size()), &needed);
        if (status != kInfoLengthMismatch) break;
        const std::size_t next = (std::max)(buffer.size() * 2, static_cast<std::size_t>(needed) + 1024U * 1024U);
        if (next > kMaxBytes) return {};
        buffer.resize(next);
    }
    if (status < 0) return {};
    HandleListHeader header{};
    std::memcpy(&header, buffer.data(), sizeof(header));
    const std::size_t room = (buffer.size() - sizeof(header)) / sizeof(HandleEntry);
    const std::size_t count = (std::min)(static_cast<std::size_t>(header.count), room);
    std::vector<HandleEntry> entries(count);
    if (count > 0) std::memcpy(entries.data(), buffer.data() + sizeof(header), count * sizeof(HandleEntry));
    return entries;
}

// The name of the object behind `handle` ("\Device\000000e3"), empty if Windows will not say.
std::string objectName(const HANDLE handle) {
    using Query = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static const auto query = fromNtdll<Query>("NtQueryObject");
    if (query == nullptr) return {};
    constexpr ULONG kObjectNameInformation = 1;
    struct CountedString {   // UNICODE_STRING, without winternl.h
        USHORT length;
        USHORT maximumLength;
        PWSTR buffer;
    };
    alignas(CountedString) std::array<std::uint8_t, 2048> buffer{};
    ULONG returned = 0;
    if (query(handle, kObjectNameInformation, buffer.data(), static_cast<ULONG>(buffer.size()), &returned) < 0) {
        return {};
    }
    CountedString name{};
    std::memcpy(&name, buffer.data(), sizeof(name));
    if (name.buffer == nullptr || name.length == 0) return {};
    return narrow(name.buffer, name.length / sizeof(wchar_t));
}

// How far one look through the list got; shared with its thread, which may outlive the look (see deviceHolders).
struct Progress {
    std::atomic<std::size_t> at{0};
    std::atomic<bool> finished{false};
    std::atomic<bool> abandoned{false};
    std::mutex lock;
    std::vector<std::uint32_t> pids;   // under lock
};

// Looks at `candidates` from `start` on: a copy of each handle into this process, its name, then closed again.
void lookThrough(const std::shared_ptr<const std::vector<HandleEntry>> candidates,
                 const std::shared_ptr<const std::vector<std::string>> names, const std::size_t start,
                 const std::shared_ptr<Progress> progress) {
    std::unordered_map<ULONG_PTR, HANDLE> processes;
    const HANDLE self = GetCurrentProcess();
    for (std::size_t i = start; i < candidates->size() && !progress->abandoned; ++i) {
        progress->at = i;
        const HandleEntry& entry = (*candidates)[i];
        const auto [process, inserted] = processes.try_emplace(entry.pid, nullptr);
        if (inserted) process->second = OpenProcess(PROCESS_DUP_HANDLE, FALSE, static_cast<DWORD>(entry.pid));
        if (process->second == nullptr) continue;   // a protected process: not ours to look at
        HANDLE copy = nullptr;
        if (!DuplicateHandle(process->second, reinterpret_cast<HANDLE>(entry.handle), self, &copy, 0, FALSE, 0)) {
            continue;
        }
        const std::string name = objectName(copy);   // can wait for ever (see deviceHolders)
        CloseHandle(copy);
        if (holders::namesDevice(name, *names)) {
            std::lock_guard guard(progress->lock);
            progress->pids.push_back(static_cast<std::uint32_t>(entry.pid));
        }
    }
    for (const auto& [pid, process] : processes) {
        if (process != nullptr) CloseHandle(process);
    }
    progress->finished = true;
}

// "firefox.exe", empty if Windows will not say.
std::string programName(const std::uint32_t pid) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) return {};
    std::wstring path(1024, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path.data(), &size);
    CloseHandle(process);
    if (!ok) return {};
    path.resize(size);
    const std::size_t slash = path.find_last_of(L'\\');
    const std::wstring file = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, file.data(), static_cast<int>(file.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string text(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, file.data(), static_cast<int>(file.size()), text.data(), bytes, nullptr, nullptr);
    return text;
}

}  // namespace

std::vector<holders::Holder> deviceHolders(const std::string& instanceId) {
    const std::wstring wide(instanceId.begin(), instanceId.end());   // device instance ids are ASCII
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(wide.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        return {};
    }
    const auto names = std::make_shared<std::vector<std::string>>();
    collectDeviceNames(node, *names, 0);
    if (names->empty()) {
        WE_LOG_DEBUG(log(), "hide: {} has no device object name: cannot tell who has it open", instanceId);
        return {};
    }

    const DebugPrivilege debug;
    // Which type number Windows gives file objects (a program's handle to a device is one): read off a handle of our
    // own, to the NUL device, opened for the purpose before the list is read.
    const HANDLE own = CreateFileW(L"\\\\.\\NUL", FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
    std::vector<HandleEntry> list = handleList();
    const DWORD selfPid = GetCurrentProcessId();
    USHORT fileType = 0;
    if (own != INVALID_HANDLE_VALUE) {
        for (const HandleEntry& entry : list) {
            if (entry.pid == selfPid && entry.handle == reinterpret_cast<ULONG_PTR>(own)) {
                fileType = entry.objectTypeIndex;
                break;
            }
        }
        CloseHandle(own);
    }
    if (fileType == 0) {
        WE_LOG_DEBUG(log(), "hide: cannot read the computer's handle list: cannot tell who has {} open", instanceId);
        return {};
    }
    const auto candidates = std::make_shared<std::vector<HandleEntry>>();
    for (const HandleEntry& entry : list) {
        // 0 and 4 are Windows itself (the idle process, System): nothing a user can close.
        if (entry.objectTypeIndex == fileType && entry.pid != selfPid && entry.pid > 4) candidates->push_back(entry);
    }
    list = {};

    // Asking a handle's name waits for ever on a few (a synchronous pipe another thread waits on: Sysinternals Handle
    // and Process Explorer meet the same). So the names are asked on a thread of their own; one that stops moving is
    // left behind, blocked (with its copy of that handle), and a new one carries on after that handle. A few such,
    // or the time up, and the answer is what was found so far.
    constexpr auto kStuckAfter = std::chrono::milliseconds(300);
    constexpr auto kTimeAllowed = std::chrono::seconds(5);
    constexpr int kMaxStuck = 8;
    const auto began = std::chrono::steady_clock::now();
    std::vector<std::uint32_t> pids;
    std::size_t start = 0;
    int stuck = 0;
    bool complete = candidates->empty();
    while (start < candidates->size()) {
        const auto progress = std::make_shared<Progress>();
        progress->at = start;
        std::thread(lookThrough, candidates, names, start, progress).detach();
        std::size_t seen = start;
        auto movedAt = std::chrono::steady_clock::now();
        while (!progress->finished) {
            Sleep(10);
            const auto now = std::chrono::steady_clock::now();
            const std::size_t at = progress->at;
            if (at != seen) {
                seen = at;
                movedAt = now;
            } else if (now - movedAt > kStuckAfter || now - began > kTimeAllowed) {
                break;
            }
        }
        const bool finished = progress->finished;   // before taking its results: nothing more comes after that
        {
            std::lock_guard guard(progress->lock);
            pids.insert(pids.end(), progress->pids.begin(), progress->pids.end());
        }
        if (finished) {
            complete = true;
            break;
        }
        progress->abandoned = true;   // should it ever wake up, it stops at the next handle
        if (std::chrono::steady_clock::now() - began > kTimeAllowed || ++stuck > kMaxStuck) break;
        start = progress->at + 1;
    }
    if (!complete) {
        WE_LOG_DEBUG(log(), "hide: looked at part of the handle list only ({} handles that did not answer, {:.1f} s)",
                     stuck, std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count());
    }
    std::sort(pids.begin(), pids.end());
    pids.erase(std::unique(pids.begin(), pids.end()), pids.end());
    std::vector<holders::Holder> found;
    found.reserve(pids.size());
    for (const std::uint32_t pid : pids) found.push_back({pid, programName(pid)});
    return found;
}

}  // namespace wheelbri::service

#else  // not Windows

namespace wheelbri::service {

std::vector<holders::Holder> deviceHolders(const std::string&) { return {}; }

}  // namespace wheelbri::service

#endif
