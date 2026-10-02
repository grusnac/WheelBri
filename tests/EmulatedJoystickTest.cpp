#include "wheelemu/EmulatedJoystick.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstring>

namespace wheelemu {
namespace {

constexpr std::uint32_t kAnyOptional = di::kAnyInstance | di::kOptional;

// Same entries as DirectInput's c_dfDIJoystick (DIJOYSTATE, 80 bytes).
std::vector<FormatEntry> joystickFormat() {
    std::vector<FormatEntry> entries = {
        {ObjectGuid::XAxis, 0, di::kAxis | kAnyOptional, 0},   {ObjectGuid::YAxis, 4, di::kAxis | kAnyOptional, 0},
        {ObjectGuid::ZAxis, 8, di::kAxis | kAnyOptional, 0},   {ObjectGuid::RxAxis, 12, di::kAxis | kAnyOptional, 0},
        {ObjectGuid::RyAxis, 16, di::kAxis | kAnyOptional, 0}, {ObjectGuid::RzAxis, 20, di::kAxis | kAnyOptional, 0},
        {ObjectGuid::Slider, 24, di::kAxis | kAnyOptional, 0}, {ObjectGuid::Slider, 28, di::kAxis | kAnyOptional, 0},
        {ObjectGuid::Pov, 32, di::kPov | kAnyOptional, 0},     {ObjectGuid::Pov, 36, di::kPov | kAnyOptional, 0},
        {ObjectGuid::Pov, 40, di::kPov | kAnyOptional, 0},     {ObjectGuid::Pov, 44, di::kPov | kAnyOptional, 0},
    };
    for (std::uint32_t i = 0; i < 32; ++i) {
        entries.push_back({ObjectGuid::Any, 48 + i, di::kButton | kAnyOptional, 0});
    }
    return entries;
}

std::int32_t readI32(const std::vector<std::uint8_t>& data, const std::size_t offset) {
    std::int32_t value = 0;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

std::uint32_t readU32(const std::vector<std::uint8_t>& data, const std::size_t offset) {
    return static_cast<std::uint32_t>(readI32(data, offset));
}

JoystickLayout g29() { return layoutFor("g29", 11); }

TargetReport sampleReport() {
    TargetReport report;
    report.steering = 65535;   // full right
    report.throttle = 0;       // Logitech pedals: 0 = fully pressed
    report.brake = 65535;      // released
    report.clutch = 32768;
    report.buttons = 0b101;    // buttons 0 and 2
    report.hat = 2;            // right
    return report;
}

TEST(EmulatedJoystick, G29Layout) {
    const EmulatedJoystick joystick(g29());
    EXPECT_EQ(joystick.axisCount(), 4U);
    EXPECT_EQ(joystick.buttonCount(), 11U);
    EXPECT_EQ(joystick.povCount(), 1U);
    EXPECT_EQ(joystick.objects()[0].guid, ObjectGuid::XAxis);
    EXPECT_NE(joystick.objects()[0].typeId & di::kFfActuator, 0U);
    EXPECT_EQ(joystick.objects()[2].guid, ObjectGuid::RzAxis);
}

TEST(EmulatedJoystick, StandardJoystickFormat) {
    EmulatedJoystick joystick(g29());
    ASSERT_EQ(joystick.setDataFormat(joystickFormat(), 80), EmulatedJoystick::FormatResult::Ok);

    std::vector<std::uint8_t> state(80, 0xCD);
    ASSERT_TRUE(joystick.writeState(sampleReport(), state));
    EXPECT_EQ(readI32(state, 0), 65535);    // X = wheel
    EXPECT_EQ(readI32(state, 4), 0);        // Y = accelerator
    EXPECT_EQ(readI32(state, 8), 0);        // Z: not on this device
    EXPECT_EQ(readI32(state, 20), 65535);   // Rz = brake
    EXPECT_EQ(readI32(state, 24), 32768);   // slider 0 = clutch
    EXPECT_EQ(readU32(state, 32), 9000U);   // POV 0: right = 90 degrees
    EXPECT_EQ(readU32(state, 36), di::kPovCentered);   // missing POVs read as centred
    EXPECT_EQ(state[48], di::kButtonDown);
    EXPECT_EQ(state[49], 0);
    EXPECT_EQ(state[50], di::kButtonDown);
    EXPECT_EQ(state[48 + 11], 0);           // beyond the device's buttons
}

TEST(EmulatedJoystick, RangesScaleAxes) {
    EmulatedJoystick joystick(g29());
    ASSERT_EQ(joystick.setDataFormat(joystickFormat(), 80), EmulatedJoystick::FormatResult::Ok);
    joystick.setRange(0, {-10000, 10000});
    TargetReport report = sampleReport();
    EXPECT_EQ(static_cast<std::int32_t>(joystick.valueOf(0, report)), 10000);
    report.steering = 0;
    EXPECT_EQ(static_cast<std::int32_t>(joystick.valueOf(0, report)), -10000);
    report.steering = 32768;
    EXPECT_EQ(static_cast<std::int32_t>(joystick.valueOf(0, report)), 0);
}

TEST(EmulatedJoystick, CustomFormatMatchesByGuidAndInstance) {
    EmulatedJoystick joystick(g29());
    // A game that only wants the brake (Rz) at 0, button instance 3 at 4, and the POV at 8.
    const std::vector<FormatEntry> entries = {
        {ObjectGuid::RzAxis, 0, di::kAxis | di::kAnyInstance, 0},
        {ObjectGuid::Any, 4, di::kButton | di::makeInstance(3), 0},
        {ObjectGuid::Pov, 8, di::kPov | di::kAnyInstance, 0},
    };
    ASSERT_EQ(joystick.setDataFormat(entries, 12), EmulatedJoystick::FormatResult::Ok);
    TargetReport report = sampleReport();
    report.buttons = 1U << 3;
    std::vector<std::uint8_t> state(12);
    ASSERT_TRUE(joystick.writeState(report, state));
    EXPECT_EQ(readI32(state, 0), 65535);
    EXPECT_EQ(state[4], di::kButtonDown);
    EXPECT_EQ(readU32(state, 8), 9000U);
    EXPECT_EQ(joystick.objectAtOffset(4), std::optional<std::size_t>(4 + 3));
}

TEST(EmulatedJoystick, MissingRequiredObjectFailsTheFormat) {
    EmulatedJoystick joystick(g29());
    const std::vector<FormatEntry> entries = {{ObjectGuid::ZAxis, 0, di::kAxis | di::kAnyInstance, 0}};
    EXPECT_EQ(joystick.setDataFormat(entries, 4), EmulatedJoystick::FormatResult::InvalidParam);
    EXPECT_FALSE(joystick.hasDataFormat());
}

TEST(EmulatedJoystick, EntryOutsideTheDataFails) {
    EmulatedJoystick joystick(g29());
    const std::vector<FormatEntry> entries = {{ObjectGuid::XAxis, 4, di::kAxis | di::kAnyInstance, 0}};
    EXPECT_EQ(joystick.setDataFormat(entries, 6), EmulatedJoystick::FormatResult::InvalidParam);
}

TEST(EmulatedJoystick, HugeOffsetCannotWrapAround) {
    EmulatedJoystick joystick(g29());
    // 0xFFFFFFFC + 4 wraps to 0 in 32 bits: it must still be outside the data.
    const std::vector<FormatEntry> entries = {{ObjectGuid::XAxis, 0xFFFFFFFCU, di::kAxis | di::kAnyInstance, 0}};
    EXPECT_EQ(joystick.setDataFormat(entries, 80), EmulatedJoystick::FormatResult::InvalidParam);
}

TEST(EmulatedJoystick, VelocityAspectDoesNotTakeThePositionAxis) {
    EmulatedJoystick joystick(g29());
    const std::vector<FormatEntry> entries = {
        {ObjectGuid::XAxis, 0, di::kAxis | kAnyOptional, 0x200},   // DIDOI_ASPECTVELOCITY
        {ObjectGuid::XAxis, 4, di::kAxis | kAnyOptional, 0},
    };
    ASSERT_EQ(joystick.setDataFormat(entries, 8), EmulatedJoystick::FormatResult::Ok);
    EXPECT_EQ(joystick.formatOffset(0), std::optional<std::uint32_t>(4));
}

TEST(EmulatedJoystick, LookupsByIdAndUsage) {
    const EmulatedJoystick joystick(g29());
    EXPECT_EQ(joystick.objectById(di::kAbsAxis | di::makeInstance(2)), std::optional<std::size_t>(2));
    EXPECT_EQ(joystick.objectById(di::kPushButton | di::makeInstance(0)), std::optional<std::size_t>(4));
    EXPECT_EQ(joystick.objectById(di::kPov | di::makeInstance(0)), std::optional<std::size_t>(15));
    EXPECT_EQ(joystick.objectByUsage(0x01, 0x31), std::optional<std::size_t>(1));
    EXPECT_EQ(joystick.objectAtOffset(20), std::optional<std::size_t>(2));   // native offset before a format
}

TEST(EmulatedJoystick, DiffReportsChangedObjectsInFormatOrder) {
    EmulatedJoystick joystick(g29());
    ASSERT_EQ(joystick.setDataFormat(joystickFormat(), 80), EmulatedJoystick::FormatResult::Ok);
    const TargetReport before = sampleReport();
    TargetReport after = before;
    after.brake = 0;
    after.buttons = 0b001;
    after.hat = kHatCentered;
    const auto changes = joystick.diff(before, after);
    ASSERT_EQ(changes.size(), 3U);
    EXPECT_EQ(changes[0].offset, 20U);
    EXPECT_EQ(changes[0].data, 0U);
    EXPECT_EQ(changes[1].offset, 32U);
    EXPECT_EQ(changes[1].data, di::kPovCentered);
    EXPECT_EQ(changes[2].offset, 50U);
    EXPECT_EQ(changes[2].data, 0U);
    EXPECT_TRUE(joystick.diff(after, after).empty());
}

TEST(EmulatedJoystick, G920LayoutIsSliderYRz) {
    EmulatedJoystick joystick(layoutFor("g920", 11));
    ASSERT_EQ(joystick.axisCount(), 4U);
    EXPECT_EQ(joystick.objects()[1].guid, ObjectGuid::YAxis);
    EXPECT_EQ(joystick.objects()[1].control, Control::Brake);
    EXPECT_EQ(joystick.objects()[2].guid, ObjectGuid::RzAxis);
    EXPECT_EQ(joystick.objects()[2].control, Control::Clutch);
    EXPECT_EQ(joystick.objects()[3].guid, ObjectGuid::Slider);
    EXPECT_EQ(joystick.objects()[3].control, Control::Throttle);
    EXPECT_EQ(di::instanceOf(joystick.objects()[3].typeId), 3U);

    ASSERT_EQ(joystick.setDataFormat(joystickFormat(), 80), EmulatedJoystick::FormatResult::Ok);
    std::vector<std::uint8_t> state(80);
    ASSERT_TRUE(joystick.writeState(sampleReport(), state));
    EXPECT_EQ(readI32(state, 4), 65535);    // Y = brake (released)
    EXPECT_EQ(readI32(state, 8), 0);        // no Z
    EXPECT_EQ(readI32(state, 20), 32768);   // Rz = clutch
    EXPECT_EQ(readI32(state, 24), 0);       // slider 0 = accelerator (fully pressed)
}

TEST(EmulatedJoystick, AxesAreNumberedInUsageOrder) {
    JoystickLayout layout;
    layout.throttle = ObjectGuid::Slider;
    layout.brake = ObjectGuid::YAxis;
    layout.clutch = ObjectGuid::Any;
    const EmulatedJoystick joystick(layout);
    ASSERT_EQ(joystick.axisCount(), 3U);
    EXPECT_EQ(joystick.objects()[1].guid, ObjectGuid::YAxis);
    EXPECT_EQ(joystick.objects()[1].control, Control::Brake);
    EXPECT_EQ(joystick.objects()[2].guid, ObjectGuid::Slider);
    EXPECT_EQ(joystick.objects()[2].control, Control::Throttle);
}

TEST(EmulatedJoystick, InvertPedalsFlipsOnlyThePedals) {
    JoystickLayout layout = layoutFor("g920", 11);
    layout.invertPedals = true;
    const EmulatedJoystick joystick(layout);
    const TargetReport report = sampleReport();
    EXPECT_EQ(joystick.valueOf(0, report), 65535U);   // wheel unchanged
    EXPECT_EQ(joystick.valueOf(3, report), 65535U);   // throttle (slider) 0 -> 65535
    EXPECT_EQ(joystick.valueOf(1, report), 0U);       // brake (Y) 65535 -> 0
}

TEST(JoystickLayout, Validation) {
    EXPECT_TRUE(isValid(layoutFor("g920", 11)));
    EXPECT_TRUE(isValid(layoutFor("g29", 11)));
    JoystickLayout layout;
    layout.brake = layout.throttle;
    EXPECT_FALSE(isValid(layout));
    layout = JoystickLayout{};
    layout.throttle = ObjectGuid::XAxis;
    EXPECT_FALSE(isValid(layout));
    layout = JoystickLayout{};
    layout.brake = ObjectGuid::Any;
    EXPECT_FALSE(isValid(layout));
    layout = JoystickLayout{};
    layout.clutch = ObjectGuid::Any;
    EXPECT_TRUE(isValid(layout));
}

TEST(JoystickLayout, AxisNames) {
    EXPECT_EQ(parseAxisName("rz"), std::optional<ObjectGuid>(ObjectGuid::RzAxis));
    EXPECT_EQ(parseAxisName("slider"), std::optional<ObjectGuid>(ObjectGuid::Slider));
    EXPECT_EQ(parseAxisName("none"), std::optional<ObjectGuid>(ObjectGuid::Any));
    EXPECT_EQ(parseAxisName("Z"), std::nullopt);
    EXPECT_EQ(axisName(ObjectGuid::ZAxis), "z");
}

}  // namespace
}  // namespace wheelemu
