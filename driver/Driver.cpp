// wheelemu-g920: a UMDF 2 HID minidriver presenting a Logitech G920 to Windows (Phase 5, plan B).
//
// It does what tools/vbridge + HIDMaestro did, inside Windows' driver host:
//   input  wheelemu-service --UDP 127.0.0.1:41329 (StatePacket)--> driver --> HID input report --> games
//   ffb    games --DirectInput--> pid.dll --HID PID reports--> driver --UDP 127.0.0.1:41330 (FfbPacket)--> service
// so the service does not change: it talks to this driver exactly as to the in-game shim or the bridge.
// Only one of them may run at a time (they share the ports).
//
// The device itself is created by wheelemu-service while it runs (a software device, see
// service/src/VirtualG920.cpp); this INF-installed driver binds to it by hardware ID wheelemu\G920.
//
// Structure follows Microsoft's vhidmini2 sample (UMDF 2): HID class requests arrive as IOCTLs on a
// parallel queue; read requests wait on a manual queue until a state packet arrives.
//
// The same driver also serves the HID side of the virtual Xbox 360 controller (hardware ID wheelemu\XBOXHID,
// core/HidXbox360.hpp): what joy.cpl and DirectInput list as "Controller (XBOX 360 For Windows)", as for a real
// one, next to the XInput side (driver/xusb). That device gets the service's pad packets on 127.0.0.1:41333 and
// has no force feedback.

#include <winsock2.h>   // before windows.h
#include <windows.h>
#include <wdf.h>
#include <hidport.h>        // $(DDK_INC_PATH)\wdm

#include "wheelemu/FfbPacket.hpp"
#include "wheelemu/HidG920.hpp"
#include "wheelemu/HidXbox360.hpp"
#include "wheelemu/HidPid.hpp"
#include "wheelemu/StatePacket.hpp"
#include "wheelemu/XboxPad.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr USHORT kStatePort = 41329;
constexpr USHORT kFfbPort = 41330;
constexpr std::int64_t kStaleMs = 1500;              // same as the shim: then centred wheel, released pedals
constexpr wchar_t kManufacturer[] = L"Logitech";
constexpr wchar_t kProduct[] = L"G920 Driving Force Racing Wheel USB";
constexpr wchar_t kSerial[] = L"WHEELEMU0001";
// The Xbox controller's HID side: the name Windows gives a real wired one.
constexpr wchar_t kXboxManufacturer[] = L"Microsoft";
constexpr wchar_t kXboxProduct[] = L"Controller (XBOX 360 For Windows)";
constexpr wchar_t kXboxSerial[] = L"WHEELEMUXBOX1";

using Report = std::vector<std::uint8_t>;

Report g920Report(const wheelemu::TargetReport& report) {
    const auto packed = wheelemu::packG920Input(report);
    return Report(packed.begin(), packed.end());
}

Report xboxReport(const wheelemu::XusbPad& pad) {
    const auto packed = wheelemu::packXboxHidInput(pad);
    return Report(packed.begin(), packed.end());
}

// ---------------------------------------------------------------------------------------------------
// Log: C:\ProgramData\wheelemu\logs\wheelemu-driver-<date>.log (the driver host runs as LocalService, which
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
    CreateDirectoryW(L"C:\\ProgramData\\wheelemu", nullptr);
    CreateDirectoryW(L"C:\\ProgramData\\wheelemu\\logs", nullptr);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"C:\\ProgramData\\wheelemu\\logs\\wheelemu-driver-%04u-%02u-%02u.log", now.wYear, now.wMonth, now.wDay);
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
        CloseHandle(file);
    }
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------------------------------

}  // namespace

struct DeviceContext;

namespace {

// The UDP side: state packets in, force feedback out, on its own thread.
class Link {
public:
    // `xbox`: the Xbox controller's HID side (pad packets on 41333, no force feedback), else the G920.
    Link(DeviceContext* device, const bool xbox)
        : device_(device), xbox_(xbox), report_(xbox ? xboxReport({}) : g920Report(wheelemu::g920Neutral())) {}
    ~Link() { stop(); }

    bool start();
    void stop();
    void sendFfb(const wheelemu::FfbPacket& packet);

    wheelemu::HidPid pid{[this](const wheelemu::FfbPacket& p) { sendFfb(p); }, [](const std::string& s) { logLine(s); }};

    // The latest input report (neutral until the service sends state).
    Report latestReport() {
        std::lock_guard lock(reportLock_);
        return report_;
    }

private:
    void run();
    void runXbox();
    void publish(Report report);

    DeviceContext* device_;
    bool xbox_;
    SOCKET stateSocket_ = INVALID_SOCKET;
    SOCKET ffbSocket_ = INVALID_SOCKET;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex reportLock_;
    Report report_;
};

}  // namespace

