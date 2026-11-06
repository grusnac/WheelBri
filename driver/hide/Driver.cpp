// SPDX-License-Identifier: GPL-3.0-or-later
// wheelbri-hide: hides the rig (the wheel and every accessory the Wheel setup tab defined) from games while
// wheelbri-service presents a virtual wheel (G25, G27, G920 or G923).
// Our replacement for HidHide (third-party), built the same way as our G920 driver: UMDF 2, user mode, signed
// with the certificate trusted on this PC; no kernel code of ours, no test mode.
//
// It is an upper filter on the game-controller interface of each rig device (install-hide.ps1 -Devices puts it on
// exactly those; the wheel maker's own tool talks to the other interfaces). It does not care which device
// it is on: it decides only by the setting below.
// Every program that opens that interface asks it first (IRP_MJ_CREATE):
//   - while wheelbri-service hides the wheel, it has written its process id to
//       HKLM\SOFTWARE\wheelbri\hide  AllowedPid (DWORD)
//     and only that process may open the device; everyone else gets "access denied", so DirectInput, SDL, raw input
//     and the game cannot see or read it (the same effect as HidHide);
//   - when AllowedPid is 0 or missing (the service is not running, or stopped cleanly), everyone may open it:
//     it is an ordinary device again;
//   - when AllowedPid names a process that is gone (the service crashed, was killed, or the PC lost power), the
//     wheel is not hidden either: a crash must never leave the wheel hidden from everything. AllowedPidStart (the
//     process's creation time, QWORD FILETIME) catches a later process that got the same id, also one this driver
//     may not look at: a process created before Windows last started is gone (startedBeforeBoot).
// Fail open: if the setting cannot be read, the wheel is not hidden (and that is logged).
// Everything else (reads, force feedback, feature reports) passes through untouched.

#include <windows.h>
#include <wdf.h>

#include "wheelbri/LogFileNames.hpp"
#include "wheelbri/LogLevel.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr wchar_t kPolicyKey[] = L"SOFTWARE\\wheelbri\\hide";
constexpr ULONG kSystemPid = 4;
using Level = wheelbri::LogLevel;

// Log: <the logs folder>\wheelbri-hide-<date>-<NN>.log (see logFolder) and the debugger (DebugView).
std::mutex g_logLock;

// The folder for the log when LogFolder (below) is not set or not usable: made by this driver, in ProgramData.
constexpr wchar_t kFallbackLogFolder[] = L"C:\\ProgramData\\wheelbri\\logs";

std::wstring fallbackLogFolder() {
    CreateDirectoryW(L"C:\\ProgramData\\wheelbri", nullptr);
    CreateDirectoryW(kFallbackLogFolder, nullptr);
    return kFallbackLogFolder;
}

// The folder for the log: the one the app shares with the drivers (HKLM\SOFTWARE\wheelbri LogFolder, the user's
// %LOCALAPPDATA%\wheelbri\logs, which the app lets LocalService write in), else the fallback folder (before the app
// ever ran as Administrator). Read for every line, so a change applies at once.
std::wstring logFolder() {
    wchar_t shared[MAX_PATH] = {};
    DWORD size = sizeof(shared);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\wheelbri", L"LogFolder", RRF_RT_REG_SZ, nullptr, shared, &size) ==
            ERROR_SUCCESS &&
        shared[0] != L'\0') {
        return shared;
    }
    return fallbackLogFolder();
}

// Whether `path` is a plain full path below a drive's root ("C:\..."): no "." or ".." parts, no "/", not the root.
bool isPlainPath(std::wstring path) {
    if (path.size() < 3 || path[1] != L':' || path[2] != L'\\' || path.find(L'/') != std::wstring::npos) return false;
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    if (path.size() <= 3) return false;   // a drive's root is no log folder
    for (std::size_t start = 3; start <= path.size();) {
        const std::size_t end = (std::min)(path.find(L'\\', start), path.size());   // (std::min): windows.h's min macro
        const std::wstring part = path.substr(start, end - start);
        if (part.empty() || part == L"." || part == L"..") return false;
        start = end + 1;
    }
    return true;
}

