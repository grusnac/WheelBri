// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Wheel.hpp"

#include "wheelbri/ChangeCounter.hpp"
#include "wheelbri/RelistTimer.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace wheelbri::service {

// The game controllers connected now, listed on a thread of its own (listing through DirectInput can take a good part
// of a second, too long for the window's thread) while the app is stopped: at once when switched on, then when a
// change is announced (`changes`: platform::DeviceChanges' counter in the app) and on core RelistTimer's other times.
// The Mapping tab uses it to say which of the rig's devices Start still needs. The lister, the counter and the times
// come from outside, so DeviceWatchTest drives it with fakes.
class DeviceWatch {
public:
    // The connected game controllers, or nullopt when the system's game-controller API cannot be used here.
    using Lister = std::function<std::optional<std::vector<WheelInfo>>()>;

    struct Options {
        RelistTimer::Settings relist;            // RelistTimer::Settings::withoutNotifications() without notices
        std::uint64_t retryListerMs = 10'000;    // after the lister said nullopt
    };

    // The app's: DirectInput (or evdev) through startWheelBackend, the system's notices, the times that suit them.
    [[nodiscard]] static Lister systemLister();
    [[nodiscard]] static Options systemOptions(bool notificationsAvailable);

    DeviceWatch(Lister list, ChangeCounter& changes, const Options& options);
    ~DeviceWatch();
    DeviceWatch(const DeviceWatch&) = delete;
    DeviceWatch& operator=(const DeviceWatch&) = delete;

    // Lists only while active (not while a session runs: it has the devices open). Switched on, it lists at once;
    // switched off, the last list is dropped.
    void setActive(bool active);

    struct Snapshot {
        std::vector<WheelInfo> devices;
        std::uint64_t generation = 0;   // a new listing
    };
    // The latest listing; nullopt until the first one, while inactive, or when game controllers cannot be read here.
    [[nodiscard]] std::optional<Snapshot> latest() const;
    // True when the lister last said the game-controller API cannot be used (it is tried again later).
    [[nodiscard]] bool unavailable() const;

private:
    void run();

    Lister list_;
    ChangeCounter& changes_;
    Options options_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool active_ = false;
    bool activated_ = false;   // switched on since the thread last looked: list at once
    bool quit_ = false;
    bool unavailable_ = false;
    std::optional<Snapshot> latest_;
    std::uint64_t generation_ = 0;
    std::thread thread_;   // last: started once everything above exists
};

}  // namespace wheelbri::service
