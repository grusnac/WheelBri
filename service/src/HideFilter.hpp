#pragma once

#include "wheelbri/MapperConfig.hpp"

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
    };

    // Where wheelbri-hide stands on each device of `targets`. `quiet`: no log lines (for polling).
    [[nodiscard]] static std::vector<Check> check(const std::vector<Target>& targets, bool quiet = false);
    [[nodiscard]] static const char* stateName(State state);

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
