#include "VirtualG920.hpp"

#include "wheelemu/Logging.hpp"

#ifdef _WIN32

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace wheelemu::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.g920");
    return logger;
}

std::string narrow(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size, nullptr, nullptr);
    return text;
}

// --- Software devices (swdevice.h, cfgmgr32.dll), loaded at run time: not every MinGW has the header ---

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

// --- Is the game-controller part there? A HID interface with the G920's ids (cfgmgr32 + hid.dll) ---

struct HiddAttributes {
    ULONG Size;
    USHORT VendorID, ProductID, VersionNumber;
};

bool hidDevicePresent(USHORT vid, USHORT pid) {
    static const HMODULE hid = LoadLibraryW(L"hid.dll");
    static const HMODULE cfg = LoadLibraryW(L"cfgmgr32.dll");
    if (hid == nullptr || cfg == nullptr) return false;
    using GetGuidFn = void(WINAPI*)(GUID*);
    using GetAttributesFn = BOOLEAN(WINAPI*)(HANDLE, HiddAttributes*);
    using ListSizeFn = DWORD(WINAPI*)(PULONG, const GUID*, PCWSTR, ULONG);
    using ListFn = DWORD(WINAPI*)(const GUID*, PCWSTR, PWSTR, ULONG, ULONG);
    const auto getGuid = reinterpret_cast<GetGuidFn>(reinterpret_cast<void*>(GetProcAddress(hid, "HidD_GetHidGuid")));
    const auto getAttributes =
        reinterpret_cast<GetAttributesFn>(reinterpret_cast<void*>(GetProcAddress(hid, "HidD_GetAttributes")));
    const auto listSize =
        reinterpret_cast<ListSizeFn>(reinterpret_cast<void*>(GetProcAddress(cfg, "CM_Get_Device_Interface_List_SizeW")));
    const auto list = reinterpret_cast<ListFn>(reinterpret_cast<void*>(GetProcAddress(cfg, "CM_Get_Device_Interface_ListW")));
    if (getGuid == nullptr || getAttributes == nullptr || listSize == nullptr || list == nullptr) return false;
    GUID guid;
    getGuid(&guid);
    ULONG length = 0;
    if (listSize(&length, &guid, nullptr, 0) != 0 || length <= 1) return false;
    std::vector<wchar_t> buffer(length);
    if (list(&guid, nullptr, buffer.data(), length, 0) != 0) return false;
    for (const wchar_t* path = buffer.data(); *path != L'\0'; path += wcslen(path) + 1) {
        HANDLE device = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (device == INVALID_HANDLE_VALUE) continue;
        HiddAttributes attributes{};
        attributes.Size = sizeof(attributes);
        const bool match = getAttributes(device, &attributes) && attributes.VendorID == vid && attributes.ProductID == pid;
        CloseHandle(device);
        if (match) return true;
    }
    return false;
}

// --- DirectInput's registry entries for VID_046D&PID_C262 ---

constexpr wchar_t kOemKey[] =
    L"System\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\Joystick\\OEM\\VID_046D&PID_C262";
constexpr wchar_t kPidDriverClsid[] = L"{EEC6993A-B3FD-11D2-A916-00C04FB98638}";
constexpr wchar_t kOemName[] = L"Logitech G920 Driving Force Racing Wheel USB";

void writeDirectInputEntries(bool on) {
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        HKEY key = nullptr;
        if (RegCreateKeyExW(root, kOemKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
            continue;
        }
        if (on) {
            // The name, and the device type the shim reports: JOY_HWS_ISCARCTRL | HASPOV, 11 buttons.
            const BYTE data[8] = {0x42, 0, 0, 0, 11, 0, 0, 0};
            RegSetValueExW(key, L"OEMName", 0, REG_SZ, reinterpret_cast<const BYTE*>(kOemName), sizeof(kOemName));
            RegSetValueExW(key, L"OEMData", 0, REG_BINARY, data, sizeof(data));
        } else {
            RegDeleteValueW(key, L"OEMData");
        }
        RegCloseKey(key);
    }
}

// Logitech's software registers its own force feedback driver for every G920; it only works with a real
// one (every effect fails with E_NOTIMPL). Without the entry DirectInput uses Windows' PID driver.
void removeForeignForceFeedbackDriver() {
    const std::wstring path = std::wstring(kOemKey) + L"\\OEMForceFeedback";
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        wchar_t clsid[64] = {};
        DWORD size = sizeof(clsid);
        if (RegGetValueW(root, path.c_str(), L"CLSID", RRF_RT_REG_SZ, nullptr, clsid, &size) != ERROR_SUCCESS) {
            continue;
        }
        if (_wcsicmp(clsid, kPidDriverClsid) == 0) continue;
        HKEY oem = nullptr;
        if (RegOpenKeyExW(root, kOemKey, 0, KEY_ALL_ACCESS, &oem) == ERROR_SUCCESS) {
            RegDeleteTreeW(oem, L"OEMForceFeedback");
            RegCloseKey(oem);
            WE_LOG_INFO(log(), "DirectInput: removed another force feedback driver ({}) from {}; Windows' HID PID driver "
                               "takes over", narrow(clsid), root == HKEY_CURRENT_USER ? "HKCU" : "HKLM");
        }
    }
}

}  // namespace

struct VirtualG920::Impl {
    SwDeviceCloseFn close = nullptr;
    HSWDEVICE_ device = nullptr;
    std::string instanceId;
    std::thread watcher;
    std::mutex lock;
    std::condition_variable wake;
    bool stop = false;

