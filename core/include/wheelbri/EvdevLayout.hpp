// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A Linux evdev game controller's axes, buttons and hats, numbered exactly as SDL3 numbers them
// (SDL_sysjoystick.c, ConfigJoystick), so the axis and button numbers in the config and the --ui window stay
// what they were when the service read the wheel through SDL:
//   buttons: key codes from BTN_JOYSTICK up to KEY_MAX, then the codes below BTN_JOYSTICK, in code order;
//   hats:    ABS_HAT0X/Y .. ABS_HAT3X/Y pairs that look digital;
//   axes:    every other absolute axis, in code order.
// Values like SDL: axes scaled from [minimum, maximum] to -32768..32767 (then /32767 as -1..1), hats from
// their -/0/+ positions.
//
// Plain numbers only (no <linux/input.h>): the service reads the capabilities and events from the device
// and hands them over, so this is unit-tested on every platform.

#include "wheelbri/WheelState.hpp"

#include <array>
#include <bitset>
#include <cstdint>
#include <vector>

namespace wheelbri::evdev {

inline constexpr int kEvSyn = 0x00;
inline constexpr int kEvKey = 0x01;
inline constexpr int kEvAbs = 0x03;
inline constexpr int kSynDropped = 3;
inline constexpr int kKeyCount = 0x300;   // KEY_CNT
inline constexpr int kAbsCount = 0x40;    // ABS_CNT
inline constexpr int kBtnMisc = 0x100;
inline constexpr int kBtnJoystick = 0x120;
inline constexpr int kBtnDigi = 0x140;
inline constexpr int kBtnTriggerHappy = 0x2c0;
inline constexpr int kAbsThrottle = 0x06;   // ABS_THROTTLE, ABS_RUDDER, ABS_WHEEL, ABS_GAS, ABS_BRAKE
inline constexpr int kAbsBrake = 0x0a;
inline constexpr int kAbsHat0X = 0x10;
inline constexpr int kAbsHat3Y = 0x17;
inline constexpr int kAbsMtFirst = 0x2f;  // ABS_MT_SLOT: touch screens and pads from here on

struct AbsInfo {
    std::int32_t value = 0;
    std::int32_t minimum = 0;
    std::int32_t maximum = 0;
    std::int32_t fuzz = 0;
    std::int32_t flat = 0;
    std::int32_t resolution = 0;
};

struct Capabilities {
    std::bitset<kKeyCount> keys;
    std::bitset<kAbsCount> axes;
    std::array<AbsInfo, kAbsCount> absInfo{};
    bool hasRelative = false;   // EV_REL: a mouse
};

// Whether a device is a game controller (wheel, pedals, joystick, gamepad) rather than a keyboard, mouse or
// touch pad: absolute axes plus joystick/gamepad buttons or driving axes (throttle, gas, brake, ...).
[[nodiscard]] bool isGameController(const Capabilities& caps);

class Layout {
public:
    explicit Layout(const Capabilities& caps);

    [[nodiscard]] int axes() const { return static_cast<int>(axisCodes_.size()); }
    [[nodiscard]] int buttons() const { return static_cast<int>(buttonCodes_.size()); }
    [[nodiscard]] int hats() const { return static_cast<int>(hatPairs_.size()); }

    // Starts from the device's current values (EVIOCGABS / EVIOCGKEY): `keysDown` has a bit per key code.
    void reset(const std::array<AbsInfo, kAbsCount>& absInfo, const std::bitset<kKeyCount>& keysDown);

    // One input_event. EV_SYN/SYN_DROPPED means events were lost: returns true, and the caller reads the
    // current values again and calls reset().
    bool apply(int type, int code, std::int32_t value);

    // The state as the service's RawInputState: the first kMaxAxes axes, the first 64 buttons, hat 0.
    void fill(RawInputState& state) const;

    // Axis i's value as SDL would report it, -32768..32767.
    [[nodiscard]] std::int32_t axisValue(int i) const;

private:
    struct Axis {
        int code;
        std::int32_t minimum;
        std::int32_t maximum;
        float scale;
    };
    struct Hat {
        std::array<std::int32_t, 2> minimum;
        std::array<std::int32_t, 2> maximum;
        std::array<int, 2> position{1, 1};   // 0 = negative, 1 = centred, 2 = positive (per axis)
    };

    [[nodiscard]] std::int32_t correct(const Axis& axis, std::int32_t raw) const;
    void setHat(Hat& hat, int axis, std::int32_t value);

    std::vector<Axis> axisCodes_;
    std::vector<int> buttonCodes_;
    std::vector<int> hatPairs_;                // hat index -> its pair (0..3: ABS_HAT0X + 2 * pair)
    std::array<int, kAbsCount> axisIndex_{};   // code -> axis index, -1
    std::array<int, kAbsCount> hatIndex_{};    // code -> hat index, -1
    std::array<int, kKeyCount> buttonIndex_{}; // code -> button index, -1
    std::vector<std::int32_t> axisRaw_;
    std::vector<Hat> hatState_;
    std::uint64_t buttonsDown_ = 0;
};

}  // namespace wheelbri::evdev
