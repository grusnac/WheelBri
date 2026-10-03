#pragma once

#include <memory>
#include <string>

namespace wheelbri::service {

// Our own way to hide the real wheel from games (driver/hide, the wheelbri-hide filter), replacing HidHide.
// The filter sits on the wheel's game-controller interface and lets only the process named in
//   HKLM\SOFTWARE\wheelbri\hide  AllowedPid
// open it; 0 or missing = everyone. So hiding is one registry value, written while the service runs.
// Windows only; elsewhere find() returns null.
class HideFilter {
public:
    // Null when no wheel of `vendorId` ("346E") has the filter running: not installed, the wheel unplugged, or
    // installed but not loaded yet (Windows loads it when the wheel's interface restarts: replug or restart Windows).
    // `quiet`: no log line for the last case (for polling).
    [[nodiscard]] static std::unique_ptr<HideFilter> find(const std::string& vendorId, bool quiet = false);
    ~HideFilter();

    HideFilter(const HideFilter&) = delete;
    HideFilter& operator=(const HideFilter&) = delete;

    // Only this process may open the wheel from now on. False (and logged) if the setting cannot be written.
    bool hide();
    // Everyone may open it again.
    void restore();

    // After a crash: clears a setting left behind (without needing the wheel plugged in).
    static void clear();

    // At start: a setting left by a service that is gone (crash) would refuse this process too; clear it.
    static void clearStale();

private:
    explicit HideFilter(std::string instanceId);
    std::string instanceId_;
    bool hidden_ = false;
};

}  // namespace wheelbri::service