    ~Impl() {
        {
            std::lock_guard guard(lock);
            stop = true;
        }
        wake.notify_all();
        if (watcher.joinable()) watcher.join();
        if (device != nullptr && close != nullptr) {
            close(device);
            WE_LOG_INFO(log(), "virtual G920 {} removed", instanceId);
        }
        writeDirectInputEntries(false);
    }
};

std::unique_ptr<VirtualG920> VirtualG920::create(std::string& error, const std::function<bool()>& cancelled) {
    const auto stopNow = [&cancelled] { return cancelled && cancelled(); };
    const HMODULE cfg = LoadLibraryW(L"cfgmgr32.dll");
    const auto swCreate =
        cfg != nullptr ? reinterpret_cast<SwDeviceCreateFn>(reinterpret_cast<void*>(GetProcAddress(cfg, "SwDeviceCreate"))) : nullptr;
    const auto swClose =
        cfg != nullptr ? reinterpret_cast<SwDeviceCloseFn>(reinterpret_cast<void*>(GetProcAddress(cfg, "SwDeviceClose"))) : nullptr;
    if (swCreate == nullptr || swClose == nullptr) {
        error = "this Windows has no software devices (SwDeviceCreate)";
        return nullptr;
    }

    // Spare identities: a new device to Windows when an earlier one is stuck half-removed.
    for (const wchar_t* instance : {L"G920_0", L"G920_1", L"G920_2", L"G920_3"}) {
        if (stopNow()) {
            error = "cancelled";
            return nullptr;
        }
        SwDeviceCreateInfo info{};
        info.cbSize = sizeof(info);
        info.pszInstanceId = instance;
        info.pszzHardwareIds = L"wheelemu\\G920\0";
        info.CapabilityFlags = kRemovable | kSilentInstall | kDriverRequired;
        info.pszDeviceDescription = L"Logitech G920 Driving Force Racing Wheel USB (wheelemu)";
        // Heap-allocated: if Windows never answers, its late callback must not touch a dead stack frame.
        auto* wait = new CreateWait;
        HSWDEVICE_ device = nullptr;
        const HRESULT hr = swCreate(L"wheelemu", L"HTREE\\ROOT\\0", &info, 0, nullptr, onCreated, wait, &device);
        if (FAILED(hr)) {
            delete wait;
            char text[160];
            std::snprintf(text, sizeof(text), "SwDeviceCreate failed (0x%08lX)%s", static_cast<unsigned long>(hr),
                          hr == E_ACCESSDENIED ? ": run the service as Administrator" : "");
            error = text;
            if (hr == E_ACCESSDENIED) return nullptr;   // the same for every identity
            // Often this identity is still being removed (Stop then Start at once): the next one is a new device.
            WE_LOG_WARN(log(), "virtual G920 {}: {}; trying the next identity", narrow(instance), text);
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
                          "the virtual G920 was not set up (0x%08lX): is the driver installed "
                          "(driver\\install-driver.ps1)?", static_cast<unsigned long>(result));
            swClose(device);
            error = text;
            return nullptr;
        }
        const std::string id = narrow(createdId);
        writeDirectInputEntries(true);
        removeForeignForceFeedbackDriver();

        const auto start = std::chrono::steady_clock::now();
        bool ready = false;
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(8) && !stopNow()) {
            if (hidDevicePresent(0x046D, 0xC262)) {
                ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (!ready && stopNow()) {
            swClose(device);
            writeDirectInputEntries(false);
            error = "cancelled";
            return nullptr;
        }
        if (!ready) {
            WE_LOG_WARN(log(), "virtual G920 {} has no game-controller driver (an earlier one is probably stuck "
                               "half-removed until Windows restarts); trying the next identity", id);
            swClose(device);
            continue;
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        WE_LOG_INFO(log(), "virtual G920 {} ready after {} ms (joy.cpl and games can see it)", id, elapsed);

        auto impl = std::make_unique<Impl>();
        impl->close = swClose;
        impl->device = device;
        impl->instanceId = id;
        Impl* raw = impl.get();
        impl->watcher = std::thread([raw] {
            try {
                std::unique_lock guard(raw->lock);
                while (!raw->wake.wait_for(guard, std::chrono::seconds(2), [raw] { return raw->stop; })) {
                    guard.unlock();
                    removeForeignForceFeedbackDriver();   // Logitech's software writes it back when it sees a "G920"
                    guard.lock();
                }
            } catch (const std::exception& e) {   // never std::terminate the service from here
                WE_LOG_ERROR(log(), "virtual G920: the registry watcher stopped: {}", e.what());
            }
        });
        return std::unique_ptr<VirtualG920>(new VirtualG920(std::move(impl)));
    }
    error = "every identity of the virtual G920 is stuck: restart Windows" + (error.empty() ? "" : " (last: " + error + ")");
    return nullptr;
}

}  // namespace wheelemu::service

#else  // not Windows

namespace wheelemu::service {

struct VirtualG920::Impl {
    std::string instanceId;
};

std::unique_ptr<VirtualG920> VirtualG920::create(std::string& error, const std::function<bool()>&) {
    error = "the virtual G920 exists on Windows only";
    return nullptr;
}

}  // namespace wheelemu::service

#endif

namespace wheelemu::service {

VirtualG920::VirtualG920(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
VirtualG920::~VirtualG920() = default;
const std::string& VirtualG920::instanceId() const { return impl_->instanceId; }

}  // namespace wheelemu::service
