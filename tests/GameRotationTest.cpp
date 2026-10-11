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
    EXPECT_EQ(parseGameRotation("180"), (GameRotationOverride{false, {180.0F, 180.0F}}));
    EXPECT_EQ(parseGameRotation(" 360 / 180 "), (GameRotationOverride{false, {360.0F, 180.0F}}));
    EXPECT_EQ(parseGameRotation("OFF"), (GameRotationOverride{true, {}}));
    for (const char* bad : {"", "fast", "45", "180/45", "/180", "180/", "9000"}) {
        EXPECT_FALSE(parseGameRotation(bad).has_value()) << bad;
    }
    EXPECT_EQ(formatGameRotation({false, {180.0F, 180.0F}}), "180");
    EXPECT_EQ(formatGameRotation({false, {360.0F, 180.0F}}), "360/180");
    EXPECT_EQ(formatGameRotation({true, {}}), "off");
    EXPECT_EQ(describeRotation({180.0F, 180.0F}), "180 deg");
    EXPECT_EQ(describeRotation({360.0F, 180.0F}), "wheel 360 / game 180 deg");
    EXPECT_EQ((SteeringRotation{360.0F, 180.0F}).ratio(), 2.0F);
}

TEST(GameRotation, TheUsersLineWinsOverTheProfile) {
    const GameProfile payback = game("nfspayback", 180.0F);
    const GameProfile plain = game("nfs2015", std::nullopt);
    GameRotations mine;
    EXPECT_EQ(rotationWhileRunning(payback, mine), (SteeringRotation{180.0F, 180.0F}));   // the profile's
    EXPECT_FALSE(rotationWhileRunning(plain, mine).has_value());                           // none: the mapping's
    mine["nfspayback"] = {false, {270.0F, 270.0F}};
    EXPECT_EQ(rotationWhileRunning(payback, mine), (SteeringRotation{270.0F, 270.0F}));
    mine["nfspayback"] = {true, {}};
    EXPECT_FALSE(rotationWhileRunning(payback, mine).has_value());                         // off
    mine["nfs2015"] = {false, {540.0F, 540.0F}};
    EXPECT_EQ(rotationWhileRunning(plain, mine), (SteeringRotation{540.0F, 540.0F}));      // a game without one too
}

TEST(GameRotation, ConfigReadsAndRefuses) {
    const EmulatorConfigResult good = load("[game_rotation]\nnfspayback = 360/180\n");
    ASSERT_TRUE(good.errors.empty()) << good.errors.front();
    EXPECT_EQ(good.config.gameRotations.at("nfspayback"), (GameRotationOverride{false, {360.0F, 180.0F}}));
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
    model.gameRotations["nfspayback"] = {false, {200.0F, 200.0F}};
    const std::string written = writeMapping(base, model).text;
    EXPECT_NE(written.find("nfspayback = 200"), std::string::npos) << written;
    EXPECT_EQ(mappingModel(load(written).config), model);
    model.gameRotations.clear();   // Default: the profile's again
    EXPECT_EQ(writeMapping(written, model).text.find("nfspayback"), std::string::npos);
    model.gameRotations["nfspayback"] = {false, {45.0F, 45.0F}};
    EXPECT_FALSE(writeMapping(base, model).error.empty());
}

TEST(GameRotation, MapperSteersWithAnotherRatio) {
    MapperConfig config;
    config.steering.sourceRotationDeg = 360.0F;
    config.steering.targetRotationDeg = 180.0F;   // the mapping's own: ratio 2
    const Mapper mapper(config);
    RawInputs inputs;
    inputs.devices[0].axes[0] = 0.25F;   // a quarter of the wheel's travel: 45 degrees
    EXPECT_FLOAT_EQ(mapper.degreeRatio(), 2.0F);
    EXPECT_FLOAT_EQ(mapper.map(inputs).steering, 0.5F);
    EXPECT_FLOAT_EQ(mapper.map(inputs, 1.0F).steering, 0.25F);   // a game's 180/180: 1:1
    EXPECT_FLOAT_EQ(mapper.map(inputs, 0.0F).steering, 0.5F);    // 0: the mapping's own
    inputs.devices[0].axes[0] = 1.0F;
    EXPECT_FLOAT_EQ(mapper.map(inputs, 1.0F).steering, 1.0F);    // full lock at the wheel's end stop
}
