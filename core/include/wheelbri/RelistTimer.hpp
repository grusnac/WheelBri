#pragma once

#include <cstdint>

namespace wheelbri {

// When to list the game controllers again, given a count of device changes the system announced (platform::
// DeviceChanges: Windows' device-interface arrivals and removals, inotify on /dev/input on Linux). Listing through
// DirectInput can take a good part of a second, so it is done only when something changed, instead of every 2 s:
//   - `settleMs` after a change was first seen, so the burst of interfaces one plugged-in device brings is listed once;
//   - again `followUpMs` after a listing a change caused, because DirectInput can list a device a moment after
//     Windows announced it;
//   - every `safetyMs` whatever happens, in case a change was never announced;
//   - at once the first time.
// Without notifications (the system refused them) safetyMs is the old 2 s: Settings::withoutNotifications().
// Allocation-free, no OS: the device watch, the Rig tab and the session's wait for the wheel share it.
class RelistTimer {
public:
    struct Settings {
        std::uint64_t settleMs = 300;
        std::uint64_t followUpMs = 2'000;
        std::uint64_t safetyMs = 30'000;

        [[nodiscard]] static Settings withoutNotifications() {
            Settings settings;
            settings.safetyMs = 2'000;
            return settings;
        }
    };

    RelistTimer() : RelistTimer(Settings{}) {}
    explicit RelistTimer(const Settings& settings) : settings_(settings) {}

    // Whether to list now (`changes`: the count of announced changes so far). It notices a new change here.
    [[nodiscard]] bool due(std::uint64_t nowMs, std::uint64_t changes);
    // A listing was done at `nowMs` with the count `changes` then.
    void listed(std::uint64_t nowMs, std::uint64_t changes);
    // The latest time due() can become true by itself (a new change can make it sooner): how long a waiter may sleep.
    [[nodiscard]] std::uint64_t nextDueMs(std::uint64_t nowMs, std::uint64_t changes) const;

    [[nodiscard]] const Settings& settings() const { return settings_; }

private:
    Settings settings_;
    bool listedOnce_ = false;
    std::uint64_t lastListMs_ = 0;
    std::uint64_t listedChanges_ = 0;      // the change count the last listing saw
    bool changePending_ = false;           // a change after the last listing, waiting for settleMs
    std::uint64_t changeSeenMs_ = 0;
    bool followUpPending_ = false;
    std::uint64_t followUpMs_ = 0;
};

}  // namespace wheelbri
