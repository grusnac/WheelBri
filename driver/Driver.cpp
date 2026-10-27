// wheelbri-wheel (wheelbri-g920 until 2026-10-19): a UMDF 2 HID minidriver presenting a Logitech G25, G27, G920 or
// G923 to Windows (Phase 5, plan B).
//
// It does what the former HIDMaestro bridge (step 0) did, inside Windows' driver host:
//   input  wheelbri-service --UDP 127.0.0.1:41329 (StatePacket)--> driver --> HID input report --> games
//   ffb    games --DirectInput--> pid.dll --HID PID reports--> driver --UDP 127.0.0.1:41330 (FfbPacket)--> service
// so the service does not change: it talks to this driver exactly as to the in-game shim.
// Only one of them may run at a time (they share the ports).
//
// The device itself is created by wheelbri-service while it runs (a software device, see
// service/src/VirtualWheelDevice.cpp); this INF-installed driver binds to it by hardware ID wheelbri\G920, \G25,
// \G27 or \G923, and that id decides which wheel it is (VirtualWheel, core/include/wheelbri/HidWheelDevice.hpp): its
// vendor and product ids and its product string. All have the same HID descriptor and input report (only the button
// count differs).
//
// Structure follows Microsoft's vhidmini2 sample (UMDF 2): HID class requests arrive as IOCTLs on a
// parallel queue; read requests wait on a manual queue until a state packet arrives.

#include <winsock2.h>   // before windows.h
#include <windows.h>
#include <wdf.h>
#include <hidport.h>        // $(DDK_INC_PATH)\wdm

#include "wheelbri/FfbPacket.hpp"
#include "wheelbri/HidWheelDevice.hpp"
#include "wheelbri/HidPid.hpp"
#include "wheelbri/HidPp.hpp"
#include "wheelbri/HidPpFfb.hpp"
#include "wheelbri/LogFileNames.hpp"
#include "wheelbri/LogLevel.hpp"
#include "wheelbri/StatePacket.hpp"
#include "wheelbri/TrueForce.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr USHORT kStatePort = 41329;
constexpr USHORT kFfbPort = 41330;
constexpr std::int64_t kStaleMs = 1500;              // same as the shim: then centred wheel, released pedals
constexpr wchar_t kManufacturer[] = L"Logitech";
constexpr wchar_t kSerial[] = L"WHEELBRI0001";

using Report = std::vector<std::uint8_t>;
using Level = wheelbri::LogLevel;

// The input report for `wheel` (its buttons, and its axes unsigned when they are: packInputFor).
Report wheelReport(const wheelbri::TargetReport& report, const wheelbri::VirtualWheel& wheel) {
    const wheelbri::InputReport packed = wheelbri::packInputFor(wheel, report);
    return Report(packed.bytes.begin(), packed.bytes.begin() + static_cast<std::ptrdiff_t>(packed.size));
}

// ---------------------------------------------------------------------------------------------------
// Log: <the logs folder>\wheelbri-driver-<date>-<NN>.log (see logFolder) and the debugger (DebugView).

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
// and a new one at midnight: <folder>\wheelbri-driver-<date>-<NN>.log with the next free number of that day (core
// LogFileNames.hpp, the same rules as every other WheelBri log). The first line of a run in a folder makes the file
// (CREATE_NEW: never an existing name, so never a link someone put there; a number another host took a moment ago
// gives the next); later lines append to it. Under g_logLock.
struct RunFile {
    std::string date;
    std::wstring path;
};
std::unordered_map<std::wstring, RunFile> g_runFiles;   // folder -> this run's file there

constexpr const char* kLogStem = "wheelbri-driver";

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

// Appends `line` to this run's <folder>\wheelbri-driver-<date>-<NN>.log (openRunFile); false if the folder is refused
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
// Level button, written at each Start and on a change), read at most once a second (a line may come from the game's
// force feedback path). Warn when it is missing: warnings and errors are always written.
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

