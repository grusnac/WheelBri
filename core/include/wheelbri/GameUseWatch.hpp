// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>

namespace wheelbri {

// Tells when the game drives from something other than the emulated wheel. A game that drives from a wheel plays its
// force feedback on it (at least the centring spring), so pedals pressed for a while with no force feedback ever
// received means the game took another controller: a gamepad or
// the keyboard. Usually the game was started before the wheel existed, or it picked the pad in its settings.
// Allocation-free; called on every pass of the service loop.
class GameUseWatch {
public:
    static constexpr std::uint64_t kPedalMs = 20'000;   // pedals pressed this long, in total, before warning

    // `pedal`: throttle or brake pressed now; `gamePackets`: force feedback packets received from the game so far.
    // Returns true when the warning turns on, false when it turns off again, nullopt otherwise.
    std::optional<bool> update(std::uint64_t nowMs, bool pedal, std::uint64_t gamePackets);

    [[nodiscard]] bool warning() const { return warning_; }

private:
    std::uint64_t lastMs_ = 0;
    bool started_ = false;
    std::uint64_t pedalMs_ = 0;
    bool warning_ = false;
};

}  // namespace wheelbri