// The path Windows really opened `handle` under ("C:\..."), empty if it cannot tell. Reached through a junction, a
// symbolic link or a mount point anywhere on the way, it is the target's path, not the one asked for: so comparing
// the two finds a link without reading the folders above (this driver runs as LocalService, which may pass through
// the user's profile folders but not look at them: checking each folder's attributes failed there, and every line
// went to the fallback folder).
std::wstring openedPath(HANDLE handle) {
    std::wstring buffer(1024, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                                   FILE_NAME_OPENED | VOLUME_NAME_DOS);
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    if (buffer.rfind(L"\\\\?\\UNC\\", 0) == 0) return {};       // a network path: never a log folder
    if (buffer.rfind(L"\\\\?\\", 0) == 0) buffer.erase(0, 4);
    return buffer;
}

bool samePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

// One log file per run of this driver (each time Windows loads it into a host: the device was made or restarted),
// and a new one at midnight: <folder>\wheelbri-hide-<date>-<NN>.log with the next free number of that day (core
// LogFileNames.hpp, the same rules as every other WheelBri log). The first line of a run in a folder makes the file
// (CREATE_NEW: never an existing name, so never a link someone put there; a number another host took a moment ago
// gives the next); later lines append to it. Under g_logLock.
struct RunFile {
    std::string date;
    std::wstring path;
};
std::unordered_map<std::wstring, RunFile> g_runFiles;   // folder -> this run's file there

constexpr const char* kLogStem = "wheelbri-hide";

