#include "HideFilter.hpp"

#include "Privileged.hpp"

#ifdef _WIN32
#include "WinText.hpp"
#include "DeviceHolders.hpp"
#include "DinputDevice.hpp"
#endif

#include "wheelbri/HideIds.hpp"
#include "wheelbri/Logging.hpp"

#ifdef _WIN32

#include <windows.h>
#include <aclapi.h>
#include <cfgmgr32.h>
#include <winioctl.h>   // CTL_CODE: the USB hub request in cycleUsbPort

#include <algorithm>
#include <array>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

#ifndef DN_NEED_RESTART
#define DN_NEED_RESTART 0x00000100
#endif

namespace wheelbri::service {

namespace {

using wintext::narrow;

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.hide");
    return logger;
}

// Strings of a REG_MULTI_SZ buffer.
std::vector<std::string> multiSz(const wchar_t* data, std::size_t chars) {
    std::vector<std::string> items;
    std::size_t start = 0;
    for (std::size_t i = 0; i < chars; ++i) {
        if (data[i] == L'\0') {
            if (i == start) break;
            items.push_back(narrow(std::wstring_view(data + start, i - start)));
            start = i + 1;
        }
    }
    return items;
}

// Device properties (the same Device Manager and Get-PnpDeviceProperty show), defined here rather than through
// <devpkey.h> and INITGUID.
constexpr DEVPROPKEY kUpperFilters{{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 19};   // DEVPKEY_Device_UpperFilters (20 is LowerFilters)
constexpr DEVPROPKEY kCompatibleIds{{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 4};   // DEVPKEY_Device_CompatibleIds
constexpr DEVPROPKEY kDriverDesc{{0xa8b865dd, 0x2e3d, 0x4094, {0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6}}, 4};

// A device's string or string-list property; empty if it has none.
std::vector<std::string> devNodeStrings(const DEVINST node, const DEVPROPKEY& key) {
    wchar_t buffer[2048] = {};
    ULONG bytes = sizeof(buffer) - 2 * sizeof(wchar_t);   // room for the list's closing zeros
    DEVPROPTYPE type = 0;
    if (CM_Get_DevNode_PropertyW(node, &key, &type, reinterpret_cast<PBYTE>(buffer), &bytes, 0) != CR_SUCCESS) return {};
    if (type != DEVPROP_TYPE_STRING && type != DEVPROP_TYPE_STRING_LIST) return {};
    return multiSz(buffer, bytes / sizeof(wchar_t) + 1);
}

std::string joined(const std::vector<std::string>& items) {
    std::string text;
    for (const std::string& item : items) text += (text.empty() ? "" : ", ") + item;
    return text.empty() ? "none" : text;
}

bool listsIgnoreCase(const std::vector<std::string>& items, std::string_view wanted) {
    for (const std::string& item : items) {
        if (hideids::equalsIgnoreCase(item, wanted)) return true;
    }
    return false;
}

// Ours: our INF's driver description ("... (wheelbri: hidden from games ...)").
bool ourDriver(const DEVINST node) {
    const std::vector<std::string> description = devNodeStrings(node, kDriverDesc);
    return !description.empty() && hideids::containsIgnoreCase(description.front(), "wheelbri");
}

bool locate(const std::string& instanceId, DEVINST& node) {
    const std::wstring wide(instanceId.begin(), instanceId.end());   // device instance ids are ASCII
    return CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(wide.c_str()), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS;
}

constexpr std::string_view kReflector = "WUDFRd";   // what every user-mode filter shows as in the upper filters

}  // namespace

const char* HideFilter::stateName(const State state) {
    switch (state) {
        case State::Ready: return "hidden while WheelBri runs";
        case State::NotInstalled: return "no filter yet";
        case State::NeedsRestart: return "filter not started yet";
        case State::NotConnected: return "not connected";
    }
    return "?";
}

namespace {

enum class NodeState { Gone, Waiting, Running };

// Where an interface stands now, from its device node's status only (nothing opens the device).
NodeState nodeState(const std::string& instanceId) {
    const std::wstring wide(instanceId.begin(), instanceId.end());   // device instance ids are ASCII
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(wide.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        return NodeState::Gone;   // not present: being restarted
    }
    ULONG status = 0;
    ULONG problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, node, 0) != CR_SUCCESS) return NodeState::Gone;
    if ((status & DN_NEED_RESTART) != 0 || (status & DN_HAS_PROBLEM) != 0) return NodeState::Waiting;
    return (status & DN_STARTED) != 0 ? NodeState::Running : NodeState::Gone;
}

std::string vetoText(const PNP_VETO_TYPE type, const std::wstring& name) {
    const std::string who = name.empty() ? std::string() : narrow(name);
    switch (type) {
        case PNP_VetoOutstandingOpen: return "a program has it open" + (who.empty() ? "" : " (" + who + ")");
        case PNP_VetoWindowsApp: return "the program " + (who.empty() ? std::string("(unnamed)") : who) + " refused";
        case PNP_VetoWindowsService: return "the service " + (who.empty() ? std::string("(unnamed)") : who) + " refused";
        case PNP_VetoInsufficientRights: return "not allowed to restart it";
        case PNP_VetoDriver: return "the driver " + who + " refused";
        case PNP_VetoDevice: return "the device " + who + " refused";
        case PNP_VetoPendingClose: return "a program is still closing it";
        default: return "Windows refused (veto type " + std::to_string(static_cast<int>(type)) + ")" +
                        (who.empty() ? "" : " (" + who + ")");
    }
}

// A restart Windows refused because a program has a device open.
struct HeldOpen {
    std::string asked;   // the interface this asked Windows to restart
    std::string node;    // the device Windows says is open (its veto name; may be empty)
};

// Restarts the interface now: removed (programs that registered for it are asked to let go), then set up again with
// its new stack. False with `why` when something refused; `heldOpen` (when given) is set when that was a program
// with the device open (added to it).
bool restartNode(const std::string& instanceId, std::string& why, std::vector<HeldOpen>* const heldOpen = nullptr) {
    const std::wstring wide(instanceId.begin(), instanceId.end());
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(wide.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        return true;   // gone already: Windows is restarting it
    }
    PNP_VETO_TYPE veto = PNP_VetoTypeUnknown;
    std::wstring vetoName(MAX_PATH, L'\0');
    const CONFIGRET removed = CM_Query_And_Remove_SubTreeW(node, &veto, vetoName.data(), MAX_PATH, CM_REMOVE_NO_RESTART);
    vetoName.resize(std::wcslen(vetoName.c_str()));
    if (removed != CR_SUCCESS) {
        why = removed == CR_REMOVE_VETOED ? vetoText(veto, vetoName)
                                          : "Windows could not restart it (CM_Query_And_Remove_SubTree " +
                                                std::to_string(removed) + ")";
        if (heldOpen != nullptr && removed == CR_REMOVE_VETOED &&
            (veto == PNP_VetoOutstandingOpen || veto == PNP_VetoPendingClose)) {
            heldOpen->push_back({instanceId, narrow(vetoName)});
        }
        return false;
    }
    // Back, with the filter: the removed node is a phantom until it is set up again.
    if (CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(wide.c_str()), CM_LOCATE_DEVNODE_PHANTOM) == CR_SUCCESS) {
        CM_Setup_DevNode(node, CM_SETUP_DEVNODE_READY);
    }
    return true;
}

