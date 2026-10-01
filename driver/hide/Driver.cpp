// wheelemu-hide: hides the real wheel (Moza R3) from games while wheelemu-service presents the virtual G920.
// Our replacement for HidHide (third-party), built the same way as our G920 driver: UMDF 2, user mode, signed
// with the certificate trusted on this PC; no kernel code of ours, no test mode.
//
// It is an upper filter on the R3's game-controller interface (HID\VID_346E&PID_0005&MI_02; Moza Pit House
// talks to the other interfaces). Every program that opens that interface asks it first (IRP_MJ_CREATE):
//   - while wheelemu-service hides the wheel, it has written its process id to
//       HKLM\SOFTWARE\wheelemu\hide  AllowedPid (DWORD)
//     and only that process may open the R3; everyone else gets "access denied", so DirectInput, SDL, raw input
//     and the game cannot see or read it (the same effect as HidHide);
//   - when AllowedPid is 0 or missing (the service is not running, or stopped cleanly), everyone may open it:
//     the R3 is an ordinary wheel again.
// Everything else (reads, force feedback, feature reports) passes through untouched.

#include <windows.h>
#include <wdf.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

constexpr wchar_t kPolicyKey[] = L"SOFTWARE\\wheelemu\\hide";
constexpr ULONG kSystemPid = 4;

// C:\ProgramData\wheelemu\logs\wheelemu-hide-<date>.log (the driver host runs as LocalService) and DebugView.
std::mutex g_logLock;

void logLine(const std::string& text) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    char line[1024];
    std::snprintf(line, sizeof(line), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\r\n", now.wYear, now.wMonth, now.wDay,
                  now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, text.c_str());
    OutputDebugStringA(line);
    std::lock_guard lock(g_logLock);
    CreateDirectoryW(L"C:\\ProgramData\\wheelemu", nullptr);
    CreateDirectoryW(L"C:\\ProgramData\\wheelemu\\logs", nullptr);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"C:\\ProgramData\\wheelemu\\logs\\wheelemu-hide-%04u-%02u-%02u.log", now.wYear, now.wMonth, now.wDay);
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
        CloseHandle(file);
    }
}

// The process allowed to open the wheel while it is hidden; 0 = not hidden.
ULONG allowedPid() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kPolicyKey, L"AllowedPid", RRF_RT_REG_DWORD, nullptr, &value, &size) !=
        ERROR_SUCCESS) {
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

VOID WINAPI onCreateForwarded(WDFREQUEST request, WDFIOTARGET, PWDF_REQUEST_COMPLETION_PARAMS params, WDFCONTEXT) {
    WdfRequestComplete(request, params->IoStatus.Status);
}

VOID EvtFileCreate(WDFDEVICE device, WDFREQUEST request, WDFFILEOBJECT) {
    const ULONG allowed = allowedPid();
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

    WDFDEVICE device;
    const NTSTATUS status = WdfDeviceCreate(&init, WDF_NO_OBJECT_ATTRIBUTES, &device);
    if (!NT_SUCCESS(status)) {
        logLine("WdfDeviceCreate failed: " + std::to_string(status));
        return status;
    }
    // No queues: everything but opening the wheel goes straight to the HID class driver below.
    logLine("attached to the wheel's game-controller interface; " +
            std::string(allowedPid() == 0 ? "not hidden now" : "hidden now"));
    return STATUS_SUCCESS;
}

}  // namespace

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    const NTSTATUS status = WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    logLine(NT_SUCCESS(status) ? "wheelemu-hide driver loaded" : "WdfDriverCreate failed: " + std::to_string(status));
    return status;
}
