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

ButtonLayout layoutButtons(const IDeviceProfile& profile) {
    ButtonLayout layout;
    std::vector<ButtonGroup> groups = profile.buttonGroups();
    // Named buttons no group has (and every one, without groups): Below, in number order.
    std::vector<bool> placed(profile.buttonCount(), false);
    for (ButtonGroup& group : groups) {
        std::erase_if(group.buttons, [&](const std::size_t b) {
            if (b >= placed.size() || placed[b] || profile.buttonName(b).empty()) return true;   // unknown or twice
            placed[b] = true;
            return false;
        });
    }
    ButtonGroup rest{groups.empty() ? std::string() : std::string("OTHER"), {}, ButtonGroup::Place::Below};
    for (std::size_t b = 0; b < placed.size(); ++b) {
        if (!placed[b] && !profile.buttonName(b).empty()) rest.buttons.push_back(b);
    }
    if (!rest.buttons.empty()) groups.push_back(std::move(rest));

    float columnY[2] = {0.0F, 0.0F};
    const auto heading = [&](const int column, const float y, const std::string& name) {
        if (name.empty()) return 0.0F;
        layout.cells.push_back({column, y, true, name, 0});
        return kButtonHeadingH;
    };
    for (const ButtonGroup& group : groups) {   // the columns first
        if (group.place == ButtonGroup::Place::Below || group.buttons.empty()) continue;
        const int column = group.place == ButtonGroup::Place::Left ? 0 : 1;
        float& y = columnY[column];
        y += heading(column, y, group.name);
        for (const std::size_t b : group.buttons) {
            layout.cells.push_back({column, y, false, {}, b});
            y += kButtonRowH;
        }
    }
    float y = std::max(columnY[0], columnY[1]);
    for (const ButtonGroup& group : groups) {   // then below both, two a row
        if (group.place != ButtonGroup::Place::Below || group.buttons.empty()) continue;
        y += heading(0, y, group.name);
        for (std::size_t i = 0; i < group.buttons.size(); ++i) {
            layout.cells.push_back({static_cast<int>(i % 2), y, false, {}, group.buttons[i]});
            if (i % 2 == 1 || i + 1 == group.buttons.size()) y += kButtonRowH;
        }
    }
    layout.height = y;
    return layout;
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
    using Place = ButtonGroup::Place;
    spec.buttonGroups = {
        {"FACE", {0, 1, 2, 3}, Place::Left},             // A, B, X, Y
        {"PADDLES", {5, 4}, Place::Left},                // L Paddle, R Paddle
        {"STICKS", {9, 8}, Place::Right},                // LSB, RSB
        {"SYSTEM", {6, 7, 10}, Place::Right},            // Menu, View, Xbox
        {"SHIFTER", {12, 13, 14, 15, 16, 17, 18}, Place::Below},
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
    using Place = ButtonGroup::Place;
    spec.buttonGroups = {
        {"BLACK", {0, 1, 2, 3}, Place::Left},            // the shifter's black buttons
        {"PADDLES", {5, 4}, Place::Left},                // L Paddle, R Paddle
        {"WHEEL", {7, 6}, Place::Right},                 // L Wheel, R Wheel
        {"RED", {15, 16, 17, 18}, Place::Right},         // the shifter's red buttons
        {"SHIFTER", {8, 9, 10, 11, 12, 13, 14}, Place::Below},
    };
    return spec;
}

DeviceProfileSpec g27Spec() {
    DeviceProfileSpec spec;
    spec.identity.shortName = "G27";
    spec.identity.vendorId = 0x046D;
    spec.identity.productId = 0xC29B;
    spec.identity.productName = "Logitech G27 Racing Wheel USB";
    spec.identity.rotationDeg = 900.0F;
    spec.steeringRange = {0, 65535};
    spec.pedalRange = {0, 65535};
    spec.pedalsReleasedAtMax = true;   // as the G25's
    // kG27Buttons: from users' reports, unverified (docs/g27-target-plan.md).
    spec.buttonNames = {
        "Red 1",     "Red 2",     "Red 3",     "Red 4",     "R Paddle", "L Paddle", "R Wheel 1", "L Wheel 1",
        "Gear 1",    "Gear 2",    "Gear 3",    "Gear 4",    "Gear 5",   "Gear 6",   "Gear R",
        "Black 1",   "Black 2",   "Black 3",   "Black 4",
        "R Wheel 2", "L Wheel 2", "R Wheel 3", "L Wheel 3",
    };
    using Place = ButtonGroup::Place;
    spec.buttonGroups = {
        {"WHEEL", {7, 6, 20, 19, 22, 21}, Place::Left},  // left and right, top to bottom
        {"PADDLES", {5, 4}, Place::Left},                // L Paddle, R Paddle
        {"RED", {0, 1, 2, 3}, Place::Right},             // the shifter's red buttons
        {"BLACK", {15, 16, 17, 18}, Place::Right},       // the shifter's black buttons
        {"SHIFTER", {8, 9, 10, 11, 12, 13, 14}, Place::Below},
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
    if (name == "g27") {
        return std::make_unique<DeviceProfile>(g27Spec());
    }
    return nullptr;
}

}  // namespace wheelbri
