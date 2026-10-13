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
    "[steering]\r\naxis = 0\r\ninvert = false\r\n"
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


TEST(MappingEdit, RotationReadFromTheConfig) {
    const MappingModel model = mappingModel(load(kConfig));
    EXPECT_EQ(model.steeringRotationDeg, 900.0F);   // the default
    EXPECT_TRUE(tuningProblem(model).empty());
}

// An older config's wheel rotation (gone since 2026-10-13: the axis goes through 1:1 and the wheel's own software is
// set to the steering rotation) is ignored, and goes when it is saved.
TEST(MappingEdit, SavingDropsTheOldWheelRotation) {
    const std::string old = kConfig + "[steering]\r\nsource_rotation_deg = 540\r\n";
    const MappingModel model = mappingModel(load(old));
    EXPECT_EQ(model.steeringRotationDeg, 900.0F);
    const MappingWriteResult written = writeMapping(old, model);
    ASSERT_TRUE(written.error.empty()) << written.error;
    EXPECT_EQ(written.text.find("source_rotation_deg"), std::string::npos) << written.text;
    EXPECT_EQ(mappingModel(load(written.text)), model);
}

// An older config's dead zones (gone since 2026-10-13: the devices' own software sets them) go when it is saved.
TEST(MappingEdit, SavingDropsOldDeadzones) {
    const std::string old = kConfig + "[throttle]\r\ndeadzone_low = 0.02\r\nsaturation_high = 0.98\r\n"
                                      "[steering]\r\ndeadzone = 0.0\r\n";
    const MappingModel model = mappingModel(load(old));
    const MappingWriteResult written = writeMapping(old, model);
    ASSERT_TRUE(written.error.empty()) << written.error;
    EXPECT_EQ(written.text.find("deadzone"), std::string::npos) << written.text;
    EXPECT_EQ(written.text.find("saturation_high"), std::string::npos) << written.text;
    EXPECT_EQ(mappingModel(load(written.text)), model);
}

TEST(MappingEdit, WritesRotationOnlyWhereItChanged) {
    MappingModel model = mappingModel(load(kConfig));
    model.steeringRotationDeg = 180.0F;
    const MappingWriteResult written = writeMapping(kConfig, model);
    ASSERT_TRUE(written.error.empty()) << written.error;
    EXPECT_NE(written.text.find("target_rotation_deg = 180\r\n"), std::string::npos) << written.text;
    EXPECT_EQ(written.text.find("deadzone"), std::string::npos);   // no dead zones: never written
    EXPECT_NE(written.text.find("; checked with --ui"), std::string::npos);   // comments kept

    const EmulatorConfig back = load(written.text);
    EXPECT_EQ(mappingModel(back), model);
    EXPECT_EQ(back.mapper.steering.targetRotationDeg, 180.0F);
    // Written again unchanged: the same text.
    EXPECT_EQ(writeMapping(written.text, model).text, written.text);
}

TEST(MappingEdit, RefusesRotationsOutOfRange) {
    const MappingModel good = mappingModel(load(kConfig));
    for (const float deg : {45.0F, 9000.0F}) {
        MappingModel model = good;
        model.steeringRotationDeg = deg;
        EXPECT_FALSE(tuningProblem(model).empty()) << deg;
        const MappingWriteResult refused = writeMapping(kConfig, model);
        EXPECT_FALSE(refused.error.empty()) << deg;
        EXPECT_TRUE(refused.text.empty()) << deg;
    }
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
