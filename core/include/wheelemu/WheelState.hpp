#pragma once

#include <array>
#include <cstdint>

namespace wheelemu {

// Hat position in 45-degree steps, clockwise from "up"; kHatCentered when released.
inline constexpr std::int8_t kHatCentered = -1;

// Snapshot of the physical wheel as reported by the input source.
// Axes are normalised to [-1, 1] exactly as the OS reports them, before any mapping.
struct RawInputState {
    static constexpr std::size_t kMaxAxes = 8;

    std::array<float, kMaxAxes> axes{};
    std::uint64_t buttons = 0;          // bit n = source button n pressed
    std::int8_t hat = kHatCentered;
};

// Target-agnostic wheel state after mapping.
struct WheelState {
    float steering = 0.0F;              // -1 (full left) .. 1 (full right) over the TARGET rotation
    float throttle = 0.0F;              // 0 (released) .. 1 (fully pressed)
    float brake = 0.0F;
    float clutch = 0.0F;
    std::uint32_t buttons = 0;          // bit n = target button n pressed
    std::int8_t hat = kHatCentered;
};

}  // namespace wheelemu
