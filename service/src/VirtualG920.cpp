#include "VirtualG920.hpp"

#include "Platform.hpp"

#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/Logging.hpp"

#ifdef _WIN32

#include <windows.h>
#include <cfgmgr32.h>
#include <cwctype>

#include <atomic>
#include <cstdio>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace wheelbri::service {

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

// --- Has Windows a driver for it? ---

// What Start says when Windows has no wheelbri-g920 driver (the app tries to install it first, see main.cpp).
constexpr char kDriverMissing[] =
    "the virtual G920's driver (wheelbri-g920) is not installed: install it once from an Administrator PowerShell "
    "(driver\\install-driver.ps1, or scripts/make/drivers.sh -Install); WheelBri's installer does it too";

// The problem code Windows shows for the device in Device Manager (0: none or not found).
constexpr ULONG kProblemFailedInstall = 28;   // CM_PROB_FAILED_INSTALL: "the drivers for this device are not installed"

ULONG problemOf(const std::wstring& instance) {
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, const_cast<DEVINSTID_W>(instance.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        return 0;
    }
    ULONG status = 0;
    ULONG problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, node, 0) != CR_SUCCESS) return 0;
    return (status & DN_HAS_PROBLEM) != 0 ? problem : 0;
}

// --- Is the game-controller part there? A HID interface with the G920's ids (cfgmgr32 + hid.dll) ---

struct HiddAttributes {
    ULONG Size;
    USHORT VendorID, ProductID, VersionNumber;
};

// "\\?\HID#VID_046D&PID_C262#7&1a2b&0&0000#{guid}" -> "HID\VID_046D&PID_C262\7&1a2b&0&0000", the device's instance id.
std::wstring instanceOfInterface(std::wstring_view path) {
    if (path.starts_with(L"\\\\?\\")) path.remove_prefix(4);
    if (const auto guid = path.rfind(L"#{"); guid != std::wstring_view::npos) path = path.substr(0, guid);
    std::wstring id(path);
    for (wchar_t& c : id) {
        c = c == L'#' ? L'\\' : static_cast<wchar_t>(towupper(c));   // interface paths are often lower case
    }
    return id;
}

// Whether the device `instance` is `ancestor` or below it (a few levels up at most: HID child, its parent, ...).
bool isBelow(const std::wstring& instance, const std::wstring& ancestor) {
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, const_cast<DEVINSTID_W>(instance.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        return false;
    }
    for (int level = 0; level < 4; ++level) {
        wchar_t id[MAX_DEVICE_ID_LEN + 1] = {};
        if (CM_Get_Device_IDW(node, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return false;
        if (_wcsicmp(id, ancestor.c_str()) == 0) return true;
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) return false;
        node = parent;
    }
    return false;
}

// A HID interface with these ids; with `ancestor` set, only one whose device sits below that device (ours, not a real
// G920 that happens to be plugged in).
bool hidDevicePresent(USHORT vid, USHORT pid, const std::wstring& ancestor = {}) {
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
        if (match && (ancestor.empty() || isBelow(instanceOfInterface(path), ancestor))) return true;
    }
    return false;
}

// --- DirectInput's registry entries for VID_046D&PID_C262 ---

constexpr wchar_t kOemKey[] =
    L"System\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\Joystick\\OEM\\VID_046D&PID_C262";
constexpr wchar_t kPidDriverClsid[] = L"{EEC6993A-B3FD-11D2-A916-00C04FB98638}";
constexpr wchar_t kOemName[] = L"Logitech G920 Driving Force Racing Wheel USB";

// The machine's entries, and the user's too when `userHive` (not in the helper: as SYSTEM, HKCU is SYSTEM's own; the
// app keeps the user's).
std::vector<HKEY> roots(const bool userHive) {
    return userHive ? std::vector<HKEY>{HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE} : std::vector<HKEY>{HKEY_LOCAL_MACHINE};
}