void logLine(const wheelbri::LogLevel level, const std::string& text) {
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
    const std::wstring folder = logFolder();
    if (appendTo(folder, now, line) || appendTo(fallbackLogFolder(), now, line)) {
        reportedFailure = false;
    } else if (!reportedFailure) {
        reportedFailure = true;   // only DebugView can show this one
        char why[160];
        std::snprintf(why, sizeof(why), "wheelbri-wheel: cannot write its log (error %lu)\r\n", GetLastError());
        OutputDebugStringA(why);
    }
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::int64_t nowUs() {
    using std::chrono::microseconds;
    return std::chrono::duration_cast<microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// At most one line per `periodMs` for a repeating problem; `suppressed` counts the lines skipped in between.
class Throttle {
public:
    explicit Throttle(const std::int64_t periodMs) : periodMs_(periodMs) {}
    bool allow(const std::int64_t now) {
        if (now - last_ < periodMs_) {
            ++suppressed_;
            return false;
        }
        last_ = now;
        return true;
    }
    std::uint64_t takeSuppressed() { return std::exchange(suppressed_, 0); }

private:
    std::int64_t periodMs_;
    std::int64_t last_ = INT64_MIN / 2;
    std::uint64_t suppressed_ = 0;
};

// ", N more since the last line" for a Throttle's skipped lines, or nothing. Built with plain appends: the same text
// as one expression with a ?: of temporaries crashed the driver host (wheelbri-wheel.dll built 2026-10-19 with
// MSVC 14.51): the receive warning's string came out with its text over its own pointer (two dumps, 21:26 and 22:24).
std::string moreSince(const std::uint64_t skipped) {
    std::string out;
    if (skipped > 0) {
        out = ", ";
        out += std::to_string(skipped);
        out += " more since the last line";
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------

}  // namespace

struct DeviceContext;

namespace {

// The UDP side: state packets in, force feedback out, on its own thread.
class Link {
public:
    explicit Link(DeviceContext* device);
    ~Link() { stop(); }

    bool start();
    void stop();
    void sendFfb(const wheelbri::FfbPacket& packet);

    wheelbri::HidPid pid{[this](const wheelbri::FfbPacket& p) { sendFfb(p); },
                         [](const Level level, const std::string& s) { logLine(level, s); }};
    // Logitech's HID++ for G HUB, on a wheel that has it (VirtualWheel::hidpp); null otherwise.
    std::unique_ptr<wheelbri::HidPp> hidpp;
    // Its 0x8123 effects played (HidPpFfb.hpp): Logitech's force feedback driver sends a TrueForce game's force so.
    std::unique_ptr<wheelbri::HidPpFfb> hidppFfb;

    // A HID++ answer: an input report for the next read, before any state (and never dropped).
    void reply(Report report);

    // The latest input report (neutral until the service sends state).
    Report latestReport() {
        std::lock_guard lock(reportLock_);
        return report_;
    }

    // A read from Windows' HID class (IOCTL_HID_READ_REPORT): answered at once with the oldest state not handed out
    // yet, else it waits in the read queue until the next state comes. Each state goes out once: until 2026-10-07
    // every state answered every read that came while it was being handed out, and the HID class sends its next read
    // as soon as one is answered, so one state could go round many times (half a core whatever the state rate,
    // measured with scripts/make/measure.sh).
    void answerRead(WDFREQUEST request);

private:
    // States not handed out yet. One is normal (the HID class always has a read waiting); more only while no read
    // waits. Kept in order, so a button pressed and released between two reads still reaches the game; when full,
    // the oldest goes (counted in the minute's line).
    static constexpr std::size_t kMaxPending = 8;
    static constexpr std::size_t kMaxReplies = 64;   // HID++ answers waiting for a read
    static constexpr std::int64_t kCountsEveryMs = 60'000;

    struct Counts {
        std::uint64_t states = 0;         // states from the service (and neutral ones when it went quiet)
        std::uint64_t answeredAtOnce = 0; // reads answered on arrival
        std::uint64_t answeredLater = 0;  // reads that waited for a state
        std::uint64_t dropped = 0;        // states lost because kMaxPending were already waiting
    };

    void run(SOCKET sock);
    void publish(Report report);
    void logCounts(std::int64_t now);
    // false = stop the thread; a socket error other than the receive timeout is logged and slowed down.
    bool receiveFailed(int received);
    void closeSockets();

    DeviceContext* device_;
    SOCKET stateSocket_ = INVALID_SOCKET;
    SOCKET ffbSocket_ = INVALID_SOCKET;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex reportLock_;     // report_, pending_, counts_; never held while a request is completed
    Report report_;
    std::deque<Report> pending_;
    std::deque<Report> replies_;   // HID++ answers, handed out before states
    Counts counts_;
    std::int64_t countsSinceMs_ = 0;
    Throttle recvErrors_{60'000};
    Throttle sendErrors_{60'000};
};

// The wheel's TrueForce interface (TrueForce.hpp, wheelbri\G923_TF): its answers to the host, and the steering from
// the service's state (127.0.0.1:41331) as status reports while a session runs. No force feedback yet.
class TrueForceLink {
public:
    explicit TrueForceLink(DeviceContext* device);
    ~TrueForceLink() { stop(); }

    // Takes the host's writes one a millisecond (pace) and listens for the service's state. Never fails the device:
    // without the state the status reports keep the wheel centred.
    void start();
    void stop();
    // What the host wrote, report ID first.
    void output(std::span<const std::uint8_t> report);
    // A read: answered at once with the oldest waiting answer, else it waits in the read queue for the next one.
    void answerRead(WDFREQUEST request);
    void finish() { tf_.finish(nowUs()); }

private:
    static constexpr std::size_t kMaxReplies = 64;

    void reply(Report report);
    void run(SOCKET sock);
    void pace();
    void closeSocket();

    DeviceContext* device_;
    wheelbri::TrueForceDevice tf_;
    SOCKET socket_ = INVALID_SOCKET;
    std::thread thread_;
    std::thread pacer_;
    std::atomic<bool> stop_{false};
    std::mutex lock_;   // replies_, and the read queue with it
    std::deque<Report> replies_;
    static constexpr int kAnswersShown = 12;
    int shownAnswers_ = 0;
};

}  // namespace

struct DeviceContext {
    WDFDEVICE device;
    const wheelbri::VirtualWheel* wheel;   // which wheel this device is (by its hardware id)
    WCHAR product[64];               // the wheel's HID product string, 0-terminated
    WDFQUEUE readQueue;              // manual: IOCTL_HID_READ_REPORT waits here for the next state
    WDFQUEUE writeQueue;             // manual, TrueForce interface only: writes wait for their millisecond
    HID_DEVICE_ATTRIBUTES attributes;
    HID_DESCRIPTOR hidDescriptor;
    Link* link;                      // owned; deleted in the device's cleanup callback
    TrueForceLink* trueForce;        // owned; only on the wheel's TrueForce interface, which has no link
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, GetDeviceContext)

namespace {

// Answers `request` with `report` (HID input report).
void completeRead(WDFREQUEST request, const Report& report);

Link::Link(DeviceContext* device)
    : device_(device), report_(wheelReport(wheelbri::neutralReport(), *device->wheel)) {
    const wheelbri::VirtualWheel* wheel = device->wheel;
    if (wheel != nullptr && wheel->hidpp) {
        wheelbri::HidPp::Identity identity;
        identity.name = std::string(wheel->product);
        identity.productId = wheel->productId;
        identity.unitId = 0x57420001;   // "WB" 0001, like the HID serial number WHEELBRI0001
        identity.deviceType = wheel->deviceType;
        const auto toLog = [](const Level level, const std::string& line) { logLine(level, line); };
        hidpp = std::make_unique<wheelbri::HidPp>(identity, toLog);
        const auto toService = [this](const wheelbri::FfbPacket& p) { sendFfb(p); };
        hidppFfb = std::make_unique<wheelbri::HidPpFfb>(toService, toLog);
        hidpp->setForceFeedback(hidppFfb.get());
    }
}

void Link::reply(Report report) {
    WDFREQUEST request = nullptr;
    {
        std::lock_guard lock(reportLock_);
        if (!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(device_->readQueue, &request))) {
            // The next read takes it. Bounded: a program that asks and never reads must not grow it without end.
            if (replies_.size() == kMaxReplies) replies_.pop_front();
            replies_.push_back(std::move(report));
            return;
        }
    }
    completeRead(request, report);
}

void completeRead(WDFREQUEST request, const Report& report) {
    WDFMEMORY memory;
    NTSTATUS status = WdfRequestRetrieveOutputMemory(request, &memory);
    if (NT_SUCCESS(status)) {
        size_t length = 0;
        WdfMemoryGetBuffer(memory, &length);
        if (length < report.size()) {
            status = STATUS_INVALID_BUFFER_SIZE;
        } else {
            status = WdfMemoryCopyFromBuffer(memory, 0, const_cast<std::uint8_t*>(report.data()), report.size());
            if (NT_SUCCESS(status)) WdfRequestSetInformation(request, report.size());
        }
    }
    WdfRequestComplete(request, status);
}

bool Link::start() {
    stop();
    stop_ = false;
    // The force feedback trace (HidPid::enableTrace) is off unless an Administrator asks for it, for each start of
    // the device: reg add HKLM\SOFTWARE\wheelbri /v PidTraceLines /t REG_DWORD /d 1000 /f
    DWORD traceLines = 0;
    DWORD traceSize = sizeof(traceLines);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\wheelbri", L"PidTraceLines", RRF_RT_REG_DWORD, nullptr, &traceLines,
                     &traceSize) == ERROR_SUCCESS &&
        traceLines > 0) {
        const int lines = static_cast<int>((std::min)(traceLines, static_cast<DWORD>(wheelbri::HidPid::kMaxTraceLines)));
        pid.enableTrace(lines);
        logLine(Level::Info, "force feedback trace on (PidTraceLines = " + std::to_string(lines) +
                                 "; its lines are written with the app's Level at trace)");
    } else {
        pid.enableTrace(0);
    }
    stateSocket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ffbSocket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (stateSocket_ == INVALID_SOCKET || ffbSocket_ == INVALID_SOCKET) {
        logLine(Level::Error,
                "cannot create UDP sockets (" + std::to_string(WSAGetLastError()) + "): the device will not start");
        closeSockets();
        return false;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(kStatePort);
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(stateSocket_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        logLine(Level::Error, "cannot listen on 127.0.0.1:41329 (" + std::to_string(WSAGetLastError()) +
                "): is the shim's game still running? Only one of them may run.");
        closeSockets();
        return false;
    }
    // The thread must wake regularly (stop requests, the stale-state check, force feedback timing).
    DWORD timeoutMs = 5;
    if (setsockopt(stateSocket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs)) != 0) {
        logLine(Level::Error, "cannot set the receive timeout (" + std::to_string(WSAGetLastError()) +
                                  "): the device will not start");
        closeSockets();
        return false;
    }
    sockaddr_in service{};
    service.sin_family = AF_INET;
    service.sin_port = htons(kFfbPort);
    service.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(ffbSocket_, reinterpret_cast<sockaddr*>(&service), sizeof(service)) != 0) {
        logLine(Level::Warn, "cannot address the service's force feedback port 127.0.0.1:41330 (" +
                std::to_string(WSAGetLastError()) + "): the game's force feedback will not reach the wheel");
    }
    // The thread gets its own copy of the socket: stop() closes it only after the thread has ended.
    try {
        thread_ = std::thread([this, sock = stateSocket_] { run(sock); });
    } catch (const std::exception& e) {   // out of resources: never let it out of a WDF callback (WUDFHost would end)
        logLine(Level::Error,
                std::string("cannot start the device's thread (") + e.what() + "): the device will not start");
        closeSockets();
        return false;
    }
    return true;
}

void Link::publish(Report report) {
    // Pairs of (waiting read, state) taken under the lock, answered after it: completing a read may bring the HID
    // class's next one at once, and answerRead must not wait for this thread.
    std::array<std::pair<WDFREQUEST, Report>, kMaxPending> answers;
    std::size_t count = 0;
    {
        std::lock_guard lock(reportLock_);
        report_ = report;
        ++counts_.states;
        if (pending_.size() == kMaxPending) {
            pending_.pop_front();
            ++counts_.dropped;
        }
        pending_.push_back(std::move(report));
        WDFREQUEST request;
        while (count < answers.size() && !pending_.empty() &&
               NT_SUCCESS(WdfIoQueueRetrieveNextRequest(device_->readQueue, &request))) {
            answers[count++] = {request, std::move(pending_.front())};
            pending_.pop_front();
            ++counts_.answeredLater;
        }
    }
    for (std::size_t i = 0; i < count; ++i) completeRead(answers[i].first, answers[i].second);
}

void Link::answerRead(const WDFREQUEST request) {
    Report report;
    {
        std::lock_guard lock(reportLock_);
        if (!replies_.empty()) {
            report = std::move(replies_.front());
            replies_.pop_front();
        } else if (pending_.empty()) {
            // Waits for the next state. Forwarded under the lock, so publish cannot miss it.
            const NTSTATUS status = WdfRequestForwardToIoQueue(request, device_->readQueue);
            if (NT_SUCCESS(status)) return;
            report = report_;   // the queue refused it: answer with the latest state rather than fail the read
        } else {
            report = std::move(pending_.front());
            pending_.pop_front();
            ++counts_.answeredAtOnce;
        }
    }
    completeRead(request, report);
}

// Once a minute: how many states came and how the reads were answered. answered at once + later = states - dropped
// (minus those still waiting) when each state goes out once.
void Link::logCounts(const std::int64_t now) {
    if (countsSinceMs_ == 0) {
        countsSinceMs_ = now;
        return;
    }
    if (now - countsSinceMs_ < kCountsEveryMs) return;
    Counts counts;
    std::size_t waiting = 0;
    {
        std::lock_guard lock(reportLock_);
        counts = counts_;
        counts_ = Counts{};
        waiting = pending_.size();
    }
    const std::int64_t seconds = (now - countsSinceMs_ + 500) / 1000;
    countsSinceMs_ = now;
    logLine(Level::Debug,
            "input, last " + std::to_string(seconds) + " s: " + std::to_string(counts.states) + " states, " +
            std::to_string(counts.answeredAtOnce + counts.answeredLater) + " reads answered (" +
            std::to_string(counts.answeredAtOnce) + " at once, " + std::to_string(counts.answeredLater) +
            " after waiting), " + std::to_string(counts.dropped) + " states dropped, " + std::to_string(waiting) +
            " waiting");
}

bool Link::receiveFailed(const int received) {
    (void)received;
    const int error = WSAGetLastError();
    // Nothing this time: the normal case. A receive timeout can also end as WSA_IO_PENDING (997), seen twice on
    // 2026-10-19 in the middle of a game; it is a timeout too, not a broken socket.
    if (error == WSAETIMEDOUT || error == WSA_IO_PENDING) return true;
    if (stop_) return false;
    const std::int64_t now = nowMs();
    if (recvErrors_.allow(now)) {
        std::string line = "receiving from wheelbri-service failed (";
        line += std::to_string(error);
        line += ")";
        line += moreSince(recvErrors_.takeSuppressed());
        logLine(Level::Warn, line);
    }
    if (error == WSAENOTSOCK || error == WSAEINVAL) return false;   // the socket is gone: nothing to wait for
    Sleep(5);   // not a busy loop on a persistent error
    return true;
}

void Link::closeSockets() {
    if (stateSocket_ != INVALID_SOCKET) closesocket(stateSocket_);
    if (ffbSocket_ != INVALID_SOCKET) closesocket(ffbSocket_);
    stateSocket_ = ffbSocket_ = INVALID_SOCKET;
}

void Link::stop() {
    stop_ = true;
    // The receive timeout (5 ms, checked in start()) wakes the thread, so this join is short. The sockets close only
    // after it: the force feedback one sends the final "stop all" on the thread's way out, and a socket closed under
    // the thread could be handed to another device's new socket in this same driver host.
    if (thread_.joinable()) thread_.join();
    closeSockets();
}

void Link::sendFfb(const wheelbri::FfbPacket& packet) {
    const auto bytes = wheelbri::encode(packet);
    if (send(ffbSocket_, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0) < 0) {
        // Normal while the service is not running (nothing listens on 41330): say so once a minute.
        const int error = WSAGetLastError();
        if (sendErrors_.allow(nowMs())) {
            std::string line = "force feedback to wheelbri-service not sent (";
            line += std::to_string(error);
            line += "; is the service running?)";
            line += moreSince(sendErrors_.takeSuppressed());
            logLine(Level::Warn, line);
        }
    }
}

void Link::run(const SOCKET sock) {
    // A name for measure.ps1 and debuggers (Windows 10 1607 and later; looked up so an older host still loads us).
    using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    if (const HMODULE kernel = GetModuleHandleW(L"kernel32.dll"); kernel != nullptr) {
        if (const auto setDescription =
                reinterpret_cast<SetDescription>(reinterpret_cast<void*>(GetProcAddress(kernel, "SetThreadDescription")))) {
            setDescription(GetCurrentThread(), L"wheelbri-wheel state receiver");
        }
    }
    logLine(Level::Info, "listening for wheelbri-service on 127.0.0.1:41329, force feedback to 127.0.0.1:41330");
    std::array<std::uint8_t, 256> buffer{};
    std::int64_t lastState = nowMs() - kStaleMs - 1;
    std::int64_t lastTick = 0;
    bool receiving = false;
    bool neutralSent = false;
    while (!stop_) {
        const int received = recv(sock, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
        if (received < 0 && !receiveFailed(received)) break;
        const std::int64_t now = nowMs();
        if (received > 0) {
            if (const auto packet = wheelbri::decodeStatePacket(std::span(buffer.data(), static_cast<std::size_t>(received)))) {
                if (!receiving) {
                    receiving = true;
                    logLine(Level::Info, "receiving wheel state from wheelbri-service (packet #" +
                                             std::to_string(packet->sequence) + ")");
                }
                lastState = now;
                neutralSent = false;
                publish(wheelReport(packet->report, *device_->wheel));
            }
        } else if (now - lastState > kStaleMs && !neutralSent) {
            if (receiving) {
                logLine(Level::Info,
                        "no wheel state for 1500 ms: centred wheel and released pedals until it comes back");
            }
            receiving = false;
            neutralSent = true;
            publish(wheelReport(wheelbri::neutralReport(), *device_->wheel));
        }
        if (now - lastTick >= 10) {
            lastTick = now;
            pid.tick(now);
            if (hidppFfb) hidppFfb->tick(now);
        }
        logCounts(now);
    }
    pid.stopAll(nowMs());
    if (hidppFfb) hidppFfb->stopAll(nowMs());
    logLine(Level::Info, "stopped: " + std::to_string(pid.reportsHandled()) +
                             " force feedback reports from the game, " +
            std::to_string(pid.packetsSent()) + " packets to the service");
}

// ---------------------------------------------------------------------------------------------------
// Request helpers (UMDF 2 HID conventions, as in vhidmini2 and HIDMaestro).

NTSTATUS copyToRequest(WDFREQUEST request, const void* source, size_t size) {
    WDFMEMORY memory;
    NTSTATUS status = WdfRequestRetrieveOutputMemory(request, &memory);
    if (!NT_SUCCESS(status)) return status;
    size_t length = 0;
    WdfMemoryGetBuffer(memory, &length);
    if (length < size) return STATUS_INVALID_BUFFER_SIZE;
    status = WdfMemoryCopyFromBuffer(memory, 0, const_cast<void*>(source), size);
    if (NT_SUCCESS(status)) WdfRequestSetInformation(request, size);
    return status;
}

// Output / feature reports written by the host: the input buffer holds [report ID, data...].
NTSTATUS readWrittenReport(WDFREQUEST request, std::uint8_t& reportId, std::span<const std::uint8_t>& data) {
    PVOID buffer = nullptr;
    size_t size = 0;
    const NTSTATUS status = WdfRequestRetrieveInputBuffer(request, 1, &buffer, &size);
    if (!NT_SUCCESS(status)) return status;
    const auto* bytes = static_cast<const std::uint8_t*>(buffer);
    reportId = bytes[0];
    data = std::span(bytes + 1, size - 1);
    return STATUS_SUCCESS;
}

NTSTATUS getString(WDFREQUEST request, const DeviceContext* ctx) {
    PVOID buffer = nullptr;
    size_t size = 0;
    NTSTATUS status = WdfRequestRetrieveInputBuffer(request, sizeof(ULONG), &buffer, &size);
    if (!NT_SUCCESS(status)) return status;
    const ULONG id = *static_cast<const ULONG*>(buffer) & 0xFFFFU;
    switch (id) {
        case HID_STRING_ID_IMANUFACTURER: return copyToRequest(request, kManufacturer, sizeof(kManufacturer));
        case HID_STRING_ID_IPRODUCT:
            return copyToRequest(request, ctx->product, (wcslen(ctx->product) + 1) * sizeof(WCHAR));
        case HID_STRING_ID_ISERIALNUMBER: return copyToRequest(request, kSerial, sizeof(kSerial));
        default: return STATUS_INVALID_PARAMETER;
    }
}

TrueForceLink::TrueForceLink(DeviceContext* device)
    : device_(device), tf_([](const Level level, const std::string& line) { logLine(level, line); }) {}

void TrueForceLink::start() {
    stop();
    stop_ = false;
    try {
        pacer_ = std::thread([this] { pace(); });
    } catch (const std::exception& e) {
        logLine(Level::Error, std::string("trueforce: no pacing thread (") + e.what() + "): writes wait forever");
    }
    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(wheelbri::kTrueForceStatePort);
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const DWORD timeoutMs = 5;   // wakes for stop requests
    const auto* timeout = reinterpret_cast<const char*>(&timeoutMs);
    if (socket_ == INVALID_SOCKET || bind(socket_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0 ||
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, timeout, sizeof(timeoutMs)) != 0) {
        logLine(Level::Warn, "trueforce: cannot listen on 127.0.0.1:41331 (" + std::to_string(WSAGetLastError()) +
                                 "): its status reports keep the wheel centred");
        closeSocket();
        return;
    }
    try {
        thread_ = std::thread([this, sock = socket_] { run(sock); });
    } catch (const std::exception& e) {
        logLine(Level::Warn, std::string("trueforce: no state thread (") + e.what() + ")");
        closeSocket();
    }
}

void TrueForceLink::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (pacer_.joinable()) pacer_.join();
    closeSocket();
}

void TrueForceLink::closeSocket() {
    if (socket_ != INVALID_SOCKET) closesocket(socket_);
    socket_ = INVALID_SOCKET;
}

void TrueForceLink::output(const std::span<const std::uint8_t> report) {
    for (Report& answer : tf_.output(report, nowUs())) reply(std::move(answer));
}

void TrueForceLink::reply(Report report) {
    WDFREQUEST request = nullptr;
    {
        std::lock_guard lock(lock_);
        const bool waiting = NT_SUCCESS(WdfIoQueueRetrieveNextRequest(device_->readQueue, &request));
        if (shownAnswers_ < kAnswersShown) {
            ++shownAnswers_;
            char text[96];
            std::snprintf(text, sizeof(text), "trueforce: answer %02x seq %02x, checksum %02x: %s", report[4], report[5],
                          report[4] == 0x05 ? report[13] : report[4] == 0x07 ? report[25] : report[33],
                          waiting ? "to a waiting read" : "queued (no read waiting)");
            logLine(Level::Info, text);
        }
        if (!waiting) {
            if (replies_.size() == kMaxReplies) replies_.pop_front();
            replies_.push_back(std::move(report));
            return;
        }
    }
    completeRead(request, report);
}

void TrueForceLink::answerRead(const WDFREQUEST request) {
    Report answer;
    {
        std::lock_guard lock(lock_);
        if (replies_.empty()) {
            const NTSTATUS status = WdfRequestForwardToIoQueue(request, device_->readQueue);
            if (!NT_SUCCESS(status)) WdfRequestComplete(request, status);
            return;
        }
        answer = std::move(replies_.front());
        replies_.pop_front();
    }
    completeRead(request, answer);
}

// One write a millisecond, as the wheel's interrupt endpoint takes them: writes completed at once let the host stream
// 14000 packets a second (2026-10-27), each answered with a status. A high-resolution timer (Windows 10 1803+; the
// driver needs 11) wakes this thread every millisecond to take the next write from the write queue.
void TrueForceLink::pace() {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == nullptr) {
        logLine(Level::Warn, "trueforce: no high-resolution timer (" + std::to_string(GetLastError()) +
                                 "): writes paced by Sleep(1)");
    }
    while (!stop_) {
        if (timer != nullptr) {
            LARGE_INTEGER due;
            due.QuadPart = -10'000;   // 1 ms, relative, in 100 ns units
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, 50);
            } else {
                Sleep(1);
            }
        } else {
            Sleep(1);
        }
        WDFREQUEST request = nullptr;
        if (!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(device_->writeQueue, &request))) continue;
        std::uint8_t reportId = 0;
        std::span<const std::uint8_t> data;
        NTSTATUS status = readWrittenReport(request, reportId, data);
        if (NT_SUCCESS(status)) {
            Report sent(data.size() + 1);
            sent[0] = reportId;
            std::copy(data.begin(), data.end(), sent.begin() + 1);
            output(sent);
            WdfRequestSetInformation(request, data.size() + 1);
        }
        WdfRequestComplete(request, status);
    }
    if (timer != nullptr) CloseHandle(timer);
}

