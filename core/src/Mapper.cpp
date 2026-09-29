#include "wheelemu/Mapper.hpp"

#include "wheelemu/Logging.hpp"

#include <algorithm>
#include <cmath>

namespace wheelemu {

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
    logPedal("throttle", config_.throttle);
    logPedal("brake", config_.brake);
    logPedal("clutch", config_.clutch);
    for (std::size_t target = 0; target < config_.buttonMap.size(); ++target) {
        if (config_.buttonMap[target] != static_cast<int>(target)) {
            WE_LOG_DEBUG(log(), "mapper: button target {} <- source {}", target, config_.buttonMap[target]);
        }
    }
    const HatButtons& hat = config_.hatButtons;
    if (hat.any()) {
        WE_LOG_DEBUG(log(), "mapper: hat from source buttons up={} right={} down={} left={}", hat.up, hat.right,
                     hat.down, hat.left);
        // A D-pad button that also drives a target button would press both at once in the game.
        for (const int source : {hat.up, hat.right, hat.down, hat.left}) {
            for (std::size_t target = 0; source >= 0 && target < config_.buttonMap.size(); ++target) {
                if (config_.buttonMap[target] == source) {
                    WE_LOG_WARN(log(), "mapper: source button {} drives the hat and also target button {}; "
                                       "set [buttons] target_{} = -1 unless that is intended",
                                source, target, target);
                }
            }
        }
    }
}

WheelState Mapper::map(const RawInputState& raw) const {
    WheelState state;
    state.steering = mapSteering(raw);
    state.throttle = mapPedal(config_.throttle, raw);
    state.brake = mapPedal(config_.brake, raw);
    state.clutch = mapPedal(config_.clutch, raw);
    state.buttons = mapButtons(raw.buttons);
    state.hat = mapHat(raw);
    return state;
}

float Mapper::mapSteering(const RawInputState& raw) const {
    const SteeringConfig& steering = config_.steering;
    if (!isValidAxis(steering.sourceAxis) || steering.targetRotationDeg <= 0.0F) {
        return 0.0F;
    }

    float value = std::clamp(raw.axes[static_cast<std::size_t>(steering.sourceAxis)], -1.0F, 1.0F);
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
    const float degreeRatio = steering.sourceRotationDeg / steering.targetRotationDeg;
    return std::clamp(std::copysign(rescaled * degreeRatio, value), -1.0F, 1.0F);
}

float Mapper::mapPedal(const PedalConfig& pedal, const RawInputState& raw) {
    if (!isValidAxis(pedal.sourceAxis)) {
        return 0.0F;
    }

    // OS axes are bipolar: -1 = one end of travel, 1 = the other.
    const float bipolar = std::clamp(raw.axes[static_cast<std::size_t>(pedal.sourceAxis)], -1.0F, 1.0F);
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

std::uint32_t Mapper::mapButtons(const std::uint64_t sourceButtons) const {
    std::uint32_t target = 0;
    for (std::size_t i = 0; i < config_.buttonMap.size(); ++i) {
        const int source = config_.buttonMap[i];
        if (source >= 0 && source < 64 && ((sourceButtons >> source) & 1U) != 0) {
            target |= 1U << i;
        }
    }
    return target;
}

std::int8_t Mapper::mapHat(const RawInputState& raw) const {
    const HatButtons& hat = config_.hatButtons;
    if (raw.hat != kHatCentered || !hat.any()) {
        return raw.hat;
    }
    auto pressed = [&raw](const int source) {
        return source >= 0 && source < 64 && ((raw.buttons >> source) & 1U) != 0;
    };
    // Opposite directions cancel, like a real D-pad that cannot report both.
    const int vertical = (pressed(hat.up) ? 1 : 0) - (pressed(hat.down) ? 1 : 0);      // +1 up, -1 down
    const int horizontal = (pressed(hat.right) ? 1 : 0) - (pressed(hat.left) ? 1 : 0);  // +1 right, -1 left

    // Index [vertical + 1][horizontal + 1] -> hat position, clockwise from up.
    static constexpr std::int8_t kHat[3][3] = {
        {5, 4, 3},             // down-left, down, down-right
        {6, kHatCentered, 2},  // left, centre, right
        {7, 0, 1},             // up-left, up, up-right
    };
    return kHat[vertical + 1][horizontal + 1];
}

}  // namespace wheelemu
