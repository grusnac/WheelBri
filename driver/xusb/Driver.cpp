// wheelemu-xusb: a virtual Xbox 360 controller for XInput, next to the virtual G920 (wheelemu-service
// --virtual-g920), so games that pick their button prompts by controller (NFS 2015) show Xbox buttons.
//
// XInput (xinput1_4.dll) finds controllers by the XUSB device interface and talks to them with XUSB IOCTLs. This
// UMDF 2 driver publishes that interface on a software device the service creates (wheelemu\XUSB) and answers
// the IOCTLs with the pad state the service sends over UDP 127.0.0.1:41332 (core/XboxPad.hpp: which wheel buttons
// press it and when; its sticks and triggers never move). No HID: DirectInput does not see a second controller.
//
// The XUSB reply formats follow HIDMaestro's companion driver (MIT, https://github.com/hifihedgehog/HIDMaestro,
// driver/companion.c), which measured them against a real Xbox 360 controller and xinput1_4 / Windows.Gaming.Input.

#include <winsock2.h>   // before windows.h
#include <windows.h>
#include <wdf.h>

#include "wheelemu/XboxPad.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace {

constexpr ULONG IOCTL_XUSB_GET_INFORMATION = 0x80006000;
constexpr ULONG IOCTL_XUSB_GET_CAPABILITIES = 0x8000E004;
constexpr ULONG IOCTL_XUSB_GET_LED_STATE = 0x8000E008;
constexpr ULONG IOCTL_XUSB_GET_STATE = 0x8000E00C;
constexpr ULONG IOCTL_XUSB_SET_STATE = 0x8000A010;
constexpr ULONG IOCTL_XUSB_WAIT_GUIDE = 0x8000E014;
constexpr ULONG IOCTL_XUSB_GET_BATTERY_INFO = 0x8000E018;
constexpr ULONG IOCTL_XUSB_POWER_INFO = 0x80006380;
constexpr ULONG IOCTL_XUSB_WAIT_FOR_INPUT = 0x8000E3AC;
constexpr ULONG IOCTL_XUSB_GET_INFORMATION_EX = 0x8000E3FC;

const GUID kXusbInterface = {0xEC87F1E3, 0xC13B, 0x4100, {0xB5, 0xF7, 0x8B, 0x84, 0xD5, 0x42, 0x60, 0xCB}};
constexpr USHORT kVendorId = 0x045E;    // Microsoft
constexpr USHORT kProductId = 0x028E;   // Xbox 360 Controller (wired)
constexpr std::int64_t kStaleMs = 1500; // no word from the service: nothing pressed

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
    swprintf_s(path, L"C:\\ProgramData\\wheelemu\\logs\\wheelemu-xusb-%04u-%02u-%02u.log", now.wYear, now.wMonth, now.wDay);
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
        std::snprintf(why, sizeof(why), "wheelemu-xusb: cannot write its log in C:\\ProgramData\\wheelemu\\logs (error %lu)\r\n",
                      GetLastError());
        OutputDebugStringA(why);
    }
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The pad state from the service, on its own thread.
class Link {
public:
    ~Link() { stop(); }

