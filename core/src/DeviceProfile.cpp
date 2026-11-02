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

// The G25's and G27's native button order (DeviceProfileSpec::nativeButtonOrder, kG25Buttons, kG27Buttons): for each
// button as named (Logitech's driver's order), its place in the wheel's own HID order.
const std::vector<std::uint8_t> kG25NativeOrder = {
    11, 8, 9, 10,                    // Red 1-4 (red 2, 3, 4, 1 natively)
    4, 5, 6, 7,                      // R Paddle, L Paddle, R Wheel, L Wheel: the same
    12, 13, 14, 15, 16, 17, 18,      // Gear 1-6, Gear R
    3, 1, 0, 2,                      // Blk top, left, bottom, right (bottom, left, right, top natively)
};
const std::vector<std::uint8_t> kG27NativeOrder = {
    11, 8, 9, 10,
    4, 5, 6, 7,
    12, 13, 14, 15, 16, 17, 22,      // reverse after the wheel's lower buttons
    3, 1, 0, 2,
    18, 20, 19, 21,                  // R Wheel 2, L Wheel 2, R Wheel 3, L Wheel 3
};

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

std::string_view toString(const ButtonOrder order) { return order == ButtonOrder::Native ? "native" : "driver"; }

std::optional<ButtonOrder> parseButtonOrder(const std::string_view text) {
    if (text == "driver") return ButtonOrder::Driver;
    if (text == "native") return ButtonOrder::Native;
    return std::nullopt;
}

int gameButtonNumber(const IDeviceProfile& profile, const ButtonOrder order, const std::size_t index) {
    if (index >= 32) return -1;
    if (order != ButtonOrder::Native || !profile.hasNativeButtonOrder()) return static_cast<int>(index) + 1;
    const std::uint32_t native = profile.nativeButtons(1U << index);
    for (int bit = 0; bit < 32; ++bit) {
        if ((native >> bit) & 1U) return bit;
    }
    return -1;
}

DeviceProfile::DeviceProfile(DeviceProfileSpec spec) : spec_(std::move(spec)) {
    for (std::size_t i = 0; i < spec_.buttonNames.size() && i < 32; ++i) {
        if (!spec_.buttonNames[i].empty()) buttonMask_ |= 1U << i;
    }
    for (std::size_t i = 0; i < native_.size(); ++i) {
        const bool given = i < spec_.nativeButtonOrder.size() && spec_.nativeButtonOrder[i] < native_.size();
        native_[i] = given ? spec_.nativeButtonOrder[i] : static_cast<std::uint8_t>(i);
    }
}

std::uint32_t DeviceProfile::nativeButtons(const std::uint32_t buttons) const {
    if (spec_.nativeButtonOrder.empty()) return buttons;
    std::uint32_t out = 0;
    for (std::size_t i = 0; i < native_.size(); ++i) {
        if ((buttons >> i) & 1U) out |= 1U << native_[i];
    }
    return out;
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

ButtonLayout layoutButtons(const IDeviceProfile& profile, const std::function<bool(const ButtonGroup&)>& fold) {
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

    const auto folded = [&fold](const ButtonGroup& group) { return !group.name.empty() && fold && fold(group); };
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
        if (folded(group)) {
            layout.cells.push_back({column, y, true, group.name, 0, true});
            y += kButtonRowH;
            continue;
        }
        y += heading(column, y, group.name);
        for (const std::size_t b : group.buttons) {
            layout.cells.push_back({column, y, false, {}, b});
            y += kButtonRowH;
        }
    }
    float y = std::max(columnY[0], columnY[1]);
    for (const ButtonGroup& group : groups) {   // then below both, two a row
        if (group.place != ButtonGroup::Place::Below || group.buttons.empty()) continue;
        if (folded(group)) {
            layout.cells.push_back({0, y, true, group.name, 0, true});
            y += kButtonRowH;
            continue;
        }
        y += heading(0, y, group.name);
        for (std::size_t i = 0; i < group.buttons.size(); ++i) {
            layout.cells.push_back({static_cast<int>(i % 2), y, false, {}, group.buttons[i]});
            if (i % 2 == 1 || i + 1 == group.buttons.size()) y += kButtonRowH;
        }
    }
    layout.height = y;
    return layout;
}

int DeviceProfile::coreButton(const CoreButton button) const {
    const auto index = static_cast<std::size_t>(button);
    return index < spec_.coreButtons.size() ? spec_.coreButtons[index] : -1;
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
    // Pressed = the low end: a real G923's pedals rest at 255, the top (its USB capture, 2026-11-01); the G920's,
    // the same family, assumed alike.
    spec.pedalsReleasedAtMax = true;
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
    spec.coreButtons = {0, 1, 2, 3, 5, 4};   // A, B, X, Y, L Paddle, R Paddle
    return spec;
}

