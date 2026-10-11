#include "wheelbri/MappingEdit.hpp"

#include "wheelbri/FileText.hpp"
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
    EXPECT_NE(notes.find("D-pad Up: no longer 4"), std::string::npos) << notes;
    EXPECT_NE(notes.find("L Paddle: no longer 5"), std::string::npos) << notes;
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

TEST(MappingEdit, ButtonSecondSourcesParseAndFormat) {
    EXPECT_EQ(parseButtonRef("5"), (ButtonRef{{{}, 5}, false, false, 0.5F}));
    EXPECT_EQ(parseButtonRef("-1"), (ButtonRef{{{}, kUnmapped}, false, false, 0.5F}));
    EXPECT_EQ(parseButtonRef("axis 3 > 40%"), (ButtonRef{{{}, 3}, true, false, 0.4F}));
    EXPECT_EQ(parseButtonRef(" 30b7:1001/axis 3 inverted > 60% "), (ButtonRef{{{0x30B7, 0x1001}, 3}, true, true, 0.6F}));
    EXPECT_EQ(parseButtonRef("axis 2"), (ButtonRef{{{}, 2}, true, false, 0.5F}));   // 50% when left out
    for (const char* bad : {"axis", "axis x", "axis 9", "axis 2 > 100%", "axis 2 > 1%", "axis 2 > lots", "x/axis 1",
                            "30b7:1001axis 1", "button"}) {
        EXPECT_FALSE(parseButtonRef(bad).has_value()) << bad;
    }
    EXPECT_EQ(formatButtonRef({{{}, 3}, true, false, 0.4F}), "axis 3 > 40%");
    EXPECT_EQ(formatButtonRef({{{0x30B7, 0x1001}, 3}, true, true, 0.5F}), "30b7:1001/axis 3 inverted > 50%");
    EXPECT_EQ(formatButtonRef({{{}, 7}, false, false, 0.5F}), "7");
    EXPECT_EQ(formatButtonRef({}), "-1");
}

TEST(MappingEdit, SecondaryListsParseAndFormat) {
    EXPECT_EQ(parseButtonRefs("-1"), std::vector<ButtonRef>{});
    EXPECT_EQ(parseButtonRefs(""), std::vector<ButtonRef>{});
    EXPECT_EQ(parseButtonRefs("20, 30b7:1001/4 ,axis 3 > 40%"),
              (std::vector<ButtonRef>{{{{}, 20}, false, false, 0.5F},
                                      {{{0x30B7, 0x1001}, 4}, false, false, 0.5F},
                                      {{{}, 3}, true, false, 0.4F}}));
    for (const char* bad : {"1,,2", "1,", ",1", "-1, 4", "4, x", "axis 9, 1"}) {
        EXPECT_FALSE(parseButtonRefs(bad).has_value()) << bad;
    }
    EXPECT_EQ(formatButtonRefs({}), "-1");
    EXPECT_EQ(formatButtonRefs({{{{}, 20}, false, false, 0.5F}, {{{0x30B7, 0x1001}, 3}, true, true, 0.4F}}),
              "20, 30b7:1001/axis 3 inverted > 40%");
}