    bool start() {
        stop();
        stop_ = false;
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET) {
            logLine("ERROR: cannot create a UDP socket (" + std::to_string(WSAGetLastError()) + "): the device will not start");
            return false;
        }
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(wheelemu::kXusbPort);
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(socket_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
            logLine("ERROR: cannot listen on 127.0.0.1:" + std::to_string(wheelemu::kXusbPort) + " (" +
                    std::to_string(WSAGetLastError()) + "): is another copy of the controller still there?");
            closesocket(std::exchange(socket_, INVALID_SOCKET));
            return false;
        }
        DWORD timeoutMs = 100;
        if (setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs)) != 0) {
            logLine("ERROR: cannot set the receive timeout (" + std::to_string(WSAGetLastError()) + "): the device will not start");
            closesocket(std::exchange(socket_, INVALID_SOCKET));
            return false;
        }
        thread_ = std::thread([this, sock = socket_] { run(sock); });
        return true;
    }

    // The receive timeout (100 ms) wakes the thread; the socket closes only after it has ended.
    void stop() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        const SOCKET sock = std::exchange(socket_, INVALID_SOCKET);
        if (sock != INVALID_SOCKET) closesocket(sock);
    }

    // The current pad, and a packet number that advances when it changes (as a real controller's does).
    wheelemu::XusbPad pad(std::uint32_t& packet) {
        std::lock_guard lock(lock_);
        if (nowMs() - lastPacketMs_ > kStaleMs && pad_ != wheelemu::XusbPad{}) {
            pad_ = {};
            ++packet_;
        }
        packet = packet_;
        return pad_;
    }

private:
    void run(const SOCKET sock) {
        logLine("listening for wheelemu-service on 127.0.0.1:" + std::to_string(wheelemu::kXusbPort));
        std::uint8_t buffer[64];
        bool receiving = false;
        std::int64_t lastErrorLog = INT64_MIN / 2;
        while (!stop_) {
            const int received = recv(sock, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
            if (received < 0) {
                const int error = WSAGetLastError();
                if (stop_) break;
                if (error != WSAETIMEDOUT) {
                    if (nowMs() - lastErrorLog >= 60'000) {
                        lastErrorLog = nowMs();
                        logLine("WARNING: receiving from wheelemu-service failed (" + std::to_string(error) + ")");
                    }
                    if (error == WSAENOTSOCK || error == WSAEINVAL) break;
                    Sleep(5);   // not a busy loop on a persistent error
                }
            }
            if (receiving) {
                std::lock_guard lock(lock_);
                if (nowMs() - lastPacketMs_ > kStaleMs) {
                    receiving = false;
                    logLine("no pad state for 1500 ms: nothing pressed until it comes back");
                }
            }
            if (received <= 0) continue;
            const auto pad = wheelemu::decodeXusbPacket(std::span(buffer, static_cast<std::size_t>(received)));
            if (!pad) continue;
            if (!receiving) {
                receiving = true;
                logLine("receiving the pad state from wheelemu-service");
            }
            std::lock_guard lock(lock_);
            lastPacketMs_ = nowMs();
            if (*pad != pad_) {
                pad_ = *pad;
                ++packet_;
            }
        }
    }

    SOCKET socket_ = INVALID_SOCKET;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex lock_;
    wheelemu::XusbPad pad_;
    std::uint32_t packet_ = 0;
    std::int64_t lastPacketMs_ = 0;
};

}  // namespace

struct DeviceContext {
    WDFQUEUE waitQueue;   // parked IOCTL_XUSB_WAIT_FOR_INPUT (Windows.Gaming.Input), completed by the timer
    WDFTIMER timer;       // runs only while the device is powered up (D0), so never after the Link is gone
    Link* link;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, GetDeviceContext)

namespace {

void complete(WDFREQUEST request, const void* data, size_t size) {
    PVOID buffer = nullptr;
    size_t length = 0;
    if (NT_SUCCESS(WdfRequestRetrieveOutputBuffer(request, size, &buffer, &length))) {
        std::memcpy(buffer, data, size);
        WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, size);
    } else {
        WdfRequestComplete(request, STATUS_BUFFER_TOO_SMALL);
    }
}

void put16(UCHAR* at, std::uint16_t value) {
    at[0] = static_cast<UCHAR>(value & 0xFFU);
    at[1] = static_cast<UCHAR>(value >> 8U);
}