// After Windows kept refusing to restart a device because a program has it open: which programs (DeviceHolders.hpp),
// added to `why` ("...; it is open in firefox.exe (pid 14196)") and logged, so the user knows what to close. Only on
// giving up: looking through the computer's handles takes up to a few seconds.
void nameHolders(const std::vector<HeldOpen>& heldOpen, std::string& why) {
    std::vector<holders::Holder> found;
    for (const HeldOpen& held : heldOpen) {
        const std::string& device =
            !held.node.empty() && nodeState(held.node) != NodeState::Gone ? held.node : held.asked;
        const std::vector<holders::Holder> holding = deviceHolders(device);
        if (!holding.empty()) {
            WE_LOG_WARN(log(), "hide: {} is open in {}: Windows can restart it only once that lets go", device,
                        holders::describe(holding));
        }
        found.insert(found.end(), holding.begin(), holding.end());
    }
    if (const std::string who = holders::describe(found); !who.empty()) why += "; it is open in " + who;
}

// The interfaces of `heldOpen`, for the caller (each once).
std::vector<std::string> askedOf(const std::vector<HeldOpen>& heldOpen) {
    std::vector<std::string> ids;
    for (const HeldOpen& held : heldOpen) {
        if (std::find(ids.begin(), ids.end(), held.asked) == ids.end()) ids.push_back(held.asked);
    }
    return ids;
}

