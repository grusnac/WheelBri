// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/EmulatedJoystick.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <limits>

namespace wheelbri {
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

JoystickLayout g920() { return layoutFor("g920", 11); }

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

TEST(EmulatedJoystick, G920Layout) {
    const EmulatedJoystick joystick(g920());
    EXPECT_EQ(joystick.axisCount(), 4U);
    EXPECT_EQ(joystick.buttonCount(), 11U);
    EXPECT_EQ(joystick.povCount(), 1U);
    EXPECT_EQ(joystick.objects()[0].guid, ObjectGuid::XAxis);
    EXPECT_NE(joystick.objects()[0].typeId & di::kFfActuator, 0U);
    EXPECT_EQ(joystick.objects()[1].guid, ObjectGuid::YAxis);   // throttle
}

// The G25's and G27's: combined pedals on Slider 0 and the clutch on Slider 1, as Logitech's driver gives them: Race
// Driver: GRID reads a G25's Slider 0 as combined pedals (lower half throttle, upper half brake), where a released
// clutch was a held brake and nothing there (0) a held throttle; Assetto Corsa Competizione reads the clutch on
// Slider 1.
TEST(EmulatedJoystick, G25LayoutHasCombinedPedalsAndItsClutchOnSlider1) {
    const JoystickLayout layout = layoutFor("g25", kG25Buttons);
    EXPECT_TRUE(isValid(layout));
    EXPECT_EQ(layout.throttle, ObjectGuid::YAxis);
    EXPECT_EQ(layout.brake, ObjectGuid::RzAxis);
    EXPECT_EQ(layout.clutch, ObjectGuid::Slider1);
    EXPECT_EQ(layoutFor("g920", kG920Buttons).clutch, ObjectGuid::Slider);
    EXPECT_EQ(layoutFor("g27", kG27Buttons).clutch, ObjectGuid::Slider1);
    EXPECT_EQ(layoutFor("g27", kG27Buttons).combined, ObjectGuid::Slider);
    EXPECT_EQ(layout.combined, ObjectGuid::Slider);
    EXPECT_EQ(layoutFor("g920", kG920Buttons).combined, ObjectGuid::Any);
    const EmulatedJoystick joystick(layout);
    EXPECT_EQ(joystick.axisCount(), 5U);
    EXPECT_EQ(joystick.buttonCount(), 19U);   // the gears and the shifter's 8 buttons too
    EXPECT_EQ(joystick.povCount(), 1U);

    // Both Sliders carry GUID_Slider, Slider 0 (the combined pedals) first, as DirectInput numbers them.
    EXPECT_EQ(joystick.objects()[3].guid, ObjectGuid::Slider);
    EXPECT_EQ(joystick.objects()[3].control, Control::Combined);
    EXPECT_EQ(joystick.objects()[4].guid, ObjectGuid::Slider);
    EXPECT_EQ(joystick.objects()[4].control, Control::Clutch);
    EXPECT_EQ(joystick.objects()[4].nativeOffset, 28U);
    EXPECT_EQ(parseAxisName("slider1"), ObjectGuid::Slider1);

    // In c_dfDIJoystick2's layout: the combined pedals on slider 0 (offset 24), the clutch on slider 1 (offset 28),
    // nothing on Z (offset 8).
    EmulatedJoystick g25(layout);
    ASSERT_EQ(g25.setDataFormat(joystickFormat(), 80), EmulatedJoystick::FormatResult::Ok);
    std::vector<std::uint8_t> state(80, 0xCD);
    ASSERT_TRUE(g25.writeState(sampleReport(), state));
    EXPECT_EQ(readI32(state, 28), 32768);   // slider 1 = clutch
    EXPECT_EQ(readI32(state, 8), 0);        // Z: none
    EXPECT_EQ(readI32(state, 24), 0);       // slider 0 = combined pedals: full throttle, brake released: the bottom
    TargetReport released = sampleReport();
    released.throttle = 65535;
    released.brake = 65535;
    ASSERT_TRUE(g25.writeState(released, state));
    EXPECT_EQ(readI32(state, 24), 32767);   // both released: the middle
    released.brake = 0;
    ASSERT_TRUE(g25.writeState(released, state));
    EXPECT_EQ(readI32(state, 24), 65535);   // full brake: the top

    // A pedal moved onto Slider (wheelbri-shim.ini, a game profile): the combined pedals give way.
    JoystickLayout moved = layout;
    moved.clutch = ObjectGuid::Slider;
    EXPECT_EQ(EmulatedJoystick(moved).axisCount(), 4U);
}

TEST(EmulatedJoystick, StandardJoystickFormat) {
    EmulatedJoystick joystick(g920());
    ASSERT_EQ(joystick.setDataFormat(joystickFormat(), 80), EmulatedJoystick::FormatResult::Ok);

    std::vector<std::uint8_t> state(80, 0xCD);
    ASSERT_TRUE(joystick.writeState(sampleReport(), state));
    EXPECT_EQ(readI32(state, 0), 65535);    // X = wheel
    EXPECT_EQ(readI32(state, 4), 0);        // Y = accelerator (fully pressed)
    EXPECT_EQ(readI32(state, 8), 0);        // Z: not on this device
    EXPECT_EQ(readI32(state, 20), 65535);   // Rz = brake (released)
    EXPECT_EQ(readI32(state, 24), 32768);   // slider 0 = clutch
    EXPECT_EQ(readU32(state, 32), 9000U);   // POV 0: right = 90 degrees
    EXPECT_EQ(readU32(state, 36), di::kPovCentered);   // missing POVs read as centred
    EXPECT_EQ(state[48], di::kButtonDown);
    EXPECT_EQ(state[49], 0);
    EXPECT_EQ(state[50], di::kButtonDown);
    EXPECT_EQ(state[48 + 11], 0);           // beyond the device's buttons
}

TEST(EmulatedJoystick, RangesScaleAxes) {
    EmulatedJoystick joystick(g920());
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
    EmulatedJoystick joystick(g920());
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
    EXPECT_EQ(readI32(state, 0), 65535);   // brake released
    EXPECT_EQ(state[4], di::kButtonDown);
    EXPECT_EQ(readU32(state, 8), 9000U);
    EXPECT_EQ(joystick.objectAtOffset(4), std::optional<std::size_t>(4 + 3));
}

TEST(EmulatedJoystick, MissingRequiredObjectFailsTheFormat) {
    EmulatedJoystick joystick(g920());
    const std::vector<FormatEntry> entries = {{ObjectGuid::ZAxis, 0, di::kAxis | di::kAnyInstance, 0}};
    EXPECT_EQ(joystick.setDataFormat(entries, 4), EmulatedJoystick::FormatResult::InvalidParam);
    EXPECT_FALSE(joystick.hasDataFormat());
}

TEST(EmulatedJoystick, EntryOutsideTheDataFails) {
    EmulatedJoystick joystick(g920());
    const std::vector<FormatEntry> entries = {{ObjectGuid::XAxis, 4, di::kAxis | di::kAnyInstance, 0}};
    EXPECT_EQ(joystick.setDataFormat(entries, 6), EmulatedJoystick::FormatResult::InvalidParam);
}

TEST(EmulatedJoystick, HugeOffsetCannotWrapAround) {
    EmulatedJoystick joystick(g920());
    // 0xFFFFFFFC + 4 wraps to 0 in 32 bits: it must still be outside the data.
    const std::vector<FormatEntry> entries = {{ObjectGuid::XAxis, 0xFFFFFFFCU, di::kAxis | di::kAnyInstance, 0}};
    EXPECT_EQ(joystick.setDataFormat(entries, 80), EmulatedJoystick::FormatResult::InvalidParam);
}

TEST(EmulatedJoystick, VelocityAspectDoesNotTakeThePositionAxis) {
    EmulatedJoystick joystick(g920());
    const std::vector<FormatEntry> entries = {
        {ObjectGuid::XAxis, 0, di::kAxis | kAnyOptional, 0x200},   // DIDOI_ASPECTVELOCITY
        {ObjectGuid::XAxis, 4, di::kAxis | kAnyOptional, 0},
    };
    ASSERT_EQ(joystick.setDataFormat(entries, 8), EmulatedJoystick::FormatResult::Ok);
    EXPECT_EQ(joystick.formatOffset(0), std::optional<std::uint32_t>(4));
}

TEST(EmulatedJoystick, LookupsByIdAndUsage) {
    const EmulatedJoystick joystick(g920());
    EXPECT_EQ(joystick.objectById(di::kAbsAxis | di::makeInstance(2)), std::optional<std::size_t>(2));
    EXPECT_EQ(joystick.objectById(di::kPushButton | di::makeInstance(0)), std::optional<std::size_t>(4));
    EXPECT_EQ(joystick.objectById(di::kPov | di::makeInstance(0)), std::optional<std::size_t>(15));
    EXPECT_EQ(joystick.objectByUsage(0x01, 0x31), std::optional<std::size_t>(1));
    EXPECT_EQ(joystick.objectAtOffset(20), std::optional<std::size_t>(2));   // native offset before a format
}

TEST(EmulatedJoystick, DiffReportsChangedObjectsInFormatOrder) {
    EmulatedJoystick joystick(g920());
    ASSERT_EQ(joystick.setDataFormat(joystickFormat(), 80), EmulatedJoystick::FormatResult::Ok);
    const TargetReport before = sampleReport();
    TargetReport after = before;
    after.brake = 0;
    after.buttons = 0b001;
    after.hat = kHatCentered;
    const auto changes = joystick.diff(before, after);
    ASSERT_EQ(changes.size(), 3U);
    EXPECT_EQ(changes[0].offset, 20U);   // Rz = brake
    EXPECT_EQ(changes[0].data, 0U);
    EXPECT_EQ(changes[1].offset, 32U);
    EXPECT_EQ(changes[1].data, di::kPovCentered);
    EXPECT_EQ(changes[2].offset, 50U);
    EXPECT_EQ(changes[2].data, 0U);
    EXPECT_TRUE(joystick.diff(after, after).empty());
}

// A real G920 on Windows: throttle Y, brake Rz, clutch Slider (what NFS Unbound reads, 2026-10-09).
TEST(EmulatedJoystick, G920LayoutIsYRzSlider) {
    EmulatedJoystick joystick(layoutFor("g920", 11));
    ASSERT_EQ(joystick.axisCount(), 4U);
    EXPECT_EQ(joystick.objects()[1].guid, ObjectGuid::YAxis);
    EXPECT_EQ(joystick.objects()[1].control, Control::Throttle);
    EXPECT_EQ(joystick.objects()[2].guid, ObjectGuid::RzAxis);
    EXPECT_EQ(joystick.objects()[2].control, Control::Brake);
    EXPECT_EQ(joystick.objects()[3].guid, ObjectGuid::Slider);
    EXPECT_EQ(joystick.objects()[3].control, Control::Clutch);
    EXPECT_EQ(di::instanceOf(joystick.objects()[3].typeId), 3U);

    ASSERT_EQ(joystick.setDataFormat(joystickFormat(), 80), EmulatedJoystick::FormatResult::Ok);
    std::vector<std::uint8_t> state(80);
    ASSERT_TRUE(joystick.writeState(sampleReport(), state));
    EXPECT_EQ(readI32(state, 4), 0);        // Y = accelerator (fully pressed)
    EXPECT_EQ(readI32(state, 8), 0);        // no Z
    EXPECT_EQ(readI32(state, 20), 65535);   // Rz = brake (released)
    EXPECT_EQ(readI32(state, 24), 32768);   // slider 0 = clutch
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
    EXPECT_EQ(joystick.valueOf(1, report), 65535U);   // throttle (Y) 0 -> 65535
    EXPECT_EQ(joystick.valueOf(2, report), 0U);       // brake (Rz) 65535 -> 0
}

TEST(JoystickLayout, Validation) {
    EXPECT_TRUE(isValid(layoutFor("g920", 11)));
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

// Before the game sets a data format there is nothing to write (GetDeviceState fails), and a buffer of the wrong
// size is refused rather than written past.
TEST(EmulatedJoystick, NoStateWithoutAFormatOrWithTheWrongSize) {
    EmulatedJoystick joystick(g920());
    std::vector<std::uint8_t> state(80, 0xCD);
    EXPECT_FALSE(joystick.writeState(sampleReport(), state));
    EXPECT_EQ(joystick.formatOffset(0), std::nullopt);
    EXPECT_EQ(state[0], 0xCD);
    ASSERT_EQ(joystick.setDataFormat(joystickFormat(), 80), EmulatedJoystick::FormatResult::Ok);
    std::vector<std::uint8_t> small(79, 0xCD);
    EXPECT_FALSE(joystick.writeState(sampleReport(), small));
    std::vector<std::uint8_t> big(81, 0xCD);
    EXPECT_FALSE(joystick.writeState(sampleReport(), big));
    EXPECT_EQ(small[0], 0xCD);
}

// A hat value the profile never makes (8 and up, or below -1, from a corrupt report): the POV reads centred.
TEST(EmulatedJoystick, HatOutOfRangeReadsCentred) {
    const EmulatedJoystick joystick(g920());
    const std::size_t pov = joystick.objects().size() - 1;
    TargetReport report = sampleReport();
    for (const std::int8_t hat : {std::int8_t{8}, std::int8_t{100}, std::int8_t{-2}, kHatCentered}) {
        report.hat = hat;
        EXPECT_EQ(joystick.valueOf(pov, report), di::kPovCentered) << static_cast<int>(hat);
    }
    report.hat = 7;
    EXPECT_EQ(joystick.valueOf(pov, report), 31500U);
}

// Report values outside 0..65535 (the profile never makes them, a hand-made one could): axes stay inside the game's
// range, pedals too after the inversion.
TEST(EmulatedJoystick, OutOfRangeReportValuesStayInTheGamesRange) {
    JoystickLayout layout = g920();
    layout.invertPedals = true;
    EmulatedJoystick joystick(layout);
    joystick.setRange(0, {-100, 100});
    joystick.setRange(1, {0, 255});
    TargetReport report;
    report.steering = std::numeric_limits<std::int32_t>::max();
    report.throttle = std::numeric_limits<std::int32_t>::min();
    EXPECT_EQ(static_cast<std::int32_t>(joystick.valueOf(0, report)), 100);
    EXPECT_EQ(joystick.valueOf(1, report), 255U);   // below 0 = 0, inverted = 65535: the top of 0..255
    report.steering = -5;
    report.throttle = 70000;
    EXPECT_EQ(static_cast<std::int32_t>(joystick.valueOf(0, report)), -100);
    EXPECT_EQ(joystick.valueOf(1, report), 0U);
}

// A target with more buttons than DirectInput's DIJOYSTATE2 format offers the game: 32 at most.
TEST(EmulatedJoystick, ButtonsCappedAt32) {
    const EmulatedJoystick joystick(layoutFor("g920", 40));
    EXPECT_EQ(joystick.buttonCount(), 32U);
    EXPECT_EQ(joystick.objects().back().guid, ObjectGuid::Pov);
}

// Format entries that ask for something this device has not: a second POV (optional: reads centred, required: the
// format fails), a force-feedback actuator other than the wheel, an explicit position aspect, an entry with no kind.
TEST(EmulatedJoystick, FormatEntriesByKindActuatorAndAspect) {
    EmulatedJoystick joystick(g920());
    const std::vector<FormatEntry> entries = {
        {ObjectGuid::Any, 0, di::kAxis | di::kAnyInstance | di::kFfActuator, 0},   // only the wheel actuates
        {ObjectGuid::Any, 4, di::kAxis | di::kAnyInstance | di::kFfActuator | di::kOptional, 0},   // no second
        {ObjectGuid::YAxis, 8, di::kAnyInstance, di::kAspectPosition},   // no kind bits, position aspect
        {ObjectGuid::Any, 12, di::kPov | di::kAnyInstance, 0},
        {ObjectGuid::Any, 16, di::kPov | kAnyOptional, 0},   // a second POV by kind: centred
        {ObjectGuid::Pov, 20, di::kPov | kAnyOptional, 0},
    };
    ASSERT_EQ(joystick.setDataFormat(entries, 24), EmulatedJoystick::FormatResult::Ok);
    EXPECT_EQ(joystick.formatOffset(0), std::optional<std::uint32_t>(0));
    EXPECT_EQ(joystick.formatOffset(1), std::optional<std::uint32_t>(8));
    std::vector<std::uint8_t> state(24, 0xCD);
    ASSERT_TRUE(joystick.writeState(sampleReport(), state));
    EXPECT_EQ(readI32(state, 0), 65535);
    EXPECT_EQ(readI32(state, 4), 0);   // nothing there: zero
    EXPECT_EQ(readU32(state, 12), 9000U);
    EXPECT_EQ(readU32(state, 16), di::kPovCentered);
    EXPECT_EQ(readU32(state, 20), di::kPovCentered);

    const std::vector<FormatEntry> twoPovs = {{ObjectGuid::Pov, 0, di::kPov | di::kAnyInstance, 0},
                                              {ObjectGuid::Pov, 4, di::kPov | di::kAnyInstance, 0}};
    EXPECT_EQ(joystick.setDataFormat(twoPovs, 8), EmulatedJoystick::FormatResult::InvalidParam);
    EXPECT_TRUE(joystick.hasDataFormat());   // a failed format keeps the one before
    EXPECT_EQ(joystick.dataSize(), 24U);
}

// Optional POV entries the device cannot fill but that are misplaced (unaligned, past the data, or straddling its
// end): nothing is written there, so they cannot write past the game's buffer.
TEST(EmulatedJoystick, MisplacedMissingPovsAreNotWritten) {
    EmulatedJoystick joystick(g920());
    const std::vector<FormatEntry> entries = {
        {ObjectGuid::Pov, 0, di::kPov | kAnyOptional, 0},    // the real one
        {ObjectGuid::Pov, 6, di::kPov | kAnyOptional, 0},    // unaligned
        {ObjectGuid::Pov, 8, di::kPov | kAnyOptional, 0},    // straddles the end (10 bytes)
        {ObjectGuid::Pov, 0xFFFFFFFCU, di::kPov | kAnyOptional, 0},   // far past it
        {ObjectGuid::Button, 4, di::kButton | kAnyOptional, 0},
        {ObjectGuid::ZAxis, 8, di::kAxis | kAnyOptional, 0},   // not on this device, not a POV: ignored
    };
    ASSERT_EQ(joystick.setDataFormat(entries, 10), EmulatedJoystick::FormatResult::Ok);
    std::vector<std::uint8_t> state(10, 0xCD);
    ASSERT_TRUE(joystick.writeState(sampleReport(), state));
    EXPECT_EQ(readU32(state, 0), 9000U);
    EXPECT_EQ(state[4], di::kButtonDown);
    for (std::size_t i = 5; i < state.size(); ++i) EXPECT_EQ(state[i], 0) << i;
}

// An axis at an unaligned offset is refused (DirectInput requires DWORD alignment); a button may sit anywhere.
TEST(EmulatedJoystick, UnalignedAxisFailsTheFormatButAButtonDoesNot) {
    EmulatedJoystick joystick(g920());
    EXPECT_EQ(joystick.setDataFormat({{ObjectGuid::XAxis, 2, di::kAxis | di::kAnyInstance, 0}}, 8),
              EmulatedJoystick::FormatResult::InvalidParam);
    EXPECT_EQ(joystick.setDataFormat({{ObjectGuid::Button, 7, di::kButton | di::kAnyInstance, 0}}, 8),
              EmulatedJoystick::FormatResult::Ok);
    EXPECT_EQ(joystick.setDataFormat({{ObjectGuid::Button, 8, di::kButton | di::kAnyInstance, 0}}, 8),
              EmulatedJoystick::FormatResult::InvalidParam);
}

// Lookups for objects the device does not have (GetProperty / GetObjectInfo on a stranger's offset, id or usage).
TEST(EmulatedJoystick, LookupsForMissingObjects) {
    EmulatedJoystick joystick(g920());
    EXPECT_EQ(joystick.objectById(di::kAbsAxis | di::makeInstance(9)), std::nullopt);
    EXPECT_EQ(joystick.objectById(di::kPov | di::makeInstance(1)), std::nullopt);
    EXPECT_EQ(joystick.objectByUsage(0x01, 0x32), std::nullopt);   // Z: not on this device
    EXPECT_EQ(joystick.objectByUsage(0x02, 0x30), std::nullopt);   // X's usage on another page
    EXPECT_EQ(joystick.objectByUsage(0x09, 11), std::optional<std::size_t>(14));   // Button 11
    EXPECT_EQ(joystick.objectAtOffset(8), std::nullopt);           // native Z: absent
    // With a format, an object the format left out is not at its native offset any more.
    ASSERT_EQ(joystick.setDataFormat({{ObjectGuid::XAxis, 4, di::kAxis | di::kAnyInstance, 0}}, 8),
              EmulatedJoystick::FormatResult::Ok);
    EXPECT_EQ(joystick.objectAtOffset(4), std::optional<std::size_t>(0));
    EXPECT_EQ(joystick.objectAtOffset(0), std::nullopt);
    EXPECT_EQ(joystick.objectAtOffset(20), std::nullopt);   // the brake: not in the format
    EXPECT_EQ(joystick.reportedOffset(2), 20U);             // still listed at its native offset
}

// Buffered input with a format that leaves objects out: only the formatted ones produce changes.
TEST(EmulatedJoystick, DiffSkipsObjectsTheFormatLeftOut) {
    EmulatedJoystick joystick(g920());
    ASSERT_EQ(joystick.setDataFormat({{ObjectGuid::XAxis, 0, di::kAxis | di::kAnyInstance, 0}}, 4),
              EmulatedJoystick::FormatResult::Ok);
    TargetReport before = sampleReport();
    TargetReport after = before;
    after.brake = 0;
    after.buttons = 0;
    after.hat = 0;
    EXPECT_TRUE(joystick.diff(before, after).empty());
    after.steering = 0;
    const auto changes = joystick.diff(before, after);
    ASSERT_EQ(changes.size(), 1U);
    EXPECT_EQ(changes[0].offset, 0U);
    EXPECT_EQ(changes[0].data, 0U);
}

TEST(JoystickLayout, NamesOfAxesThatAreNotAxes) {
    EXPECT_EQ(axisName(ObjectGuid::Any), "none");
    EXPECT_EQ(axisName(ObjectGuid::Pov), "none");
    EXPECT_EQ(parseAxisName(""), std::nullopt);
    EXPECT_EQ(parseAxisName("x"), std::optional<ObjectGuid>(ObjectGuid::XAxis));
    JoystickLayout layout;
    layout.clutch = ObjectGuid::Pov;   // not an axis
    EXPECT_FALSE(isValid(layout));
    layout = JoystickLayout{};
    layout.clutch = ObjectGuid::XAxis;   // the wheel's
    EXPECT_FALSE(isValid(layout));
    layout = JoystickLayout{};
    layout.clutch = ObjectGuid::YAxis;   // the throttle's
    EXPECT_FALSE(isValid(layout));
}

// The combined pedals take an axis of their own only: not one that is no axis (Any, a button's guid), not the wheel's
// X, and not any pedal's: then the device has the three pedals and the wheel alone.
TEST(EmulatedJoystick, CombinedPedalsNeedAFreeAxis) {
    JoystickLayout layout = layoutFor("g25", kG920Buttons);
    ASSERT_EQ(EmulatedJoystick(layout).axisCount(), 5U);
    for (const ObjectGuid taken : {ObjectGuid::Any, ObjectGuid::Button, ObjectGuid::XAxis, layout.throttle,
                                   layout.brake, layout.clutch}) {
        JoystickLayout other = layout;
        other.combined = taken;
        EXPECT_EQ(EmulatedJoystick(other).axisCount(), 4U) << static_cast<int>(taken);
    }
}

}  // namespace
}  // namespace wheelbri