void writeDirectInputEntries(const bool on, const std::vector<HKEY>& hives) {
    for (HKEY root : hives) {
        HKEY key = nullptr;
        if (RegCreateKeyExW(root, kOemKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
            continue;
        }
        if (on) {
            // The name, and the device type the shim reports: JOY_HWS_ISCARCTRL | HASPOV, 19 buttons (the shifter's too).
            const BYTE data[8] = {0x42, 0, 0, 0, static_cast<BYTE>(kG920Buttons), 0, 0, 0};
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
void removeForeignForceFeedbackDriver(const std::vector<HKEY>& hives) {
    const std::wstring path = std::wstring(kOemKey) + L"\\OEMForceFeedback";
    for (HKEY root : hives) {
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
            // Logitech's software may write it back every few seconds (the watcher below): a note a minute at most.
            static logging::RateLimiter limiter(std::chrono::minutes(1));
            std::uint64_t suppressed = 0;
            if (limiter.allow(suppressed)) {
                WE_LOG_INFO(log(), "DirectInput: removed another force feedback driver ({}) from {}; Windows' HID PID "
                                   "driver takes over ({} more removals since the last note)",
                            narrow(clsid), root == HKEY_CURRENT_USER ? "HKCU" : "HKLM", suppressed);
            }
        }
    }
}

}  // namespace

struct VirtualG920::Impl {
    SwDeviceCloseFn close = nullptr;
    HSWDEVICE_ device = nullptr;
    std::string instanceId;
    bool userHive = true;
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
        writeDirectInputEntries(false, roots(userHive));
    }
};

// Windows keeps one key per driver package in its store: SYSTEM\DriverDatabase\DriverPackages\<inf>_<arch>_<hash>
// (what pnputil /add-driver adds and /delete-driver removes). Readable without Administrator.
VirtualG920::DriverState VirtualG920::driverState() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\DriverDatabase\\DriverPackages", 0,
                      KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return DriverState::Unknown;
    }
    constexpr wchar_t kPrefix[] = L"wheelbri-g920.inf_";
    DriverState state = DriverState::Missing;
    for (DWORD index = 0;; ++index) {
        wchar_t name[256];
        DWORD length = static_cast<DWORD>(std::size(name));
        const LONG result = RegEnumKeyExW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (result == ERROR_NO_MORE_ITEMS) break;
        if (result == ERROR_MORE_DATA) continue;   // a longer name: not ours
        if (result != ERROR_SUCCESS) {
            state = DriverState::Unknown;
            break;
        }
        if (_wcsnicmp(name, kPrefix, std::size(kPrefix) - 1) == 0) {
            state = DriverState::Installed;
            break;
        }
    }
    RegCloseKey(key);
    return state;
}

