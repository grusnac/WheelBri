#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/FfbLimits.hpp"
#include "wheelbri/FileText.hpp"
#include "wheelbri/IniDocument.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>

using namespace wheelbri;

namespace {

EmulatorConfig load(const std::string& text) {
    const auto parsed = IniDocument::parse(text);
    EXPECT_TRUE(parsed.errors.empty());
    const auto result = loadEmulatorConfig(parsed.document);
    EXPECT_TRUE(result.errors.empty()) << (result.errors.empty() ? std::string() : result.errors.front());
    return result.config;
}

}  // namespace

// The strength is the wheel's own app's: WheelBri passes all the game asks; its limiter keeps the guards.
TEST(FfbLimits, TheDefaultsPassNearlyAllTheGameAsks) {
    const FfbSafetyConfig defaults;
    EXPECT_FLOAT_EQ(defaults.masterGain, kWheelAppMasterGain);
    EXPECT_FLOAT_EQ(defaults.maxMagnitude, kWheelAppMaxMagnitude);
    EXPECT_FLOAT_EQ(defaults.maxMagnitude, 1.0F);   // all the game asks: the wheel's app sets how strong
    EXPECT_GT(defaults.maxSlewPerSecond, 0.0F);      // the jolt guard stays
    EXPECT_GT(defaults.frozenForceSeconds, 0.0F);    // and the frozen push fade
}

TEST(FfbLimits, EveryShippedConfigHasTheNewLimits) {
    for (const char* name : {"g25.ini", "g27.ini", "g920.ini", "g923.ini"}) {
        const std::optional<std::string> text = readFileText(std::filesystem::path(WHEELBRI_CONFIG_DIR) / name);
        ASSERT_TRUE(text.has_value()) << name;
        const EmulatorConfig config = load(*text);
        EXPECT_FLOAT_EQ(config.ffb.masterGain, kWheelAppMasterGain) << name;
        EXPECT_FLOAT_EQ(config.ffb.maxMagnitude, kWheelAppMaxMagnitude) << name;
    }
}

// A user's own config moves to the new limits and keeps everything else, comments and CRLF included.
TEST(FfbLimits, MovingAConfigChangesOnlyTheTwoKeys) {
    const std::string old = "; my own\r\n[emulator]\r\ntarget_profile = g25\r\n[ffb]\r\nmaster_gain = 0.7\r\n"
                            "max_magnitude = 0.8             ; hard cap\r\nmax_slew_per_second = 30\r\n"
                            "invert = true\r\n";
    const std::string moved = withWheelAppLimits(old);
    EXPECT_EQ(moved, "; my own\r\n[emulator]\r\ntarget_profile = g25\r\n[ffb]\r\nmaster_gain = 1.0\r\n"
                     "max_magnitude = 1.0\r\nmax_slew_per_second = 30\r\ninvert = true\r\n");
    const EmulatorConfig config = load(moved);
    EXPECT_FLOAT_EQ(config.ffb.maxMagnitude, 1.0F);
    EXPECT_FLOAT_EQ(config.ffb.maxSlewPerSecond, 30.0F);
    EXPECT_TRUE(config.ffbInvert);
    // Opening a config: only the exact older pair is moved; anything else is the user's own choice.
    const std::optional<std::string> fromOld = movedFromOldLimits(old);
    ASSERT_TRUE(fromOld.has_value());
    EXPECT_EQ(*fromOld, moved);
    EXPECT_FALSE(movedFromOldLimits(moved).has_value());   // already moved: nothing to do
    EXPECT_FALSE(movedFromOldLimits("[ffb]\nmaster_gain = 0.7\nmax_magnitude = 0.6\n").has_value());
    EXPECT_FALSE(movedFromOldLimits("[ffb]\nmaster_gain = 0.5\nmax_magnitude = 0.8\n").has_value());
    EXPECT_FALSE(movedFromOldLimits("[ffb]\nmaster_gain = 0.7\n").has_value());
    EXPECT_FALSE(movedFromOldLimits("[ffb]\nmaster_gain = soft\nmax_magnitude = 0.8\n").has_value());
    EXPECT_FALSE(movedFromOldLimits("[emulator]\ntarget_profile = g920\n").has_value());
    EXPECT_TRUE(movedFromOldLimits("[ffb]\nmaster_gain = 0.70 # mine\nmax_magnitude = 0.80\n").has_value());
    // A game's strength or cap that only repeats the older limits goes with them; other per-game values stay.
    const std::optional<std::string> games =
        movedFromOldLimits("[ffb]\nmaster_gain = 0.7\nmax_magnitude = 0.8\n[game_tuning]\n"
                           "nfsunbound.ffb_strength = 0.7\ngrid.ffb_cap = 0.8\ngrid.ffb_strength = 0.5\n"
                           "nfs2015.ffb_cap = 0.6\nnfsunbound.ffb_spring = 0.8\n");
    ASSERT_TRUE(games.has_value());
    EXPECT_EQ(*games, "[ffb]\nmaster_gain = 1.0\nmax_magnitude = 1.0\n[game_tuning]\ngrid.ffb_strength = 0.5\n"
                      "nfs2015.ffb_cap = 0.6\nnfsunbound.ffb_spring = 0.8\n");
    // A config with no [ffb] gets one.
    const EmulatorConfig added = load(withWheelAppLimits("[emulator]\ntarget_profile = g920\n"));
    EXPECT_FLOAT_EQ(added.ffb.masterGain, 1.0F);
    EXPECT_FLOAT_EQ(added.ffb.maxMagnitude, 1.0F);
}

// A limit with something after the number, or one that is not finite, is not the older pair; a game's value that is
// not a number (or has something after it) is left alone when the pair moves.
TEST(FfbLimits, OnlyWholeFiniteNumbersAreTheOlderPair) {
    EXPECT_FALSE(movedFromOldLimits("[ffb]\nmaster_gain = 0.7x\nmax_magnitude = 0.8\n").has_value());
    EXPECT_FALSE(movedFromOldLimits("[ffb]\nmaster_gain = inf\nmax_magnitude = 0.8\n").has_value());
    EXPECT_FALSE(movedFromOldLimits("[ffb]\nmaster_gain = 0.7\nmax_magnitude = nan\n").has_value());
    const std::optional<std::string> moved = movedFromOldLimits(
        "[ffb]\nmaster_gain = 0.7\nmax_magnitude = 0.8\n[game_tuning]\nnfs2015.ffb_strength = 0.7x\n"
        "nfsheat.ffb_cap = high\n");
    ASSERT_TRUE(moved.has_value());
    EXPECT_NE(moved->find("nfs2015.ffb_strength = 0.7x"), std::string::npos) << *moved;
    EXPECT_NE(moved->find("nfsheat.ffb_cap = high"), std::string::npos) << *moved;
}
