#include "wheelbri/DeviceProfile.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace wheelbri {

namespace {

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

DeviceProfile::DeviceProfile(DeviceProfileSpec spec) : spec_(std::move(spec)) {}

TargetReport DeviceProfile::encode(const WheelState& state) const {
    TargetReport report;
    const float steeringFraction = (std::clamp(state.steering, -1.0F, 1.0F) + 1.0F) * 0.5F;
    report.steering = lerp(spec_.steeringRange, steeringFraction);
    report.throttle = encodePedal(state.throttle);
    report.brake = encodePedal(state.brake);
    report.clutch = encodePedal(state.clutch);
    report.buttons = state.buttons;
    report.hat = state.hat;
    return report;
}

std::string_view DeviceProfile::buttonName(const std::size_t index) const {
    return index < spec_.buttonNames.size() ? std::string_view(spec_.buttonNames[index]) : std::string_view();
}

std::int32_t DeviceProfile::encodePedal(const float travel) const {
    const float clamped = std::clamp(travel, 0.0F, 1.0F);
    return lerp(spec_.pedalRange, spec_.pedalsReleasedAtMax ? 1.0F - clamped : clamped);
}

DeviceProfileSpec g29Spec() {
    DeviceProfileSpec spec;
    spec.identity.shortName = "G29";
    spec.identity.vendorId = 0x046D;
    spec.identity.productId = 0xC24F;
    spec.identity.productName = "Logitech G29 Driving Force Racing Wheel USB";
    spec.identity.rotationDeg = 900.0F;
    spec.steeringRange = {0, 65535};
    spec.pedalRange = {0, 65535};
    spec.pedalsReleasedAtMax = true;
    // DirectInput button order of the G29 in PC mode, as documented by the sim-racing community
    // (buttons 1..25 in joy.cpl). Unverified against a real G29, like the rest of this profile.
    spec.buttonNames = {
        "Cross",   "Square",  "Circle",  "Triangle", "R paddle", "L paddle",   "R2",        "L2",    "Share",
        "Options", "R3",      "L3",      "Gear 1",   "Gear 2",   "Gear 3",     "Gear 4",    "Gear 5", "Gear 6",
        "Gear R",  "Plus",    "Minus",   "Dial CW",  "Dial CCW", "Enter",      "PS",
    };
    return spec;
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
    spec.pedalsReleasedAtMax = true;   // same pedal set and encoding as the G29 (assumed)
    // DirectInput button order in PC mode (joy.cpl buttons 1..11). The optional Driving Force Shifter adds
    // more buttons after these; their order is not known yet, so they are left unnamed.
    spec.buttonNames = {
        "A", "B", "X", "Y", "R paddle", "L paddle", "Menu", "View", "RSB", "LSB", "Xbox",
    };
    return spec;
}

std::unique_ptr<IDeviceProfile> makeProfile(const std::string_view name) {
    if (name == "g920") {
        return std::make_unique<DeviceProfile>(g920Spec());
    }
    if (name == "g29") {
        return std::make_unique<DeviceProfile>(g29Spec());
    }
    return nullptr;
}

}  // namespace wheelbri
