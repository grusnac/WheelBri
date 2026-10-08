#pragma once

#include "Wheel.hpp"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace wheelbri::service {

// The game controllers connected now, listed on a thread of its own (listing through DirectInput can take a good part
// of a second, too long for the window's thread) while the app is stopped: at once, then when the system says a game
// controller came or went (platform::DeviceChanges, core RelistTimer), and every 30 s in case it did not (every 2 s
// where the system gives no such notice). The Mapping tab uses it to say which of the rig's devices Start still needs.
class DeviceWatch {
public:
    DeviceWatch();
    ~DeviceWatch();
    DeviceWatch(const DeviceWatch&) = delete;
    DeviceWatch& operator=(const DeviceWatch&) = delete;

    // Lists only while active (not while a session runs: it has the devices open). Inactive, the last list is dropped.
    void setActive(bool active);

    struct Snapshot {
        std::vector<WheelInfo> devices;
        std::uint64_t generation = 0;   // a new listing
    };
    // The latest listing; nullopt until the first one, or when game controllers cannot be read here at all.
    [[nodiscard]] std::optional<Snapshot> latest() const;
    // True when the system's game-controller API could not be started (nothing to wait for).
    [[nodiscard]] bool unavailable() const;

private:
    void run();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool active_ = false;
    bool activated_ = false;   // switched on since the thread last looked: list at once
    bool quit_ = false;
    bool unavailable_ = false;
    std::optional<Snapshot> latest_;
    std::uint64_t generation_ = 0;
    std::thread thread_;
};

}  // namespace wheelbri::service
