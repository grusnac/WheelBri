#include "wheelbri/DeviceProfile.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace wheelbri {

namespace {

// A value that is not a number (NaN, infinity) as `neutral`: std::clamp lets NaN through, and lround(NaN) is
// INT32_MIN on Windows, which the game would read as full left lock.
float finiteOr(const float value, const float neutral) { return std::isfinite(value) ? value : neutral; }

std::int32_t lerp(const AxisRange& range, const float fraction) {
    const double span = static_cast<double>(range.max) - static_cast<double>(range.min);
    return static_cast<std::int32_t>(std::lround(range.min + span * static_cast<double>(fraction)));
}

}  // namespace

std::string toString(const Guid& guid) {
    char buffer[39];
    std::snprintf(buffer, sizeof(buffer), "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                  static_cast<unsigned>(guid.data1), guid.data2, guid.data3,
                  guid.data4[0], guid.data4[1], guid.data4[2], guid.data4[3],
                  guid.data4[4], guid.data4[5], guid.data4[6], guid.data4[7]);
    return buffer;
}

Guid DeviceIdentity::directInputProductGuid() const {
    Guid guid;
    guid.data1 = (static_cast<std::uint32_t>(productId) << 16) | vendorId;
    guid.data4 = {0x00, 0x00, 'P', 'I', 'D', 'V', 'I', 'D'};
    return guid;
}

DeviceProfile::DeviceProfile(DeviceProfileSpec spec) : spec_(std::move(spec)) {
    for (std::size_t i = 0; i < spec_.buttonNames.size() && i < 32; ++i) {
        if (!spec_.buttonNames[i].empty()) buttonMask_ |= 1U << i;
    }
}

TargetReport DeviceProfile::encode(const WheelState& state) const {
    TargetReport report;
    const float steeringFraction = (std::clamp(finiteOr(state.steering, 0.0F), -1.0F, 1.0F) + 1.0F) * 0.5F;
    report.steering = lerp(spec_.steeringRange, steeringFraction);
    report.throttle = encodePedal(state.throttle);
    report.brake = encodePedal(state.brake);
    report.clutch = encodePedal(state.clutch);
    report.buttons = state.buttons & buttonMask_;   // a mapped button the wheel does not have never reaches the game
    report.hat = state.hat;
    return report;
}

std::string_view DeviceProfile::buttonName(const std::size_t index) const {
    return index < spec_.buttonNames.size() ? std::string_view(spec_.buttonNames[index]) : std::string_view();
}

std::int32_t DeviceProfile::encodePedal(const float travel) const {
    const float clamped = std::clamp(finiteOr(travel, 0.0F), 0.0F, 1.0F);   // not a number: released
    return lerp(spec_.pedalRange, spec_.pedalsReleasedAtMax ? 1.0F - clamped : clamped);
}

DeviceProfileSpec g920Spec() {
    DeviceProfileSpec spec;
    spec.identity.shortName = "G920";
    spec.identity.vendorId = 0x046D;
    spec.identity.productId = 0xC262;
    spec.identity.productName = "Logitech G920 Driving Force Racing Wheel USB";
    spec.identity.rotationDeg = 900.0F;
    spec.steeringRange = {0, 65535};
    spec.pedalRange = {0, 65535};
    spec.pedalsReleasedAtMax = true;   // pressed = the low end (assumed from Logitech's pedal sets)
    // DirectInput button order in PC mode (joy.cpl buttons 1..11), then the Driving Force Shifter on the wheel's
    // shifter port: joy.cpl 13..18 for gears 1..6 and 19 for reverse (Logitech's Driving Force Shifter FAQ). joy.cpl
    // 12 exists on the G920 but nothing presses it.
    spec.buttonNames = {
        "A",      "B",      "X",      "Y",      "R Paddle", "L Paddle", "Menu", "View", "RSB", "LSB",
        "Xbox",   "",       "Gear 1", "Gear 2", "Gear 3",   "Gear 4",   "Gear 5", "Gear 6", "Gear R",
    };
    return spec;
}

DeviceProfileSpec g25Spec() {
    DeviceProfileSpec spec;
    spec.identity.shortName = "G25";
    spec.identity.vendorId = 0x046D;
    spec.identity.productId = 0xC299;
    spec.identity.productName = "Logitech G25 Racing Wheel USB";
    spec.identity.rotationDeg = 900.0F;
    spec.steeringRange = {0, 65535};
    spec.pedalRange = {0, 65535};
    spec.pedalsReleasedAtMax = true;   // Logitech's pedals of that time report released as the top of the range
    // Gears 1-6 and reverse at 8..14 (Logitech's numbering); the rest is a guess (kG25Buttons): the shifter's 4 black
    // buttons, the paddles, the wheel's two buttons, then the shifter's 4 red buttons.
    spec.buttonNames = {
        "Black 1", "Black 2", "Black 3", "Black 4", "R Paddle", "L Paddle", "R Wheel", "L Wheel",
        "Gear 1",  "Gear 2",  "Gear 3",  "Gear 4",  "Gear 5",   "Gear 6",   "Gear R",
        "Red 1",   "Red 2",   "Red 3",   "Red 4",
    };
    return spec;
}

std::unique_ptr<IDeviceProfile> makeProfile(const std::string_view name) {
    if (name == "g920") {
        return std::make_unique<DeviceProfile>(g920Spec());
    }
    if (name == "g25") {
        return std::make_unique<DeviceProfile>(g25Spec());
    }
    return nullptr;
}

}  // namespace wheelbri
