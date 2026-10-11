#include "wheelbri/Mapper.hpp"

#include "wheelbri/Logging.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

namespace wheelbri {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("core.mapper");
    return logger;
}

void logPedal(const char* name, const PedalConfig& pedal) {
    WE_LOG_DEBUG(log(), "mapper: {:<8} axis={} invert={} deadzone_low={:.3f} saturation_high={:.3f} gamma={:.2f}", name,
                 pedal.sourceAxis, pedal.invert, pedal.deadzoneLow, pedal.saturationHigh, pedal.gamma);
}

bool isValidAxis(const int axis) {
    return axis >= 0 && static_cast<std::size_t>(axis) < RawInputState::kMaxAxes;
}

bool isValidDevice(const int device) {
    return device >= 0 && static_cast<std::size_t>(device) < kMaxInputDevices;
}

// The axis' value, or nullopt for an axis that is not mapped (or out of range).
std::optional<float> axisValue(const RawInputs& inputs, const int device, const int axis) {
    if (!isValidDevice(device) || !isValidAxis(axis)) return std::nullopt;
    return inputs.devices[static_cast<std::size_t>(device)].axes[static_cast<std::size_t>(axis)];
}

bool buttonDown(const RawInputs& inputs, const int device, const int button) {
    return isValidDevice(device) && button >= 0 && button < 64 &&
           ((inputs.devices[static_cast<std::size_t>(device)].buttons >> button) & 1U) != 0;
}

}  // namespace

Mapper::Mapper(const MapperConfig& config) : config_(config) {
    const SteeringConfig& steering = config_.steering;
    WE_LOG_DEBUG(log(), "mapper: steering axis={} rotation {:.0f} -> {:.0f} deg (ratio {:.3f}) deadzone={:.3f} invert={}",
                 steering.sourceAxis, steering.sourceRotationDeg, steering.targetRotationDeg,
                 steering.targetRotationDeg > 0.0F ? steering.sourceRotationDeg / steering.targetRotationDeg : 0.0F,
                 steering.deadzone, steering.invert);
    if (!isValidAxis(steering.sourceAxis)) {
        WE_LOG_WARN(log(), "mapper: steering axis {} is out of range, steering will stay centred", steering.sourceAxis);
    }
    if (steering.sourceRotationDeg < steering.targetRotationDeg) {
        WE_LOG_INFO(log(), "mapper: physical wheel turns {:.0f} deg but the target reports {:.0f}: full lock will "
                           "read as {:.0f}% of the target's range",
                    steering.sourceRotationDeg, steering.targetRotationDeg,
                    100.0F * steering.sourceRotationDeg / steering.targetRotationDeg);
    }
    for (std::size_t i = 0; i < config_.accessories.size(); ++i) {
        if (!config_.accessories[i].empty()) {
            WE_LOG_DEBUG(log(), "mapper: device {} = accessory {:04x}:{:04x}", i + 1, config_.accessories[i].vendorId,
                         config_.accessories[i].productId);
        }
    }
    logPedal("throttle", config_.throttle);
    logPedal("brake", config_.brake);
    logPedal("clutch", config_.clutch);
    for (std::size_t target = 0; target < config_.buttonMap.size(); ++target) {
        if (config_.buttonMap[target] != static_cast<int>(target)) {
            WE_LOG_DEBUG(log(), "mapper: button target {} <- source {}", target, config_.buttonMap[target]);
        }
        for (const ButtonSource& alt : config_.buttonSecondary[target]) {
            if (!alt.mapped()) continue;
            WE_LOG_DEBUG(log(), "mapper: button target {} also <- device {} {} {}{}", target, alt.device,
                         alt.axis ? "axis" : "button", alt.index,
                         alt.axis ? std::format(" past {:.0f}%{}", alt.threshold * 100.0F, alt.invert ? " (inverted)" : "")
                                  : std::string());
        }
    }
    const HatButtons& hat = config_.hatButtons;
    if (hat.any()) {
        WE_LOG_DEBUG(log(), "mapper: hat from source buttons up={} right={} down={} left={}", hat.up, hat.right,
                     hat.down, hat.left);
        // A D-pad button that also drives a target button would press both at once in the game.
        const std::array<int, 4> sources = {hat.up, hat.right, hat.down, hat.left};
        for (std::size_t direction = 0; direction < sources.size(); ++direction) {
            const int source = sources[direction];
            for (std::size_t target = 0; source >= 0 && target < config_.buttonMap.size(); ++target) {
                if (config_.buttonMap[target] == source && config_.buttonDevice[target] == hat.devices[direction]) {
                    WE_LOG_WARN(log(), "mapper: source button {} drives the hat and also target button {}; "
                                       "set [buttons] target_{} = -1 unless that is intended",
                                source, target, target);
                }
            }
        }
    }
}

WheelState Mapper::map(const RawInputState& raw) const {
    RawInputs inputs;
    inputs.devices[0] = raw;
    return map(inputs);
}

float Mapper::degreeRatio() const {
    const SteeringConfig& steering = config_.steering;
    return steering.targetRotationDeg > 0.0F ? steering.sourceRotationDeg / steering.targetRotationDeg : 0.0F;
}

WheelState Mapper::map(const RawInputs& inputs) const { return map(inputs, degreeRatio()); }

WheelState Mapper::map(const RawInputs& inputs, const float ratio) const {
    WheelState state;
    state.steering = mapSteering(inputs, std::isfinite(ratio) && ratio > 0.0F ? ratio : degreeRatio());
    state.throttle = mapPedal(config_.throttle, inputs);
    state.brake = mapPedal(config_.brake, inputs);
    state.clutch = mapPedal(config_.clutch, inputs);
    state.buttons = mapButtons(inputs);
    state.hat = mapHat(inputs);
    return state;
}

