#include "HideFilter.hpp"

#include "Privileged.hpp"

#ifdef _WIN32
#include "DinputDevice.hpp"
#endif

#include "wheelbri/HidHideCli.hpp"
#include "wheelbri/Logging.hpp"

#ifdef _WIN32

#include <windows.h>
#include <aclapi.h>
#include <cfgmgr32.h>

#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

#ifndef DN_NEED_RESTART
#define DN_NEED_RESTART 0x00000100
#endif

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.hide");
    return logger;
}

std::string narrow(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size, nullptr, nullptr);
    return text;
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
        if (hidhide::equalsIgnoreCase(item, wanted)) return true;
    }
    return false;
}

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

// Restarts the interface now: removed (programs that registered for it are asked to let go), then set up again with
// its new stack. False with `why` when something refused.
bool restartNode(const std::string& instanceId, std::string& why) {
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
        return false;
    }
    // Back, with the filter: the removed node is a phantom until it is set up again.
    if (CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(wide.c_str()), CM_LOCATE_DEVNODE_PHANTOM) == CR_SUCCESS) {
        CM_Setup_DevNode(node, CM_SETUP_DEVNODE_READY);
    }
    return true;
}

}  // namespace

bool HideFilter::waitUntilStarted(const std::vector<std::string>& interfaces, const int timeoutMs,
                                  const std::function<bool()>& cancelled, std::string& why) {
    constexpr int kStepMs = 250;
    constexpr int kRestartAfterMs = 6'000;   // Windows usually restarts it by itself within ~7 s; then we do it
    constexpr int kRetryEveryMs = 5'000;     // and again (closing a program that had it open is then enough)
    int nextRestartMs = kRestartAfterMs;
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
            for (const std::string& id : waiting) {
                std::string refused;
                if (restartNode(id, refused)) {
                    WE_LOG_INFO(log(), "hide: restarting {} so the filter starts", id);
                } else {
                    why = refused;
                    WE_LOG_WARN(log(), "hide: cannot restart {} yet: {}", id, refused);
                }
            }
        }
        Sleep(kStepMs);
    }
    if (why.empty()) why = "Windows did not restart it in time";
    return false;
}

std::vector<HideFilter::Check> HideFilter::check(const std::vector<Target>& targets, const bool quiet) {
    std::vector<Check> checks;
    for (const Target& target : targets) checks.push_back({target, State::NotConnected, {}, "not connected", {}});

    // The game-controller interfaces are exactly what DirectInput lists (what games see), whatever HID usage the
    // device reports: a multi-axis controller (many wheels are) has no compatible id that says "game controller" and the
    // registry does not give compatible ids here. A vendor tool's other interfaces are never listed.
    for (const std::string& path : dinput::gameControllerPaths()) {
        const std::string id = hidhide::instanceIdOfHidPath(path);
        const auto usb = hidhide::hidDeviceIdOf(id);
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
        // description ("... (wheelbri: hidden from games ...)"; "wheelemu" before the rename).
        const std::vector<std::string> description = devNodeStrings(node, kDriverDesc);
        const std::vector<std::string> filters = devNodeStrings(node, kUpperFilters);
        const bool ourDriver = !description.empty() && (hidhide::containsIgnoreCase(description.front(), "wheelbri") ||
                                                        hidhide::containsIgnoreCase(description.front(), "wheelemu"));
        if (!listsIgnoreCase(filters, "WUDFRd") || !ourDriver) {
            state = State::NotInstalled;
            detail = "no wheelbri-hide on " + id + ": driver '" + joined(description) + "', upper filters " + joined(filters);
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
            const std::string what = "'" + c.target.name + "' (" + hidhide::deviceIdText(c.target.id) + ")";
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
    if (const std::string usage = hidhide::neverHiddenUsage(devNodeStrings(node, kCompatibleIds)); !usage.empty()) {
        return instanceId + " is " + usage + ": never hidden";
    }
    return {};
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
                                  std::string& why) {
    why = "not on Windows";
    return false;
}

std::vector<HideFilter::Check> HideFilter::check(const std::vector<Target>& targets, bool) {
    std::vector<Check> checks;
    for (const Target& target : targets) checks.push_back({target, State::NotConnected, {}, "not on Windows", {}});
    return checks;
}
const char* HideFilter::stateName(const State state) {
    return state == State::Ready ? "hidden while WheelBri runs" : "not hidden";
}
std::string HideFilter::interfaceProblem(const std::string& instanceId) {
    return instanceId + ": the hide filter is for Windows only";
}
bool HideFilter::hide() { return false; }
void HideFilter::restore() {}
void HideFilter::clear() {}
void HideFilter::clearStale() {}
HideFilter::~HideFilter() = default;

}  // namespace wheelbri::service

#endif
