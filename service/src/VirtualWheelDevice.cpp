#include "VirtualWheelDevice.hpp"

#include "Platform.hpp"

#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/HidWheelDevice.hpp"
#include "wheelbri/Logging.hpp"
#include "wheelbri/LogitechFfbDriver.hpp"
#include "wheelbri/TrueForce.hpp"

#ifdef _WIN32

#include <windows.h>
#include <cfgmgr32.h>
#include <objbase.h>
#include <cwchar>
#include <cwctype>

#include <array>
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
    static const logging::Logger logger = logging::get("service.driver");
    return logger;
}

std::wstring widen(const std::string_view text) {
    return std::wstring(text.begin(), text.end());   // the identities are ASCII
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

// What Start says when Windows has no wheelbri-wheel driver (the app tries to install it first, see main.cpp).
constexpr char kDriverMissing[] =
    "the virtual wheel's driver (wheelbri-wheel) is not installed: install it once from an Administrator PowerShell "
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

// --- DirectInput's registry entries for the wheel's ids (VID_046D&PID_C262 for the G920, PID_C26E for the G923...) ---

std::wstring oemKey(const VirtualWheel& wheel) {
    wchar_t ids[32];
    std::swprintf(ids, std::size(ids), L"VID_%04X&PID_%04X", wheel.vendorId, wheel.productId);
    return std::wstring(L"System\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\Joystick\\OEM\\") +
           ids;
}
constexpr wchar_t kPidDriverClsid[] = L"{EEC6993A-B3FD-11D2-A916-00C04FB98638}";

// The machine's entries, and the user's too when `userHive` (not in the helper: as SYSTEM, HKCU is SYSTEM's own; the
// app keeps the user's).
std::vector<HKEY> roots(const bool userHive) {
    return userHive ? std::vector<HKEY>{HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE} : std::vector<HKEY>{HKEY_LOCAL_MACHINE};
}

// The Axes key WheelBri wrote for the G923 until round 93 (HidWheelDevice.hpp: no wheel has axis entries now): removed
// whenever the wheel's entries are written or cleared, but only when it is WheelBri's (its "WheelBri" value), so one
// Logitech's software made for a real wheel is left alone.
void removeOwnAxisEntries(HKEY root, const std::wstring& path, const VirtualWheel& wheel) {
    const std::wstring axesPath = path + L"\\Axes";
    HKEY axes = nullptr;
    if (RegOpenKeyExW(root, axesPath.c_str(), 0, KEY_READ, &axes) != ERROR_SUCCESS) return;
    const bool ours = RegQueryValueExW(axes, L"WheelBri", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(axes);
    if (!ours) return;
    HKEY oem = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_ALL_ACCESS, &oem) != ERROR_SUCCESS) return;
    const LSTATUS status = RegDeleteTreeW(oem, L"Axes");
    RegCloseKey(oem);
    const char* hive = root == HKEY_CURRENT_USER ? "HKCU" : "HKLM";
    if (status == ERROR_SUCCESS) {
        WE_LOG_INFO(log(), "DirectInput: removed WheelBri's old axis entries for the {} from {}", wheel.instancePrefix,
                    hive);
    } else {
        WE_LOG_WARN(log(), "DirectInput: cannot remove WheelBri's old axis entries for the {} from {} (error {}): "
                           "games may read its pedals on the wrong axes",
                    wheel.instancePrefix, hive, status);
    }
}

void writeDirectInputEntries(const bool on, const std::vector<HKEY>& hives, const VirtualWheel& wheel) {
    const std::wstring path = oemKey(wheel);
    const std::wstring name = widen(wheel.oemName);
    for (HKEY root : hives) {
        HKEY key = nullptr;
        if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
            continue;
        }
        if (on) {
            // The name, and its hat and buttons (a car controller but for the G25 and G27: directInputOemData).
            const std::array<std::uint8_t, 8> data = directInputOemData(wheel);
            RegSetValueExW(key, L"OEMName", 0, REG_SZ, reinterpret_cast<const BYTE*>(name.c_str()),
                           static_cast<DWORD>((name.size() + 1) * sizeof(wchar_t)));
            RegSetValueExW(key, L"OEMData", 0, REG_BINARY, data.data(), static_cast<DWORD>(data.size()));
        } else {
            RegDeleteValueW(key, L"OEMData");
        }
        RegCloseKey(key);
        removeOwnAxisEntries(root, path, wheel);
    }
}