void TrueForceLink::run(const SOCKET sock) {
    logLine(Level::Info, "trueforce: listening for wheelbri-service on 127.0.0.1:41331");
    std::array<std::uint8_t, 256> buffer{};
    while (!stop_) {
        const int received = recv(sock, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
        if (received <= 0) {
            const int error = WSAGetLastError();
            if (error == WSAETIMEDOUT || error == WSA_IO_PENDING) continue;
            if (stop_ || error == WSAENOTSOCK || error == WSAEINVAL) break;
            Sleep(5);
            continue;
        }
        const auto packet = wheelbri::decodeStatePacket(std::span(buffer.data(), static_cast<std::size_t>(received)));
        if (!packet) continue;
        if (auto status = tf_.steering(packet->report.steering, nowUs())) reply(std::move(*status));
    }
}

// The wheel's TrueForce interface (TrueForce.hpp): its descriptor and strings; what the host writes and reads goes
// through its TrueForceLink.
void trueForceControl(DeviceContext* ctx, WDFREQUEST request, const ULONG code) {
    NTSTATUS status = STATUS_NOT_SUPPORTED;
    switch (code) {
        case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
            status = copyToRequest(request, &ctx->hidDescriptor, ctx->hidDescriptor.bLength);
            break;
        case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
            status = copyToRequest(request, &ctx->attributes, sizeof(HID_DEVICE_ATTRIBUTES));
            break;
        case IOCTL_HID_GET_REPORT_DESCRIPTOR: {
            const auto descriptor = wheelbri::trueForceReportDescriptor();
            status = copyToRequest(request, descriptor.data(), descriptor.size());
            break;
        }
        case IOCTL_HID_GET_STRING:
            status = getString(request, ctx);
            break;
        case IOCTL_HID_READ_REPORT:
            ctx->trueForce->answerRead(request);   // completes it now or with the next answer
            return;
        case IOCTL_HID_WRITE_REPORT:
        case IOCTL_UMDF_HID_SET_OUTPUT_REPORT:
            // Taken one a millisecond by TrueForceLink::pace, which completes it.
            status = WdfRequestForwardToIoQueue(request, ctx->writeQueue);
            if (NT_SUCCESS(status)) return;
            break;
        default:
            break;
    }
    WdfRequestComplete(request, status);
}

VOID EvtIoDeviceControl(WDFQUEUE queue, WDFREQUEST request, size_t, size_t, ULONG code) {
    DeviceContext* ctx = GetDeviceContext(WdfIoQueueGetDevice(queue));
    if (ctx->trueForce != nullptr) {
        trueForceControl(ctx, request, code);
        return;
    }
    NTSTATUS status = STATUS_NOT_IMPLEMENTED;
    const bool needsLink = code == IOCTL_HID_WRITE_REPORT || code == IOCTL_UMDF_HID_SET_OUTPUT_REPORT ||
                           code == IOCTL_UMDF_HID_SET_FEATURE || code == IOCTL_UMDF_HID_GET_FEATURE ||
                           code == IOCTL_UMDF_HID_GET_INPUT_REPORT;
    if (needsLink && ctx->link == nullptr) {   // being removed
        WdfRequestComplete(request, STATUS_DEVICE_NOT_READY);
        return;
    }
    std::uint8_t reportId = 0;
    std::span<const std::uint8_t> data;
    switch (code) {
        case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
            status = copyToRequest(request, &ctx->hidDescriptor, ctx->hidDescriptor.bLength);
            break;
        case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
            status = copyToRequest(request, &ctx->attributes, sizeof(HID_DEVICE_ATTRIBUTES));
            break;
        case IOCTL_HID_GET_REPORT_DESCRIPTOR: {
            const auto descriptor = wheelbri::reportDescriptorFor(*ctx->wheel);
            status = copyToRequest(request, descriptor.data(), descriptor.size());
            break;
        }
        case IOCTL_HID_READ_REPORT:
            if (ctx->link == nullptr) {   // being removed
                status = STATUS_DEVICE_NOT_READY;
                break;
            }
            ctx->link->answerRead(request);   // completes it now or when the next state arrives
            return;
        case IOCTL_HID_WRITE_REPORT:
        case IOCTL_UMDF_HID_SET_OUTPUT_REPORT:
            status = readWrittenReport(request, reportId, data);
            if (NT_SUCCESS(status)) {
                const wheelbri::ReportRoute route = wheelbri::routeReport(*ctx->wheel, reportId);
                if (route.to == wheelbri::ReportRoute::To::HidPp && ctx->link->hidpp) {
                    Report asked(data.size() + 1);
                    asked[0] = reportId;
                    std::copy(data.begin(), data.end(), asked.begin() + 1);
                    if (auto answer = ctx->link->hidpp->handle(asked, nowMs())) ctx->link->reply(std::move(*answer));
                } else if (route.to == wheelbri::ReportRoute::To::Pid) {
                    ctx->link->pid.output(route.id, data, nowMs());
                }
                WdfRequestSetInformation(request, data.size() + 1);
            }
            break;
        case IOCTL_UMDF_HID_SET_FEATURE:
            status = readWrittenReport(request, reportId, data);
            if (NT_SUCCESS(status)) {
                const wheelbri::ReportRoute route = wheelbri::routeReport(*ctx->wheel, reportId);
                if (route.to == wheelbri::ReportRoute::To::Pid) ctx->link->pid.setFeature(route.id, data, nowMs());
                WdfRequestSetInformation(request, data.size() + 1);
            }
            break;
        case IOCTL_UMDF_HID_GET_FEATURE: {
            // The requested report ID is byte 0 of the input buffer; the response goes to the output buffer.
            PVOID in = nullptr;
            size_t inSize = 0;
            PVOID out = nullptr;
            size_t outSize = 0;
            status = WdfRequestRetrieveOutputBuffer(request, 1, &out, &outSize);
            if (!NT_SUCCESS(status)) break;
            if (NT_SUCCESS(WdfRequestRetrieveInputBuffer(request, 1, &in, &inSize))) {
                reportId = static_cast<const std::uint8_t*>(in)[0];
            }
            if (reportId == 0) reportId = static_cast<const std::uint8_t*>(out)[0];
            const wheelbri::ReportRoute route = wheelbri::routeReport(*ctx->wheel, reportId);
            const std::size_t written =
                route.to == wheelbri::ReportRoute::To::Pid
                    ? ctx->link->pid.getFeature(route.id, std::span(static_cast<std::uint8_t*>(out), outSize))
                    : 0;
            if (written > 0) static_cast<std::uint8_t*>(out)[0] = wheelbri::wirePidId(*ctx->wheel, route.id);
            if (written == 0) {
                status = STATUS_NOT_SUPPORTED;
            } else {
                WdfRequestSetInformation(request, written);
                status = STATUS_SUCCESS;
            }
            break;
        }
        case IOCTL_UMDF_HID_GET_INPUT_REPORT: {
            const auto report = ctx->link->latestReport();
            status = copyToRequest(request, report.data(), report.size());
            break;
        }
        case IOCTL_HID_GET_STRING:
            status = getString(request, ctx);
            break;
        default:
            status = STATUS_NOT_IMPLEMENTED;
            break;
    }
    WdfRequestComplete(request, status);
}

// Powered up: start listening. Powered down (device being removed): stop before the queues go away.
NTSTATUS EvtDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE) {
    DeviceContext* ctx = GetDeviceContext(device);
    if (ctx->trueForce != nullptr) {
        ctx->trueForce->start();
        return STATUS_SUCCESS;
    }
    return ctx->link != nullptr && ctx->link->start() ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

NTSTATUS EvtDeviceD0Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE) {
    DeviceContext* ctx = GetDeviceContext(device);
    if (ctx->trueForce != nullptr) ctx->trueForce->stop();
    if (ctx->link != nullptr) ctx->link->stop();
    return STATUS_SUCCESS;
}