DeviceProfileSpec g25Spec() {
    DeviceProfileSpec spec;
    spec.identity.shortName = "G25";
    spec.identity.vendorId = 0x046D;
    spec.identity.productId = 0xC299;
    spec.identity.productName = "Logitech G25 Racing Wheel USB";
    spec.identity.rotationDeg = 900.0F;
    spec.identity.centringSpring = true;   // Logitech's software had one for it (CentringSpring.hpp)
    spec.steeringRange = {0, 65535};
    spec.pedalRange = {0, 65535};
    spec.pedalsReleasedAtMax = true;   // Logitech's pedals of that time report released as the top of the range
    // As games of its time saw it on Windows (Logitech's driver), from several sources (docs/targets.md): the
    // shifter's 4 red buttons 0-3 (left to right), the paddles 4 (right) and 5 (left), the wheel's two buttons
    // 6 (right) and 7 (left), gears 1-6 and reverse 8-14 (Logitech's own numbering; in sequential mode the shifter
    // presses 8 and 9), the shifter's 4 black buttons 15-18 (its diamond: top, left, bottom, right). The D-pad is a
    // hat. The native order (kG25Buttons) besides it.
    spec.buttonNames = {
        "Red 1",  "Red 2",  "Red 3",  "Red 4",  "R Paddle", "L Paddle", "R Wheel", "L Wheel",
        "Gear 1", "Gear 2", "Gear 3", "Gear 4", "Gear 5",   "Gear 6",   "Gear R",
        "Blk top", "Blk left", "Blk bottom", "Blk right",
    };
    using Place = ButtonGroup::Place;
    spec.buttonGroups = {
        {"RED", {0, 1, 2, 3}, Place::Left},              // the shifter's red buttons, left to right
        {"PADDLES", {5, 4}, Place::Left},                // L Paddle, R Paddle
        {"WHEEL", {7, 6}, Place::Right},                 // L Wheel, R Wheel
        {"BLACK", {15, 16, 17, 18}, Place::Right},       // the shifter's black diamond
        {"SHIFTER", {8, 9, 10, 11, 12, 13, 14}, Place::Below},
    };
    spec.nativeButtonOrder = kG25NativeOrder;
    // An Xbox-style rim's A, B, X, Y on the black diamond by place (bottom 17, right 18, left 16, top 15); paddles.
    spec.coreButtons = {17, 18, 16, 15, 5, 4};
    return spec;
}

DeviceProfileSpec g27Spec() {
    DeviceProfileSpec spec;
    spec.identity.shortName = "G27";
    spec.identity.vendorId = 0x046D;
    spec.identity.productId = 0xC29B;
    spec.identity.productName = "Logitech G27 Racing Wheel USB";
    spec.identity.rotationDeg = 900.0F;
    spec.identity.centringSpring = true;   // Logitech's software had one for it (CentringSpring.hpp)
    spec.steeringRange = {0, 65535};
    spec.pedalRange = {0, 65535};
    spec.pedalsReleasedAtMax = true;   // as the G25's
    // kG27Buttons: from users' reports, unverified (docs/targets.md).
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
    spec.nativeButtonOrder = kG27NativeOrder;
    spec.coreButtons = {17, 18, 16, 15, 5, 4};   // as the G25's: Black 1-4 are its diamond's top, left, bottom, right
    return spec;
}

DeviceProfileSpec g923Spec() {
    DeviceProfileSpec spec = g920Spec();   // the same axes, ranges and first 19 buttons
    spec.identity.shortName = "G923";
    spec.identity.productId = 0xC26E;
    spec.identity.productName = "Logitech G923 Racing Wheel for Xbox One and PC";
    // kG923Buttons: the G920's, then the 5 the G920 lacks; the order is unverified (docs/targets.md).
    spec.buttonNames.insert(spec.buttonNames.end(), {"Plus", "Minus", "Dial Right", "Dial Left", "Dial Enter"});
    using Place = ButtonGroup::Place;
    spec.buttonGroups = {
        {"FACE", {0, 1, 2, 3}, Place::Left},             // A, B, X, Y
        {"PADDLES", {5, 4}, Place::Left},                // L Paddle, R Paddle
        {"PLUS MINUS", {19, 20}, Place::Left},           // Plus, Minus
        {"STICKS", {9, 8}, Place::Right},                // LSB, RSB
        {"SYSTEM", {6, 7, 10}, Place::Right},            // Menu, View, Xbox
        {"DIAL", {22, 21, 23}, Place::Right},            // Dial Left, Dial Right, Dial Enter
        {"SHIFTER", {12, 13, 14, 15, 16, 17, 18}, Place::Below},
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
    if (name == "g923") {
        return std::make_unique<DeviceProfile>(g923Spec());
    }
    return nullptr;
}

}  // namespace wheelbri
