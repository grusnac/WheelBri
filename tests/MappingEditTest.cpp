#include "wheelbri/MappingEdit.hpp"

#include "wheelbri/IniDocument.hpp"

#include <gtest/gtest.h>

using namespace wheelbri;

namespace {

EmulatorConfig load(const std::string& text) {
    const auto parsed = IniDocument::parse(text);
    EXPECT_TRUE(parsed.errors.empty());
    const auto result = loadEmulatorConfig(parsed.document);
    EXPECT_TRUE(result.errors.empty()) << (result.errors.empty() ? "" : result.errors.front());
    return result.config;
}

const std::string kConfig =
    "; my config\r\n"
    "[emulator]\r\nsource_device = 346e            ; the wheel\r\n"
    "[steering]\r\naxis = 0\r\ninvert = false\r\nsource_rotation_deg = 540\r\n"
    "[throttle]\r\naxis = 2                        ; checked with --ui\r\ninvert = false\r\ngamma = 1.0\r\n"
    "[brake]\r\naxis = 5\r\n"
    "[clutch]\r\naxis = 6\r\n"
    "[buttons]\r\ntarget_4 = 13                   ; R paddle\r\ntarget_22 = -1\r\n"
    "[hat]\r\nup = 4\r\nright = 5\r\ndown = 6\r\nleft = 7\r\n";

}  // namespace

TEST(MappingEdit, ParsesAndFormatsSources) {
    EXPECT_EQ(parseSource("2"), (SourceRef{{}, 2}));
    EXPECT_EQ(parseSource("-1"), (SourceRef{{}, kUnmapped}));
    EXPECT_EQ(parseSource("044f:b696/1"), (SourceRef{{0x044F, 0xB696}, 1}));
    EXPECT_FALSE(parseSource("").has_value());
    EXPECT_FALSE(parseSource("x").has_value());
    EXPECT_FALSE(parseSource("044f:b696/-1").has_value());
    EXPECT_FALSE(parseSource("0:0/1").has_value());
    EXPECT_FALSE(parseSource("12345:1/1").has_value());
    EXPECT_EQ(formatSource({{0x044F, 0xB696}, 1}), "044f:b696/1");
    EXPECT_EQ(formatSource({{}, 13}), "13");
    EXPECT_EQ(formatSource({{0x044F, 0xB696}, kUnmapped}), "-1");
    EXPECT_EQ(parseDeviceId("346e:0005"), (DeviceId{0x346E, 0x0005}));
    EXPECT_FALSE(parseDeviceId("346e").has_value());
}

TEST(MappingEdit, NumbersDevicesUpToThree) {
    MapperConfig mapper;
    EXPECT_EQ(deviceNumber(mapper, {}), 0);
    EXPECT_EQ(deviceNumber(mapper, {1, 1}), 1);
    EXPECT_EQ(deviceNumber(mapper, {1, 2}), 2);
    EXPECT_EQ(deviceNumber(mapper, {1, 1}), 1);
    EXPECT_EQ(deviceNumber(mapper, {1, 3}), 3);
    EXPECT_FALSE(deviceNumber(mapper, {1, 4}).has_value());
    EXPECT_EQ(sourceOf(mapper, 2, 7), (SourceRef{{1, 2}, 7}));
    EXPECT_EQ(sourceOf(mapper, 0, 7), (SourceRef{{}, 7}));
    EXPECT_EQ(sourceOf(mapper, 1, kUnmapped), SourceRef{});
}

TEST(MappingEdit, UnchangedModelWritesTheSameText) {
    const MappingModel model = mappingModel(load(kConfig));
    EXPECT_EQ(model.axes[MappingModel::kThrottle], (SourceRef{{}, 2}));
    EXPECT_EQ(model.buttons[4], (SourceRef{{}, 13}));
    EXPECT_EQ(model.buttons[5], (SourceRef{{}, 5}));   // the default
    EXPECT_FALSE(model.buttons[22].mapped());
    const MappingWriteResult written = writeMapping(kConfig, model);
    ASSERT_TRUE(written.error.empty()) << written.error;
    EXPECT_EQ(written.text, kConfig);   // comments and all
}

