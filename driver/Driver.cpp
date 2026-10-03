// wheelbri-g920: a UMDF 2 HID minidriver presenting a Logitech G920 to Windows (Phase 5, plan B).
//
// It does what the former HIDMaestro bridge (step 0) did, inside Windows' driver host:
//   input  wheelbri-service --UDP 127.0.0.1:41329 (StatePacket)--> driver --> HID input report --> games
//   ffb    games --DirectInput--> pid.dll --HID PID reports--> driver --UDP 127.0.0.1:41330 (FfbPacket)--> service
// so the service does not change: it talks to this driver exactly as to the in-game shim.
// Only one of them may run at a time (they share the ports).
//
// The device itself is created by wheelbri-service while it runs (a software device, see
// service/src/VirtualG920.cpp); this INF-installed driver binds to it by hardware ID wheelbri\G920.
//
// Structure follows Microsoft's vhidmini2 sample (UMDF 2): HID class requests arrive as IOCTLs on a
// parallel queue; read requests wait on a manual queue until a state packet arrives.

#include <winsock2.h>   // before windows.h
#include <windows.h>
#include <wdf.h>
#include <hidport.h>        // $(DDK_INC_PATH)\wdm

#include "wheelbri/FfbPacket.hpp"
#include "wheelbri/HidG920.hpp"
#include "wheelbri/HidPid.hpp"
#include "wheelbri/StatePacket.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr USHORT kStatePort = 41329;
constexpr USHORT kFfbPort = 41330;
constexpr std::int64_t kStaleMs = 1500;              // same as the shim: then centred wheel, released pedals
constexpr wchar_t kManufacturer[] = L"Logitech";
constexpr wchar_t kProduct[] = L"G920 Driving Force Racing Wheel USB";
constexpr wchar_t kSerial[] = L"WHEELBRI0001";

using Report = std::vector<std::uint8_t>;

Report g920Report(const wheelbri::TargetReport& report) {
    const auto packed = wheelbri::packG920Input(report);
    return Report(packed.begin(), packed.end());
}

// ---------------------------------------------------------------------------------------------------
// Log: C:\ProgramData\wheelbri\logs\wheelbri-driver-<date>.log (the driver host runs as LocalService, which
// cannot write to a user's profile) and the debugger (DebugView).

std::mutex g_logLock;

void logLine(const std::string& text) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    char line[1024];
    std::snprintf(line, sizeof(line), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\r\n", now.wYear, now.wMonth, now.wDay,
                  now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, text.c_str());
    OutputDebugStringA(line);
    std::lock_guard lock(g_logLock);
    CreateDirectoryW(L"C:\\ProgramData\\wheelbri", nullptr);
    CreateDirectoryW(L"C:\\ProgramData\\wheelbri\\logs", nullptr);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"C:\\ProgramData\\wheelbri\\logs\\wheelbri-driver-%04u-%02u-%02u.log", now.wYear, now.wMonth, now.wDay);
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    static bool reportedFailure = false;
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
        CloseHandle(file);
        reportedFailure = false;
    } else if (!reportedFailure) {
        reportedFailure = true;   // only DebugView can show this one
        char why[160];
        std::snprintf(why, sizeof(why), "wheelbri-g920: cannot write its log in C:\\ProgramData\\wheelbri\\logs (error %lu)\r\n",
                      GetLastError());
        OutputDebugStringA(why);
    }
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
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

// ---------------------------------------------------------------------------------------------------

}  // namespace

struct DeviceContext;

namespace {

// The UDP side: state packets in, force feedback out, on its own thread.
class Link {
public:
    explicit Link(DeviceContext* device) : device_(device), report_(g920Report(wheelbri::g920Neutral())) {}
    ~Link() { stop(); }

    bool start();
    void stop();
    void sendFfb(const wheelbri::FfbPacket& packet);

    wheelbri::HidPid pid{[this](const wheelbri::FfbPacket& p) { sendFfb(p); }, [](const std::string& s) { logLine(s); }};

    // The latest input report (neutral until the service sends state).
    Report latestReport() {
        std::lock_guard lock(reportLock_);
        return report_;
    }

private:
    void run(SOCKET sock);
    void publish(Report report);
    // false = stop the thread; a socket error other than the receive timeout is logged and slowed down.
    bool receiveFailed(int received);
    void closeSockets();