HANDLE openRunFile(const std::wstring& folder, const SYSTEMTIME& now, std::wstring& path) {
    char dateText[16];
    std::snprintf(dateText, sizeof(dateText), "%04u-%02u-%02u", now.wYear, now.wMonth, now.wDay);
    const std::string date = dateText;
    constexpr DWORD kShare = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    constexpr DWORD kAccess = FILE_APPEND_DATA | FILE_READ_ATTRIBUTES;
    constexpr DWORD kFlags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT;
    RunFile& run = g_runFiles[folder];
    if (run.date == date && !run.path.empty()) {
        path = run.path;
        return CreateFileW(path.c_str(), kAccess, kShare, nullptr, OPEN_ALWAYS, kFlags, nullptr);
    }
    // The names of that day's files there (ours are ASCII; any other name just does not count).
    std::vector<std::string> names;
    const std::wstring pattern = folder + L"\\" + std::wstring(kLogStem, kLogStem + std::strlen(kLogStem)) + L"-" +
                                 std::wstring(date.begin(), date.end()) + L"*";
    WIN32_FIND_DATAW found{};
    const HANDLE search = FindFirstFileW(pattern.c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            std::string name;
            for (const wchar_t* c = found.cFileName; *c != L'\0' && *c < 0x80; ++c) name.push_back(static_cast<char>(*c));
            names.push_back(std::move(name));
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    int index = wheelbri::logfiles::nextIndex(names, kLogStem, date);
    for (int attempt = 0; attempt < 100 && index <= wheelbri::logfiles::kMaxIndex; ++attempt, ++index) {
        const std::string name = wheelbri::logfiles::numberedName(kLogStem, date, index);
        path = folder + L"\\" + std::wstring(name.begin(), name.end());
        const HANDLE file = CreateFileW(path.c_str(), kAccess, kShare, nullptr, CREATE_NEW, kFlags, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            run.date = date;
            run.path = path;
            return file;
        }
        if (GetLastError() != ERROR_FILE_EXISTS) return INVALID_HANDLE_VALUE;
    }
    SetLastError(ERROR_FILE_EXISTS);
    return INVALID_HANDLE_VALUE;
}

// Appends `line` to this run's <folder>\wheelbri-hide-<date>-<NN>.log (openRunFile); false if the folder is refused
// or the file cannot be opened.
// LogFolder lies in the user's own profile: the user could replace it, or a folder above it, with a link and so make
// this driver, which runs as LocalService, write wherever LocalService may. So the folder is opened first (nothing is
// created yet) and used only when it is no link itself and Windows opened it under the very path asked for; the file
// is opened without following a link (FILE_FLAG_OPEN_REPARSE_POINT), and one that is a link, has other names (hard
// links) or was opened under another path is not written. The path is a std::wstring: a long LogFolder only makes
// CreateFileW fail (then the fallback folder is used), where swprintf_s into a MAX_PATH buffer ended the driver host.
bool appendTo(std::wstring folder, const SYSTEMTIME& now, const char* line) {
    if (!isPlainPath(folder)) {
        SetLastError(ERROR_CANT_RESOLVE_FILENAME);
        return false;
    }
    while (folder.back() == L'\\') folder.pop_back();   // not empty: isPlainPath checked it
    HANDLE directory = CreateFileW(folder.c_str(), FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION folderInfo{};
    const bool folderOk = GetFileInformationByHandle(directory, &folderInfo) &&
                          (folderInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                          (folderInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
                          samePath(openedPath(directory), folder);
    CloseHandle(directory);
    if (!folderOk) {
        SetLastError(ERROR_CANT_RESOLVE_FILENAME);
        return false;
    }
    std::wstring path;
    HANDLE file = openRunFile(folder, now, path);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        info.nNumberOfLinks > 1 || !samePath(openedPath(file), path)) {
        CloseHandle(file);
        SetLastError(ERROR_CANT_RESOLVE_FILENAME);
        return false;
    }
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
    CloseHandle(file);
    return true;
}

// The level the lines are written at: HKLM\SOFTWARE\wheelbri LogLevel (REG_SZ trace, debug, info or warn: the app's
// Level button, written at each Start and on a change), read at most once a second. Warn when it is missing: warnings
// and errors are always written.
wheelbri::LogLevel currentLevel() {
    static std::atomic<std::uint64_t> readAtMs{0};
    static std::atomic<std::uint8_t> level{static_cast<std::uint8_t>(wheelbri::kDefaultAppLogLevel)};
    const std::uint64_t now = GetTickCount64();
    const std::uint64_t last = readAtMs.load();
    if (last == 0 || now - last >= 1000) {
        readAtMs = now;
        wchar_t value[16] = {};
        DWORD size = sizeof(value);
        std::string name;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\wheelbri", L"LogLevel", RRF_RT_REG_SZ, nullptr, value,
                         &size) == ERROR_SUCCESS) {
            for (const wchar_t* c = value; *c != L'\0'; ++c) {
                name.push_back(*c < 0x80 ? static_cast<char>(*c) : '?');
            }
        }
        level = static_cast<std::uint8_t>(wheelbri::appLogLevel(name));
    }
    return static_cast<wheelbri::LogLevel>(level.load());
}

// As spdlog names them in the other logs, so the Logs tab reads the level the same way.
const char* levelText(const wheelbri::LogLevel level) {
    switch (level) {
        case wheelbri::LogLevel::Trace: return "trace";
        case wheelbri::LogLevel::Debug: return "debug";
        case wheelbri::LogLevel::Info: return "info";
        case wheelbri::LogLevel::Warn: return "warning";
        case wheelbri::LogLevel::Error: return "error";
        default: return "critical";
    }
}

void logLine(const Level level, const std::string& text) {
    if (!wheelbri::logLevelAllows(currentLevel(), level)) return;
    SYSTEMTIME now;
    GetLocalTime(&now);
    char line[1024];
    const int length = std::snprintf(line, sizeof(line), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%s] %s\r\n", now.wYear,
                                     now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
                                     levelText(level), text.c_str());
    if (length < 0 || static_cast<std::size_t>(length) >= sizeof(line)) {
        // Cut to fit: it still ends as a line (with "..."), or the next line would be glued to it in the file.
        std::memcpy(line + sizeof(line) - 6, "...\r\n", 6);
    }
    OutputDebugStringA(line);
    std::lock_guard lock(g_logLock);
    static bool reportedFailure = false;
    if (appendTo(logFolder(), now, line) || appendTo(fallbackLogFolder(), now, line)) {
        reportedFailure = false;
    } else if (!reportedFailure) {
        reportedFailure = true;   // only DebugView can show this one
        char why[160];
        std::snprintf(why, sizeof(why), "wheelbri-hide: cannot write its log (error %lu)\r\n", GetLastError());
        OutputDebugStringA(why);
    }
}

// Logged once per distinct problem (a game opens the wheel often).
void logOnce(const Level level, std::string text) {
    static std::mutex lock;
    static std::string last;
    std::lock_guard guard(lock);
    if (text == last) return;
    last = text;
    logLine(level, text);
}

// Whether a process created at `processStart` was created before Windows last started, so is certainly gone (a
// restart ends every process). All in 100 ns units: `processStart` and `now` as FILETIME values (since 1601, UTC),
// `uptime` the time since Windows started (GetTickCount64, which counts sleep too). Windows started at
// `now - uptime`; kBootMargin absorbs the two clocks being read a moment apart and small corrections of the clock
// since. Nonsense input (an uptime longer than `now`) gives false: not known to be gone. After a shut down with Fast
// startup the uptime continues from before, so such a start counts as the same boot (the old behaviour: hidden).
constexpr ULONGLONG kBootMargin = 60ULL * 10'000'000ULL;   // 60 s