TEST(MappingEdit, WritesAccessoriesAndReadsThemBack) {
    MappingModel model = mappingModel(load(kConfig));
    model.axes[MappingModel::kThrottle] = {{0x044F, 0xB696}, 1};
    model.invert[MappingModel::kThrottle] = true;
    model.axes[MappingModel::kClutch] = {};
    model.buttons[4] = {{0x1234, 0x0001}, 3};
    model.buttons[22] = {{}, 22};          // back to the default: its line goes
    model.hat[0] = {};
    const MappingWriteResult written = writeMapping(kConfig, model);
    ASSERT_TRUE(written.error.empty()) << written.error;
    EXPECT_NE(written.text.find("axis = 044f:b696/1\r\n"), std::string::npos);
    EXPECT_NE(written.text.find("; my config\r\n"), std::string::npos);
    EXPECT_EQ(written.text.find("target_22"), std::string::npos);
    EXPECT_EQ(written.text.find("up ="), std::string::npos);
    EXPECT_NE(written.text.find("source_rotation_deg = 540"), std::string::npos);   // not the mapping's: kept

    const EmulatorConfig back = load(written.text);
    EXPECT_EQ(mappingModel(back), model);
    EXPECT_EQ(back.mapper.accessoryCount(), 2U);
    EXPECT_TRUE(back.mapper.throttle.invert);
}

TEST(MappingEdit, TheWheelsOwnIdIsTheWheelAndFourAccessoriesAreRefused) {
    MappingModel model = mappingModel(load(kConfig));
    model.sourceDevice = "346e:0005";
    model.axes[MappingModel::kBrake] = {{0x346E, 0x0005}, 5};
    const MappingWriteResult written = writeMapping(kConfig, model);
    ASSERT_TRUE(written.error.empty());
    EXPECT_NE(written.text.find("source_device = 346e:0005"), std::string::npos);
    EXPECT_EQ(load(written.text).mapper.accessoryCount(), 0U);

    for (std::uint16_t i = 0; i < 4; ++i) model.buttons[i] = {{0x1111, static_cast<std::uint16_t>(i + 1)}, 0};
    const MappingWriteResult refused = writeMapping(kConfig, model);
    EXPECT_FALSE(refused.error.empty());
    EXPECT_TRUE(refused.text.empty());
}