// The 29-byte XUSB state: version 0x0103, a state byte, the packet number, then XINPUT_GAMEPAD at byte 11.
// GET_STATE (xinput1_4) has state 0x01; WAIT_FOR_INPUT (Windows.Gaming.Input) has 0x03 "resumed", report id 0 at
// byte 9 and the payload marker 0x14 at byte 10.
void buildState(DeviceContext* ctx, UCHAR state[29], const bool waitForInput) {
    std::memset(state, 0, 29);
    put16(state, 0x0103);
    std::uint32_t packet = 0;
    const wheelemu::XusbPad pad = ctx->link != nullptr ? ctx->link->pad(packet) : wheelemu::XusbPad{};
    state[2] = waitForInput ? 0x03 : 0x01;
    std::memcpy(&state[5], &packet, sizeof(packet));
    if (waitForInput) {
        state[9] = 0x00;
        state[10] = 0x14;
    }
    put16(&state[11], pad.buttons);
    state[13] = pad.leftTrigger;
    state[14] = pad.rightTrigger;
    put16(&state[15], static_cast<std::uint16_t>(pad.thumbLX));
    put16(&state[17], static_cast<std::uint16_t>(pad.thumbLY));
    put16(&state[19], static_cast<std::uint16_t>(pad.thumbRX));
    put16(&state[21], static_cast<std::uint16_t>(pad.thumbRY));
}

VOID EvtIoDeviceControl(WDFQUEUE queue, WDFREQUEST request, size_t outputLength, size_t, ULONG code) {
    DeviceContext* ctx = GetDeviceContext(WdfIoQueueGetDevice(queue));
    switch (code) {
        case IOCTL_XUSB_GET_INFORMATION: {
            UCHAR info[12] = {};
            put16(&info[0], 0x0103);
            info[2] = 0x01;   // one controller on this device
            put16(&info[8], kVendorId);
            put16(&info[10], kProductId);
            complete(request, info, sizeof(info));
            return;
        }
        case IOCTL_XUSB_GET_INFORMATION_EX: {
            UCHAR info[64] = {};
            put16(&info[0], 0x0103);
            info[2] = 0x01;
            info[3] = 0x01;
            put16(&info[8], kVendorId);
            put16(&info[10], kProductId);
            complete(request, info, outputLength < sizeof(info) ? outputLength : sizeof(info));
            return;
        }
        case IOCTL_XUSB_GET_CAPABILITIES: {
            // A wired gamepad with every button and axis (as a real one reports; ours just never moves them).
            static const UCHAR v1[24] = {0x03, 0x01, 0x00, 0x01, 0xFF, 0xF7, 0xFF, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF,
                                         0xC0, 0xFF, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF};
            const UCHAR v2[36] = {0x03, 0x01, 0x01, 0x01, 0x0C, 0x00, 0x5E, 0x04, 0x8E, 0x02, 0x10, 0x01,
                                  0x00, 0xFA, 0x34, 0x22, 0xFF, 0xF7, 0xFF, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF,
                                  0xC0, 0xFF, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF};
            if (outputLength >= sizeof(v2)) {
                complete(request, v2, sizeof(v2));
            } else {
                complete(request, v1, outputLength < sizeof(v1) ? outputLength : sizeof(v1));
            }
            return;
        }
        case IOCTL_XUSB_GET_STATE: {
            UCHAR state[29];
            buildState(ctx, state, false);
            complete(request, state, sizeof(state));
            return;
        }
        case IOCTL_XUSB_WAIT_FOR_INPUT: {
            const NTSTATUS status = WdfRequestForwardToIoQueue(request, ctx->waitQueue);
            if (!NT_SUCCESS(status)) WdfRequestComplete(request, status);
            return;
        }
        case IOCTL_XUSB_SET_STATE:   // rumble and LEDs: nothing to do
        case IOCTL_XUSB_POWER_INFO:
            WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, 0);
            return;
        case IOCTL_XUSB_GET_LED_STATE: {
            const UCHAR led[3] = {0, 0, 0x06};
            complete(request, led, sizeof(led));
            return;
        }
        case IOCTL_XUSB_GET_BATTERY_INFO: {
            const UCHAR battery[4] = {0, 0, 0x01, 0x03};   // wired, full
            complete(request, battery, sizeof(battery));
            return;
        }
        case IOCTL_XUSB_WAIT_GUIDE:   // xinput1_4 polls GET_STATE instead
        default:
            WdfRequestComplete(request, STATUS_INVALID_DEVICE_REQUEST);
            return;
    }
}