bool startedBeforeBoot(const ULONGLONG processStart, const ULONGLONG now, const ULONGLONG uptime) {
    if (uptime >= now) return false;
    const ULONGLONG bootedAt = now - uptime;
    return processStart < bootedAt && bootedAt - processStart > kBootMargin;
}

// When process `pid` was created, read from the system's process list, which needs no handle to the process (the
// driver host, LocalService, may not open it): nullopt when no such process runs, or the list cannot be read
// (`readable` false). NtQuerySystemInformation(SystemProcessInformation) through ntdll; CreateTime sits at offset 32
// of each entry on x64 and x86 alike, inside winternl.h's Reserved1 (bytes 8..55).
std::optional<ULONGLONG> creationTimeOf(const DWORD pid, bool& readable) {
    readable = false;
    using QueryFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<QueryFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")));
    if (query == nullptr) return std::nullopt;
    constexpr ULONG kSystemProcessInformation = 5;
    constexpr LONG kInfoLengthMismatch = static_cast<LONG>(0xC0000004L);
    std::vector<std::uint8_t> buffer(256 * 1024);
    LONG status = 0;
    for (int attempt = 0; attempt < 5; ++attempt) {
        ULONG needed = 0;
        status = query(kSystemProcessInformation, buffer.data(), static_cast<ULONG>(buffer.size()), &needed);
        if (status != kInfoLengthMismatch) break;
        buffer.resize(std::max<std::size_t>(buffer.size() * 2, static_cast<std::size_t>(needed) + 64 * 1024));
    }
    if (status < 0) return std::nullopt;
    readable = true;
    constexpr std::size_t kCreateTime = 32;
    struct UnicodeStringLayout {   // UNICODE_STRING, without winternl.h
        USHORT length;
        USHORT maximumLength;
        PWSTR buffer;
    };
    // UniqueProcessId follows ImageName (a UNICODE_STRING at 56) and BasePriority (padded to a pointer): 80 on x64.
    constexpr std::size_t kProcessId = 56 + sizeof(UnicodeStringLayout) + sizeof(void*);
    std::size_t offset = 0;
    while (offset + kProcessId + sizeof(void*) <= buffer.size()) {
        const std::uint8_t* entry = buffer.data() + offset;
        ULONG next = 0;
        std::memcpy(&next, entry, sizeof(next));
        ULONG_PTR id = 0;
        std::memcpy(&id, entry + kProcessId, sizeof(id));
        if (id == pid) {
            ULONGLONG created = 0;
            std::memcpy(&created, entry + kCreateTime, sizeof(created));
            return created;
        }
        if (next == 0) break;
        offset += next;
    }
    return std::nullopt;
}

// Whether `pid` still runs and is the process that wrote the setting. Unknown (cannot look) counts as running:
// the driver host runs as LocalService and may not open an elevated service. Unless the process that wrote the
// setting was created before this boot of Windows: then the id belongs to another program now (the service crashed
// and the PC restarted), and the wheel must not stay hidden.
bool stillRunning(const DWORD pid, const std::optional<ULONGLONG> started) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        const DWORD error = GetLastError();
        if (error == ERROR_INVALID_PARAMETER) return false;   // no such process
        if (started) {
            // Not allowed to open it: WheelBri lets LocalService look at itself, so this is most likely another
            // program that got the id after a crash. The process list says when the process with that id started,
            // without opening it.
            bool readable = false;
            const std::optional<ULONGLONG> created = creationTimeOf(pid, readable);
            if (readable) return created && *created == *started;
            FILETIME now{};
            GetSystemTimeAsFileTime(&now);
            const ULONGLONG nowValue = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32U) | now.dwLowDateTime;
            if (startedBeforeBoot(*started, nowValue, GetTickCount64() * 10'000ULL)) return false;
        }
        return true;
    }
    bool running = true;
    DWORD code = 0;
    if (GetExitCodeProcess(process, &code) && code != STILL_ACTIVE) running = false;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (running && started && GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        const ULONGLONG createdAt = (static_cast<ULONGLONG>(created.dwHighDateTime) << 32U) | created.dwLowDateTime;
        if (createdAt != *started) running = false;   // the id was reused by another program
    }
    CloseHandle(process);
    return running;
}