TEST(MappingEdit, AutoMapsFromTheRigsRoles) {
    Rig rig;
    RigDevice pedals;
    pedals.name = "Pedals";
    pedals.id = {0x30B7, 0x1001};
    pedals.axes[0] = {"Gas", ControlRole::Throttle, true};
    pedals.axes[1] = {"Brake", ControlRole::Brake, false};
    RigDevice wheel;
    wheel.name = "My wheel";
    wheel.id = {0x346E, 0x0005};
    wheel.axes[0] = {"Wheel", ControlRole::Steering, false};
    wheel.buttons[13] = {"R paddle", ControlRole::ShiftUp, false};
    wheel.buttons[12] = {"L paddle", ControlRole::LeftPaddle, false};
    wheel.buttons[4] = {"Up", ControlRole::DpadUp, false};
    wheel.buttons[0] = {"A", ControlRole::Button, false};
    wheel.buttons[30] = {"1st", ControlRole::Gear1, false};         // the G920's Driving Force Shifter: Gear 1
    wheel.buttons[31] = {"R", ControlRole::GearReverse, false};     // ... and Gear R
    wheel.buttons[32] = {"7th", ControlRole::Gear7, false};         // the G920's shifter has 6 gears: left out
    rig.devices = {pedals, wheel};

    MappingModel model = mappingModel(load(kConfig));
    const DeviceProfile g920(g920Spec());
    const AutoMapResult result = autoMap(model, rig, g920);
    EXPECT_EQ(result.set, 8);   // 3 axes, 2 paddles, 1 D-pad direction, 2 gears
    const std::string notes = [&] {
        std::string all;
        for (const std::string& note : result.notes) all += note + "\n";
        return all;
    }();
    EXPECT_NE(notes.find("gear 7 '7th' (button 32 of My wheel): the G920 has no Gear 7"), std::string::npos) << notes;
    // The default's Gear 2 <- button 13 went: button 13 is the R paddle now (one button, one G920 input).
    EXPECT_FALSE(model.buttons[13].mapped());
    EXPECT_NE(notes.find("Gear 2: no longer 13"), std::string::npos) << notes;
    EXPECT_EQ(model.sourceDevice, "346e:0005");
    EXPECT_EQ(model.axes[MappingModel::kSteering], (SourceRef{{}, 0}));
    EXPECT_EQ(model.axes[MappingModel::kThrottle], (SourceRef{{0x30B7, 0x1001}, 0}));
    EXPECT_TRUE(model.invert[MappingModel::kThrottle]);
    EXPECT_EQ(model.axes[MappingModel::kBrake], (SourceRef{{0x30B7, 0x1001}, 1}));
    EXPECT_EQ(model.axes[MappingModel::kClutch], (SourceRef{{}, 6}));   // no clutch in the rig: as it was
    EXPECT_EQ(model.buttons[4], (SourceRef{{}, 13}));                   // G920 R paddle <- shift up
    EXPECT_EQ(model.buttons[5], (SourceRef{{}, 12}));                   // G920 L paddle <- left paddle
    EXPECT_EQ(model.buttons[5], (SourceRef{{}, 12}));                   // G920 L paddle
    EXPECT_EQ(model.hat[0], (SourceRef{{}, 4}));
    EXPECT_EQ(model.buttons[12], (SourceRef{{}, 30}));                  // G920 Gear 1 (joy.cpl 13)
    EXPECT_EQ(model.buttons[18], (SourceRef{{}, 31}));                  // G920 Gear R (joy.cpl 19)
}


TEST(MappingEdit, RotationReadFromTheConfig) {
    const MappingModel model = mappingModel(load(kConfig));
    EXPECT_EQ(model.wheelRotationDeg, 540.0F);
    EXPECT_EQ(model.gameRotationDeg, 900.0F);   // the default
    EXPECT_TRUE(tuningProblem(model).empty());
}

TEST(MappingEdit, WritesRotationOnlyWhereItChanged) {
    MappingModel model = mappingModel(load(kConfig));
    model.wheelRotationDeg = 180.0F;
    model.gameRotationDeg = 180.0F;
    const MappingWriteResult written = writeMapping(kConfig, model);
    ASSERT_TRUE(written.error.empty()) << written.error;
    EXPECT_NE(written.text.find("source_rotation_deg = 180\r\n"), std::string::npos) << written.text;
    EXPECT_NE(written.text.find("target_rotation_deg = 180\r\n"), std::string::npos) << written.text;
    EXPECT_EQ(written.text.find("deadzone"), std::string::npos);   // dead zones are the rig's, never written here
    EXPECT_NE(written.text.find("; checked with --ui"), std::string::npos);   // comments kept

    const EmulatorConfig back = load(written.text);
    EXPECT_EQ(mappingModel(back), model);
    EXPECT_EQ(back.mapper.steering.targetRotationDeg, 180.0F);
    // Written again unchanged: the same text.
    EXPECT_EQ(writeMapping(written.text, model).text, written.text);
}

TEST(MappingEdit, RefusesRotationsOutOfRange) {
    const MappingModel good = mappingModel(load(kConfig));
    for (const bool game : {true, false}) {
        MappingModel model = good;
        (game ? model.gameRotationDeg : model.wheelRotationDeg) = game ? 45.0F : 9000.0F;
        EXPECT_FALSE(tuningProblem(model).empty()) << game;
        const MappingWriteResult refused = writeMapping(kConfig, model);
        EXPECT_FALSE(refused.error.empty()) << game;
        EXPECT_TRUE(refused.text.empty()) << game;
    }
}