float Mapper::mapSteering(const RawInputs& inputs, const float ratio) const {
    const SteeringConfig& steering = config_.steering;
    const std::optional<float> raw = axisValue(inputs, steering.sourceDevice, steering.sourceAxis);
    if (!raw || steering.targetRotationDeg <= 0.0F) {
        return 0.0F;
    }

    float value = std::clamp(*raw, -1.0F, 1.0F);
    if (steering.invert) {
        value = -value;
    }

    const float magnitude = std::abs(value);
    if (magnitude <= steering.deadzone) {
        return 0.0F;
    }
    // Rescale so the output leaves the dead zone smoothly instead of jumping.
    const float rescaled = (magnitude - steering.deadzone) / (1.0F - steering.deadzone);

    // Keep degrees 1:1: a 540-degree physical wheel at full lock is only 270 of the target's 450 degrees.
    return std::clamp(std::copysign(rescaled * ratio, value), -1.0F, 1.0F);
}

float Mapper::mapPedal(const PedalConfig& pedal, const RawInputs& inputs) {
    const std::optional<float> raw = axisValue(inputs, pedal.sourceDevice, pedal.sourceAxis);
    if (!raw) {
        return 0.0F;
    }

    // OS axes are bipolar: -1 = one end of travel, 1 = the other.
    const float bipolar = std::clamp(*raw, -1.0F, 1.0F);
    float travel = (bipolar + 1.0F) * 0.5F;
    if (pedal.invert) {
        travel = 1.0F - travel;
    }

    const float span = pedal.saturationHigh - pedal.deadzoneLow;
    if (span <= 0.0F) {
        return 0.0F;
    }
    const float normalised = std::clamp((travel - pedal.deadzoneLow) / span, 0.0F, 1.0F);
    return pedal.gamma == 1.0F ? normalised : std::pow(normalised, pedal.gamma);
}

std::uint32_t Mapper::mapButtons(const RawInputs& inputs) const {
    std::uint32_t target = 0;
    for (std::size_t i = 0; i < config_.buttonMap.size(); ++i) {
        bool down = buttonDown(inputs, config_.buttonDevice[i], config_.buttonMap[i]);
        for (std::size_t s = 0; s < MapperConfig::kMaxSecondaries; ++s) {
            const ButtonSource& alt = config_.buttonSecondary[i][s];
            if (!alt.mapped()) continue;
            if (!alt.axis) {
                down = down || buttonDown(inputs, alt.device, alt.index);
                continue;
            }
            // An axis: its travel from where it rests; pressed from the threshold, released kAxisButtonRelease below
            // it, so a lever resting right at the threshold does not flicker. Every axis is followed (not only until
            // one source is down), so each latch stays right.
            const std::optional<float> raw = axisValue(inputs, alt.device, alt.index);
            const float v = std::clamp(raw.value_or(alt.invert ? 1.0F : -1.0F), -1.0F, 1.0F);
            const float travel = alt.invert ? (1.0F - v) * 0.5F : (v + 1.0F) * 0.5F;
            bool& latched = axisDown_[i][s];
            latched = latched ? travel >= alt.threshold - kAxisButtonRelease : travel >= alt.threshold;
            down = down || latched;
        }
        if (down) target |= 1U << i;
    }
    return target;
}

std::int8_t Mapper::mapHat(const RawInputs& inputs) const {
    const HatButtons& hat = config_.hatButtons;
    const std::int8_t wheelHat = inputs.devices[0].hat;   // a real hat: the wheel's
    if (wheelHat != kHatCentered || !hat.any()) {
        return wheelHat;
    }
    auto pressed = [&inputs, &hat](const int source, const std::size_t direction) {
        return buttonDown(inputs, hat.devices[direction], source);
    };
    // Opposite directions cancel, like a real D-pad that cannot report both.
    const int vertical = (pressed(hat.up, 0) ? 1 : 0) - (pressed(hat.down, 2) ? 1 : 0);      // +1 up, -1 down
    const int horizontal = (pressed(hat.right, 1) ? 1 : 0) - (pressed(hat.left, 3) ? 1 : 0);  // +1 right, -1 left

    // Index [vertical + 1][horizontal + 1] -> hat position, clockwise from up.
    static constexpr std::int8_t kHat[3][3] = {
        {5, 4, 3},             // down-left, down, down-right
        {6, kHatCentered, 2},  // left, centre, right
        {7, 0, 1},             // up-left, up, up-right
    };
    return kHat[vertical + 1][horizontal + 1];
}

std::vector<std::string> missingMappedAxes(const MapperConfig& config,
                                           const std::array<int, kMaxInputDevices>& axisCounts) {
    std::vector<std::string> problems;
    const auto check = [&](const char* what, const int axis, const int device) {
        if (axis < 0 || device < 0 || static_cast<std::size_t>(device) >= axisCounts.size()) return;
        const int count = axisCounts[static_cast<std::size_t>(device)];
        if (count < 0 || axis < count) return;
        problems.push_back(std::string(what) + ": axis " + std::to_string(axis) + " of " +
                           (device == 0 ? std::string("the wheel") : "accessory " + std::to_string(device)) +
                           ", which has " + std::to_string(count) + (count == 1 ? " axis" : " axes"));
    };
    check("steering", config.steering.sourceAxis, config.steering.sourceDevice);
    check("throttle", config.throttle.sourceAxis, config.throttle.sourceDevice);
    check("brake", config.brake.sourceAxis, config.brake.sourceDevice);
    check("clutch", config.clutch.sourceAxis, config.clutch.sourceDevice);
    return problems;
}

}  // namespace wheelbri