// Every 8 ms (a wired Xbox 360 controller's polling rate): one parked WAIT_FOR_INPUT gets the current state.
VOID EvtTimer(WDFTIMER timer) {
    DeviceContext* ctx = GetDeviceContext(static_cast<WDFDEVICE>(WdfTimerGetParentObject(timer)));
    WDFREQUEST request;
    if (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(ctx->waitQueue, &request))) {
        UCHAR state[29];
        buildState(ctx, state, true);
        complete(request, state, sizeof(state));
    }
}

NTSTATUS EvtDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE) {
    DeviceContext* ctx = GetDeviceContext(device);
    if (ctx->link == nullptr || !ctx->link->start()) return STATUS_UNSUCCESSFUL;
    WdfTimerStart(ctx->timer, WDF_REL_TIMEOUT_IN_MS(8));
    return STATUS_SUCCESS;
}

NTSTATUS EvtDeviceD0Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE) {
    DeviceContext* ctx = GetDeviceContext(device);
    if (ctx->timer != nullptr) WdfTimerStop(ctx->timer, TRUE);   // waits for a running callback: it reads the Link
    if (ctx->link != nullptr) ctx->link->stop();
    return STATUS_SUCCESS;
}

VOID EvtDeviceCleanup(WDFOBJECT object) {
    DeviceContext* ctx = GetDeviceContext(static_cast<WDFDEVICE>(object));
    delete ctx->link;
    ctx->link = nullptr;
    logLine("virtual Xbox controller removed");
}

NTSTATUS EvtDeviceAdd(WDFDRIVER, PWDFDEVICE_INIT init) {
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
    ctx->link = nullptr;
    ctx->timer = nullptr;

    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = EvtIoDeviceControl;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, nullptr);
    if (!NT_SUCCESS(status)) {
        logLine("default queue: " + std::to_string(status));
        return status;
    }
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &ctx->waitQueue);
    if (!NT_SUCCESS(status)) {
        logLine("wait queue: " + std::to_string(status));
        return status;
    }

    WDF_TIMER_CONFIG timerConfig;
    WDF_TIMER_CONFIG_INIT_PERIODIC(&timerConfig, EvtTimer, 8);
    WDF_OBJECT_ATTRIBUTES timerAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
    timerAttributes.ParentObject = device;
    // UMDF runs timer callbacks at passive level, so WdfTimerStop(.., TRUE) in D0Exit may wait for one.
    status = WdfTimerCreate(&timerConfig, &timerAttributes, &ctx->timer);
    if (!NT_SUCCESS(status)) {
        logLine("timer: " + std::to_string(status));
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(device, &kXusbInterface, nullptr);
    if (!NT_SUCCESS(status)) {
        logLine("XUSB interface: " + std::to_string(status));
        return status;
    }
    ctx->link = new Link();
    logLine("virtual Xbox 360 controller added (XInput, VID 045E PID 028E)");
    return STATUS_SUCCESS;
}

}  // namespace

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
    WSADATA wsa;
    if (const int error = WSAStartup(MAKEWORD(2, 2), &wsa); error != 0) {
        logLine("ERROR: WSAStartup failed (" + std::to_string(error) + "): the driver cannot talk to wheelemu-service");
        return STATUS_UNSUCCESSFUL;
    }
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    const NTSTATUS status = WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    logLine(NT_SUCCESS(status) ? "wheelemu-xusb driver loaded" : "WdfDriverCreate failed: " + std::to_string(status));
    return status;
}