// How long a program that has the device open gets to let go, once Windows asked it to (each restart asks), before a
// caller that can take the device back (heldOpen given: Start) is told so instead of waiting out its whole timeout.
constexpr int kHeldGraceMs = 3'000;

std::string deviceIdOf(const DEVINST node) {
    std::array<wchar_t, MAX_DEVICE_ID_LEN + 1> id{};
    if (CM_Get_Device_IDW(node, id.data(), static_cast<ULONG>(id.size()), 0) != CR_SUCCESS) return {};
    return narrow(id.data());
}

// The USB device behind interface `instanceId` (HID collection -> USB interface -> USB device): the nearest node above
// it whose id starts with "USB\" and that is no interface of a composite device ("&MI_"). False if there is none.
bool usbDeviceOf(const std::string& instanceId, DEVINST& usbDevice, std::string& usbId) {
    const std::wstring wide(instanceId.begin(), instanceId.end());
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(wide.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        return false;
    }
    for (int depth = 0; depth < 6; ++depth) {
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) return false;
        node = parent;
        std::string id = deviceIdOf(node);
        for (char& c : id) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
        if (id.rfind("USB\\", 0) == 0 && id.find("&MI_") == std::string::npos) {
            usbDevice = node;
            usbId = id;
            return true;
        }
    }
    return false;
}

