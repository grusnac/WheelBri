#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace wheelbri {

// Tells when the game has loaded its force feedback but plays none of it while you drive. Seen with NFS 2015 on
// 2026-10-07: at the start of a race it created its effects (constant, spring, damper) and never started them, so
// the wheel was limp until an alt-tab made it create and start them again. Lasting effects (constant force and the
// conditions: spring, damper, friction, inertia) loaded, none of them wanted, and pedals pressed for a while means
// the game forgot to start them. Short effects (sine, square, ...) do not count either way: they come and go.
// Allocation-free; called on every pass of the service loop.
class FfbIdleWatch {
public:
    static constexpr std::uint64_t kPedalMs = 10'000;   // pedals pressed this long, in total, before warning

    // `pedal`: throttle or brake pressed now; `lastingLoaded` / `lastingWanted`: how many lasting effects the game has
    // loaded and how many of those it wants playing (FfbController). Pedal time counts only while effects are loaded
    // and none is wanted, and starts again from zero when one is. Returns true when the warning turns on, false when
    // it turns off (an effect wanted, or none loaded any more), nullopt otherwise.
    std::optional<bool> update(std::uint64_t nowMs, bool pedal, std::size_t lastingLoaded, std::size_t lastingWanted);

    [[nodiscard]] bool warning() const { return warning_; }

private:
    std::uint64_t lastMs_ = 0;
    bool started_ = false;
    std::uint64_t pedalMs_ = 0;
    bool warning_ = false;
};

}  // namespace wheelbri
