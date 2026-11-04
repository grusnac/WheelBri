#include "VirtualWheelDevice.hpp"

#include "Platform.hpp"

#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/HidWheelDevice.hpp"
#include "wheelbri/Logging.hpp"
#include "wheelbri/LogitechFfbDriver.hpp"
#include "wheelbri/TrueForce.hpp"

#ifdef _WIN32
#include "WinText.hpp"

#include <windows.h>
#include <cfgmgr32.h>
#include <objbase.h>
#include <shlobj.h>   // SHGetKnownFolderPath: Program Files
#include <cwchar>
#include <cwctype>

#include <array>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <condition_variable>
#include <mutex>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace wheelbri::service {

namespace {

using wintext::widen;
using wintext::narrow;

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.driver");
    return logger;
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
    static const HMODULE hid = LoadLibraryExW(L"hid.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static const HMODULE cfg = LoadLibraryExW(L"cfgmgr32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
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
// What RegDeleteTreeW needs on the parent of the subtree it deletes. Never KEY_ALL_ACCESS: that asks for WRITE_DAC and
// WRITE_OWNER too, which a user may not have on a key Logitech's software made, and the open then fails.
constexpr REGSAM kDeleteSubtreeAccess = DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE;

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
    if (RegOpenKeyExW(root, path.c_str(), 0, kDeleteSubtreeAccess, &oem) != ERROR_SUCCESS) return;
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

void removeClassicEntry(HKEY root, const VirtualWheel& wheel);   // below

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
        if (!on && wheel.classic) removeClassicEntry(root, wheel);   // WheelBri's own, written while it ran
        if (!(on && wheel.classic)) removeOwnAxisEntries(root, path, wheel);   // a classic wheel's are its own
    }
}

// The CLSID an OEMForceFeedback entry names (empty: none), and whether it is WheelBri's old copy.
struct FfbEntry {
    std::wstring clsid;
    bool wheelBris = false;
};

