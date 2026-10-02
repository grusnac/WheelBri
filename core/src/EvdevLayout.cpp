#include "wheelemu/EvdevLayout.hpp"

#include <algorithm>
#include <cmath>

namespace wheelemu::evdev {

namespace {

constexpr std::int32_t kAxisMin = -32768;
constexpr std::int32_t kAxisMax = 32767;

// SDL's GuessIfAxesAreDigitalHat (SDL_JOYSTICK_LINUX_DIGITAL_HATS not set).
bool looksDigital(const AbsInfo* x, const AbsInfo* y) {
    if (x == nullptr && y == nullptr) {
        return false;
    }
    const auto unit = [](const AbsInfo* a) { return a == nullptr || (a->minimum == -1 && a->maximum == 1); };
    if (unit(x) && unit(y)) {
        return true;
    }
    const auto plain = [](const AbsInfo* a) { return a == nullptr || (a->fuzz == 0 && a->flat == 0 && a->resolution == 0); };
    return plain(x) && plain(y);
}

// [y][x] position (0 negative, 1 centre, 2 positive) -> 0..7 clockwise from up, or centred.
constexpr std::int8_t kHatIndex[3][3] = {
    {7, 0, 1},
    {6, kHatCentered, 2},
    {5, 4, 3},
};

}  // namespace

bool isGameController(const Capabilities& caps) {
    if (caps.hasRelative) {
        return false;   // a mouse (or a trackball)
    }
    bool anyAxis = false;
    for (int code = 0; code < kAbsMtFirst; ++code) {
        anyAxis = anyAxis || caps.axes.test(static_cast<std::size_t>(code));
    }
    if (!anyAxis) {
        return false;
    }
    // Pedals and other driving controls: throttle, rudder, wheel, gas or brake axes, with or without buttons.
    for (int code = kAbsThrottle; code <= kAbsBrake; ++code) {
        if (caps.axes.test(static_cast<std::size_t>(code))) return true;
    }
    // BTN_0..BTN_9: joysticks that number their buttons that way.
    for (int code = kBtnMisc; code < kBtnMisc + 10; ++code) {
        if (caps.keys.test(static_cast<std::size_t>(code))) return true;
    }
    // Joystick and gamepad buttons (BTN_JOYSTICK..BTN_THUMBR, below the BTN_DIGI touch/pen codes), or the
    // extra "trigger happy" buttons many wheels and button boxes use. Touch pads have BTN_TOUCH (BTN_DIGI range).
    for (int code = kBtnJoystick; code < kBtnDigi; ++code) {
        if (caps.keys.test(static_cast<std::size_t>(code))) return true;
    }
    for (int code = kBtnTriggerHappy; code < kBtnTriggerHappy + 0x28; ++code) {
        if (caps.keys.test(static_cast<std::size_t>(code))) return true;
    }
    return false;
}

Layout::Layout(const Capabilities& caps) {
    axisIndex_.fill(-1);
    hatIndex_.fill(-1);
    buttonIndex_.fill(-1);

    for (int code = kBtnJoystick; code < kKeyCount - 1; ++code) {   // up to KEY_MAX, exclusive (SDL: i < KEY_MAX)
        if (caps.keys.test(static_cast<std::size_t>(code))) {
            buttonIndex_[static_cast<std::size_t>(code)] = static_cast<int>(buttonCodes_.size());
            buttonCodes_.push_back(code);
        }
    }
    for (int code = 0; code < kBtnJoystick; ++code) {
        if (caps.keys.test(static_cast<std::size_t>(code))) {
            buttonIndex_[static_cast<std::size_t>(code)] = static_cast<int>(buttonCodes_.size());
            buttonCodes_.push_back(code);
        }
    }

    for (int code = kAbsHat0X; code <= kAbsHat3Y; code += 2) {
        const bool hasX = caps.axes.test(static_cast<std::size_t>(code));
        const bool hasY = caps.axes.test(static_cast<std::size_t>(code + 1));
        const AbsInfo* x = hasX ? &caps.absInfo[static_cast<std::size_t>(code)] : nullptr;
        const AbsInfo* y = hasY ? &caps.absInfo[static_cast<std::size_t>(code + 1)] : nullptr;
        if (!looksDigital(x, y)) {
            continue;
        }
        const int index = static_cast<int>(hatPairs_.size());
        hatPairs_.push_back((code - kAbsHat0X) / 2);
        hatIndex_[static_cast<std::size_t>(code)] = index;
        hatIndex_[static_cast<std::size_t>(code + 1)] = index;
        Hat hat;
        hat.minimum = {x != nullptr ? x->minimum : -1, y != nullptr ? y->minimum : -1};
        hat.maximum = {x != nullptr ? x->maximum : 1, y != nullptr ? y->maximum : 1};
        hatState_.push_back(hat);
    }

    for (int code = 0; code < kAbsCount - 1; ++code) {   // SDL: i < ABS_MAX
        if (hatIndex_[static_cast<std::size_t>(code)] >= 0 || !caps.axes.test(static_cast<std::size_t>(code))) {
            continue;
        }
        const AbsInfo& info = caps.absInfo[static_cast<std::size_t>(code)];
        Axis axis{code, info.minimum, info.maximum, 1.0F};
        if (info.maximum != info.minimum) {
            axis.scale = static_cast<float>(kAxisMax - kAxisMin) / static_cast<float>(info.maximum - info.minimum);
        }
        axisIndex_[static_cast<std::size_t>(code)] = static_cast<int>(axisCodes_.size());
        axisCodes_.push_back(axis);
    }
    axisRaw_.assign(axisCodes_.size(), 0);
    std::array<AbsInfo, kAbsCount> values = caps.absInfo;
    reset(values, {});
}

void Layout::reset(const std::array<AbsInfo, kAbsCount>& absInfo, const std::bitset<kKeyCount>& keysDown) {
    for (std::size_t i = 0; i < axisCodes_.size(); ++i) {
        axisRaw_[i] = absInfo[static_cast<std::size_t>(axisCodes_[i].code)].value;
    }
    for (std::size_t i = 0; i < hatPairs_.size(); ++i) {
        const int code = kAbsHat0X + 2 * hatPairs_[i];
        setHat(hatState_[i], 0, absInfo[static_cast<std::size_t>(code)].value);
        setHat(hatState_[i], 1, absInfo[static_cast<std::size_t>(code + 1)].value);
    }
    buttonsDown_ = 0;
    for (std::size_t i = 0; i < buttonCodes_.size() && i < 64; ++i) {
        if (keysDown.test(static_cast<std::size_t>(buttonCodes_[i]))) {
            buttonsDown_ |= 1ULL << i;
        }
    }
}

bool Layout::apply(const int type, const int code, const std::int32_t value) {
    if (type == kEvKey) {
        if (code >= 0 && code < kKeyCount) {
            const int index = buttonIndex_[static_cast<std::size_t>(code)];
            if (index >= 0 && index < 64) {
                if (value != 0) {
                    buttonsDown_ |= 1ULL << index;
                } else {
                    buttonsDown_ &= ~(1ULL << index);
                }
            }
        }
    } else if (type == kEvAbs) {
        if (code >= 0 && code < kAbsCount) {
            if (const int hat = hatIndex_[static_cast<std::size_t>(code)]; hat >= 0) {
                setHat(hatState_[static_cast<std::size_t>(hat)], (code - kAbsHat0X) % 2, value);
            } else if (const int axis = axisIndex_[static_cast<std::size_t>(code)]; axis >= 0) {
                axisRaw_[static_cast<std::size_t>(axis)] = value;
            }
        }
    } else if (type == kEvSyn && code == kSynDropped) {
        return true;
    }
    return false;
}

std::int32_t Layout::correct(const Axis& axis, std::int32_t raw) const {
    if (axis.minimum != axis.maximum) {
        raw = static_cast<std::int32_t>(
            std::floor(static_cast<float>(raw - axis.minimum) * axis.scale + static_cast<float>(kAxisMin) + 0.5F));
    }
    return std::clamp(raw, kAxisMin, kAxisMax);
}

std::int32_t Layout::axisValue(const int i) const {
    if (i < 0 || i >= axes()) {
        return 0;
    }
    return correct(axisCodes_[static_cast<std::size_t>(i)], axisRaw_[static_cast<std::size_t>(i)]);
}

// SDL's HandleHat with its default hat dead zones (a third of the range).
void Layout::setHat(Hat& hat, const int axis, std::int32_t value) {
    const auto a = static_cast<std::size_t>(axis);
    int position = 1;
    if (value < 0) {
        if (value <= hat.minimum[a]) {
            hat.minimum[a] = value;
            position = 0;
        } else {
            position = value < hat.minimum[a] / 3 ? 0 : 1;
        }
    } else if (value > 0) {
        if (value >= hat.maximum[a]) {
            hat.maximum[a] = value;
            position = 2;
        } else {
            position = value > hat.maximum[a] / 3 ? 2 : 1;
        }
    }
    hat.position[a] = position;
}

void Layout::fill(RawInputState& state) const {
    const std::size_t count = std::min(axisCodes_.size(), RawInputState::kMaxAxes);
    for (std::size_t i = 0; i < count; ++i) {
        state.axes[i] = std::clamp(static_cast<float>(axisValue(static_cast<int>(i))) / 32767.0F, -1.0F, 1.0F);
    }
    state.buttons = buttonsDown_;
    if (hatState_.empty()) {
        state.hat = kHatCentered;
    } else {
        const Hat& hat = hatState_.front();
        state.hat = kHatIndex[hat.position[1]][hat.position[0]];
    }
}

}  // namespace wheelemu::evdev