TEST(MappingEdit, AutoMapsTheG920sNamedButtons) {
    Rig rig;
    RigDevice wheel;
    wheel.id = {0x346E, 0x0005};
    wheel.axes[0] = {"Wheel", ControlRole::Steering, false};
    wheel.buttons[20] = {"Y", ControlRole::ButtonY, false};
    wheel.buttons[21] = {"Start", ControlRole::Menu, false};
    wheel.buttons[22] = {"Home", ControlRole::Xbox, false};
    wheel.buttons[23] = {"RSB", ControlRole::Rsb, false};
    rig.devices = {wheel};
    MappingModel model = mappingModel(load(kConfig));
    const DeviceProfile g920(g920Spec());
    EXPECT_EQ(autoMap(model, rig, g920).set, 5);
    EXPECT_EQ(model.buttons[3], (SourceRef{{}, 20}));    // G920 Y
    EXPECT_EQ(model.buttons[6], (SourceRef{{}, 21}));    // G920 Menu
    EXPECT_EQ(model.buttons[10], (SourceRef{{}, 22}));   // G920 Xbox
    EXPECT_EQ(model.buttons[8], (SourceRef{{}, 23}));    // G920 RSB
}

namespace {

RigDevice r3() {
    RigDevice wheel;
    wheel.name = "R3";
    wheel.id = {0x346E, 0x0005};
    wheel.kind = DeviceKind::Wheel;
    wheel.axes[0] = {"Wheel", ControlRole::Steering, false};
    return wheel;
}

std::string allNotes(const AutoMapResult& result) {
    std::string all;
    for (const std::string& note : result.notes) all += note + "\n";
    return all;
}

}  // namespace

TEST(MappingEdit, AutoMapARoleTwiceKeepsTheFirstAndSaysSo) {
    Rig rig;
    RigDevice wheel = r3();
    wheel.buttons[20] = {"Y", ControlRole::ButtonY, false};
    wheel.buttons[21] = {"Y too", ControlRole::ButtonY, false};
    wheel.buttons[22] = {"Up", ControlRole::ShiftUp, false};
    wheel.buttons[23] = {"R", ControlRole::RightPaddle, false};   // the same G920 button as Shift up
    wheel.axes[1] = {"Wheel 2", ControlRole::Steering, false};
    rig.devices = {wheel};
    MappingModel model = mappingModel(load(kConfig));
    const AutoMapResult result = autoMap(model, rig, DeviceProfile(g920Spec()));
    EXPECT_EQ(result.set, 3);   // steering, Y, R paddle
    EXPECT_EQ(model.buttons[3], (SourceRef{{}, 20}));
    EXPECT_EQ(model.buttons[4], (SourceRef{{}, 22}));
    EXPECT_EQ(model.axes[MappingModel::kSteering], (SourceRef{{}, 0}));
    const std::string notes = allNotes(result);
    EXPECT_NE(notes.find("y is also 'Y too' (button 21 of R3): 'Y' (button 20 of R3) kept"), std::string::npos) << notes;
    EXPECT_NE(notes.find("right paddle is also 'R' (button 23 of R3)"), std::string::npos) << notes;
    EXPECT_NE(notes.find("steering is also 'Wheel 2' (axis 1 of R3)"), std::string::npos) << notes;
}

TEST(MappingEdit, AutoMapSaysWhatTheTargetLacks) {
    Rig rig;
    RigDevice wheel = r3();
    wheel.axes[3] = {"Hand", ControlRole::Handbrake, false};
    wheel.axes[4] = {"Knob", ControlRole::OtherAxis, false};   // a plain axis: nothing to say
    rig.devices = {wheel};
    MappingModel model = mappingModel(load(kConfig));
    const MappingModel before = model;
    const AutoMapResult result = autoMap(model, rig, DeviceProfile(g920Spec()));
    EXPECT_EQ(result.set, 1);
    ASSERT_EQ(result.notes.size(), 1U) << allNotes(result);
    EXPECT_NE(result.notes[0].find("has no handbrake axis"), std::string::npos);
    EXPECT_EQ(model.axes[MappingModel::kThrottle], before.axes[MappingModel::kThrottle]);   // untouched
}