FfbEntry readFfbEntry(HKEY root, const std::wstring& path) {
    FfbEntry entry;
    wchar_t clsid[64] = {};
    DWORD size = sizeof(clsid);
    if (RegGetValueW(root, path.c_str(), L"CLSID", RRF_RT_REG_SZ, nullptr, clsid, &size) == ERROR_SUCCESS) {
        entry.clsid = clsid;
    }
    entry.wheelBris =
        RegGetValueW(root, path.c_str(), L"WheelBri", RRF_RT_REG_DWORD, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    return entry;
}

// --- The G25's and G27's classic identities (LogitechFfbDriver.hpp): WheelBri's own entry, in HKCU ------------------

bool setBinary(HKEY key, const wchar_t* name, const std::uint8_t* data, const std::size_t size) {
    return RegSetValueExW(key, name, 0, REG_BINARY, data, static_cast<DWORD>(size)) == ERROR_SUCCESS;
}

bool setString(HKEY key, const wchar_t* name, const std::wstring_view text) {
    const std::wstring value(text);
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool setMark(HKEY key) {
    const DWORD marker = 1;
    return RegSetValueExW(key, L"WheelBri", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&marker), sizeof(marker)) ==
           ERROR_SUCCESS;
}

bool hasMark(HKEY root, const std::wstring& path) {
    return RegGetValueW(root, path.c_str(), L"WheelBri", RRF_RT_REG_DWORD, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

// Whether WheelBri's classic entry is there as it writes it: the class, its mark, and the wheel axis' marked entry.
bool classicEntryInPlace(HKEY root, const VirtualWheel& wheel) {
    const std::wstring key = oemKey(wheel);
    const std::wstring path = key + L"\\OEMForceFeedback";
    const FfbEntry entry = readFfbEntry(root, path);
    return entry.wheelBris && logitech_ffb::sameClsid(entry.clsid, logitech_ffb::classicClsid(wheel.productId)) &&
           hasMark(root, key + L"\\Axes");
}

// Writes WheelBri's classic entry for the wheel in `root` (in place of whatever was there), and the wheel axis' entry.
// The CLSID goes last: until then DirectInput sees no driver.
bool writeClassicEntry(HKEY root, const VirtualWheel& wheel) {
    const std::wstring key = oemKey(wheel);
    HKEY oem = nullptr;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, kDeleteSubtreeAccess | KEY_CREATE_SUB_KEY, nullptr, &oem,
                        nullptr) != ERROR_SUCCESS) {
        return false;
    }
    RegDeleteTreeW(oem, L"OEMForceFeedback");
    RegDeleteTreeW(oem, L"Axes");
    bool ok = true;
    HKEY axis = nullptr;
    if (RegCreateKeyExW(oem, L"Axes\\0", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &axis, nullptr) == ERROR_SUCCESS) {
        ok = setString(axis, nullptr, logitech_ffb::kWheelAxisName) && ok;
        ok = setBinary(axis, L"Attributes", logitech_ffb::kWheelAxisAttributes.data(),
                       logitech_ffb::kWheelAxisAttributes.size()) && ok;
        ok = setBinary(axis, L"FFAttributes", logitech_ffb::kWheelAxisFfAttributes.data(),
                       logitech_ffb::kWheelAxisFfAttributes.size()) && ok;
        RegCloseKey(axis);
    } else {
        ok = false;
    }
    HKEY axes = nullptr;
    if (RegOpenKeyExW(oem, L"Axes", 0, KEY_SET_VALUE, &axes) == ERROR_SUCCESS) {
        ok = setMark(axes) && ok;   // removeOwnAxisEntries takes it away again
        RegCloseKey(axes);
    }
    HKEY ffb = nullptr;
    if (RegCreateKeyExW(oem, L"OEMForceFeedback", 0, nullptr, 0, KEY_SET_VALUE | KEY_CREATE_SUB_KEY, nullptr, &ffb,
                        nullptr) != ERROR_SUCCESS) {
        RegCloseKey(oem);
        return false;
    }
    for (const logitech_ffb::ClassicEffect& effect : logitech_ffb::kClassicEffects) {
        HKEY e = nullptr;
        const std::wstring name = L"Effects\\" + std::wstring(effect.guid);
        if (RegCreateKeyExW(ffb, name.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &e, nullptr) != ERROR_SUCCESS) {
            ok = false;
            continue;
        }
        const auto attributes = logitech_ffb::bytes(effect.attributes);
        ok = setString(e, nullptr, effect.name) && ok;
        ok = setBinary(e, L"Attributes", attributes.data(), attributes.size()) && ok;
        RegCloseKey(e);
    }
    const auto createdBy = logitech_ffb::bytes(std::array<std::uint32_t, 1>{logitech_ffb::kClassicCreatedBy});
    const auto attributes = logitech_ffb::bytes(logitech_ffb::kClassicAttributes);
    ok = setBinary(ffb, L"CreatedBy", createdBy.data(), createdBy.size()) && ok;
    ok = setBinary(ffb, L"Attributes", attributes.data(), attributes.size()) && ok;
    ok = setMark(ffb) && ok;
    ok = ok && setString(ffb, L"CLSID", logitech_ffb::classicClsid(wheel.productId));
    RegCloseKey(ffb);
    RegCloseKey(oem);
    return ok;
}

// WheelBri's classic entry for the wheel goes (Stop): only when it is WheelBri's (its mark).
void removeClassicEntry(HKEY root, const VirtualWheel& wheel) {
    const std::wstring key = oemKey(wheel);
    if (!readFfbEntry(root, key + L"\\OEMForceFeedback").wheelBris) return;
    HKEY oem = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, kDeleteSubtreeAccess, &oem) != ERROR_SUCCESS) return;
    const LSTATUS status = RegDeleteTreeW(oem, L"OEMForceFeedback");
    RegCloseKey(oem);
    if (status == ERROR_SUCCESS) {
        WE_LOG_INFO(log(), "DirectInput: removed WheelBri's Logitech driver entry for the classic {}", wheel.instancePrefix);
    }
}

