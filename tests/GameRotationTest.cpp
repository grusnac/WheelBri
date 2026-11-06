// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/GameRotation.hpp"

#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/Mapper.hpp"
#include "wheelbri/MappingEdit.hpp"

#include <gtest/gtest.h>

using namespace wheelbri;

namespace {

GameProfile game(const std::string& id, std::optional<float> rotation) {
    GameProfile profile;
    profile.id = id;
    profile.label = "NFS X";
    profile.rotationDeg = rotation;
    return profile;
}

EmulatorConfigResult load(const std::string& text) {
    return loadEmulatorConfig(IniDocument::parse(text).document);
}

}  // namespace

TEST(GameRotation, ParsesAndFormats) {
    EXPECT_EQ(parseGameRotation("180"), (GameRotationOverride{false, 180.0F}));
    // An older file's wheel/game pair (before 2026-10-13): the game's number, the wheel being set to it now.
    EXPECT_EQ(parseGameRotation(" 360 / 180 "), (GameRotationOverride{false, 180.0F}));
    EXPECT_EQ(parseGameRotation("OFF"), (GameRotationOverride{true, {}}));
    for (const char* bad : {"", "fast", "45", "180/45", "/180", "180/", "9000", "x/180"}) {
        EXPECT_FALSE(parseGameRotation(bad).has_value()) << bad;
    }
    EXPECT_EQ(formatGameRotation({false, 180.0F}), "180");
    EXPECT_EQ(formatGameRotation({true, {}}), "off");
    EXPECT_EQ(describeRotation(180.0F), "180 deg");
}

TEST(GameRotation, TheUsersLineWinsOverTheProfile) {
    const GameProfile payback = game("nfspayback", 180.0F);
    const GameProfile plain = game("nfs2015", std::nullopt);
    GameRotations mine;
    EXPECT_EQ(rotationWhileRunning(payback, mine), 180.0F);              // the profile's
    EXPECT_FALSE(rotationWhileRunning(plain, mine).has_value());         // none: the mapping's
    mine["nfspayback"] = {false, 270.0F};
    EXPECT_EQ(rotationWhileRunning(payback, mine), 270.0F);
    mine["nfspayback"] = {true, {}};
    EXPECT_FALSE(rotationWhileRunning(payback, mine).has_value());       // off
    mine["nfs2015"] = {false, 540.0F};
    EXPECT_EQ(rotationWhileRunning(plain, mine), 540.0F);                // a game without one too
}

// A profile for another target: its rotation is not used; the user's own line for the game still is.
TEST(GameRotation, AProfileForAnotherTargetGivesNoRotation) {
    const GameProfile payback = game("nfspayback", 180.0F);
    GameRotations mine;
    EXPECT_FALSE(rotationWhileRunning(payback, mine, false).has_value());
    mine["nfspayback"] = {false, 270.0F};
    EXPECT_EQ(rotationWhileRunning(payback, mine, false), 270.0F);
}

TEST(GameRotation, ConfigReadsAndRefuses) {
    const EmulatorConfigResult good = load("[game_rotation]\nnfspayback = 360/180\n");
    ASSERT_TRUE(good.errors.empty()) << good.errors.front();
    EXPECT_EQ(good.config.gameRotations.at("nfspayback"), (GameRotationOverride{false, 180.0F}));
    const EmulatorConfigResult bad = load("[game_rotation]\nnfspayback = fast\nmygame = 180\n");
    ASSERT_EQ(bad.errors.size(), 2U);
    const std::string all = bad.errors[0] + "\n" + bad.errors[1];
    EXPECT_NE(all.find("nfspayback = 'fast': expected degrees"), std::string::npos) << all;
    EXPECT_NE(all.find("mygame: no built-in game profile"), std::string::npos) << all;
    EXPECT_TRUE(bad.config.gameRotations.empty());
}

TEST(GameRotation, MappingWritesAndDropsLines) {
    const std::string first = "[steering]\naxis = 0\n[game_rotation]\nnfspayback = off   ; mine\n";
    MappingModel model = mappingModel(load(first).config);
    EXPECT_EQ(model.gameRotations.at("nfspayback"), (GameRotationOverride{true, {}}));
    const std::string base = writeMapping(first, model).text;   // the mapping's other lines written once
    EXPECT_NE(base.find("nfspayback = off   ; mine"), std::string::npos) << base;   // unchanged: kept as it was
    EXPECT_EQ(writeMapping(base, model).text, base);
    model.gameRotations["nfspayback"] = {false, 200.0F};
    const std::string written = writeMapping(base, model).text;
    EXPECT_NE(written.find("nfspayback = 200"), std::string::npos) << written;
    EXPECT_EQ(mappingModel(load(written).config), model);
    model.gameRotations.clear();   // Default: the profile's again
    EXPECT_EQ(writeMapping(written, model).text.find("nfspayback"), std::string::npos);
    model.gameRotations["nfspayback"] = {false, 45.0F};
    EXPECT_FALSE(writeMapping(base, model).error.empty());
}

// An older mapping that kept the game's rotation as a wheel/game pair is saved back as the game's number.
TEST(GameRotation, OlderPairSavedAsOneNumber) {
    const std::string old = "[steering]\naxis = 0\n[game_rotation]\nnfspayback = 360/180\n";
    const MappingModel model = mappingModel(load(old).config);
    EXPECT_EQ(model.gameRotations.at("nfspayback"), (GameRotationOverride{false, 180.0F}));
    const std::string written = writeMapping(old, model).text;
    EXPECT_NE(written.find("nfspayback = 180"), std::string::npos) << written;
    EXPECT_EQ(written.find("360"), std::string::npos) << written;
}

// Tabs around the value (a hand-aligned file), in any case: read like spaces.
TEST(GameRotation, TabsAreSpacing) {
    EXPECT_EQ(parseGameRotation("\t270\t"), (GameRotationOverride{false, 270.0F}));
    EXPECT_EQ(parseGameRotation("\tOff\t"), (GameRotationOverride{true, {}}));
    EXPECT_EQ(parseGameRotation("540\t/\t180"), (GameRotationOverride{false, 180.0F}));
    EXPECT_FALSE(parseGameRotation("\t").has_value());
    EXPECT_EQ(formatGameRotation({false, 270.5F}), "270.5");
}