// The process allowed to open the wheel while it is hidden; 0 = not hidden.
ULONG allowedPid() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LSTATUS status =
        RegGetValueW(HKEY_LOCAL_MACHINE, kPolicyKey, L"AllowedPid", RRF_RT_REG_DWORD, nullptr, &value, &size);
    if (status != ERROR_SUCCESS) {
        if (status != ERROR_FILE_NOT_FOUND) {
            logOnce(Level::Warn, "cannot read HKLM\\SOFTWARE\\wheelbri\\hide AllowedPid (error " +
                                     std::to_string(status) + "): the wheel is not hidden");
        }
        return 0;
    }
    if (value == 0) return 0;
    ULONGLONG started = 0;
    DWORD startedSize = sizeof(started);
    const bool haveStart = RegGetValueW(HKEY_LOCAL_MACHINE, kPolicyKey, L"AllowedPidStart", RRF_RT_REG_QWORD, nullptr,
                                        &started, &startedSize) == ERROR_SUCCESS;
    if (!stillRunning(value, haveStart && started != 0 ? std::optional<ULONGLONG>(started) : std::nullopt)) {
        logOnce(Level::Warn, "process " + std::to_string(value) +
                " hid the wheel but is gone (crashed or killed?): not hidden, every program may open it");
        return 0;
    }
    return value;
}

// A process's program name ("steam.exe"), without opening it: this host runs as LocalService, which may not open
// the user's processes, but NtQuerySystemInformation(SystemProcessIdInformation) names any (as Task Manager does).
// Empty when Windows will not say. Only for the log: which programs open the wheel (2026-10-06: putting the filter on
// waited 8.5 s for a device something held open, and only process numbers were logged).
std::string processName(const ULONG pid) {
    using Query = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
    static const Query query = [] {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll == nullptr ? nullptr
                                : reinterpret_cast<Query>(
                                      reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQuerySystemInformation")));
    }();
    if (query == nullptr) return {};
    struct CountedString {
        USHORT length;
        USHORT maximumLength;
        PWSTR buffer;
    };
    struct ProcessIdInformation {
        HANDLE processId;
        CountedString imageName;
    };
    constexpr ULONG kSystemProcessIdInformation = 88;
    std::wstring path(1024, L'\0');
    ProcessIdInformation info{reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(pid)),
                              {0, static_cast<USHORT>(path.size() * sizeof(wchar_t)), path.data()}};
    if (query(kSystemProcessIdInformation, &info, sizeof(info), nullptr) < 0) return {};
    path.resize(info.imageName.length / sizeof(wchar_t));
    const std::size_t slash = path.find_last_of(L'\\');
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    if (name.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0, nullptr,
                                         nullptr);
    std::string text(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), text.data(), size, nullptr, nullptr);
    }
    return text;
}

// "process 8964 (steam.exe)", or the number alone.
std::string describeProcess(const ULONG pid) {
    const std::string name = processName(pid);
    return "process " + std::to_string(pid) + (name.empty() ? std::string() : " (" + name + ")");
}

// One log line per program that opens the wheel or is refused (a game retries often), until the hiding changes. At
// most kMaxSeen programs are remembered (a program that opens and closes it in a loop gets new process ids): past that
// the list starts again, which only means a line said again.
constexpr std::size_t kMaxSeen = 256;
std::mutex g_seenLock;
std::unordered_map<ULONG, bool> g_seen;
ULONG g_lastAllowed = ~0UL;

void noteDecision(const ULONG pid, const ULONG allowed, const bool refused) {
    std::lock_guard lock(g_seenLock);
    if (allowed != g_lastAllowed) {
        g_lastAllowed = allowed;
        g_seen.clear();
        logLine(Level::Info, allowed == 0 ? "not hidden: every program may open the wheel"
                             : "hidden: only " + describeProcess(allowed) + " may open the wheel");
    }
    if (g_seen.size() >= kMaxSeen) g_seen.clear();
    if (!g_seen.emplace(pid, true).second) return;
    if (refused) {
        logLine(Level::Info, "refused " + describeProcess(pid) + " (the wheel is hidden from it)");
    } else if (pid != allowed) {
        // Who has the wheel open while it is not hidden: such a program keeps Windows from restarting the device when
        // the filter is put on, and makes the first Start slow.
        logLine(Level::Info, "opened by " + describeProcess(pid));
    }
}