TEST(MappingEdit, SecondariesWriteAndReadBack) {
    MappingModel model = mappingModel(load(kConfig));
    // G920 X <- also the handbrake lever, a button box button and wheel button 20; A <- also wheel button 21.
    model.secondaries[2] = {{{{0x30B7, 0x1001}, 3}, true, false, 0.4F},
                            {{{0x30B7, 0x1001}, 4}, false, false, 0.5F},
                            {{{}, 20}, false, false, 0.5F}};
    model.secondaries[0] = {{{{}, 21}, false, false, 0.5F}};
    const MappingWriteResult written = writeMapping(kConfig, model);
    ASSERT_TRUE(written.error.empty()) << written.error;
    EXPECT_NE(written.text.find("target_2_or = 30b7:1001/axis 3 > 40%, 30b7:1001/4, 20"), std::string::npos)
        << written.text;
    EXPECT_NE(written.text.find("target_0_or = 21"), std::string::npos) << written.text;
    const EmulatorConfig back = load(written.text);
    EXPECT_EQ(mappingModel(back), model);
    EXPECT_TRUE(back.mapper.buttonSecondary[2][0].axis);
    EXPECT_NEAR(back.mapper.buttonSecondary[2][0].threshold, 0.4F, 1e-6F);
    EXPECT_EQ(back.mapper.buttonSecondary[2][2], (ButtonSource{0, 20, false, false, 0.5F}));
    EXPECT_FALSE(back.mapper.buttonSecondary[2][3].mapped());
    EXPECT_EQ(back.mapper.buttonMap[2], 2);   // the role assignment stays
    EXPECT_EQ(writeMapping(written.text, model).text, written.text);   // unchanged: the same text

    model.secondaries[2].clear();
    EXPECT_EQ(writeMapping(written.text, model).text.find("target_2_or"), std::string::npos);
    model.secondaries[2] = {{{{}, 1}, true, false, 0.99F}};
    EXPECT_FALSE(writeMapping(kConfig, model).error.empty());   // threshold out of range
    model.secondaries[2].clear();
    for (int i = 0; i < 9; ++i) model.secondaries[2].push_back({{{}, 40 + i}, false, false, 0.5F});
    EXPECT_FALSE(writeMapping(kConfig, model).error.empty());   // 9: at most 8
}

TEST(MappingEdit, UsesOfAControl) {
    MappingModel model = mappingModel(load(kConfig));
    model.sourceDevice = "346e:0005";
    model.secondaries[2] = {{{{0x30B7, 0x1001}, 3}, true, false, 0.5F}, {{{0x346E, 0x0005}, 30}, false, false, 0.5F}};
    const DeviceProfile g920(g920Spec());
    using Kind = ControlUse::Kind;
    EXPECT_EQ(usesOf(model, g920, {{{}, 2}, true}), (std::vector<ControlUse>{{Kind::Axis, MappingModel::kThrottle}}));
    // Button 13: the R paddle, and Gear 2 too (target 13 <- button 13 by default: this config does not clear it).
    EXPECT_EQ(usesOf(model, g920, {{{}, 13}, false}), (std::vector<ControlUse>{{Kind::Button, 4}, {Kind::Button, 13}}));
    EXPECT_EQ(usesOf(model, g920, {{{}, 4}, false}), (std::vector<ControlUse>{{Kind::Hat, 0}}));
    EXPECT_EQ(usesOf(model, g920, {{{0x30B7, 0x1001}, 3}, true}), (std::vector<ControlUse>{{Kind::Secondary, 2}}));
    EXPECT_TRUE(usesOf(model, g920, {{{0x30B7, 0x1001}, 3}, false}).empty());   // button 3, not axis 3
    // On the wheel's own id: the wheel's, either way it is asked.
    EXPECT_EQ(usesOf(model, g920, {{{}, 30}, false}), (std::vector<ControlUse>{{Kind::Secondary, 2}}));
    EXPECT_EQ(usesOf(model, g920, {{{0x346E, 0x0005}, 30}, false}), (std::vector<ControlUse>{{Kind::Secondary, 2}}));
    // Buttons the G920 does not name reach no game: not uses (target 25 <- button 25 by default).
    EXPECT_TRUE(usesOf(model, g920, {{{}, 25}, false}).empty());
    EXPECT_TRUE(usesOf(model, g920, {{{}, 60}, false}).empty());

    EXPECT_EQ(describeUse({Kind::Button, 0}, g920), "Role: G920 A");
    EXPECT_EQ(describeUse({Kind::Axis, MappingModel::kThrottle}, g920), "Role: G920 throttle");
    EXPECT_EQ(describeUse({Kind::Hat, 0}, g920), "Role: G920 D-pad Up");
    EXPECT_EQ(describeUse({Kind::Secondary, 2}, g920), "Secondary of G920 X");
}