std::unique_ptr<VirtualG920> VirtualG920::create(std::string& error, const std::function<bool()>& cancelled,
                                                 const bool userHive) {
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
    // Without our driver every identity would wait 8 s for nothing and end in "restart Windows", which cannot help.
    if (driverState() == DriverState::Missing) {
        error = kDriverMissing;
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
        info.pszzHardwareIds = L"wheelbri\\G920\0";
        info.CapabilityFlags = kRemovable | kSilentInstall | kDriverRequired;
        info.pszDeviceDescription = L"Logitech G920 Driving Force Racing Wheel USB (wheelbri)";
        // Heap-allocated: if Windows never answers, its late callback must not touch a dead stack frame.
        auto* wait = new CreateWait;
        HSWDEVICE_ device = nullptr;
        const HRESULT hr = swCreate(L"wheelbri", L"HTREE\\ROOT\\0", &info, 0, nullptr, onCreated, wait, &device);
        if (FAILED(hr)) {
            delete wait;
            char text[160];
            std::snprintf(text, sizeof(text), "SwDeviceCreate failed (0x%08lX)%s", static_cast<unsigned long>(hr),
                          hr == E_ACCESSDENIED ? ": run the service as Administrator" : "");
            error = text;
            if (hr == E_ACCESSDENIED) {   // the same for every identity
                writeDirectInputEntries(false, roots(userHive));
                return nullptr;
            }
            // Often this identity is still being removed (Stop then Start at once): the next one is a new device.
            WE_LOG_WARN(log(), "virtual G920 {}: {}; trying the next identity", narrow(instance), text);
            continue;
        }
        bool finished = false;
        HRESULT result = E_PENDING;
        std::wstring createdId;
        bool cancelledWhileWaiting = false;
        {
            // Up to 15 s, in short steps so Stop is not kept waiting.
            std::unique_lock guard(wait->lock);
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            while (!wait->finished && std::chrono::steady_clock::now() < until) {
                guard.unlock();
                if (stopNow()) {
                    cancelledWhileWaiting = true;
                    guard.lock();
                    break;
                }
                guard.lock();
                wait->done.wait_for(guard, std::chrono::milliseconds(250), [wait] { return wait->finished; });
            }
            finished = wait->finished;
            result = wait->result;
            createdId = wait->instanceId;
        }
        if (finished) delete wait;   // else leaked on purpose (see above)
        if (cancelledWhileWaiting && !finished) {
            swClose(device);
            writeDirectInputEntries(false, roots(userHive));
            error = "cancelled";
            return nullptr;
        }
        if (!finished || FAILED(result)) {
            char text[200];
            std::snprintf(text, sizeof(text),
                          "the virtual G920 was not set up (0x%08lX): is the driver installed "
                          "(driver\\install-driver.ps1)?", static_cast<unsigned long>(result));
            swClose(device);
            writeDirectInputEntries(false, roots(userHive));   // an earlier identity may have written them
            error = text;
            return nullptr;
        }
        const std::string id = narrow(createdId);
        writeDirectInputEntries(true, roots(userHive));
        removeForeignForceFeedbackDriver(roots(userHive));

        const auto start = std::chrono::steady_clock::now();
        bool ready = false;
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(8) && !stopNow()) {
            if (hidDevicePresent(0x046D, 0xC262, createdId)) {
                ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (!ready && stopNow()) {
            swClose(device);
            writeDirectInputEntries(false, roots(userHive));
            error = "cancelled";
            return nullptr;
        }
        if (!ready) {
            const ULONG problem = problemOf(createdId);
            // No driver for it at all (removed since the check above, or Windows refused it): another identity gets
            // the same, so say what is missing instead.
            if (problem == kProblemFailedInstall && driverState() != DriverState::Installed) {
                swClose(device);
                writeDirectInputEntries(false, roots(userHive));
                error = std::string(kDriverMissing) + " (Windows found no driver for " + id + ")";
                return nullptr;
            }
            WE_LOG_WARN(log(), "virtual G920 {} has no game-controller driver{} (an earlier one is probably stuck "
                               "half-removed until Windows restarts); trying the next identity",
                        id, problem != 0 ? " (Device Manager problem code " + std::to_string(problem) + ")" : std::string());
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
        impl->userHive = userHive;
        Impl* raw = impl.get();
        impl->watcher = std::thread([raw] {
            platform::nameThisThread("wheelbri g920 watch");
            try {
                std::unique_lock guard(raw->lock);
                while (!raw->wake.wait_for(guard, std::chrono::seconds(2), [raw] { return raw->stop; })) {
                    guard.unlock();
                    removeForeignForceFeedbackDriver(roots(raw->userHive));   // Logitech's software writes it back when it sees a "G920"
                    guard.lock();
                }
            } catch (const std::exception& e) {   // never std::terminate the service from here
                WE_LOG_ERROR(log(), "virtual G920: the registry watcher stopped: {}", e.what());
            }
        });
        return std::unique_ptr<VirtualG920>(new VirtualG920(std::move(impl)));
    }
    writeDirectInputEntries(false, roots(userHive));   // written for an identity that then never got its driver
    error = "every identity of the virtual G920 is stuck: restart Windows" + (error.empty() ? "" : " (last: " + error + ")");
    return nullptr;
}

struct DirectInputUserEntries::Impl {
    std::thread watcher;
    std::mutex lock;
    std::condition_variable wake;
    bool stop = false;
};

DirectInputUserEntries::DirectInputUserEntries() : impl_(std::make_unique<Impl>()) {
    const std::vector<HKEY> user{HKEY_CURRENT_USER};
    writeDirectInputEntries(true, user);
    removeForeignForceFeedbackDriver(user);
    Impl* raw = impl_.get();
    impl_->watcher = std::thread([raw] {
        platform::nameThisThread("wheelbri dinput watch");
        try {
            std::unique_lock guard(raw->lock);
            while (!raw->wake.wait_for(guard, std::chrono::seconds(2), [raw] { return raw->stop; })) {
                guard.unlock();
                removeForeignForceFeedbackDriver({HKEY_CURRENT_USER});
                guard.lock();
            }
        } catch (const std::exception& e) {
            WE_LOG_ERROR(log(), "virtual G920: the user's registry watcher stopped: {}", e.what());
        }
    });
}

DirectInputUserEntries::~DirectInputUserEntries() {
    {
        std::lock_guard guard(impl_->lock);
        impl_->stop = true;
    }
    impl_->wake.notify_all();
    if (impl_->watcher.joinable()) impl_->watcher.join();
    writeDirectInputEntries(false, {HKEY_CURRENT_USER});
}

}  // namespace wheelbri::service

#else  // not Windows

namespace wheelbri::service {

struct VirtualG920::Impl {
    std::string instanceId;
};

VirtualG920::DriverState VirtualG920::driverState() {
    return DriverState::Unknown;
}

struct DirectInputUserEntries::Impl {};
DirectInputUserEntries::DirectInputUserEntries() = default;
DirectInputUserEntries::~DirectInputUserEntries() = default;

std::unique_ptr<VirtualG920> VirtualG920::create(std::string& error, const std::function<bool()>&, bool) {
    error = "the virtual G920 exists on Windows only";
    return nullptr;
}

}  // namespace wheelbri::service

#endif

namespace wheelbri::service {

VirtualG920::VirtualG920(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
VirtualG920::~VirtualG920() = default;
const std::string& VirtualG920::instanceId() const { return impl_->instanceId; }

}  // namespace wheelbri::service