// Logitech's software registers its own force feedback driver for every G920 (and G25); it only works with a real
// one (every effect fails with E_NOTIMPL). Without the entry DirectInput uses Windows' PID driver.
void removeForeignForceFeedbackDriver(const std::vector<HKEY>& hives, const VirtualWheel& wheel) {
    const std::wstring key = oemKey(wheel);
    const std::wstring path = key + L"\\OEMForceFeedback";
    for (HKEY root : hives) {
        wchar_t clsid[64] = {};
        DWORD size = sizeof(clsid);
        if (RegGetValueW(root, path.c_str(), L"CLSID", RRF_RT_REG_SZ, nullptr, clsid, &size) != ERROR_SUCCESS) {
            continue;
        }
        if (_wcsicmp(clsid, kPidDriverClsid) == 0) continue;
        HKEY oem = nullptr;
        if (RegOpenKeyExW(root, key.c_str(), 0, KEY_ALL_ACCESS, &oem) == ERROR_SUCCESS) {
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

// Whether Logitech's force feedback driver is installed: its class registered, and its DLL there.
bool logitechFfbDriverInstalled() {
    const std::wstring path = L"CLSID\\" + std::wstring(logitech_ffb::kClsid) + L"\\InProcServer32";
    wchar_t dll[MAX_PATH] = {};
    DWORD size = sizeof(dll);
    if (RegGetValueW(HKEY_CLASSES_ROOT, path.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, dll, &size) != ERROR_SUCCESS) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(dll);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool setBinary(HKEY key, const wchar_t* name, const std::uint8_t* data, const std::size_t size) {
    return RegSetValueExW(key, name, 0, REG_BINARY, data, static_cast<DWORD>(size)) == ERROR_SUCCESS;
}

bool setString(HKEY key, const wchar_t* name, const std::wstring& text) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(text.c_str()),
                          static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

// The OEMForceFeedback entry Logitech's software writes for the wheel (LogitechFfbDriver.hpp), in place of whatever
// was there, with a WheelBri value to say who wrote it. The CLSID goes last: until then DirectInput sees no driver.
bool writeLogitechFfbDriver(HKEY root, const std::wstring& key) {
    HKEY oem = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_ALL_ACCESS, &oem) == ERROR_SUCCESS) {
        RegDeleteTreeW(oem, L"OEMForceFeedback");
        RegCloseKey(oem);
    }
    HKEY ffb = nullptr;
    const std::wstring path = key + L"\\OEMForceFeedback";
    if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &ffb, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = true;
    for (const logitech_ffb::Effect& effect : logitech_ffb::kEffects) {
        HKEY entry = nullptr;
        const std::wstring name = L"Effects\\" + std::wstring(effect.guid);
        if (RegCreateKeyExW(ffb, name.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &entry, nullptr) !=
            ERROR_SUCCESS) {
            ok = false;
            continue;
        }
        const auto attributes = logitech_ffb::bytes(effect.attributes);
        ok = setString(entry, nullptr, std::wstring(effect.name)) && ok;
        ok = setBinary(entry, L"Attributes", attributes.data(), attributes.size()) && ok;
        RegCloseKey(entry);
    }
    const auto createdBy = logitech_ffb::bytes(std::array<std::uint32_t, 1>{logitech_ffb::kCreatedBy});
    const auto attributes = logitech_ffb::bytes(logitech_ffb::kAttributes);
    const DWORD marker = 1;
    ok = setBinary(ffb, L"CreatedBy", createdBy.data(), createdBy.size()) && ok;
    ok = setBinary(ffb, L"Attributes", attributes.data(), attributes.size()) && ok;
    ok = RegSetValueExW(ffb, L"WheelBri", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&marker), sizeof(marker)) ==
             ERROR_SUCCESS && ok;
    ok = ok && setString(ffb, L"CLSID", std::wstring(logitech_ffb::kClsid));
    RegCloseKey(ffb);
    return ok;
}

std::wstring registeredFfbDriver(HKEY root, const std::wstring& path) {
    wchar_t clsid[64] = {};
    DWORD size = sizeof(clsid);
    return RegGetValueW(root, path.c_str(), L"CLSID", RRF_RT_REG_SZ, nullptr, clsid, &size) == ERROR_SUCCESS ? clsid
                                                                                                             : L"";
}

// With [emulator] trueforce Logitech's driver is the one: a TrueForce game's force reaches a G923 only through it
// (DirectInput -> hidpp_forcefeedback_x64.dll -> HID++ 0x8123, played by the driver's HidPpFfb), and ACC sets the
// range through its Escape. Written when it is missing or another driver is there (a run with it off removed it), as
// long as Logitech's driver is installed; says when that changes. Without it the entry goes, as before.
void handleForeignForceFeedbackDriver(const std::vector<HKEY>& hives, const VirtualWheel& wheel, const bool keep) {
    if (!keep) {
        removeForeignForceFeedbackDriver(hives, wheel);
        return;
    }
    const std::wstring key = oemKey(wheel);
    const std::wstring path = key + L"\\OEMForceFeedback";
    const bool installed = logitechFfbDriverInstalled();
    std::wstring found;
    for (HKEY root : hives) {
        const char* hive = root == HKEY_CURRENT_USER ? "HKCU" : "HKLM";
        if (logitech_ffb::action(registeredFfbDriver(root, path), installed) == logitech_ffb::Action::Write) {
            const bool written = writeLogitechFfbDriver(root, key);
            static logging::RateLimiter limiter(std::chrono::minutes(1));
            std::uint64_t suppressed = 0;
            if (limiter.allow(suppressed)) {
                if (written) {
                    WE_LOG_INFO(log(), "DirectInput: wrote Logitech's force feedback driver for the virtual {} in {} "
                                       "({} more writes since the last note)", wheel.instancePrefix, hive, suppressed);
                } else {
                    WE_LOG_WARN(log(), "DirectInput: cannot write Logitech's force feedback driver for the virtual {} "
                                       "in {} ({} more tries since the last note)", wheel.instancePrefix, hive,
                                suppressed);
                }
            }
        }
        if (const std::wstring clsid = registeredFfbDriver(root, path);
            logitech_ffb::action(clsid, installed) == logitech_ffb::Action::Keep) {
            found = clsid;
        }
    }
    static std::atomic<int> said{-1};
    const int state = !found.empty() ? 1 : installed ? 0 : 2;
    if (said.exchange(state) == state) return;
    if (state == 1) {
        WE_LOG_INFO(log(), "DirectInput: Logitech's force feedback driver ({}) drives the virtual {} (trueforce "
                           "on); its effects reach WheelBri over HID++ 0x8123", narrow(found), wheel.instancePrefix);
    } else if (state == 2) {
        WE_LOG_WARN(log(), "DirectInput: Logitech's force feedback driver is not installed (G HUB installs it): the "
                           "virtual {} gets Windows' HID PID driver, and TrueForce games no force",
                    wheel.instancePrefix);
    } else {
        WE_LOG_WARN(log(), "DirectInput: Logitech's force feedback driver could not be registered for the virtual {}; "
                           "Windows' HID PID driver takes the effects", wheel.instancePrefix);
    }
}

// The wheel's TrueForce interface (TrueForce.hpp): its own software device in the wheel's container, since the
// TrueForce SDK pairs a wheel's interfaces by container. Spare identities as for the wheel. Null when it could not be
// made; the wheel does not depend on it.
HSWDEVICE_ createTrueForce(SwDeviceCreateFn swCreate, SwDeviceCloseFn swClose, const VirtualWheel& wheel,
                           const GUID& container, std::string& instanceId) {
    std::wstring hardwareIds = widen(trueForceHardwareId(wheel));
    hardwareIds.push_back(L'\0');
    const std::wstring description = widen(wheel.oemName) + L" TrueForce (wheelbri)";
    const std::wstring name = widen(wheel.instancePrefix) + widen(kTrueForceIdSuffix);
    for (const std::wstring& instance : {name + L"_0", name + L"_1", name + L"_2", name + L"_3"}) {
        SwDeviceCreateInfo info{};
        info.cbSize = sizeof(info);
        info.pszInstanceId = instance.c_str();
        info.pszzHardwareIds = hardwareIds.c_str();
        info.pContainerId = &container;
        info.CapabilityFlags = kRemovable | kSilentInstall | kDriverRequired;
        info.pszDeviceDescription = description.c_str();
        auto* wait = new CreateWait;   // leaked if Windows never answers, as for the wheel
        HSWDEVICE_ device = nullptr;
        const HRESULT hr = swCreate(L"wheelbri", L"HTREE\\ROOT\\0", &info, 0, nullptr, onCreated, wait, &device);
        if (FAILED(hr)) {
            delete wait;
            WE_LOG_WARN(log(), "trueforce: SwDeviceCreate {} failed (0x{:08X})", narrow(instance),
                        static_cast<unsigned long>(hr));
            continue;
        }
        bool finished = false;
        HRESULT result = E_PENDING;
        {
            std::unique_lock guard(wait->lock);
            finished = wait->done.wait_for(guard, std::chrono::seconds(15), [wait] { return wait->finished; });
            result = wait->result;
            instanceId = narrow(wait->instanceId);
        }
        if (finished) delete wait;
        if (finished && SUCCEEDED(result)) return device;
        WE_LOG_WARN(log(), "trueforce: {} was not set up (0x{:08X}): is the driver new enough?", narrow(instance),
                    static_cast<unsigned long>(result));
        swClose(device);
    }
    return nullptr;
}

}  // namespace