    DeviceContext* device_;
    SOCKET stateSocket_ = INVALID_SOCKET;
    SOCKET ffbSocket_ = INVALID_SOCKET;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex reportLock_;
    Report report_;
    Throttle recvErrors_{60'000};
    Throttle sendErrors_{60'000};
};

}  // namespace

struct DeviceContext {
    WDFDEVICE device;
    WDFQUEUE readQueue;              // manual: IOCTL_HID_READ_REPORT waits here for the next state
    HID_DEVICE_ATTRIBUTES attributes;
    HID_DESCRIPTOR hidDescriptor;
    Link* link;                      // owned; deleted in the device's cleanup callback
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, GetDeviceContext)

namespace {

// Completes every waiting read with `report`.
void completeReads(DeviceContext* ctx, const Report& report) {
    WDFREQUEST request;
    while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(ctx->readQueue, &request))) {
        WDFMEMORY memory;
        size_t length = 0;
        NTSTATUS status = WdfRequestRetrieveOutputMemory(request, &memory);
        if (NT_SUCCESS(status)) {
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
}

bool Link::start() {
    stop();
    stop_ = false;
    stateSocket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ffbSocket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (stateSocket_ == INVALID_SOCKET || ffbSocket_ == INVALID_SOCKET) {
        logLine("ERROR: cannot create UDP sockets (" + std::to_string(WSAGetLastError()) + "): the device will not start");
        closeSockets();
        return false;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(kStatePort);
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(stateSocket_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        logLine("cannot listen on 127.0.0.1:41329 (" + std::to_string(WSAGetLastError()) +
                "): is the shim's game still running? Only one of them may run.");
        closeSockets();
        return false;
    }
    // The thread must wake regularly (stop requests, the stale-state check, force feedback timing).
    DWORD timeoutMs = 5;
    if (setsockopt(stateSocket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs)) != 0) {
        logLine("ERROR: cannot set the receive timeout (" + std::to_string(WSAGetLastError()) + "): the device will not start");
        closeSockets();
        return false;
    }
    sockaddr_in service{};
    service.sin_family = AF_INET;
    service.sin_port = htons(kFfbPort);
    service.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(ffbSocket_, reinterpret_cast<sockaddr*>(&service), sizeof(service)) != 0) {
        logLine("WARNING: cannot address the service's force feedback port 127.0.0.1:41330 (" +
                std::to_string(WSAGetLastError()) + "): the game's force feedback will not reach the wheel");
    }
    // The thread gets its own copy of the socket: stop() closes it only after the thread has ended.
    thread_ = std::thread([this, sock = stateSocket_] { run(sock); });
    return true;
}

void Link::publish(Report report) {
    {
        std::lock_guard lock(reportLock_);
        report_ = report;
    }
    completeReads(device_, report);
}

bool Link::receiveFailed(const int received) {
    (void)received;
    const int error = WSAGetLastError();
    if (error == WSAETIMEDOUT) return true;   // nothing this time: the normal case
    if (stop_) return false;
    const std::int64_t now = nowMs();
    if (recvErrors_.allow(now)) {
        const auto skipped = recvErrors_.takeSuppressed();
        logLine("WARNING: receiving from wheelbri-service failed (" + std::to_string(error) + ")" +
                (skipped > 0 ? ", " + std::to_string(skipped) + " more since the last line" : std::string()));
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
            const auto skipped = sendErrors_.takeSuppressed();
            logLine("WARNING: force feedback to wheelbri-service not sent (" + std::to_string(error) +
                    "; is the service running?)" +
                    (skipped > 0 ? ", " + std::to_string(skipped) + " more since the last line" : std::string()));
        }
    }
}

void Link::run(const SOCKET sock) {
    logLine("listening for wheelbri-service on 127.0.0.1:41329, force feedback to 127.0.0.1:41330");
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
                    logLine("receiving wheel state from wheelbri-service (packet #" + std::to_string(packet->sequence) + ")");
                }
                lastState = now;
                neutralSent = false;
                publish(g920Report(packet->report));
            }
        } else if (now - lastState > kStaleMs && !neutralSent) {
            if (receiving) {
                logLine("no wheel state for 1500 ms: centred wheel and released pedals until it comes back");
            }
            receiving = false;
            neutralSent = true;
            publish(g920Report(wheelbri::g920Neutral()));
        }
        if (now - lastTick >= 10) {
            lastTick = now;
            pid.tick(now);
        }
    }
    pid.stopAll(nowMs());
    logLine("stopped: " + std::to_string(pid.reportsHandled()) + " force feedback reports from the game, " +
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

NTSTATUS getString(WDFREQUEST request) {
    PVOID buffer = nullptr;
    size_t size = 0;
    NTSTATUS status = WdfRequestRetrieveInputBuffer(request, sizeof(ULONG), &buffer, &size);
    if (!NT_SUCCESS(status)) return status;
    const ULONG id = *static_cast<const ULONG*>(buffer) & 0xFFFFU;
    switch (id) {
        case HID_STRING_ID_IMANUFACTURER: return copyToRequest(request, kManufacturer, sizeof(kManufacturer));
        case HID_STRING_ID_IPRODUCT: return copyToRequest(request, kProduct, sizeof(kProduct));
        case HID_STRING_ID_ISERIALNUMBER: return copyToRequest(request, kSerial, sizeof(kSerial));
        default: return STATUS_INVALID_PARAMETER;
    }
}

VOID EvtIoDeviceControl(WDFQUEUE queue, WDFREQUEST request, size_t, size_t, ULONG code) {
    DeviceContext* ctx = GetDeviceContext(WdfIoQueueGetDevice(queue));
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
            const auto descriptor = wheelbri::g920ReportDescriptor();
            status = copyToRequest(request, descriptor.data(), descriptor.size());
            break;
        }
        case IOCTL_HID_READ_REPORT:
            status = WdfRequestForwardToIoQueue(request, ctx->readQueue);
            if (NT_SUCCESS(status)) return;   // completed when the next state arrives
            break;
        case IOCTL_HID_WRITE_REPORT:
        case IOCTL_UMDF_HID_SET_OUTPUT_REPORT:
            status = readWrittenReport(request, reportId, data);
            if (NT_SUCCESS(status)) {
                ctx->link->pid.output(reportId, data, nowMs());
                WdfRequestSetInformation(request, data.size() + 1);
            }
            break;
        case IOCTL_UMDF_HID_SET_FEATURE:
            status = readWrittenReport(request, reportId, data);
            if (NT_SUCCESS(status)) {
                ctx->link->pid.setFeature(reportId, data, nowMs());
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
            const std::size_t written =
                ctx->link->pid.getFeature(reportId, std::span(static_cast<std::uint8_t*>(out), outSize));
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
            status = getString(request);
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
    return ctx->link != nullptr && ctx->link->start() ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

NTSTATUS EvtDeviceD0Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE) {
    DeviceContext* ctx = GetDeviceContext(device);
    if (ctx->link != nullptr) ctx->link->stop();
    return STATUS_SUCCESS;
}

VOID EvtDeviceCleanup(WDFOBJECT object) {
    DeviceContext* ctx = GetDeviceContext(static_cast<WDFDEVICE>(object));
    delete ctx->link;
    ctx->link = nullptr;
    logLine("virtual G920 removed");
}

NTSTATUS EvtDeviceAdd(WDFDRIVER, PWDFDEVICE_INIT init) {
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
        logLine("WdfDeviceCreate failed: " + std::to_string(status));
        return status;
    }
    DeviceContext* ctx = GetDeviceContext(device);
    ctx->device = device;
    ctx->link = nullptr;

    RtlZeroMemory(&ctx->attributes, sizeof(ctx->attributes));
    ctx->attributes.Size = sizeof(HID_DEVICE_ATTRIBUTES);
    ctx->attributes.VendorID = wheelbri::kG920VendorId;
    ctx->attributes.ProductID = wheelbri::kG920ProductId;
    ctx->attributes.VersionNumber = 0x0100;

    RtlZeroMemory(&ctx->hidDescriptor, sizeof(ctx->hidDescriptor));
    ctx->hidDescriptor.bLength = sizeof(HID_DESCRIPTOR);
    ctx->hidDescriptor.bDescriptorType = 0x21;
    ctx->hidDescriptor.bcdHID = 0x0100;
    ctx->hidDescriptor.bNumDescriptors = 1;
    ctx->hidDescriptor.DescriptorList[0].bReportType = 0x22;
    ctx->hidDescriptor.DescriptorList[0].wReportLength = static_cast<USHORT>(
        wheelbri::g920ReportDescriptor().size());

    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = EvtIoDeviceControl;
    WDFQUEUE queue;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) {
        logLine("default queue: " + std::to_string(status));
        return status;
    }
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &ctx->readQueue);
    if (!NT_SUCCESS(status)) {
        logLine("read queue: " + std::to_string(status));
        return status;
    }

    ctx->link = new Link(ctx);
    logLine("virtual G920 added (VID 046D PID C262)");
    return STATUS_SUCCESS;
}

}  // namespace

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
    WSADATA wsa;
    if (const int error = WSAStartup(MAKEWORD(2, 2), &wsa); error != 0) {
        logLine("ERROR: WSAStartup failed (" + std::to_string(error) + "): the driver cannot talk to wheelbri-service");
        return STATUS_UNSUCCESSFUL;
    }
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    const NTSTATUS status = WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    logLine(NT_SUCCESS(status) ? "wheelbri-g920 driver loaded" : "WdfDriverCreate failed: " + std::to_string(status));
    return status;
}
