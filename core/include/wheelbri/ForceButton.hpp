// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace wheelbri {

// The Force button (IDeviceProfile::forceButton, the RGT's): each press switches the target's centring spring
// (CentringSpring.hpp) off, the next back on at the strength the mapping sets; as a real RGT's Force button switches
// its recentring. On at every Start (the session's own: nothing is saved). With a strength of 0 there is nothing to
// switch. No allocations (the loop calls it every pass).
class ForceButton {
public:
    ForceButton(std::size_t button, float percent) : button_(button), percent_(percent) {}

    // The mapped buttons (WheelState::buttons): the spring's new strength when a press switched it, else nullopt.
    [[nodiscard]] std::optional<float> update(std::uint32_t buttons) {
        const bool down = button_ < 32 && ((buttons >> button_) & 1U) != 0;
        const bool pressed = down && !down_;
        down_ = down;
        if (!pressed || percent_ <= 0.0F) return std::nullopt;
        on_ = !on_;
        return on_ ? percent_ : 0.0F;
    }
    // Whether the spring is on (a strength above 0 and not switched off).
    [[nodiscard]] bool on() const { return on_ && percent_ > 0.0F; }

private:
    std::size_t button_;
    float percent_;
    bool down_ = false;
    bool on_ = true;
};

}  // namespace wheelbri
