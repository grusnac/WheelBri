#pragma once

#include "wheelbri/MapperConfig.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wheelbri::service {

// Our own way to hide the rig from games (driver/hide, the wheelbri-hide filter), replacing HidHide.
// The filter sits on the game-controller interface of each device of the active rig (the Wheel setup tab's: the
// wheel, pedals, shifter, button box, whatever it lists) and lets only the process named in
//   HKLM\SOFTWARE\wheelbri\hide  AllowedPid
// open it; 0 or missing = everyone. So hiding is one registry value, written while the service runs.
// Which devices get the filter comes from the rig, never from a list of ids in the code: install-hide.ps1 -Devices
// puts it on exactly those (the INF matches any game controller, but Windows never picks it by itself).
// The filter is in the wheel's stack only while a Mode Virtual session runs: being a user-mode filter, every report
// the wheel sends to every program that has it open goes through its host (WUDFHost), about half a core on a Moza R3
// (2026-10-24), even when nothing is hidden. So Stop takes it out of the stack (setFilterListed false + restartNow)
// and Start puts it back the same way, without the installer, once the interface has our driver.
// Windows only; elsewhere check() says nothing is connected and hide() does nothing.
class HideFilter {
public:
    // A device of the rig: its USB ids and the name the rig gives it (for the window and the log).
    struct Target {
        DeviceId id;
        std::string name;
    };
    enum class State {
        Ready,          // the filter runs on every game-controller interface of the device
        NotInstalled,   // connected, but at least one of its game-controller interfaces has no filter
        NeedsRestart,   // the filter is set but Windows starts it only after the interface (or Windows) restarts
        NotConnected,   // no game-controller interface of the device is present (unplugged)
    };
    struct Check {
        Target target;
        State state = State::NotConnected;
        std::vector<std::string> interfaces;   // the device's game-controller interfaces (instance ids)
        std::string detail;                    // why it is not Ready (the log's words)
        std::vector<std::string> otherInterfaces;   // its other HID collections, with their compatible ids (the log)
        std::vector<std::string> detached;          // interfaces with our driver whose filter was taken out (Stop)
    };

    // Where wheelbri-hide stands on each device of `targets`. `quiet`: no log lines (for polling).
    [[nodiscard]] static std::vector<Check> check(const std::vector<Target>& targets, bool quiet = false);
    [[nodiscard]] static const char* stateName(State state);

    // After the filter was put on: waits until Windows has restarted every interface in `interfaces` with it (gone for a
    // moment, then back started, without a pending restart), up to `timeoutMs`. Windows usually does this by itself a
    // few seconds after the installer, unless something has the device open: so nothing of ours opens it meanwhile
    // (only the device's status is read), and when it still waits after a few seconds the device is restarted here
    // (CM_Query_And_Remove_SubTree, then set up again), which also asks programs that registered for it to let go.
    // False, with `why` (who refused, as Windows says it), when it did not happen in time. `cancelled`: Stop.
    // `restartAfterMs`: how long Windows gets to restart it by itself first; 0 when the installer already found the
    // device waiting for a restart, which Windows then never does by itself (2026-10-06: 6 s waited for nothing).
    // `heldOpen` (Start, which can take the wheel back with reconnect): when given, it gives up as soon as a program
    // that has a device open has refused to let go for a few seconds (Windows asks it at each restart), and lists
    // those interfaces there; empty when that was not why it failed.
    [[nodiscard]] static bool waitUntilStarted(const std::vector<std::string>& interfaces, int timeoutMs,
                                               const std::function<bool()>& cancelled, std::string& why,
                                               int restartAfterMs = 6'000,
                                               std::vector<std::string>* heldOpen = nullptr);

    // Empty when `instanceId` is a HID interface present now that is not a keyboard, mouse, system or consumer control
    // or digitizer (hideids::neverHiddenUsage on its compatible ids); else why it must not get the filter. The helper
    // checks every interface it is asked to hide with this (and with helper::hideDeviceProblem), whatever the app said.
    [[nodiscard]] static std::string interfaceProblem(const std::string& instanceId);

    // Whether `instanceId` (present now) runs our INF's driver, which install-hide.ps1 put on it once, with or without
    // the filter in its stack.
    [[nodiscard]] static bool hasOurDriver(const std::string& instanceId);
    // The filter in (`listed`) or out of the interface's stack as the registry says it (its upper filters, WUDFRd);
    // Windows goes by it when the interface restarts (restartNow). Only on an interface with our driver: any other
    // (a vendor's own user-mode filter also shows as WUDFRd) is never touched. Empty on success, `changed` true when
    // something was written; else why not.
    [[nodiscard]] static std::string setFilterListed(const std::string& instanceId, bool listed, bool& changed);
    // Restarts every interface in `interfaces` now (removed, so programs that registered for it let go, then set up
    // again; tried again every second while one refuses) and waits until each is back and started, up to `timeoutMs`.
    // False with `why` (who refused, as Windows says it). `cancelled`: Stop. `heldOpen`: as for waitUntilStarted.
    [[nodiscard]] static bool restartNow(const std::vector<std::string>& interfaces, int timeoutMs,
                                         const std::function<bool()>& cancelled, std::string& why,
                                         std::vector<std::string>* heldOpen = nullptr);
    // Start only, and only after a program that has the wheel open would not let go (restartNow or waitUntilStarted
    // listed those interfaces in `heldOpen`): reconnects the USB device of each interface in `interfaces`
    // (power-cycles its hub port, as unplugging it would; no program can refuse that, so every handle to it ends) and
    // waits until each interface is back and started, with the filter, up to `timeoutMs`. The rig must be hidden before (it is, from the start of
    // the session), or the program could open it again as it comes back. False with `why` (the hub refused, or the
    // device never went away: then only unplugging it helps).
    [[nodiscard]] static bool reconnect(const std::vector<std::string>& interfaces, int timeoutMs,
                                        const std::function<bool()>& cancelled, std::string& why);

    HideFilter() = default;
    ~HideFilter();

    HideFilter(const HideFilter&) = delete;
    HideFilter& operator=(const HideFilter&) = delete;

    // Only this process may open the rig's devices (every device that has the filter) from now on. False (and
    // logged) if the setting cannot be written.
    bool hide();
    // Everyone may open them again.
    void restore();

    // After a crash: clears a setting left behind (without needing the devices plugged in).
    static void clear();

    // At start: a setting left by a service that is gone (crash) would refuse this process too; clear it.
    static void clearStale();

private:
    bool hidden_ = false;
};

}  // namespace wheelbri::service