struct DeviceContext {
    WDFDEVICE device;
    WDFQUEUE readQueue;              // manual: IOCTL_HID_READ_REPORT waits here for the next state
    HID_DEVICE_ATTRIBUTES attributes;
    HID_DESCRIPTOR hidDescriptor;
    bool xbox;                       // the Xbox controller's HID side (wheelemu\XBOXHID), else the G920
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
        logLine("cannot create UDP sockets: " + std::to_string(WSAGetLastError()));
        return false;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(xbox_ ? wheelemu::kXboxHidPort : kStatePort);
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(stateSocket_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        logLine(xbox_ ? "Xbox controller (HID): cannot listen on 127.0.0.1:41333 (" + std::to_string(WSAGetLastError()) + ")"
                      : "cannot listen on 127.0.0.1:41329 (" + std::to_string(WSAGetLastError()) +
                            "): is the shim's game or wheelemu-bridge still running? Only one of them may run.");
        return false;
    }
    DWORD timeoutMs = 5;
    setsockopt(stateSocket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    sockaddr_in service{};
    service.sin_family = AF_INET;
    service.sin_port = htons(kFfbPort);
    service.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    connect(ffbSocket_, reinterpret_cast<sockaddr*>(&service), sizeof(service));
    thread_ = std::thread([this] {
        if (xbox_) {
            runXbox();
        } else {
            run();
        }
    });
    return true;
}

void Link::publish(Report report) {
    {
        std::lock_guard lock(reportLock_);
        report_ = report;
    }
    completeReads(device_, report);
}

// The Xbox controller's HID side: the same pad packets as driver/xusb gets, as HID input reports.
void Link::runXbox() {
    logLine("Xbox controller (HID): listening for wheelemu-service on 127.0.0.1:41333");
    std::array<std::uint8_t, 64> buffer{};
    std::int64_t lastPad = nowMs() - kStaleMs - 1;
    bool receiving = false;
    bool neutralSent = false;
    while (!stop_) {
        const int received = recv(stateSocket_, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
        const std::int64_t now = nowMs();
        if (received > 0) {
            if (const auto pad = wheelemu::decodeXusbPacket(std::span(buffer.data(), static_cast<std::size_t>(received)))) {
                if (!receiving) {
                    receiving = true;
                    logLine("Xbox controller (HID): receiving the pad state from wheelemu-service");
                }
                lastPad = now;
                neutralSent = false;
                publish(xboxReport(*pad));
            }
        } else if (now - lastPad > kStaleMs && !neutralSent) {
            receiving = false;
            neutralSent = true;
            publish(xboxReport({}));
        }
    }
    logLine("Xbox controller (HID): stopped");
}

void Link::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (stateSocket_ != INVALID_SOCKET) closesocket(stateSocket_);
    if (ffbSocket_ != INVALID_SOCKET) closesocket(ffbSocket_);
    stateSocket_ = ffbSocket_ = INVALID_SOCKET;
}

void Link::sendFfb(const wheelemu::FfbPacket& packet) {
    const auto bytes = wheelemu::encode(packet);
    send(ffbSocket_, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
}

void Link::run() {
    logLine("listening for wheelemu-service on 127.0.0.1:41329, force feedback to 127.0.0.1:41330");
    std::array<std::uint8_t, 256> buffer{};
    std::int64_t lastState = nowMs() - kStaleMs - 1;
    std::int64_t lastTick = 0;
    bool receiving = false;
    bool neutralSent = false;
    while (!stop_) {
        const int received = recv(stateSocket_, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
        const std::int64_t now = nowMs();
        if (received > 0) {
            if (const auto packet = wheelemu::decodeStatePacket(std::span(buffer.data(), static_cast<std::size_t>(received)))) {
                if (!receiving) {
                    receiving = true;
                    logLine("receiving wheel state from wheelemu-service (packet #" + std::to_string(packet->sequence) + ")");
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
            publish(g920Report(wheelemu::g920Neutral()));
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

NTSTATUS getString(WDFREQUEST request, const bool xbox) {
    PVOID buffer = nullptr;
    size_t size = 0;
    NTSTATUS status = WdfRequestRetrieveInputBuffer(request, sizeof(ULONG), &buffer, &size);
    if (!NT_SUCCESS(status)) return status;
    const ULONG id = *static_cast<const ULONG*>(buffer) & 0xFFFFU;
    switch (id) {
        case HID_STRING_ID_IMANUFACTURER:
            return xbox ? copyToRequest(request, kXboxManufacturer, sizeof(kXboxManufacturer))
                        : copyToRequest(request, kManufacturer, sizeof(kManufacturer));
        case HID_STRING_ID_IPRODUCT:
            return xbox ? copyToRequest(request, kXboxProduct, sizeof(kXboxProduct))
                        : copyToRequest(request, kProduct, sizeof(kProduct));
        case HID_STRING_ID_ISERIALNUMBER:
            return xbox ? copyToRequest(request, kXboxSerial, sizeof(kXboxSerial))
                        : copyToRequest(request, kSerial, sizeof(kSerial));
        default: return STATUS_INVALID_PARAMETER;
    }
}

VOID EvtIoDeviceControl(WDFQUEUE queue, WDFREQUEST request, size_t, size_t, ULONG code) {
    DeviceContext* ctx = GetDeviceContext(WdfIoQueueGetDevice(queue));
    NTSTATUS status = STATUS_NOT_IMPLEMENTED;
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
            const auto descriptor = ctx->xbox ? wheelemu::xboxHidReportDescriptor() : wheelemu::g920ReportDescriptor();
            status = copyToRequest(request, descriptor.data(), descriptor.size());
            break;
        }
        case IOCTL_HID_READ_REPORT:
            status = WdfRequestForwardToIoQueue(request, ctx->readQueue);
            if (NT_SUCCESS(status)) return;   // completed when the next state arrives
            break;
        case IOCTL_HID_WRITE_REPORT:
        case IOCTL_UMDF_HID_SET_OUTPUT_REPORT:
            if (ctx->xbox) {   // no force feedback on the Xbox controller's HID side
                status = STATUS_NOT_SUPPORTED;
                break;
            }
            status = readWrittenReport(request, reportId, data);
            if (NT_SUCCESS(status)) {
                ctx->link->pid.output(reportId, data, nowMs());
                WdfRequestSetInformation(request, data.size() + 1);
            }
            break;
        case IOCTL_UMDF_HID_SET_FEATURE:
            if (ctx->xbox) {
                status = STATUS_NOT_SUPPORTED;
                break;
            }
            status = readWrittenReport(request, reportId, data);
            if (NT_SUCCESS(status)) {
                ctx->link->pid.setFeature(reportId, data, nowMs());
                WdfRequestSetInformation(request, data.size() + 1);
            }
            break;
        case IOCTL_UMDF_HID_GET_FEATURE: {
            if (ctx->xbox) {
                status = STATUS_NOT_SUPPORTED;
                break;
            }
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
            status = getString(request, ctx->xbox);
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
    logLine(ctx->xbox ? "virtual Xbox controller (HID) removed" : "virtual G920 removed");
}

// The device's hardware ID decides what it is: wheelemu\XBOXHID is the Xbox controller's HID side, else the G920.
bool isXboxHid(PWDFDEVICE_INIT init) {
    wchar_t ids[512] = {};
    ULONG length = 0;
    if (!NT_SUCCESS(WdfFdoInitQueryProperty(init, DevicePropertyHardwareID, sizeof(ids) - sizeof(wchar_t), ids, &length))) {
        return false;
    }
    for (const wchar_t* id = ids; *id != L'\0'; id += wcslen(id) + 1) {
        if (_wcsicmp(id, L"wheelemu\\XBOXHID") == 0) return true;
    }
    return false;
}

NTSTATUS EvtDeviceAdd(WDFDRIVER, PWDFDEVICE_INIT init) {
    const bool xbox = isXboxHid(init);
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
    ctx->xbox = xbox;
    ctx->link = nullptr;

    RtlZeroMemory(&ctx->attributes, sizeof(ctx->attributes));
    ctx->attributes.Size = sizeof(HID_DEVICE_ATTRIBUTES);
    ctx->attributes.VendorID = xbox ? wheelemu::kXboxHidVendorId : wheelemu::kG920VendorId;
    ctx->attributes.ProductID = xbox ? wheelemu::kXboxHidProductId : wheelemu::kG920ProductId;
    ctx->attributes.VersionNumber = 0x0100;

    RtlZeroMemory(&ctx->hidDescriptor, sizeof(ctx->hidDescriptor));
    ctx->hidDescriptor.bLength = sizeof(HID_DESCRIPTOR);
    ctx->hidDescriptor.bDescriptorType = 0x21;
    ctx->hidDescriptor.bcdHID = 0x0100;
    ctx->hidDescriptor.bNumDescriptors = 1;
    ctx->hidDescriptor.DescriptorList[0].bReportType = 0x22;
    ctx->hidDescriptor.DescriptorList[0].wReportLength = static_cast<USHORT>(
        (xbox ? wheelemu::xboxHidReportDescriptor() : wheelemu::g920ReportDescriptor()).size());

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

    ctx->link = new Link(ctx, xbox);
    logLine(xbox ? "virtual Xbox controller (HID) added (VID 045E PID 028E)" : "virtual G920 added (VID 046D PID C262)");
    return STATUS_SUCCESS;
}

}  // namespace

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    const NTSTATUS status = WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    logLine(NT_SUCCESS(status) ? "wheelemu-g920 driver loaded" : "WdfDriverCreate failed: " + std::to_string(status));
    return status;
}