// Power-cycles the hub port `usbDevice` is plugged into (IOCTL_USB_HUB_CYCLE_PORT, what USB Device Viewer's "cycle
// port" sends): to Windows the device is unplugged and plugged in again, which no program can refuse, so every handle
// to it and its interfaces ends. Empty on success, else why not.
std::string cycleUsbPort(const DEVINST usbDevice) {
    ULONG port = 0;
    ULONG size = sizeof(port);
    if (CM_Get_DevNode_Registry_PropertyW(usbDevice, CM_DRP_ADDRESS, nullptr, &port, &size, 0) != CR_SUCCESS ||
        port == 0) {
        return "Windows does not say which port of its USB hub it is on";
    }
    DEVINST hub = 0;
    if (CM_Get_Parent(&hub, usbDevice, 0) != CR_SUCCESS) return "its USB hub is not found";
    const std::string hubId = deviceIdOf(hub);
    const std::wstring hubWide(hubId.begin(), hubId.end());
    // GUID_DEVINTERFACE_USB_HUB
    GUID hubInterface{0xf18a0e88, 0xc30c, 0x11d0, {0x88, 0x15, 0x00, 0xa0, 0xc9, 0x06, 0xbe, 0xd8}};
    ULONG chars = 0;
    if (CM_Get_Device_Interface_List_SizeW(&chars, &hubInterface, const_cast<wchar_t*>(hubWide.c_str()),
                                           CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
        chars <= 1) {
        return "its USB hub (" + hubId + ") offers no hub interface";
    }
    std::wstring paths(chars, L'\0');
    if (CM_Get_Device_Interface_ListW(&hubInterface, const_cast<wchar_t*>(hubWide.c_str()), paths.data(), chars,
                                      CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
        return "its USB hub (" + hubId + ") offers no hub interface";
    }
    const HANDLE file = CreateFileW(paths.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, 0, nullptr);   // the first path in the list
    if (file == INVALID_HANDLE_VALUE) {
        return "cannot open its USB hub " + hubId + " (error " + std::to_string(GetLastError()) + ")";
    }
    struct CyclePortParams {   // USB_CYCLE_PORT_PARAMS, without usbioctl.h
        ULONG connectionIndex;
        ULONG statusReturned;
    } params{port, 0};
    // IOCTL_USB_HUB_CYCLE_PORT: FILE_DEVICE_USB (= FILE_DEVICE_UNKNOWN), function USB_HUB_CYCLE_PORT (273).
    constexpr DWORD kCyclePort = CTL_CODE(FILE_DEVICE_UNKNOWN, 273, METHOD_BUFFERED, FILE_ANY_ACCESS);
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(file, kCyclePort, &params, sizeof(params), &params, sizeof(params), &returned,
                                    nullptr);
    const DWORD error = GetLastError();
    CloseHandle(file);
    if (!ok) {
        return "its USB hub " + hubId + " refused to reconnect port " + std::to_string(port) + " (error " +
               std::to_string(error) + ")";
    }
    return {};
}

}  // namespace

bool HideFilter::waitUntilStarted(const std::vector<std::string>& interfaces, const int timeoutMs,
                                  const std::function<bool()>& cancelled, std::string& why, const int restartAfterMs,
                                  std::vector<std::string>* const heldOpenOut) {
    constexpr int kStepMs = 250;
    constexpr int kRetryEveryMs = 5'000;     // and again (closing a program that had it open is then enough)
    constexpr int kHeldRetryEveryMs = 1'000; // while a program holds it (Windows asks it again each time)
    int nextRestartMs = restartAfterMs;      // Windows usually restarts it by itself within ~7 s; then we do it
    std::vector<HeldOpen> heldOpen;          // the last round's refusals because a program had the device open
    int heldSinceMs = -1;                    // when the refusals because of a program holding it began
    if (heldOpenOut != nullptr) heldOpenOut->clear();
    for (int waited = 0; waited <= timeoutMs; waited += kStepMs) {
        bool allRunning = true;
        std::vector<std::string> waiting;
        for (const std::string& id : interfaces) {
            const NodeState state = nodeState(id);
            if (state != NodeState::Running) allRunning = false;
            if (state == NodeState::Waiting) waiting.push_back(id);
        }
        if (allRunning) {
            if (waited > 0) WE_LOG_INFO(log(), "hide: Windows restarted the device with the filter after {:.1f} s",
                                        static_cast<double>(waited) / 1000.0);
            return true;
        }
        if (cancelled && cancelled()) {
            why = "stopped";
            return false;
        }
        if (waited >= nextRestartMs && !waiting.empty()) {
            nextRestartMs = waited + kRetryEveryMs;
            heldOpen.clear();
            for (const std::string& id : waiting) {
                std::string refused;
                if (restartNode(id, refused, &heldOpen)) {
                    WE_LOG_INFO(log(), "hide: restarting {} so the filter starts", id);
                } else {
                    why = refused;
                    WE_LOG_WARN(log(), "hide: cannot restart {} yet: {}", id, refused);
                }
            }
            if (heldOpen.empty()) {
                heldSinceMs = -1;
            } else {
                if (heldSinceMs < 0) heldSinceMs = waited;
                nextRestartMs = waited + kHeldRetryEveryMs;
                // A caller that can take the device back: told once the program had its time to let go.
                if (heldOpenOut != nullptr && waited - heldSinceMs >= kHeldGraceMs) break;
            }
        }
        Sleep(kStepMs);
    }
    if (why.empty()) why = "Windows did not restart it in time";
    if (heldOpenOut != nullptr) *heldOpenOut = askedOf(heldOpen);
    nameHolders(heldOpen, why);
    return false;
}

std::vector<HideFilter::Check> HideFilter::check(const std::vector<Target>& targets, const bool quiet) {
    std::vector<Check> checks;
    for (const Target& target : targets) checks.push_back({target, State::NotConnected, {}, "not connected", {}, {}});

    // The game-controller interfaces are exactly what DirectInput lists (what games see), whatever HID usage the
    // device reports: a multi-axis controller (many wheels are) has no compatible id that says "game controller" and the
    // registry does not give compatible ids here. A vendor tool's other interfaces are never listed.
    for (const std::string& path : dinput::gameControllerPaths()) {
        const std::string id = hideids::instanceIdOfHidPath(path);
        const auto usb = hideids::hidDeviceIdOf(id);
        if (!usb) continue;
        Check* entry = nullptr;
        for (Check& c : checks) {
            if (c.target.id == *usb) entry = &c;
        }
        if (entry == nullptr) continue;
        DEVINST node = 0;
        const std::wstring wide(id.begin(), id.end());   // device instance ids are ASCII
        if (CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(wide.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
            entry->otherInterfaces.push_back(id + " (DirectInput lists it, but Windows does not find its device)");
            continue;
        }
        entry->interfaces.push_back(id);
        State state = State::Ready;
        std::string detail;
        // Ours: the reflector in its upper filters (any user-mode filter shows as WUDFRd) and our INF's driver
        // description ("... (wheelbri: hidden from games ...)").
        const std::vector<std::string> description = devNodeStrings(node, kDriverDesc);
        const std::vector<std::string> filters = devNodeStrings(node, kUpperFilters);
        const bool ours = ourDriver(node);
        if (!listsIgnoreCase(filters, kReflector) || !ours) {
            state = State::NotInstalled;
            if (ours) {
                entry->detached.push_back(id);
                detail = "wheelbri-hide is out of " + id + "'s stack (a Stop took it out): Start puts it back";
            } else {
                detail = "no wheelbri-hide on " + id + ": driver '" + joined(description) + "', upper filters " +
                         joined(filters);
            }
        } else {
            // Listed in the registry is not running: after the driver is switched, Windows may load the new stack
            // only when the interface restarts (something still had it open). Until then nothing is hidden.
            ULONG status = 0;
            ULONG problem = 0;
            if (CM_Get_DevNode_Status(&status, &problem, node, 0) == CR_SUCCESS &&
                ((status & DN_NEED_RESTART) != 0 || (status & DN_HAS_PROBLEM) != 0)) {
                state = State::NeedsRestart;
                detail = "wheelbri-hide is set on " + id + " but not running yet (" +
                         ((status & DN_NEED_RESTART) != 0 ? std::string("Windows wants a restart")
                                                          : "problem code " + std::to_string(problem)) +
                         ")";
            }
        }
        // The device counts as its worst interface.
        const auto rank = [](const State s) {
            return s == State::NotInstalled ? 3 : s == State::NeedsRestart ? 2 : s == State::Ready ? 1 : 0;
        };
        if (entry->interfaces.size() == 1 || rank(state) > rank(entry->state)) {
            entry->state = state;
            entry->detail = detail;
        }
    }
    if (!quiet) {
        for (const Check& c : checks) {
            const std::string what = "'" + c.target.name + "' (" + hideids::deviceIdText(c.target.id) + ")";
            if (c.state == State::Ready) {
                std::string list;
                for (const std::string& i : c.interfaces) list += (list.empty() ? "" : ", ") + i;
                WE_LOG_INFO(log(), "hide: wheelbri-hide is on {}: {}", what, list);
            } else {
                WE_LOG_WARN(log(), "hide: {}: {}{}", what, stateName(c.state), c.detail.empty() ? "" : " (" + c.detail + ")");
            }
            if (c.interfaces.empty() && !c.otherInterfaces.empty()) {
                std::string list;
                for (const std::string& i : c.otherInterfaces) list += (list.empty() ? "" : ", ") + i;
                WE_LOG_WARN(log(), "hide: {}: {}", what, list);
            }
        }
    }
    return checks;
}

std::string HideFilter::interfaceProblem(const std::string& instanceId) {
    DEVINST node = 0;
    const std::wstring wide(instanceId.begin(), instanceId.end());   // device instance ids are ASCII
    if (CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(wide.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        return instanceId + " is not connected";
    }
    if (const std::string usage = hideids::neverHiddenUsage(devNodeStrings(node, kCompatibleIds)); !usage.empty()) {
        return instanceId + " is " + usage + ": never hidden";
    }
    return {};
}

bool HideFilter::hasOurDriver(const std::string& instanceId) {
    DEVINST node = 0;
    return locate(instanceId, node) && ourDriver(node);
}

std::string HideFilter::setFilterListed(const std::string& instanceId, const bool listed, bool& changed) {
    changed = false;
    DEVINST node = 0;
    if (!locate(instanceId, node)) return instanceId + " is not connected";
    if (!ourDriver(node)) return instanceId + " does not run wheelbri-hide's driver: its filters are left alone";
    const std::vector<std::string> filters = devNodeStrings(node, kUpperFilters);
    if (listsIgnoreCase(filters, kReflector) == listed) return {};   // as wanted already
    std::wstring value;   // REG_MULTI_SZ: each one and its zero, then one more
    for (const std::string& filter : filters) {
        if (hideids::equalsIgnoreCase(filter, kReflector)) continue;
        value.append(filter.begin(), filter.end());   // driver service names are ASCII
        value.push_back(L'\0');
    }
    if (listed) {
        value.append(kReflector.begin(), kReflector.end());
        value.push_back(L'\0');
    }
    CONFIGRET result = CR_SUCCESS;
    if (value.empty()) {
        result = CM_Set_DevNode_Registry_PropertyW(node, CM_DRP_UPPERFILTERS, nullptr, 0, 0);   // none left
    } else {
        value.push_back(L'\0');
        result = CM_Set_DevNode_Registry_PropertyW(node, CM_DRP_UPPERFILTERS, value.data(),
                                                   static_cast<ULONG>(value.size() * sizeof(wchar_t)), 0);
    }
    if (result != CR_SUCCESS) {
        return "cannot write " + instanceId + "'s upper filters (CM_Set_DevNode_Registry_Property " +
               std::to_string(result) + ")";
    }
    changed = true;
    WE_LOG_INFO(log(), "hide: wheelbri-hide {} {}'s stack (from its next restart)", listed ? "back in" : "out of",
                instanceId);
    return {};
}

bool HideFilter::restartNow(const std::vector<std::string>& interfaces, const int timeoutMs,
                            const std::function<bool()>& cancelled, std::string& why,
                            std::vector<std::string>* const heldOpenOut) {
    constexpr int kStepMs = 100;
    constexpr int kRetryEveryMs = 1'000;   // a program that refused may have let go meanwhile (Windows asked it to)
    std::vector<std::string> toRestart = interfaces;
    int nextTryMs = 0;
    std::vector<HeldOpen> heldOpen;   // the last round's refusals because a program had the device open
    int heldSinceMs = -1;             // when the refusals because of a program holding it began
    if (heldOpenOut != nullptr) heldOpenOut->clear();
    for (int waited = 0; waited <= timeoutMs; waited += kStepMs) {
        if (waited >= nextTryMs && !toRestart.empty()) {
            nextTryMs = waited + kRetryEveryMs;
            std::vector<std::string> refused;
            heldOpen.clear();
            for (const std::string& id : toRestart) {
                std::string reason;
                if (restartNode(id, reason, &heldOpen)) {
                    WE_LOG_INFO(log(), "hide: restarting {}", id);
                } else {
                    refused.push_back(id);
                    why = id + ": " + reason;
                    WE_LOG_DEBUG(log(), "hide: cannot restart {} yet: {}", id, reason);
                }
            }
            toRestart = std::move(refused);
            if (heldOpen.empty()) {
                heldSinceMs = -1;
            } else {
                if (heldSinceMs < 0) heldSinceMs = waited;
                // A caller that can take the device back: told once the program had its time to let go.
                if (heldOpenOut != nullptr && waited - heldSinceMs >= kHeldGraceMs) break;
            }
        }
        bool allBack = toRestart.empty();
        for (const std::string& id : interfaces) allBack = allBack && nodeState(id) == NodeState::Running;
        if (allBack) {
            WE_LOG_INFO(log(), "hide: the rig's devices are back after {:.1f} s", static_cast<double>(waited) / 1000.0);
            return true;
        }
        if (cancelled && cancelled()) {
            why = "stopped";
            return false;
        }
        Sleep(kStepMs);
    }
    if (why.empty()) why = "Windows did not set it up again in time";
    if (heldOpenOut != nullptr) *heldOpenOut = askedOf(heldOpen);
    nameHolders(heldOpen, why);
    WE_LOG_WARN(log(), "hide: not every device restarted: {}", why);
    return false;
}

bool HideFilter::reconnect(const std::vector<std::string>& interfaces, const int timeoutMs,
                           const std::function<bool()>& cancelled, std::string& why) {
    // Each USB device once (the wheel's interfaces share one).
    std::vector<std::string> cycled;
    for (const std::string& id : interfaces) {
        DEVINST usbDevice = 0;
        std::string usbId;
        if (!usbDeviceOf(id, usbDevice, usbId)) {
            why = id + ": no USB device found above it, so it cannot be reconnected";
            WE_LOG_ERROR(log(), "hide: {}", why);
            return false;
        }
        if (std::find(cycled.begin(), cycled.end(), usbId) != cycled.end()) continue;
        WE_LOG_WARN(log(), "hide: reconnecting {} (its USB port is power-cycled, as unplugging it would) so the "
                           "program holding it loses it",
                    usbId);
        if (const std::string problem = cycleUsbPort(usbDevice); !problem.empty()) {
            why = usbId + ": " + problem;
            WE_LOG_ERROR(log(), "hide: cannot reconnect {}", why);
            return false;
        }
        cycled.push_back(usbId);
    }
    // Gone, then back and started with the filter (in its stack since it was put on; the rig is hidden already, so
    // from its first moment nothing but WheelBri may open it).
    constexpr int kStepMs = 100;
    constexpr int kMustGoWithinMs = 5'000;
    std::vector<bool> wentAway(interfaces.size(), false);
    for (int waited = 0; waited <= timeoutMs; waited += kStepMs) {
        bool allBack = true;
        for (std::size_t i = 0; i < interfaces.size(); ++i) {
            const NodeState state = nodeState(interfaces[i]);
            if (state == NodeState::Gone) wentAway[i] = true;
            allBack = allBack && wentAway[i] && state == NodeState::Running;
        }
        if (allBack) {
            WE_LOG_INFO(log(), "hide: the wheel is back after {:.1f} s, with the filter",
                        static_cast<double>(waited) / 1000.0);
            return true;
        }
        if (waited >= kMustGoWithinMs && std::find(wentAway.begin(), wentAway.end(), false) != wentAway.end()) {
            why = "the USB hub accepted the reconnect, but the wheel never went away (this hub may not support it): "
                  "unplug the wheel and plug it in again";
            WE_LOG_ERROR(log(), "hide: {}", why);
            return false;
        }
        if (cancelled && cancelled()) {
            why = "stopped";
            return false;
        }
        Sleep(kStepMs);
    }
    why = "the wheel did not come back within " + std::to_string(timeoutMs / 1000) + " s after the reconnect";
    WE_LOG_ERROR(log(), "hide: {}", why);
    return false;
}

bool HideFilter::hide() {
    std::string message;
    hidden_ = privileged::allowOnly(GetCurrentProcess(), message);
    if (hidden_) {
        WE_LOG_INFO(log(), "hide: the rig is hidden from games (only this process, {}, may open its devices)",
                    GetCurrentProcessId());
    }
    return hidden_;
}

void HideFilter::restore() {
    if (!hidden_) return;
    if (privileged::writeAllowedPid(0, 0)) {
        WE_LOG_INFO(log(), "hide: the rig is visible to games again");
        hidden_ = false;
    }
    // Not written: still hidden, tried again by the destructor (and cleared by the next session's clearStale).
}

void HideFilter::clear() {
    if (privileged::writeAllowedPid(0, 0)) {
        WE_LOG_INFO(log(), "hide: wheelbri-hide lets every program open the rig's devices again");
    }
}

void HideFilter::clearStale() {
    const DWORD pid = privileged::allowedPid();
    if (pid == 0) return;
    if (pid == GetCurrentProcessId()) {
        // This process, but no session hides anything now (clearStale runs before hiding): an earlier session's
        // restore could not write the value. Without this the rig stays hidden until the app closes.
        WE_LOG_WARN(log(), "hide: the rig was still hidden for this process by an earlier session: clearing");
        if (!privileged::writeAllowedPid(0, 0)) WE_LOG_ERROR(log(), "hide: cannot clear it; the rig stays hidden from games");
        return;
    }
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    const bool alive = process != nullptr && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    if (process != nullptr) CloseHandle(process);
    if (alive) {
        WE_LOG_WARN(log(), "hide: process {} hides the wheel (another wheelbri-service?); this one may not see it", pid);
        return;
    }
    WE_LOG_WARN(log(), "hide: the wheel was still hidden for process {}, which is gone (crash?): clearing", pid);
    if (!privileged::writeAllowedPid(0, 0)) {
        WE_LOG_ERROR(log(), "hide: the wheel stays hidden: run wheelbri-service --unhide as Administrator");
    }
}

HideFilter::~HideFilter() { restore(); }

}  // namespace wheelbri::service

#else  // not Windows

namespace wheelbri::service {

bool HideFilter::waitUntilStarted(const std::vector<std::string>&, int, const std::function<bool()>&,
                                  std::string& why, int, std::vector<std::string>* const heldOpen) {
    if (heldOpen != nullptr) heldOpen->clear();
    why = "not on Windows";
    return false;
}

std::vector<HideFilter::Check> HideFilter::check(const std::vector<Target>& targets, bool) {
    std::vector<Check> checks;
    for (const Target& target : targets) checks.push_back({target, State::NotConnected, {}, "not on Windows", {}, {}});
    return checks;
}
const char* HideFilter::stateName(const State state) {
    return state == State::Ready ? "hidden while WheelBri runs" : "not hidden";
}
std::string HideFilter::interfaceProblem(const std::string& instanceId) {
    return instanceId + ": the hide filter is for Windows only";
}
bool HideFilter::hasOurDriver(const std::string&) { return false; }
std::string HideFilter::setFilterListed(const std::string& instanceId, bool, bool& changed) {
    changed = false;
    return instanceId + ": the hide filter is for Windows only";
}
bool HideFilter::restartNow(const std::vector<std::string>&, int, const std::function<bool()>&, std::string& why,
                            std::vector<std::string>* const heldOpen) {
    if (heldOpen != nullptr) heldOpen->clear();
    why = "not on Windows";
    return false;
}
bool HideFilter::reconnect(const std::vector<std::string>&, int, const std::function<bool()>&, std::string& why) {
    why = "not on Windows";
    return false;
}
bool HideFilter::hide() { return false; }
void HideFilter::restore() {}
void HideFilter::clear() {}
void HideFilter::clearStale() {}
HideFilter::~HideFilter() = default;

}  // namespace wheelbri::service

#endif
