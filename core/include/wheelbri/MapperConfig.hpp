#pragma once

#include "wheelbri/WheelState.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace wheelbri {

inline constexpr int kUnmapped = -1;

// A USB device by its ids (an accessory the mapping reads).
struct DeviceId {
    std::uint16_t vendorId = 0;
    std::uint16_t productId = 0;

    [[nodiscard]] bool empty() const { return vendorId == 0 && productId == 0; }
    friend bool operator==(const DeviceId&, const DeviceId&) = default;
};

// Every source below is a control on one device: `sourceDevice` 0 is the wheel ([emulator] source_device),
// 1..3 are MapperConfig::accessories[0..2]. In the config a source is written "2" (the wheel's axis or button 2)
// or "044f:b696/2" (that accessory's).

struct SteeringConfig {
    int sourceAxis = 0;
    int sourceDevice = 0;
    float sourceRotationDeg = 900.0F;   // rotation configured on the rig's wheel (its maker's software)
    float targetRotationDeg = 900.0F;   // rotation the emulated wheel reports (the G920 = 900)
    float deadzone = 0.0F;              // fraction of half-range around centre, 0..1
    bool invert = false;
};

struct PedalConfig {
    int sourceAxis = kUnmapped;
    int sourceDevice = 0;
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
    // Their devices, in the same order (up, right, down, left).
    std::array<int, 4> devices{};

    [[nodiscard]] bool any() const { return up >= 0 || right >= 0 || down >= 0 || left >= 0; }
};

// One of a target button's secondary sources ([buttons] target_N_or, a list): a button, or one of the rig's
// one-sided axes (throttle, brake, clutch, handbrake: the Mapping tab offers only those) that presses it past
// `threshold` of its travel. The target button is pressed while its main source (buttonMap / buttonDevice, the role
// assignment) or any secondary is. E.g. the G920's X <- the wheel's X, or a handbrake lever, or a button box button.
struct ButtonSource {
    int device = 0;
    int index = kUnmapped;   // a button, or an axis when `axis`
    bool axis = false;
    bool invert = false;     // the axis is released at +1 (as the rig says it rests)
    float threshold = 0.5F;  // of the axis' travel from where it rests: pressed from here on (released 5% below)

    [[nodiscard]] bool mapped() const { return index >= 0; }
    friend bool operator==(const ButtonSource&, const ButtonSource&) = default;
};

struct MapperConfig {
    static constexpr std::size_t kMaxTargetButtons = 32;
    static constexpr std::size_t kMaxSourceButtons = 64;   // source buttons the mapping can use (0..63)
    static constexpr std::size_t kMaxAccessories = kMaxInputDevices - 1;
    static constexpr std::size_t kMaxSecondaries = 8;   // per target button (fixed: the service loop never allocates)

    // The accessories the mapping reads (device 1, 2, 3); empty = not used. Filled from the sources' ids.
    std::array<DeviceId, kMaxAccessories> accessories{};

    SteeringConfig steering;
    PedalConfig throttle;
    PedalConfig brake;
    PedalConfig clutch;
    HatButtons hatButtons;

    // What the physical wheel's buttons are called ("Start", "L paddle"), indexed by source button; empty =
    // no name. Display only: the --ui window and the app show them next to the numbers, the mapping ignores them.
    std::array<std::string, kMaxSourceButtons> sourceButtonLabels{};
    // The same for the wheel's axes ("Wheel", "Gas pedal"), indexed by source axis ([axis_labels]).
    std::array<std::string, RawInputState::kMaxAxes> sourceAxisLabels{};

    // buttonMap[target] = source button index, or kUnmapped; on device buttonDevice[target].
    std::array<int, kMaxTargetButtons> buttonMap = identityButtonMap();
    std::array<int, kMaxTargetButtons> buttonDevice{};
    // buttonSecondary[target]: its secondary sources (ButtonSource above), the mapped ones first; the rest unmapped.
    std::array<std::array<ButtonSource, kMaxSecondaries>, kMaxTargetButtons> buttonSecondary{};

    // How many accessories are used (the first empty slot ends them).
    [[nodiscard]] std::size_t accessoryCount() const {
        std::size_t count = 0;
        while (count < accessories.size() && !accessories[count].empty()) ++count;
        return count;
    }

    static constexpr std::array<int, kMaxTargetButtons> identityButtonMap() {
        std::array<int, kMaxTargetButtons> map{};
        for (std::size_t i = 0; i < map.size(); ++i) {
            map[i] = static_cast<int>(i);
        }
        return map;
    }
};

}  // namespace wheelbri
