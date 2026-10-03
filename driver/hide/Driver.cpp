// wheelbri-hide: hides the rig (the wheel and every accessory the Wheel setup tab defined) from games while
// wheelbri-service presents the virtual G920.
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
//     process's creation time, QWORD FILETIME) catches a later process that got the same id.
// Fail open: if the setting cannot be read, the wheel is not hidden (and that is logged).
// Everything else (reads, force feedback, feature reports) passes through untouched.

#include <windows.h>
#include <wdf.h>

#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace {

constexpr wchar_t kPolicyKey[] = L"SOFTWARE\\wheelbri\\hide";
constexpr ULONG kSystemPid = 4;

// Log: <the logs folder>\wheelbri-hide-<date>.log (see logFolder) and the debugger (DebugView).
std::mutex g_logLock;

// The folder for the log: the one the app shares with the drivers (HKLM\SOFTWARE\wheelbri LogFolder, the user's
// %LOCALAPPDATA%\wheelbri\logs, which the app lets LocalService write in), else C:\ProgramData\wheelbri\logs (before the
// app ever ran as Administrator). Read for every line, so a change applies at once.
std::wstring logFolder() {
    wchar_t shared[MAX_PATH] = {};
    DWORD size = sizeof(shared);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\wheelbri", L"LogFolder", RRF_RT_REG_SZ, nullptr, shared, &size) ==
            ERROR_SUCCESS &&
        shared[0] != L'\0') {
        return shared;
    }
    CreateDirectoryW(L"C:\\ProgramData\\wheelbri", nullptr);
    CreateDirectoryW(L"C:\\ProgramData\\wheelbri\\logs", nullptr);
    return L"C:\\ProgramData\\wheelbri\\logs";
}

// Appends `line` to <folder>\wheelbri-hide-<date>.log; false if the file cannot be opened.
bool appendTo(const std::wstring& folder, const SYSTEMTIME& now, const char* line) {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\wheelbri-hide-%04u-%02u-%02u.log", folder.c_str(), now.wYear, now.wMonth, now.wDay);
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
    CloseHandle(file);
    return true;
}

void logLine(const std::string& text) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    char line[1024];
    std::snprintf(line, sizeof(line), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\r\n", now.wYear, now.wMonth, now.wDay,
                  now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, text.c_str());
    OutputDebugStringA(line);
    std::lock_guard lock(g_logLock);
    static bool reportedFailure = false;
    if (appendTo(logFolder(), now, line) || appendTo(L"C:\\ProgramData\\wheelbri\\logs", now, line)) {
        reportedFailure = false;
    } else if (!reportedFailure) {
        reportedFailure = true;   // only DebugView can show this one
        char why[160];
        std::snprintf(why, sizeof(why), "wheelbri-hide: cannot write its log (error %lu)\r\n", GetLastError());
        OutputDebugStringA(why);
    }
}

// Logged once per distinct problem (a game opens the wheel often).
void logOnce(std::string text) {
    static std::mutex lock;
    static std::string last;
    std::lock_guard guard(lock);
    if (text == last) return;
    last = text;
    logLine(text);
}

// Whether `pid` still runs and is the process that wrote the setting. Unknown (cannot look) counts as running:
// the driver host runs as LocalService and may not open an elevated service.
bool stillRunning(const DWORD pid, const std::optional<ULONGLONG> started) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        const DWORD error = GetLastError();
        if (error == ERROR_INVALID_PARAMETER) return false;   // no such process
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
            logOnce("WARNING: cannot read HKLM\\SOFTWARE\\wheelbri\\hide AllowedPid (error " + std::to_string(status) +
                    "): the wheel is not hidden");
        }
        return 0;
    }
    if (value == 0) return 0;
    ULONGLONG started = 0;
    DWORD startedSize = sizeof(started);
    const bool haveStart = RegGetValueW(HKEY_LOCAL_MACHINE, kPolicyKey, L"AllowedPidStart", RRF_RT_REG_QWORD, nullptr,
                                        &started, &startedSize) == ERROR_SUCCESS;
    if (!stillRunning(value, haveStart && started != 0 ? std::optional<ULONGLONG>(started) : std::nullopt)) {
        logOnce("process " + std::to_string(value) +
                " hid the wheel but is gone (crashed or killed?): not hidden, every program may open it");
        return 0;
    }
    return value;
}

// One log line per refused program (a game retries often), until the hiding changes.
std::mutex g_seenLock;
std::unordered_map<ULONG, bool> g_refused;
ULONG g_lastAllowed = ~0UL;

void noteDecision(const ULONG pid, const ULONG allowed, const bool refused) {
    std::lock_guard lock(g_seenLock);
    if (allowed != g_lastAllowed) {
        g_lastAllowed = allowed;
        g_refused.clear();
        logLine(allowed == 0 ? "not hidden: every program may open the wheel"
                             : "hidden: only process " + std::to_string(allowed) + " may open the wheel");
    }
    if (refused && g_refused.emplace(pid, true).second) {
        logLine("refused process " + std::to_string(pid) + " (the wheel is hidden from it)");
    }
}

// Per device: never hide the virtual G920 (046D:C262), even if the filter ended up on it (Windows put it there by
// itself once): hiding it would hide the wheel games are meant to see.
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
    const bool virtualG920 = upper.find("VID_046D&PID_C262") != std::string::npos;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContext);
    WDFDEVICE device;
    const NTSTATUS status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        logLine("WdfDeviceCreate failed on " + id + ": " + std::to_string(status));
        return status;
    }
    deviceContext(device)->neverHide = virtualG920;
    // No queues: everything but opening the device goes straight to the HID class driver below.
    if (virtualG920) {
        logLine("attached to " + id + ", the virtual G920: never hidden (this filter does not belong there; "
                "reinstall WheelBri to take it off)");
    } else {
        logLine("attached to " + id + "; " + std::string(allowedPid() == 0 ? "not hidden now" : "hidden now"));
    }
    return STATUS_SUCCESS;
}

}  // namespace

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    const NTSTATUS status = WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    logLine(NT_SUCCESS(status) ? "wheelbri-hide driver loaded" : "WdfDriverCreate failed: " + std::to_string(status));
    return status;
}