TEST(MappingEdit, AutoMapFreesAButtonFromItsOlderPlaces) {
    // The config's D-pad up is button 4 and its button 5 (L paddle) is button 5 (the default).
    Rig rig;
    RigDevice wheel = r3();
    wheel.buttons[4] = {"Y", ControlRole::ButtonY, false};
    wheel.buttons[5] = {"Start", ControlRole::Menu, false};
    rig.devices = {wheel};
    MappingModel model = mappingModel(load(kConfig));
    ASSERT_EQ(model.hat[0], (SourceRef{{}, 4}));
    ASSERT_EQ(model.buttons[5], (SourceRef{{}, 5}));
    const AutoMapResult result = autoMap(model, rig, DeviceProfile(g920Spec()));
    EXPECT_EQ(model.buttons[3], (SourceRef{{}, 4}));   // Y
    EXPECT_EQ(model.buttons[6], (SourceRef{{}, 5}));   // Menu
    EXPECT_FALSE(model.hat[0].mapped());              // no longer D-pad up as well
    EXPECT_FALSE(model.buttons[5].mapped());          // no longer L paddle as well
    EXPECT_FALSE(model.hat[1].mapped());              // D-pad right was button 5 too
    const std::string notes = allNotes(result);
    EXPECT_NE(notes.find("d-pad up: no longer 4"), std::string::npos) << notes;
    EXPECT_NE(notes.find("L paddle: no longer 5"), std::string::npos) << notes;
    // Saved and read back: the same.
    const MappingWriteResult written = writeMapping(kConfig, model);
    ASSERT_TRUE(written.error.empty()) << written.error;
    EXPECT_EQ(mappingModel(load(written.text)), model);
}

TEST(MappingEdit, AutoMapWithAnotherWheelKeepsWhatReadTheOldOne) {
    std::string config = kConfig;
    config.replace(config.find("source_device = 346e "), 21, "source_device = 1111:2222 ");
    MappingModel model = mappingModel(load(config));
    ASSERT_EQ(model.buttons[4], (SourceRef{{}, 13}));   // on the old wheel
    Rig rig;
    rig.devices = {r3()};
    const AutoMapResult result = autoMap(model, rig, DeviceProfile(g920Spec()));
    EXPECT_EQ(result.set, 1);
    EXPECT_EQ(model.sourceDevice, "346e:0005");
    EXPECT_EQ(model.axes[MappingModel::kSteering], (SourceRef{{}, 0}));                   // the new wheel's
    EXPECT_EQ(model.buttons[4], (SourceRef{{0x1111, 0x2222}, 13}));                       // still the old wheel's
    EXPECT_EQ(model.axes[MappingModel::kThrottle], (SourceRef{{0x1111, 0x2222}, 2}));
}

TEST(MappingEdit, AutoMapWithoutRolesChangesNothing) {
    Rig rig;
    RigDevice pedals;
    pedals.name = "Pedals";
    pedals.id = {0x30B7, 0x1001};
    pedals.kind = DeviceKind::Pedals;
    pedals.axes[0] = {"Gas", ControlRole::None, false};
    pedals.buttons[0] = {"Horn", ControlRole::Button, false};
    rig.devices = {pedals};
    MappingModel model = mappingModel(load(kConfig));
    const MappingModel before = model;
    const AutoMapResult result = autoMap(model, rig, DeviceProfile(g920Spec()));
    EXPECT_EQ(result.set, 0);
    EXPECT_TRUE(result.notes.empty());
    EXPECT_EQ(model, before);   // no wheel in the rig: source_device kept too
}