// While a classic wheel runs: WheelBri's entry in HKCU (DirectInput reads it first), written again whenever it is
// missing or another one is there. HKLM stays as it is.
void handleClassicEntry(const std::vector<HKEY>& hives, const VirtualWheel& wheel) {
    for (HKEY root : hives) {
        if (root != HKEY_CURRENT_USER || classicEntryInPlace(root, wheel)) continue;
        const bool written = writeClassicEntry(root, wheel);
        static logging::RateLimiter limiter(std::chrono::minutes(1));
        std::uint64_t suppressed = 0;
        if (!limiter.allow(suppressed)) continue;
        if (written) {
            WE_LOG_INFO(log(), "DirectInput: wrote Logitech's older-wheel driver entry ({}) and the wheel axis' for the "
                               "classic {} in HKCU ({} more writes since the last note)",
                        narrow(std::wstring(logitech_ffb::classicClsid(wheel.productId)).c_str()), wheel.instancePrefix,
                        suppressed);
        } else {
            WE_LOG_WARN(log(), "DirectInput: cannot write Logitech's older-wheel driver entry for the classic {} in HKCU: "
                               "games get no force feedback on it ({} more tries since the last note)",
                        wheel.instancePrefix, suppressed);
        }
    }
}

// The virtual wheel's OEMForceFeedback entry, in each of `hives` (LogitechFfbDriver.hpp). For the G920 and G923
// Logitech's entry stays when its software wrote one (in either hive: both are read, though the WheelBri Helper
// handles HKLM and the app HKCU), and Windows' own HID PID entry goes while it is there, since DirectInput takes
// HKCU's first. For the G25 and G27 another program's entry goes. WheelBri's own old copy always goes. Logitech's
// software writes its entry back every few seconds while it runs (hence the watchers below): a note a minute at
// most per hive for removals, one line per hive whenever what stays changes.
void handleForeignForceFeedbackDriver(const std::vector<HKEY>& hives, const VirtualWheel& wheel) {
    if (wheel.classic) {
        handleClassicEntry(hives, wheel);
        return;
    }
    const std::wstring key = oemKey(wheel);
    const std::wstring path = key + L"\\OEMForceFeedback";
    const bool keepOthers = logitech_ffb::keepsOtherDrivers(wheel.hidpp || wheel.classic);
    bool otherInstalled = false;
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        const FfbEntry entry = readFfbEntry(root, path);
        otherInstalled = otherInstalled || logitech_ffb::isOtherDriver(entry.clsid, entry.wheelBris);
    }
    for (HKEY root : hives) {
        const bool user = root == HKEY_CURRENT_USER;
        const char* hive = user ? "HKCU" : "HKLM";
        const FfbEntry entry = readFfbEntry(root, path);
        const std::string name = !entry.clsid.empty() ? narrow(entry.clsid.c_str()) : std::string("no CLSID");
        if (logitech_ffb::action(entry.clsid, entry.wheelBris, keepOthers, otherInstalled) ==
            logitech_ffb::Action::Leave) {
            if (!keepOthers) continue;
            // What DirectInput finds for the virtual wheel in this hive, when it changes.
            static std::mutex lock;
            static std::wstring said[2] = {L"?", L"?"};
            const std::lock_guard guard(lock);
            if (said[user ? 0 : 1] == entry.clsid) continue;
            said[user ? 0 : 1] = entry.clsid;
            if (entry.clsid.empty()) {
                WE_LOG_INFO(log(), "DirectInput: no force feedback driver entry for the virtual {} in {}",
                            wheel.instancePrefix, hive);
            } else if (logitech_ffb::sameClsid(entry.clsid, logitech_ffb::kPidClsid)) {
                WE_LOG_INFO(log(), "DirectInput: the virtual {} has Windows' HID PID driver in {}",
                            wheel.instancePrefix, hive);
            } else {
                WE_LOG_INFO(log(), "DirectInput: the virtual {} has the force feedback driver {}{} in {}: it stays",
                            wheel.instancePrefix, name,
                            logitech_ffb::sameClsid(entry.clsid, logitech_ffb::kGHubClsid) ? " (G HUB's)" : "", hive);
            }
            continue;
        }
        HKEY oem = nullptr;
        LSTATUS status = RegOpenKeyExW(root, key.c_str(), 0, kDeleteSubtreeAccess, &oem);
        if (status == ERROR_SUCCESS) {
            status = RegDeleteTreeW(oem, L"OEMForceFeedback");
            RegCloseKey(oem);
        }
        const char* what = entry.wheelBris ? "WheelBri's old copy of a force feedback driver entry"
                           : logitech_ffb::sameClsid(entry.clsid, logitech_ffb::kPidClsid)
                               ? "Windows' HID PID entry, which would hide Logitech's driver"
                               : "another force feedback driver";
        static logging::RateLimiter limiters[2] = {logging::RateLimiter(std::chrono::minutes(1)),
                                                   logging::RateLimiter(std::chrono::minutes(1))};
        std::uint64_t suppressed = 0;
        if (!limiters[user ? 0 : 1].allow(suppressed)) continue;
        if (status == ERROR_SUCCESS) {
            WE_LOG_INFO(log(), "DirectInput: removed {} ({}) from {} for the virtual {} ({} more removals since the "
                               "last note)",
                        what, name, hive, wheel.instancePrefix, suppressed);
        } else {
            WE_LOG_WARN(log(), "DirectInput: cannot remove {} ({}) from {} (error {}): games' effects on the virtual "
                               "{} may not reach WheelBri ({} more tries since the last note)",
                        what, name, hive, status, wheel.instancePrefix, suppressed);
        }
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
    // Loaded once for the process (it was loaded again at every Start, and never freed), from System32 only.
    static const HMODULE cfg = LoadLibraryExW(L"cfgmgr32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
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
        handleForeignForceFeedbackDriver(roots(userHive), wheel);

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
                    // Logitech's software writes its entry back when it sees a "G920" (or a "G25")
                    handleForeignForceFeedbackDriver(roots(raw->userHive), *raw->wheel);
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
    std::thread watcher;
    std::mutex lock;
    std::condition_variable wake;
    bool stop = false;
};

DirectInputUserEntries::DirectInputUserEntries(const VirtualWheel& wheel)
    : impl_(std::make_unique<Impl>()) {
    impl_->wheel = &wheel;
    const std::vector<HKEY> user{HKEY_CURRENT_USER};
    writeDirectInputEntries(true, user, wheel);
    handleForeignForceFeedbackDriver(user, wheel);
    Impl* raw = impl_.get();
    impl_->watcher = std::thread([raw] {
        platform::nameThisThread("wheelbri dinput watch");
        try {
            std::unique_lock guard(raw->lock);
            while (!raw->wake.wait_for(guard, std::chrono::seconds(2), [raw] { return raw->stop; })) {
                guard.unlock();
                handleForeignForceFeedbackDriver({HKEY_CURRENT_USER}, *raw->wheel);
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

bool classicFfbDriverInstalled(const VirtualWheel& wheel) {
    const std::wstring_view clsid = logitech_ffb::classicClsid(wheel.productId);
    if (clsid.empty()) return false;
    const std::wstring path = L"CLSID\\" + std::wstring(clsid) + L"\\InProcServer32";
    wchar_t dll[MAX_PATH] = {};
    DWORD size = sizeof(dll);
    if (RegGetValueW(HKEY_CLASSES_ROOT, path.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, dll, &size) != ERROR_SUCCESS) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(dll);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string trueForceSdkPath() {
    PWSTR folder = nullptr;
    std::filesystem::path programFiles;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &folder)) && folder != nullptr) {
        programFiles = folder;
    }
    CoTaskMemFree(folder);
    if (programFiles.empty()) return {};
    // G HUB keeps one folder per SDK version (1_3_12): the highest one that has the DLL.
    std::error_code ec;
    std::filesystem::path found;
    for (const auto& entry : std::filesystem::directory_iterator(programFiles / L"Logi" / L"trueforce", ec)) {
        const std::filesystem::path dll = entry.path() / L"trueforce_sdk_x64.dll";
        std::error_code fileEc;
        if (std::filesystem::is_regular_file(dll, fileEc) && (found.empty() || found < dll)) found = dll;
    }
    return found.empty() ? std::string() : narrow(found.wstring());
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
DirectInputUserEntries::DirectInputUserEntries(const VirtualWheel&) {}
DirectInputUserEntries::~DirectInputUserEntries() = default;

std::unique_ptr<VirtualWheelDevice> VirtualWheelDevice::create(const VirtualWheel&, std::string& error,
                                                 const std::function<bool()>&, bool, bool) {
    error = "the virtual wheel exists on Windows only";
    return nullptr;
}

std::string trueForceSdkPath() { return {}; }

bool classicFfbDriverInstalled(const VirtualWheel&) { return false; }

}  // namespace wheelbri::service

#endif

namespace wheelbri::service {

VirtualWheelDevice::VirtualWheelDevice(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
VirtualWheelDevice::~VirtualWheelDevice() = default;
const std::string& VirtualWheelDevice::instanceId() const { return impl_->instanceId; }

}  // namespace wheelbri::service
