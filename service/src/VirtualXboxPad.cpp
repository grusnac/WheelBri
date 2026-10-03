#include "VirtualXboxPad.hpp"

#include "wheelbri/Logging.hpp"

#ifdef _WIN32

#include <windows.h>
#include <cfgmgr32.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.xbox");
    return logger;
}

std::string narrow(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size, nullptr, nullptr);
    return text;
}

// Software devices (swdevice.h), loaded at run time as in VirtualG920.cpp: not every MinGW has the header.
DECLARE_HANDLE(HSWDEVICE_);

struct SwDeviceCreateInfo {
    ULONG cbSize;
    PCWSTR pszInstanceId;
    PCWSTR pszzHardwareIds;
    PCWSTR pszzCompatibleIds;
    const GUID* pContainerId;
    ULONG CapabilityFlags;
    PCWSTR pszDeviceDescription;
    PCWSTR pszDeviceLocation;
    const void* pSecurityDescriptor;
};

using SwCallback = VOID(WINAPI*)(HSWDEVICE_ device, HRESULT result, PVOID context, PCWSTR instanceId);
using SwDeviceCreateFn = HRESULT(WINAPI*)(PCWSTR enumerator, PCWSTR parent, const SwDeviceCreateInfo* info,
                                          ULONG propertyCount, const void* properties, SwCallback callback,
                                          PVOID context, HSWDEVICE_* device);
using SwDeviceCloseFn = VOID(WINAPI*)(HSWDEVICE_ device);

constexpr ULONG kRemovable = 0x1, kSilentInstall = 0x2, kDriverRequired = 0x8;

struct CreateWait {
    std::mutex lock;
    std::condition_variable done;
    bool finished = false;
    HRESULT result = E_PENDING;
    std::wstring instanceId;
};

VOID WINAPI onCreated(HSWDEVICE_, HRESULT result, PVOID context, PCWSTR instanceId) {
    auto* wait = static_cast<CreateWait*>(context);
    std::lock_guard guard(wait->lock);
    wait->result = result;
    wait->instanceId = instanceId != nullptr ? instanceId : L"";
    wait->finished = true;
    wait->done.notify_all();
}

const GUID kXusbInterface = {0xEC87F1E3, 0xC13B, 0x4100, {0xB5, 0xF7, 0x8B, 0x84, 0xD5, 0x42, 0x60, 0xCB}};