struct VirtualWheelDevice::Impl {
    const VirtualWheel* wheel = nullptr;
    SwDeviceCloseFn close = nullptr;
    HSWDEVICE_ device = nullptr;
    std::string instanceId;
    bool userHive = true;
    bool keepLogitechFfb = false;   // [emulator] trueforce: Logitech's force feedback driver stays
    HSWDEVICE_ trueForceDevice = nullptr;   // its TrueForce interface, when asked for and made
    std::string trueForceId;
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
        if (trueForceDevice != nullptr && close != nullptr) {
            close(trueForceDevice);
            WE_LOG_INFO(log(), "virtual {} TrueForce {} removed", wheel->instancePrefix, trueForceId);
        }
        if (device != nullptr && close != nullptr) {
            close(device);
            WE_LOG_INFO(log(), "virtual {} {} removed", wheel->instancePrefix, instanceId);
        }
        writeDirectInputEntries(false, roots(userHive), *wheel);
    }
};

// Windows keeps one key per driver package in its store: SYSTEM\DriverDatabase\DriverPackages\<inf>_<arch>_<hash>
// (what pnputil /add-driver adds and /delete-driver removes). Readable without Administrator.
VirtualWheelDevice::DriverState VirtualWheelDevice::driverState() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\DriverDatabase\\DriverPackages", 0,
                      KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return DriverState::Unknown;
    }
    // Only wheelbri-wheel: the same driver under its old name (wheelbri-g920, until 2026-10-19) counts as missing, so
    // Start installs the new one, which removes the old (install-driver.ps1).
    constexpr wchar_t kPrefix[] = L"wheelbri-wheel.inf_";
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