VOID EvtDeviceCleanup(WDFOBJECT object) {
    DeviceContext* ctx = GetDeviceContext(static_cast<WDFDEVICE>(object));
    delete ctx->link;
    ctx->link = nullptr;
    if (ctx->trueForce != nullptr) {
        ctx->trueForce->finish();
        delete ctx->trueForce;
        ctx->trueForce = nullptr;
    ctx->writeQueue = nullptr;
        logLine(Level::Info, "virtual TrueForce interface removed");
        return;
    }
    logLine(Level::Info, "virtual " + (ctx->wheel != nullptr ? std::string(ctx->wheel->instancePrefix) : "wheel") +
                             " removed");
}

// The first hardware id of the device being added ("wheelbri\G25"); empty if Windows does not say.
std::string hardwareId(PWDFDEVICE_INIT init) {
    wchar_t buffer[256] = {};
    ULONG length = 0;
    if (!NT_SUCCESS(WdfFdoInitQueryProperty(init, DevicePropertyHardwareID, sizeof(buffer) - sizeof(wchar_t), buffer,
                                            &length))) {
        return {};
    }
    std::string text;
    for (const wchar_t* c = buffer; *c != L'\0'; ++c) text += static_cast<char>(*c < 128 ? *c : '?');
    return text;
}

