// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/MappingEdit.hpp"

#include "wheelbri/FileText.hpp"
#include "wheelbri/IniDocument.hpp"

#include <gtest/gtest.h>

#include <limits>

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
    EXPECT_EQ(parseButtonRef("axis 2 > 0%"), (ButtonRef{{{}, 2}, true, false, 0.0F}));     // the whole travel
    EXPECT_EQ(parseButtonRef("axis 2 > 100%"), (ButtonRef{{{}, 2}, true, false, 1.0F}));
    for (const char* bad : {"axis", "axis x", "axis 9", "axis 2 > 101%", "axis 2 > -1%", "axis 2 > lots", "x/axis 1",
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
    model.secondaries[2] = {{{{}, 1}, true, false, 1.01F}};
    EXPECT_FALSE(writeMapping(kConfig, model).error.empty());   // threshold out of range
    // The whole travel: 0% and 100% are written and read back.
    model.secondaries[2] = {{{{}, 1}, true, false, 0.0F}, {{{}, 2}, true, false, 1.0F}};
    const MappingWriteResult ends = writeMapping(kConfig, model);
    ASSERT_TRUE(ends.error.empty()) << ends.error;
    EXPECT_NE(ends.text.find("target_2_or = axis 1 > 0%, axis 2 > 100%"), std::string::npos) << ends.text;
    EXPECT_EQ(mappingModel(load(ends.text)).secondaries[2], model.secondaries[2]);
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
    for (const char* name : {"g920.ini"}) {
        const std::optional<std::string> text = readFileText(std::filesystem::path(WHEELBRI_CONFIG_DIR) / name);
        ASSERT_TRUE(text.has_value()) << name;
        EXPECT_TRUE(controlsUsedTwice(mappingModel(load(*text)), g920).empty()) << name;
    }
    const DeviceProfile g25(g25Spec());
    for (const char* name : {"g25.ini"}) {
        const std::optional<std::string> text = readFileText(std::filesystem::path(WHEELBRI_CONFIG_DIR) / name);
        ASSERT_TRUE(text.has_value()) << name;
        const EmulatorConfig config = load(*text);
        EXPECT_EQ(config.targetProfile, "g25") << name;
        EXPECT_TRUE(controlsUsedTwice(mappingModel(config), g25).empty()) << name;
        // Every input comes from the Rig (AutoMapTest: A, B, X, Y on the black diamond): nothing of one wheel's here.
        EXPECT_TRUE(config.fromRig);
        for (const int source : config.mapper.buttonMap) EXPECT_EQ(source, kUnmapped);
        EXPECT_EQ(config.mapper.steering.sourceAxis, kUnmapped);
        EXPECT_FALSE(config.mapper.hatButtons.any());
    }
    const DeviceProfile g27(g27Spec());
    for (const char* name : {"g27.ini"}) {
        const std::optional<std::string> text = readFileText(std::filesystem::path(WHEELBRI_CONFIG_DIR) / name);
        ASSERT_TRUE(text.has_value()) << name;
        const EmulatorConfig config = load(*text);
        EXPECT_EQ(config.targetProfile, "g27") << name;
        EXPECT_TRUE(controlsUsedTwice(mappingModel(config), g27).empty()) << name;
    }
    const DeviceProfile g923(g923Spec());
    const std::optional<std::string> text = readFileText(std::filesystem::path(WHEELBRI_CONFIG_DIR) / "g923.ini");
    ASSERT_TRUE(text.has_value());
    const EmulatorConfig config = load(*text);
    EXPECT_EQ(config.targetProfile, "g923");
    EXPECT_TRUE(controlsUsedTwice(mappingModel(config), g923).empty());
}

// Ids and sources as a hand edit may break them: too many hex digits, a missing half, a non-hex digit, an index
// below -1 or below 0 on an accessory.
TEST(MappingEdit, RejectsMalformedIdsAndSources) {
    for (const char* bad : {"12345:1", ":1", "1:", "g:1", "1:-1", "346e:0005:1", " 346e:0005", "0000:0000"}) {
        EXPECT_FALSE(parseDeviceId(bad).has_value()) << bad;
    }
    EXPECT_EQ(parseDeviceId("1:0"), (DeviceId{1, 0}));   // only both zero is the empty id
    for (const char* bad : {"-2", "1:1/-1", "1:1/x", "1:1/", "/3", "2 ", "+2", "99999999999"}) {
        EXPECT_FALSE(parseSource(bad).has_value()) << bad;
    }
}

// A secondary list hand-aligned with tabs, and one that names "-1" only once: read as written.
TEST(MappingEdit, SecondaryListsTakeTabs) {
    EXPECT_EQ(parseButtonRefs("\t4\t,\t5 "), (std::vector<ButtonRef>{{{{}, 4}, false, false, 0.5F},
                                                                      {{{}, 5}, false, false, 0.5F}}));
    EXPECT_EQ(parseButtonRefs("  -1  "), std::vector<ButtonRef>{});
    EXPECT_FALSE(parseButtonRefs("-1, -1, 4").has_value());
    EXPECT_FALSE(parseButtonRef("axis 3 > 50% inverted").has_value());   // the threshold comes last
    EXPECT_FALSE(parseButtonRef("1:1/axis -1").has_value());
    // Unmapped entries in a list being written are left out.
    EXPECT_EQ(formatButtonRefs({ButtonRef{}, {{{}, 3}, false, false, 0.5F}, ButtonRef{}}), "3");
    EXPECT_EQ(formatButtonRefs({ButtonRef{}}), "-1");
}

// A device number that is not an accessory slot (negative, past the last, an empty slot) reads as nothing.
TEST(MappingEdit, UnknownDeviceNumbersAreNothing) {
    MapperConfig mapper;
    mapper.accessories[0] = {1, 1};
    EXPECT_EQ(sourceOf(mapper, 1, 3), (SourceRef{{1, 1}, 3}));
    EXPECT_EQ(sourceOf(mapper, -1, 3), SourceRef{});
    EXPECT_EQ(sourceOf(mapper, 2, 3), SourceRef{});   // empty slot
    EXPECT_EQ(sourceOf(mapper, 4, 3), SourceRef{});   // past kMaxAccessories
}

// Uses past the known names (a future profile with more axes or hat directions) still read as something.
TEST(MappingEdit, DescribesUsesPastTheKnownNames) {
    const DeviceProfile g920(g920Spec());
    EXPECT_EQ(describeUse({ControlUse::Kind::Axis, 7}, g920), "Role: G920 axis");
    EXPECT_EQ(describeUse({ControlUse::Kind::Hat, 4}, g920), "Role: G920 D-pad");
    EXPECT_EQ(describeUse({ControlUse::Kind::Hat, 3}, g920), "Role: G920 D-pad Left");
    EXPECT_EQ(describeUse({ControlUse::Kind::Axis, MappingModel::kClutch}, g920), "Role: G920 clutch");
}

// Values that must never be saved, NaN among them (every comparison with it is false, so a plain range check lets
// it through): an axis threshold, a game's gain, a game setting. A game rotation that is off is not checked.
TEST(MappingEdit, RefusesOutOfRangeAndNotANumberValues) {
    const MappingModel good = mappingModel(load(kConfig));
    const float nan = std::numeric_limits<float>::quiet_NaN();

    MappingModel model = good;
    model.secondaries[2] = {{{{}, 1}, true, false, nan}};
    EXPECT_NE(tuningProblem(model).find("button 2: an axis presses it between 0% and 100%"), std::string::npos)
        << tuningProblem(model);
    EXPECT_FALSE(writeMapping(kConfig, model).error.empty());
    model.secondaries[2] = {{{{}, 1}, true, false, -0.01F}};
    EXPECT_FALSE(tuningProblem(model).empty());
    model.secondaries[2] = {{{{}, 1}, true, false, 0.0F}};
    EXPECT_TRUE(tuningProblem(model).empty());
    model.secondaries[2] = {{{{}, 1}, false, false, nan}};   // a button's threshold is never used
    EXPECT_TRUE(tuningProblem(model).empty());
    model.secondaries[2] = {{{{}, 1}, true, false, 1.0F}};
    EXPECT_TRUE(tuningProblem(model).empty());

    model = good;
    model.gameSettings["nfsunbound"]["spring"] = nan;
    EXPECT_EQ(tuningProblem(model), "nfsunbound.spring: from 0 to 1");
    model.gameSettings["nfsunbound"]["spring"] = -0.5F;
    EXPECT_FALSE(tuningProblem(model).empty());

    model = good;
    model.gameRotations["nfs2015"] = {true, nan};   // off: its degrees mean nothing
    EXPECT_TRUE(tuningProblem(model).empty());
    model.gameRotations["nfs2015"] = {false, nan};
    EXPECT_NE(tuningProblem(model).rfind("nfs2015: ", 0), std::string::npos);
}

// Lines for games the user no longer sets go when saving, even a hand-written key with no dot; lines that already
// say the value (in another spelling: "1.0" for 1, "0.80" for 0.8) are left as written, comments and all. The retired
// [game_ffb_gain] loses its lines.
TEST(MappingEdit, GameLinesAreDroppedOrKeptAsWritten) {
    const std::string base = kConfig +
                             "[game_settings]\r\nnfsunbound.spring = 0.80   ; mine\r\nnfsunbound = 1\r\n"
                             "[game_ffb_gain]\r\nnfsunbound = 2.0  ; strong\r\nnfs2015 = 1\r\n"
                             "[game_rotation]\r\nnfs2015 = 360/180\r\nnfspayback = 270  ; tight\r\n";
    const auto parsed = IniDocument::parse(base);
    ASSERT_TRUE(parsed.errors.empty());
    MappingModel model = mappingModel(loadEmulatorConfig(parsed.document).config);
    const MappingWriteResult written = writeMapping(base, model);
    ASSERT_TRUE(written.error.empty()) << written.error;
    const std::string& text = written.text;
    EXPECT_NE(text.find("nfsunbound.spring = 0.80   ; mine\r\n"), std::string::npos) << text;
    EXPECT_EQ(text.find("nfsunbound = 1\r\n"), std::string::npos) << text;   // no dot: not a setting
    EXPECT_EQ(text.find("nfsunbound = 2.0"), std::string::npos) << text;   // [game_ffb_gain]: retired
    EXPECT_EQ(text.find("nfs2015 = 1\r\n"), std::string::npos) << text;
    EXPECT_NE(text.find("nfs2015 = 180\r\n"), std::string::npos) << text;    // the older pair as the one number
    EXPECT_NE(text.find("nfspayback = 270  ; tight\r\n"), std::string::npos) << text;
    EXPECT_EQ(writeMapping(text, model).text, text);   // and saving again changes nothing
}

// An inverted pedal set back to normal: its "invert = true" line (here in a second [clutch] section, as a hand edit
// may leave it) becomes false, not left saying true; a pedal newly inverted gets the line.
TEST(MappingEdit, InvertIsWrittenOverAnExistingLineOnly) {
    MappingModel model = mappingModel(load(kConfig + "[clutch]\r\ninvert = true\r\n"));
    ASSERT_TRUE(model.invert[MappingModel::kClutch]);
    model.invert[MappingModel::kClutch] = false;
    model.invert[MappingModel::kBrake] = true;
    const std::string text = writeMapping(kConfig + "[clutch]\r\ninvert = true\r\n", model).text;
    const EmulatorConfig back = load(text);
    EXPECT_FALSE(back.mapper.clutch.invert);
    EXPECT_TRUE(back.mapper.brake.invert);
    const std::size_t first = text.find("invert = true");
    ASSERT_NE(first, std::string::npos) << text;
    EXPECT_EQ(text.find("invert = true", first + 1), std::string::npos) << text;   // the brake's only
}

// [buttons] order: written when it is not what the file says (no line: the driver's order), and read back.
TEST(MappingEdit, ButtonOrderIsWrittenWhenItChanges) {
    MappingModel model = mappingModel(load(kConfig));
    EXPECT_EQ(model.buttonOrder, ButtonOrder::Driver);
    EXPECT_EQ(writeMapping(kConfig, model).text.find("order ="), std::string::npos);   // the default: no line
    model.buttonOrder = ButtonOrder::Native;
    const std::string text = writeMapping(kConfig, model).text;
    EXPECT_NE(text.find("order = native"), std::string::npos) << text;
    EXPECT_EQ(load(text).buttonOrder, ButtonOrder::Native);
    model.buttonOrder = ButtonOrder::Driver;
    EXPECT_EQ(load(writeMapping(text, model).text).buttonOrder, ButtonOrder::Driver);
}

// Lines the reader could not read (a hand edit) are written again with the model's value, not kept; a game setting
// of a known game that the user no longer sets goes; an unmapped secondary in the model is not written.
TEST(MappingEdit, UnreadableLinesAreWrittenAgain) {
    const std::string base = kConfig +
                             "[game_rotation]\r\nnfs2015 = sideways\r\n"
                             "[game_settings]\r\nnfsunbound.spring = lots\r\nnfsunbound.force_feedback = 1\r\n";
    MappingModel model = mappingModel(load(kConfig));
    model.gameRotations["nfs2015"] = {false, 270.0F};
    model.gameSettings["nfsunbound"]["spring"] = 0.7F;
    model.secondaries[1] = {ButtonRef{}, {{{}, 9}, false, false, 0.5F}};
    model.steeringRotationDeg = 540.0F;
    const MappingWriteResult written = writeMapping(base + "[steering]\r\ntarget_rotation_deg = wide\r\n", model);
    ASSERT_TRUE(written.error.empty()) << written.error;
    const auto parsed = IniDocument::parse(written.text);
    ASSERT_TRUE(parsed.errors.empty());
    const EmulatorConfigResult back = loadEmulatorConfig(parsed.document);
    ASSERT_TRUE(back.errors.empty()) << back.errors.front();
    EXPECT_FLOAT_EQ(back.config.mapper.steering.targetRotationDeg, 540.0F);
    EXPECT_EQ(back.config.gameRotations.at("nfs2015"), (GameRotationOverride{false, 270.0F}));
    EXPECT_FLOAT_EQ(back.config.gameSettings.at("nfsunbound").at("spring"), 0.7F);
    EXPECT_EQ(back.config.gameSettings.at("nfsunbound").count("force_feedback"), 0U);
    EXPECT_NE(written.text.find("target_1_or = 9\r\n"), std::string::npos) << written.text;
    // One button ref given straight, with spaces around it (the list reader trims them first).
    EXPECT_EQ(parseButtonRef("  5  "), (ButtonRef{{{}, 5}, false, false, 0.5F}));
}

// [game_tuning]: the user's values are written one line each, an unchanged line keeps its text ("yes" for true), a
// value no longer set loses its line, a retired key ([game_tuning] ffb_*) goes, and writing again changes nothing.
TEST(MappingEdit, WritesGameTuning) {
    const std::string base = kConfig + "[game_tuning]\r\nnfsunbound.throttle_invert = yes   ; mine\r\n"
                                       "nfsunbound.ffb_spring = 0.8\r\nnfs2015.brake_invert = true\r\n";
    const auto parsed = IniDocument::parse(base);
    ASSERT_TRUE(parsed.errors.empty());
    MappingModel model = mappingModel(loadEmulatorConfig(parsed.document).config);
    ASSERT_EQ(model.gameTunings.size(), 2U);
    model.gameTunings.erase("nfs2015");
    GameTuning& unbound = model.gameTunings["nfsunbound"];
    unbound.pedalAxes[1] = ObjectGuid::YAxis;
    unbound.buttons[4] = {{}, 13};
    const MappingWriteResult written = writeMapping(base, model);
    ASSERT_TRUE(written.error.empty()) << written.error;
    const std::string& text = written.text;
    EXPECT_NE(text.find("nfsunbound.throttle_invert = yes   ; mine\r\n"), std::string::npos) << text;
    EXPECT_EQ(text.find("ffb_spring"), std::string::npos) << text;
    EXPECT_NE(text.find("nfsunbound.brake_axis = y"), std::string::npos) << text;
    EXPECT_NE(text.find("nfsunbound.button_4 = 13"), std::string::npos) << text;
    EXPECT_EQ(text.find("nfs2015.brake_invert"), std::string::npos) << text;
    EXPECT_EQ(writeMapping(text, model).text, text);
    const auto again = IniDocument::parse(text);
    EXPECT_EQ(mappingModel(loadEmulatorConfig(again.document).config).gameTunings, model.gameTunings);
}

// tuningProblem refuses a value the config reader would refuse (NaN, out of range), so Save never writes one.
TEST(MappingEdit, RefusesBadGameTuning) {
    MappingModel model = mappingModel(load(kConfig));
    model.gameTunings["nfs2015"].buttons[999] = {{}, 2};
    EXPECT_NE(tuningProblem(model).find("nfs2015 button_999"), std::string::npos) << tuningProblem(model);
    model.gameTunings["nfs2015"].buttons.clear();
    model.gameTunings["nfs2015"].buttons[4] = {{}, 2};
    EXPECT_EQ(tuningProblem(model), "");
}

// Game lines the base does not have yet are added; lines with another value, or one the reader cannot read, are
// replaced; a secondary list that changed is written again.
TEST(MappingEdit, GameLinesAreAddedAndReplaced) {
    const std::string base = kConfig +
                             "[buttons]\r\ntarget_1_or = 9\r\n"
                             "[game_settings]\r\nnfsunbound.spring = 0.5\r\n"
                             "[game_tuning]\r\nnfsunbound.brake_invert = false\r\nnfsunbound.brake_axis = lots\r\n";
    MappingModel model = mappingModel(load(kConfig));
    model.gameRotations["nfspayback"] = {false, 180.0F};               // no line yet
    model.gameSettings["nfsunbound"]["spring"] = 0.7F;                 // another value
    model.gameSettings["nfsunbound"]["force_feedback"] = 0.9F;         // no line yet
    model.gameTunings["nfsunbound"].pedalInvert[1] = true;             // another value
    model.gameTunings["nfsunbound"].pedalAxes[1] = ObjectGuid::YAxis;  // over a line that cannot be read
    model.secondaries[1] = {{{{}, 10}, false, false, 0.5F}};           // 9 before
    const MappingWriteResult written = writeMapping(base, model);
    ASSERT_TRUE(written.error.empty()) << written.error;
    const std::string& text = written.text;
    EXPECT_NE(text.find("nfspayback = 180"), std::string::npos) << text;
    EXPECT_NE(text.find("nfsunbound.spring = 0.7"), std::string::npos) << text;
    EXPECT_NE(text.find("nfsunbound.force_feedback = 0.9"), std::string::npos) << text;
    EXPECT_NE(text.find("nfsunbound.brake_invert = true"), std::string::npos) << text;
    EXPECT_NE(text.find("nfsunbound.brake_axis = y"), std::string::npos) << text;
    EXPECT_NE(text.find("target_1_or = 10"), std::string::npos) << text;
    EXPECT_EQ(text.find("lots"), std::string::npos) << text;
    EXPECT_EQ(writeMapping(text, model).text, text);
}

// An axis used twice, and a control on an accessory used twice, are named as such; an empty secondary is no use.
TEST(MappingEdit, AxesAndAccessoriesUsedTwiceAreNamed) {
    MappingModel model = mappingModel(load(kConfig));
    const DeviceProfile g920(g920Spec());
    model.buttons[5] = model.buttons[6] = model.buttons[7] = model.buttons[13] = {};
    model.secondaries[0] = {ButtonRef{}, {{{}, 2}, true, false, 0.5F}};   // the throttle's axis presses A too
    model.secondaries[1] = {{{{0x30B7, 0x1001}, 3}, false, false, 0.5F}};
    model.secondaries[3] = {{{{0x30B7, 0x1001}, 3}, false, false, 0.5F}};
    const std::vector<std::string> twice = controlsUsedTwice(model, g920);
    ASSERT_EQ(twice.size(), 2U);
    EXPECT_EQ(twice[0], "axis 2 of the wheel: role: G920 throttle, secondary of G920 A");
    EXPECT_EQ(twice[1], "button 3 of 30b7:1001: secondary of G920 B, secondary of G920 Y");
}

// A centring spring below 0 is refused, like one past the top or not a number.
TEST(MappingEdit, ANegativeCentringSpringIsRefused) {
    MappingModel model = mappingModel(load(kConfig));
    model.centringSpringPercent = -1.0F;
    EXPECT_EQ(tuningProblem(model).rfind("centring spring: from 0 (off) to ", 0), 0U) << tuningProblem(model);
}