TEST(MappingEdit, ControlsUsedTwice) {
    MappingModel model = mappingModel(load(kConfig));
    const DeviceProfile g920(g920Spec());
    // This config's D-pad buttons 5, 6, 7 also drive G920 L paddle, Menu and View, and its R paddle (13) Gear 2
    // (target N <- button N by default).
    std::vector<std::string> twice = controlsUsedTwice(model, g920);
    ASSERT_EQ(twice.size(), 4U);
    EXPECT_EQ(twice[0], "button 5 of the wheel: role: G920 D-pad Right, role: G920 L Paddle");
    EXPECT_EQ(twice[3], "button 13 of the wheel: role: G920 R Paddle, role: G920 Gear 2");
    model.buttons[5] = model.buttons[6] = model.buttons[7] = model.buttons[13] = {};
    EXPECT_TRUE(controlsUsedTwice(model, g920).empty());
    model.secondaries[1] = {{{{}, 13}, false, false, 0.5F}};   // R paddle's button as B's secondary too
    twice = controlsUsedTwice(model, g920);
    ASSERT_EQ(twice.size(), 1U);
    EXPECT_EQ(twice[0], "button 13 of the wheel: role: G920 R Paddle, secondary of G920 B");
    // The built-in configs use every control once.
    for (const char* name : {"g920-virtual-device.ini", "g920-in-game-shim.ini"}) {
        const std::optional<std::string> text = readFileText(std::filesystem::path(WHEELBRI_CONFIG_DIR) / name);
        ASSERT_TRUE(text.has_value()) << name;
        EXPECT_TRUE(controlsUsedTwice(mappingModel(load(*text)), g920).empty()) << name;
    }
}

TEST(MappingEdit, AutoMapKeepsSecondaries) {
    MappingModel model = mappingModel(load(kConfig));
    model.secondaries[2] = {{{{}, 13}, false, false, 0.5F},   // X also <- button 13, whose role is the R paddle
                            {{{}, 3}, true, false, 0.5F}};    // ... and the axis whose role is the clutch
    Rig rig;
    RigDevice wheel;
    wheel.id = {0x346E, 0x0005};
    wheel.axes[0] = {"Wheel", ControlRole::Steering, false};
    wheel.axes[3] = {"Clutch", ControlRole::Clutch, false};
    wheel.axes[4] = {"Hand", ControlRole::Handbrake, false};
    wheel.buttons[13] = {"R", ControlRole::RightPaddle, false};
    wheel.buttons[0] = {"A", ControlRole::ButtonA, false};
    rig.devices = {wheel};
    const MappingModel before = model;
    const AutoMapResult result = autoMap(model, rig, DeviceProfile(g920Spec()));
    EXPECT_EQ(model.secondaries, before.secondaries);   // the user's: kept
    EXPECT_FALSE(model.buttons[4].mapped());            // R paddle left empty (was button 13)
    EXPECT_FALSE(model.axes[MappingModel::kClutch].mapped());
    EXPECT_EQ(model.buttons[0], (SourceRef{{}, 0}));
    EXPECT_EQ(result.set, 2);   // steering and A
    std::string all;
    for (const std::string& note : result.notes) all += note + "\n";
    EXPECT_NE(all.find("'R' (button 13 of 346e:0005) is a secondary of X: G920 R Paddle left empty; remove it from X "
                       "to put it on G920 R Paddle"),
              std::string::npos)
        << all;
    EXPECT_NE(all.find("is a secondary of X: G920 clutch left empty"), std::string::npos) << all;
    EXPECT_NE(all.find("add it as a secondary of the button your game brakes with"), std::string::npos) << all;
}

TEST(MappingEdit, AutoMapFreesAnAxisPlacedElsewhere) {
    MappingModel model = mappingModel(load(kConfig));   // throttle axis 2, brake 5
    Rig rig;
    RigDevice wheel;
    wheel.id = {0x346E, 0x0005};
    wheel.axes[0] = {"Wheel", ControlRole::Steering, false};
    wheel.axes[5] = {"Gas", ControlRole::Throttle, false};   // axis 5 is the throttle now: no longer the brake
    rig.devices = {wheel};
    const AutoMapResult result = autoMap(model, rig, DeviceProfile(g920Spec()));
    EXPECT_EQ(model.axes[MappingModel::kThrottle], (SourceRef{{}, 5}));
    EXPECT_FALSE(model.axes[MappingModel::kBrake].mapped());
    std::string all;
    for (const std::string& note : result.notes) all += note + "\n";
    EXPECT_NE(all.find("brake: no longer 5"), std::string::npos) << all;
}