NTSTATUS EvtDeviceAdd(WDFDRIVER, PWDFDEVICE_INIT init) {
    // Which wheel: by the hardware id the service gave the software device. An unknown one (an older service, or a
    // hand-made device) is the G920, as before there were two.
    const std::string id = hardwareId(init);
    // wheelbri\G923_TF: that wheel's TrueForce interface (TrueForce.hpp), a device of its own.
    const wheelbri::VirtualWheel* trueForceOf = wheelbri::trueForceWheelForHardwareId(id);
    const wheelbri::VirtualWheel* wheel =
        trueForceOf != nullptr ? trueForceOf : wheelbri::virtualWheelForHardwareId(id);
    if (wheel == nullptr) {
        logLine(Level::Warn, "hardware id '" + id + "' is not a known wheel: presenting the G920");
        wheel = wheelbri::virtualWheel("g920");
    }
    WdfFdoInitSetFilter(init);
    WDF_PNPPOWER_EVENT_CALLBACKS power;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&power);
    power.EvtDeviceD0Entry = EvtDeviceD0Entry;
    power.EvtDeviceD0Exit = EvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &power);
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContext);
    attributes.EvtCleanupCallback = EvtDeviceCleanup;
    WDFDEVICE device;
    NTSTATUS status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        logLine(Level::Error, "WdfDeviceCreate failed: " + std::to_string(status));
        return status;
    }
    DeviceContext* ctx = GetDeviceContext(device);
    ctx->device = device;
    ctx->wheel = wheel;
    ctx->link = nullptr;
    ctx->trueForce = nullptr;
    RtlZeroMemory(ctx->product, sizeof(ctx->product));
    for (std::size_t i = 0; i < wheel->product.size() && i + 1 < std::size(ctx->product); ++i) {
        ctx->product[i] = static_cast<WCHAR>(wheel->product[i]);
    }

    RtlZeroMemory(&ctx->attributes, sizeof(ctx->attributes));
    ctx->attributes.Size = sizeof(HID_DEVICE_ATTRIBUTES);
    ctx->attributes.VendorID = wheel->vendorId;
    ctx->attributes.ProductID = wheel->productId;
    ctx->attributes.VersionNumber = 0x0100;

    RtlZeroMemory(&ctx->hidDescriptor, sizeof(ctx->hidDescriptor));
    ctx->hidDescriptor.bLength = sizeof(HID_DESCRIPTOR);
    ctx->hidDescriptor.bDescriptorType = 0x21;
    ctx->hidDescriptor.bcdHID = 0x0100;
    ctx->hidDescriptor.bNumDescriptors = 1;
    ctx->hidDescriptor.DescriptorList[0].bReportType = 0x22;
    ctx->hidDescriptor.DescriptorList[0].wReportLength = static_cast<USHORT>(
        trueForceOf != nullptr ? wheelbri::trueForceReportDescriptor().size()
                               : wheelbri::reportDescriptorFor(*wheel).size());

    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = EvtIoDeviceControl;
    WDFQUEUE queue;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) {
        logLine(Level::Error, "default queue: " + std::to_string(status));
        return status;
    }
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &ctx->readQueue);
    if (!NT_SUCCESS(status)) {
        logLine(Level::Error, "read queue: " + std::to_string(status));
        return status;
    }

    if (trueForceOf != nullptr) {
        WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
        status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &ctx->writeQueue);
        if (!NT_SUCCESS(status)) {
            logLine(Level::Error, "trueforce write queue: " + std::to_string(status));
            return status;
        }
        ctx->trueForce = new TrueForceLink(ctx);
        logLine(Level::Info, "virtual " + std::string(wheel->instancePrefix) + " TrueForce interface added (" + id + ")");
        return STATUS_SUCCESS;
    }
    ctx->link = new Link(ctx);
    char ids[16];
    std::snprintf(ids, sizeof(ids), "%04X PID %04X", wheel->vendorId, wheel->productId);
    logLine(Level::Info, "virtual " + std::string(wheel->instancePrefix) + " added (" + id + ": VID " + ids + ")");
    return STATUS_SUCCESS;
}