// Is an XUSB interface of `instanceId` there (XInput can find the controller)?
bool xusbInterfacePresent(const std::wstring& instanceId) {
    ULONG length = 0;
    if (CM_Get_Device_Interface_List_SizeW(&length, const_cast<GUID*>(&kXusbInterface),
                                           const_cast<wchar_t*>(instanceId.c_str()),
                                           CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
        length <= 1) {
        return false;
    }
    std::vector<wchar_t> buffer(length);
    if (CM_Get_Device_Interface_ListW(const_cast<GUID*>(&kXusbInterface), const_cast<wchar_t*>(instanceId.c_str()),
                                      buffer.data(), length, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
        return false;
    }
    return buffer[0] != L'\0';
}

// Is the device `instanceId` started (its driver loaded and running)?
bool deviceStarted(const std::wstring& instanceId) {
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(instanceId.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        return false;
    }
    ULONG status = 0;
    ULONG problem = 0;
    return CM_Get_DevNode_Status(&status, &problem, node, 0) == CR_SUCCESS && (status & DN_STARTED) != 0 &&
           problem == 0;
}

// Creates one software device; its instance id, or empty (and closes it) if it failed or did not start in time.
std::wstring createDevice(const SwDeviceCreateFn swCreate, const SwDeviceCloseFn swClose, const wchar_t* instance,
                          const wchar_t* hardwareIds, const wchar_t* description, bool (*ready)(const std::wstring&),
                          HSWDEVICE_& device, HRESULT& result, const std::function<bool()>& stopNow) {
    SwDeviceCreateInfo info{};
    info.cbSize = sizeof(info);
    info.pszInstanceId = instance;
    info.pszzHardwareIds = hardwareIds;
    info.CapabilityFlags = kRemovable | kSilentInstall | kDriverRequired;
    info.pszDeviceDescription = description;
    auto* wait = new CreateWait;   // heap: a late callback must not touch a dead stack frame
    device = nullptr;
    result = swCreate(L"wheelbri", L"HTREE\\ROOT\\0", &info, 0, nullptr, onCreated, wait, &device);
    if (FAILED(result)) {
        delete wait;
        return {};
    }
    bool finished = false;
    std::wstring createdId;
    {
        std::unique_lock guard(wait->lock);
        wait->done.wait_for(guard, std::chrono::seconds(15), [wait] { return wait->finished; });
        finished = wait->finished;
        result = finished ? wait->result : E_PENDING;
        createdId = wait->instanceId;
    }
    if (finished) delete wait;   // else leaked on purpose (see above)
    if (!finished || FAILED(result)) {
        swClose(device);
        device = nullptr;
        return {};
    }
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(8) && !stopNow()) {
        if (ready(createdId)) return createdId;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    swClose(device);
    device = nullptr;
    return {};
}

}  // namespace

struct VirtualXboxPad::Impl {
    SwDeviceCloseFn close = nullptr;
    HSWDEVICE_ device = nullptr;      // the XInput side (driver/xusb)
    HSWDEVICE_ hidDevice = nullptr;   // the HID side (wheelbri-g920 driver): what joy.cpl lists
    std::string instanceId;
    std::string hidInstanceId;

    ~Impl() {
        if (hidDevice != nullptr && close != nullptr) {
            close(hidDevice);
            WE_LOG_INFO(log(), "virtual Xbox controller (HID) {} removed", hidInstanceId);
        }
        if (device != nullptr && close != nullptr) {
            close(device);
            WE_LOG_INFO(log(), "virtual Xbox controller {} removed", instanceId);
        }
    }
};

namespace {

// The HID side next to the XInput one, as a real wired Xbox 360 controller has: joy.cpl and DirectInput list it as
// "Controller (XBOX 360 For Windows)". Served by the wheelbri-g920 driver (hardware ID wheelbri\XBOXHID). Without
// it the controller still works for XInput (games' Xbox prompts); only joy.cpl does not list it.
void addHidSide(HSWDEVICE_& hidDevice, std::string& hidInstanceId, const SwDeviceCreateFn swCreate,
                const SwDeviceCloseFn swClose, const std::function<bool()>& stopNow) {
    for (const wchar_t* instance : {L"XBOXHID_0", L"XBOXHID_1", L"XBOXHID_2", L"XBOXHID_3"}) {
        if (stopNow()) return;
        HSWDEVICE_ device = nullptr;
        HRESULT result = S_OK;
        const auto start = std::chrono::steady_clock::now();
        const std::wstring id = createDevice(swCreate, swClose, instance, L"wheelbri\\XBOXHID\0",
                                             L"Controller (XBOX 360 For Windows) (wheelbri)", deviceStarted, device, result,
                                             stopNow);
        if (!id.empty()) {
            hidDevice = device;
            hidInstanceId = narrow(id);
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            WE_LOG_INFO(log(), "virtual Xbox controller (HID) {} ready after {} ms (joy.cpl lists it)",
                        hidInstanceId, elapsed);
            return;
        }
        if (stopNow()) return;
        if (result == E_ACCESSDENIED) {
            WE_LOG_WARN(log(), "virtual Xbox controller (HID) not set up (access denied: run as Administrator)");
            return;
        }
        if (FAILED(result) && result != E_PENDING) {
            // Not set up at all: the G920 driver (which serves it) is probably older than this program.
            WE_LOG_WARN(log(), "virtual Xbox controller (HID) {} not set up (0x{:08X}): joy.cpl will not list it; "
                               "reinstall the G920 driver (driver\\install-driver.ps1), it serves this too",
                        narrow(instance), static_cast<unsigned long>(result));
            return;
        }
        WE_LOG_WARN(log(), "virtual Xbox controller (HID) {} did not start; trying the next identity", narrow(instance));
    }
    WE_LOG_WARN(log(), "virtual Xbox controller (HID): no identity started: joy.cpl will not list it (reinstall the "
                       "G920 driver, driver\\install-driver.ps1, which serves it)");
}

}  // namespace

std::unique_ptr<VirtualXboxPad> VirtualXboxPad::create(std::string& error, const std::function<bool()>& cancelled) {
    const std::function<bool()> stopNow = [&cancelled] { return cancelled && cancelled(); };
    const HMODULE cfg = LoadLibraryW(L"cfgmgr32.dll");
    const auto swCreate =
        cfg != nullptr ? reinterpret_cast<SwDeviceCreateFn>(reinterpret_cast<void*>(GetProcAddress(cfg, "SwDeviceCreate"))) : nullptr;
    const auto swClose =
        cfg != nullptr ? reinterpret_cast<SwDeviceCloseFn>(reinterpret_cast<void*>(GetProcAddress(cfg, "SwDeviceClose"))) : nullptr;
    if (swCreate == nullptr || swClose == nullptr) {
        error = "this Windows has no software devices (SwDeviceCreate)";
        return nullptr;
    }
    // Spare identities, as for the G920: a new device to Windows when an earlier one is stuck half-removed.
    for (const wchar_t* instance : {L"XUSB_0", L"XUSB_1", L"XUSB_2", L"XUSB_3"}) {
        if (stopNow()) {
            error = "cancelled";
            return nullptr;
        }
        SwDeviceCreateInfo info{};
        info.cbSize = sizeof(info);
        info.pszInstanceId = instance;
        info.pszzHardwareIds = L"wheelbri\\XUSB\0";
        info.CapabilityFlags = kRemovable | kSilentInstall | kDriverRequired;
        info.pszDeviceDescription = L"Xbox 360 Controller (wheelbri)";
        auto* wait = new CreateWait;   // heap: a late callback must not touch a dead stack frame
        HSWDEVICE_ device = nullptr;
        const HRESULT hr = swCreate(L"wheelbri", L"HTREE\\ROOT\\0", &info, 0, nullptr, onCreated, wait, &device);
        if (FAILED(hr)) {
            delete wait;
            char text[160];
            std::snprintf(text, sizeof(text), "SwDeviceCreate failed (0x%08lX)%s", static_cast<unsigned long>(hr),
                          hr == E_ACCESSDENIED ? ": run the service as Administrator" : "");
            error = text;
            if (hr == E_ACCESSDENIED) return nullptr;
            WE_LOG_WARN(log(), "virtual Xbox controller {}: {}; trying the next identity", narrow(instance), text);
            continue;
        }
        bool finished = false;
        HRESULT result = E_PENDING;
        std::wstring createdId;
        {
            std::unique_lock guard(wait->lock);
            wait->done.wait_for(guard, std::chrono::seconds(15), [wait] { return wait->finished; });
            finished = wait->finished;
            result = wait->result;
            createdId = wait->instanceId;
        }
        if (finished) delete wait;   // else leaked on purpose (see above)
        if (!finished || FAILED(result)) {
            char text[200];
            std::snprintf(text, sizeof(text),
                          "the virtual Xbox controller was not set up (0x%08lX): is its driver installed "
                          "(driver\\xusb\\install-xusb.ps1)?", static_cast<unsigned long>(result));
            swClose(device);
            error = text;
            return nullptr;
        }
        const auto start = std::chrono::steady_clock::now();
        bool ready = false;
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(8) && !stopNow()) {
            if (xusbInterfacePresent(createdId)) {
                ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const std::string id = narrow(createdId);
        if (!ready && stopNow()) {
            swClose(device);
            error = "cancelled";
            return nullptr;
        }
        if (!ready) {
            WE_LOG_WARN(log(), "virtual Xbox controller {} has no driver (stuck half-removed until Windows restarts, "
                               "or driver\\xusb\\install-xusb.ps1 not run); trying the next identity", id);
            swClose(device);
            continue;
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        WE_LOG_INFO(log(), "virtual Xbox 360 controller {} ready after {} ms (XInput)", id, elapsed);
        auto impl = std::make_unique<Impl>();
        impl->close = swClose;
        impl->device = device;
        impl->instanceId = id;
        addHidSide(impl->hidDevice, impl->hidInstanceId, swCreate, swClose, stopNow);
        return std::unique_ptr<VirtualXboxPad>(new VirtualXboxPad(std::move(impl)));
    }
    error = std::string("every identity of the virtual Xbox controller is stuck, or its driver is not installed "
                        "(driver\\xusb\\install-xusb.ps1)") +
            (error.empty() ? "" : " (last: " + error + ")");
    return nullptr;
}

bool VirtualXboxPad::hidSide() const { return impl_->hidDevice != nullptr; }

}  // namespace wheelbri::service

#else  // not Windows

namespace wheelbri::service {

struct VirtualXboxPad::Impl {
    std::string instanceId;
};

std::unique_ptr<VirtualXboxPad> VirtualXboxPad::create(std::string& error, const std::function<bool()>&) {
    error = "the virtual Xbox controller exists on Windows only";
    return nullptr;
}

bool VirtualXboxPad::hidSide() const { return false; }

}  // namespace wheelbri::service

#endif

namespace wheelbri::service {

VirtualXboxPad::VirtualXboxPad(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
VirtualXboxPad::~VirtualXboxPad() = default;
const std::string& VirtualXboxPad::instanceId() const { return impl_->instanceId; }

}  // namespace wheelbri::service
