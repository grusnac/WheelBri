#pragma once

#include <array>
#include <cstdint>

namespace wheelemu {

inline constexpr int kUnmapped = -1;

struct SteeringConfig {
    int sourceAxis = 0;
    float sourceRotationDeg = 900.0F;   // rotation configured on the physical wheel (Moza Pit House)
    float targetRotationDeg = 900.0F;   // rotation the emulated wheel reports (G29/G920 = 900)
    float deadzone = 0.0F;              // fraction of half-range around centre, 0..1
    bool invert = false;
};

struct PedalConfig {
    int sourceAxis = kUnmapped;
    bool invert = false;                // true when the OS reports "pressed" as the low end
    float deadzoneLow = 0.0F;           // fraction of travel ignored at the released end
    float saturationHigh = 1.0F;        // fraction of travel that already counts as fully pressed
    float gamma = 1.0F;                 // response curve: <1 more sensitive early, >1 less
};

// Source buttons that act as a D-pad (some rims report their D-pad as four buttons instead of a hat).
// When any is set, the emulated wheel's hat is built from them; a real source hat still wins when pressed.
struct HatButtons {
    int up = kUnmapped;
    int right = kUnmapped;
    int down = kUnmapped;
    int left = kUnmapped;

    [[nodiscard]] bool any() const { return up >= 0 || right >= 0 || down >= 0 || left >= 0; }
};

struct MapperConfig {
    static constexpr std::size_t kMaxTargetButtons = 32;

    SteeringConfig steering;
    PedalConfig throttle;
    PedalConfig brake;
    PedalConfig clutch;
    HatButtons hatButtons;

    // buttonMap[target] = source button index, or kUnmapped.
    std::array<int, kMaxTargetButtons> buttonMap = identityButtonMap();

    static constexpr std::array<int, kMaxTargetButtons> identityButtonMap() {
        std::array<int, kMaxTargetButtons> map{};
        for (std::size_t i = 0; i < map.size(); ++i) {
            map[i] = static_cast<int>(i);
        }
        return map;
    }
};

}  // namespace wheelemu