// The driver goes away: Winsock is released with it (WSAStartup in DriverEntry).
VOID EvtDriverCleanup(WDFOBJECT) { WSACleanup(); }

}  // namespace

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
    WSADATA wsa;
    if (const int error = WSAStartup(MAKEWORD(2, 2), &wsa); error != 0) {
        logLine(Level::Error,
                "WSAStartup failed (" + std::to_string(error) + "): the driver cannot talk to wheelbri-service");
        return STATUS_UNSUCCESSFUL;
    }
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = EvtDriverCleanup;
    const NTSTATUS status = WdfDriverCreate(driverObject, registryPath, &attributes, &config, WDF_NO_HANDLE);
    // The build in the log: which driver Windows really loaded (an older installed copy can win). WHEELBRI_BUILD comes
    // from the project (yyyyMMdd_HHmmss, UTC): the WDK builds reproducibly, without __DATE__ / __TIME__.
#define WHEELBRI_TEXT2(x) #x
#define WHEELBRI_TEXT(x) WHEELBRI_TEXT2(x)
#ifdef WHEELBRI_BUILD
    const std::string built = " (built " WHEELBRI_TEXT(WHEELBRI_BUILD) " UTC)";
#else
    const std::string built;
#endif
    logLine(NT_SUCCESS(status) ? Level::Info : Level::Error,
            NT_SUCCESS(status) ? "wheelbri-wheel driver loaded" + built
                               : "WdfDriverCreate failed: " + std::to_string(status));
    if (!NT_SUCCESS(status)) WSACleanup();   // no driver object, so no cleanup callback either
    return status;
}