std::unique_ptr<VirtualWheelDevice> VirtualWheelDevice::create(const VirtualWheel& wheel, std::string& error,
                                                 const std::function<bool()>& cancelled, const bool userHive,
                                                 const bool trueForce) {
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
    std::wstring hardwareIds = widen(wheel.hardwareId);
    hardwareIds.push_back(L'\0');   // a list: the string's own end ends it
    const std::wstring description = widen(wheel.oemName) + L" (wheelbri)";
    const std::wstring wheelName = widen(wheel.instancePrefix);   // "G920", "G25"
    const std::string label = "virtual " + std::string(wheel.instancePrefix);
    // The wheel and its TrueForce interface share a container (one physical device to Windows and the SDK).
    GUID container{};
    const bool withTrueForce = trueForce && wheel.trueForce && SUCCEEDED(CoCreateGuid(&container));
    const bool keepLogitechFfb = trueForce && wheel.trueForce;
    for (const std::wstring& instance : {wheelName + L"_0", wheelName + L"_1", wheelName + L"_2", wheelName + L"_3"}) {
        if (stopNow()) {
            error = "cancelled";
            return nullptr;
        }
        SwDeviceCreateInfo info{};
        info.cbSize = sizeof(info);
        info.pszInstanceId = instance.c_str();
        info.pszzHardwareIds = hardwareIds.c_str();
        info.CapabilityFlags = kRemovable | kSilentInstall | kDriverRequired;
        if (withTrueForce) info.pContainerId = &container;
        info.pszDeviceDescription = description.c_str();
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
                writeDirectInputEntries(false, roots(userHive), wheel);
                return nullptr;
            }
            // Often this identity is still being removed (Stop then Start at once): the next one is a new device.
            WE_LOG_WARN(log(), "{} {}: {}; trying the next identity", label, narrow(instance), text);
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
            writeDirectInputEntries(false, roots(userHive), wheel);
            error = "cancelled";
            return nullptr;
        }
        if (!finished || FAILED(result)) {
            char text[200];
            std::snprintf(text, sizeof(text),
                          "the %s was not set up (0x%08lX): is the driver installed "
                          "(driver\\install-driver.ps1)?", label.c_str(), static_cast<unsigned long>(result));
            swClose(device);
            writeDirectInputEntries(false, roots(userHive), wheel);   // an earlier identity may have written them
            error = text;
            return nullptr;
        }
        const std::string id = narrow(createdId);
        writeDirectInputEntries(true, roots(userHive), wheel);
        handleForeignForceFeedbackDriver(roots(userHive), wheel, keepLogitechFfb);

        const auto start = std::chrono::steady_clock::now();
        bool ready = false;
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(8) && !stopNow()) {
            if (hidDevicePresent(wheel.vendorId, wheel.productId, createdId)) {
                ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (!ready && stopNow()) {
            swClose(device);
            writeDirectInputEntries(false, roots(userHive), wheel);
            error = "cancelled";
            return nullptr;
        }
        if (!ready) {
            const ULONG problem = problemOf(createdId);
            // No driver for it at all (removed since the check above, or Windows refused it): another identity gets
            // the same, so say what is missing instead.
            if (problem == kProblemFailedInstall && driverState() != DriverState::Installed) {
                swClose(device);
                writeDirectInputEntries(false, roots(userHive), wheel);
                error = std::string(kDriverMissing) + " (Windows found no driver for " + id + ")";
                return nullptr;
            }
            // The driver is there but has no entry for this wheel: an older build (from before that wheel).
            if (problem == kProblemFailedInstall && wheel.profile != "g920") {
                swClose(device);
                writeDirectInputEntries(false, roots(userHive), wheel);
                error = "the installed wheelbri-wheel driver is older than the " + label +
                        ": install the new one (scripts/make/drivers.sh -Only wheel -Install, or WheelBri's installer)";
                return nullptr;
            }
            WE_LOG_WARN(log(), "{} {} has no game-controller driver{} (an earlier one is probably stuck "
                               "half-removed until Windows restarts); trying the next identity",
                        label, id, problem != 0 ? " (Device Manager problem code " + std::to_string(problem) + ")" : std::string());
            swClose(device);
            continue;
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        WE_LOG_INFO(log(), "{} {} ready after {} ms (joy.cpl and games can see it)", label, id, elapsed);

        auto impl = std::make_unique<Impl>();
        impl->wheel = &wheel;
        impl->close = swClose;
        impl->device = device;
        impl->instanceId = id;
        impl->userHive = userHive;
        impl->keepLogitechFfb = keepLogitechFfb;
        if (withTrueForce) {
            impl->trueForceDevice = createTrueForce(swCreate, swClose, wheel, container, impl->trueForceId);
            if (impl->trueForceDevice != nullptr) {
                WE_LOG_INFO(log(), "{} TrueForce interface {} created", label, impl->trueForceId);
            } else {
                WE_LOG_WARN(log(), "{}: no TrueForce interface, the wheel works without it", label);
            }
        }
        Impl* raw = impl.get();
        impl->watcher = std::thread([raw] {
            platform::nameThisThread("wheelbri g920 watch");
            try {
                std::unique_lock guard(raw->lock);
                while (!raw->wake.wait_for(guard, std::chrono::seconds(2), [raw] { return raw->stop; })) {
                    guard.unlock();
                    // Logitech's software writes it back when it sees a "G920" (or a "G25")
                    handleForeignForceFeedbackDriver(roots(raw->userHive), *raw->wheel, raw->keepLogitechFfb);
                    guard.lock();
                }
            } catch (const std::exception& e) {   // never std::terminate the service from here
                WE_LOG_ERROR(log(), "virtual {}: the registry watcher stopped: {}", raw->wheel->instancePrefix, e.what());
            }
        });
        return std::unique_ptr<VirtualWheelDevice>(new VirtualWheelDevice(std::move(impl)));
    }
    writeDirectInputEntries(false, roots(userHive), wheel);   // written for an identity that then never got its driver
    error = "every identity of the " + label + " is stuck: restart Windows" +
            (error.empty() ? "" : " (last: " + error + ")");
    return nullptr;
}

struct DirectInputUserEntries::Impl {
    const VirtualWheel* wheel = nullptr;
    bool keepLogitechFfb = false;
    std::thread watcher;
    std::mutex lock;
    std::condition_variable wake;
    bool stop = false;
};

DirectInputUserEntries::DirectInputUserEntries(const VirtualWheel& wheel, const bool keepLogitechFfb)
    : impl_(std::make_unique<Impl>()) {
    impl_->wheel = &wheel;
    impl_->keepLogitechFfb = keepLogitechFfb;
    const std::vector<HKEY> user{HKEY_CURRENT_USER};
    writeDirectInputEntries(true, user, wheel);
    handleForeignForceFeedbackDriver(user, wheel, keepLogitechFfb);
    Impl* raw = impl_.get();
    impl_->watcher = std::thread([raw] {
        platform::nameThisThread("wheelbri dinput watch");
        try {
            std::unique_lock guard(raw->lock);
            while (!raw->wake.wait_for(guard, std::chrono::seconds(2), [raw] { return raw->stop; })) {
                guard.unlock();
                handleForeignForceFeedbackDriver({HKEY_CURRENT_USER}, *raw->wheel, raw->keepLogitechFfb);
                guard.lock();
            }
        } catch (const std::exception& e) {
            WE_LOG_ERROR(log(), "virtual {}: the user's registry watcher stopped: {}", raw->wheel->instancePrefix,
                         e.what());
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
    writeDirectInputEntries(false, {HKEY_CURRENT_USER}, *impl_->wheel);
}

}  // namespace wheelbri::service

#else  // not Windows

namespace wheelbri::service {

struct VirtualWheelDevice::Impl {
    std::string instanceId;
};

VirtualWheelDevice::DriverState VirtualWheelDevice::driverState() {
    return DriverState::Unknown;
}

struct DirectInputUserEntries::Impl {};
DirectInputUserEntries::DirectInputUserEntries(const VirtualWheel&, bool) {}
DirectInputUserEntries::~DirectInputUserEntries() = default;

std::unique_ptr<VirtualWheelDevice> VirtualWheelDevice::create(const VirtualWheel&, std::string& error,
                                                 const std::function<bool()>&, bool, bool) {
    error = "the virtual wheel exists on Windows only";
    return nullptr;
}

}  // namespace wheelbri::service

#endif

namespace wheelbri::service {

VirtualWheelDevice::VirtualWheelDevice(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
VirtualWheelDevice::~VirtualWheelDevice() = default;
const std::string& VirtualWheelDevice::instanceId() const { return impl_->instanceId; }

}  // namespace wheelbri::service