// Per device: never hide a virtual wheel, even if the filter ended up on it (Windows put it there by itself once):
// hiding it would hide the wheel games are meant to see. The hardware ids of the four (kVirtualWheels in
// core/src/HidWheelDevice.cpp): G25, G27, G920, G923.
constexpr const char* kVirtualWheelIds[] = {"VID_046D&PID_C299", "VID_046D&PID_C29B", "VID_046D&PID_C262",
                                            "VID_046D&PID_C26E"};

struct DeviceContext {
    bool neverHide;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, deviceContext)

// The first hardware id of the device being added ("HID\VID_346E&PID_0005&REV_..&MI_02"), for the log and the
// check above; empty if Windows does not say.
std::string hardwareId(PWDFDEVICE_INIT init) {
    wchar_t buffer[512] = {};
    ULONG length = 0;
    if (!NT_SUCCESS(WdfFdoInitQueryProperty(init, DevicePropertyHardwareID, sizeof(buffer) - sizeof(wchar_t), buffer,
                                            &length))) {
        return {};
    }
    std::string text;
    for (const wchar_t* c = buffer; *c != L'\0'; ++c) text += static_cast<char>(*c < 128 ? *c : '?');
    return text;
}

VOID WINAPI onCreateForwarded(WDFREQUEST request, WDFIOTARGET, PWDF_REQUEST_COMPLETION_PARAMS params, WDFCONTEXT) {
    WdfRequestComplete(request, params->IoStatus.Status);
}

VOID EvtFileCreate(WDFDEVICE device, WDFREQUEST request, WDFFILEOBJECT) {
    const ULONG allowed = deviceContext(device)->neverHide ? 0 : allowedPid();
    const ULONG pid = static_cast<ULONG>(WdfRequestGetRequestorProcessId(request));
    const bool refuse = allowed != 0 && pid != allowed && pid != kSystemPid;
    noteDecision(pid, allowed, refuse);
    if (refuse) {
        WdfRequestComplete(request, STATUS_ACCESS_DENIED);
        return;
    }
    WdfRequestFormatRequestUsingCurrentType(request);
    WdfRequestSetCompletionRoutine(request, onCreateForwarded, WDF_NO_CONTEXT);
    if (!WdfRequestSend(request, WdfDeviceGetIoTarget(device), WDF_NO_SEND_OPTIONS)) {
        WdfRequestComplete(request, WdfRequestGetStatus(request));
    }
}

NTSTATUS EvtDeviceAdd(WDFDRIVER, PWDFDEVICE_INIT init) {
    WdfFdoInitSetFilter(init);
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, EvtFileCreate, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK);
    fileConfig.AutoForwardCleanupClose = WdfTrue;
    WdfDeviceInitSetFileObjectConfig(init, &fileConfig, WDF_NO_OBJECT_ATTRIBUTES);

    const std::string id = hardwareId(init);
    std::string upper = id;
    for (char& c : upper) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    // One of the virtual wheels (VirtualWheel in core): ours, never hidden.
    const bool virtualWheel = std::any_of(std::begin(kVirtualWheelIds), std::end(kVirtualWheelIds),
                                          [&upper](const char* const wheelId) {
                                              return upper.find(wheelId) != std::string::npos;
                                          });

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContext);
    WDFDEVICE device;
    const NTSTATUS status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        logLine(Level::Error, "WdfDeviceCreate failed on " + id + ": " + std::to_string(status));
        return status;
    }
    deviceContext(device)->neverHide = virtualWheel;
    // No queues: everything but opening the device goes straight to the HID class driver below.
    if (virtualWheel) {
        logLine(Level::Warn, "attached to " + id +
                                 ", a virtual wheel: never hidden (this filter does not belong there; reinstall "
                                 "WheelBri to take it off)");
    } else {
        logLine(Level::Info,
                "attached to " + id + "; " + std::string(allowedPid() == 0 ? "not hidden now" : "hidden now"));
    }
    return STATUS_SUCCESS;
}

}  // namespace

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    const NTSTATUS status = WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    logLine(NT_SUCCESS(status) ? Level::Info : Level::Error,
            NT_SUCCESS(status) ? "wheelbri-hide driver loaded" : "WdfDriverCreate failed: " + std::to_string(status));
    return status;
}
