#include "wheelbri/GameTuning.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

using namespace wheelbri;

namespace {

GameTuning tuned(const std::string& key, const std::string& value, std::string* problem = nullptr) {
    GameTuning tuning;
    const std::string why = applyTuningKey(tuning, key, value, 19);
    if (problem != nullptr) *problem = why;
    return tuning;
}

}  // namespace

// Every key the Games tab writes is read back.
TEST(GameTuning, ReadsEveryKey) {
    GameTuning t;
    const std::vector<std::pair<std::string, std::string>> lines = {
        {"throttle_axis", "slider"}, {"brake_axis", "Y"},        {"clutch_axis", "none"},  {"brake_invert", "true"},
        {"clutch_invert", "false"},  {"ffb_strength", "0.6"},    {"ffb_constant", "3"},    {"ffb_periodic", "0"},
        {"ffb_spring", "0.8"},       {"ffb_damper", "1"},        {"ffb_friction", "4"},    {"ffb_inertia", "0.5"},
        {"ffb_cap", "0.6"},          {"ffb_slew", "30"},         {"ffb_fade_s", "0"},      {"ffb_invert", "false"},
        {"ffb_effects", "off"},      {"button_4", "13"},         {"button_18", "30b7:1001/2"}};
    for (const auto& [key, value] : lines) EXPECT_EQ(applyTuningKey(t, key, value, 19), "") << key;
    EXPECT_EQ(t.pedalAxes[0], ObjectGuid::Slider);
    EXPECT_EQ(t.pedalAxes[1], ObjectGuid::YAxis);
    EXPECT_EQ(t.pedalAxes[2], ObjectGuid::Any);
    EXPECT_EQ(t.pedalInvert[1], true);
    EXPECT_FALSE(t.pedalInvert[0].has_value());
    EXPECT_EQ(t.pedalInvert[2], false);
    EXPECT_FLOAT_EQ(*t.ffbStrength, 0.6F);
    EXPECT_FLOAT_EQ(*t.ffbGains[static_cast<std::size_t>(FfbGroup::Constant)], 3.0F);
    EXPECT_FLOAT_EQ(*t.ffbGains[static_cast<std::size_t>(FfbGroup::Periodic)], 0.0F);
    EXPECT_FLOAT_EQ(*t.ffbGains[static_cast<std::size_t>(FfbGroup::Friction)], 4.0F);
    EXPECT_FLOAT_EQ(*t.ffbCap, 0.6F);
    EXPECT_FLOAT_EQ(*t.ffbSlew, 30.0F);
    EXPECT_FLOAT_EQ(*t.ffbFadeSeconds, 0.0F);
    EXPECT_EQ(t.ffbInvert, false);
    EXPECT_EQ(t.ffbEffects, false);
    EXPECT_EQ(t.buttons.at(4), (SourceRef{{}, 13}));
    EXPECT_EQ(t.buttons.at(18), (SourceRef{{0x30b7, 0x1001}, 2}));
}

// What is refused, and why; the tuning stays as it was.
TEST(GameTuning, RefusesBadValues) {
    std::string why;
    EXPECT_TRUE(tuned("throttle_axis", "none", &why).empty());
    EXPECT_EQ(why, "expected x, y, z, rx, ry, rz or slider");   // only the clutch may be left out
    EXPECT_TRUE(tuned("clutch_axis", "w", &why).empty());
    EXPECT_EQ(why, "expected x, y, z, rx, ry, rz, slider or none");
    EXPECT_TRUE(tuned("clutch_axis", "Rz2", &why).empty());   // a digit is not lowered, and no axis has one
    EXPECT_EQ(why, "expected x, y, z, rx, ry, rz, slider or none");
    EXPECT_TRUE(tuned("brake_invert", "maybe", &why).empty());
    EXPECT_EQ(why, "expected true or false");
    EXPECT_TRUE(tuned("brake_curve", "1.5", &why).empty());   // how a pedal feels: the rig's own software
    EXPECT_EQ(why, "unknown key");
    EXPECT_TRUE(tuned("ffb_constant", "4.5", &why).empty());
    EXPECT_EQ(why, "expected a number from 0 to 4");
    EXPECT_TRUE(tuned("ffb_spring", "-1", &why).empty());
    EXPECT_TRUE(tuned("ffb_strength", "1.2", &why).empty());
    EXPECT_TRUE(tuned("ffb_cap", "nan", &why).empty());
    EXPECT_FALSE(why.empty());
    EXPECT_TRUE(tuned("button_19", "2", &why).empty());   // the target has 19: 0..18
    EXPECT_EQ(why, "no such button on the target (0 to 18)");
    EXPECT_TRUE(tuned("button_x", "2", &why).empty());
    EXPECT_TRUE(tuned("button_3", "two", &why).empty());
    EXPECT_EQ(why, "expected a button: 2, -1 (nothing) or 044f:b696/2");
    EXPECT_TRUE(tuned("ffb_ramp", "1", &why).empty());
    EXPECT_EQ(why, "unknown key");
}

// tuningLines writes what applyTuningKey reads: a full round trip, and an empty tuning writes nothing.
TEST(GameTuning, LinesRoundTrip) {
    GameTuning t;
    t.pedalAxes = {ObjectGuid::Slider, ObjectGuid::YAxis, ObjectGuid::RzAxis};
    t.pedalInvert[0] = true;
    t.pedalInvert[2] = false;
    t.ffbStrength = 0.65F;
    t.ffbGains[static_cast<std::size_t>(FfbGroup::Damper)] = 0.5F;
    t.ffbCap = 0.7F;
    t.ffbSlew = 25.0F;
    t.ffbFadeSeconds = 3.0F;
    t.ffbInvert = true;
    t.ffbEffects = true;
    t.buttons[5] = {{}, -1};
    t.secondaries[2] = {ButtonRef{{{}, 20}, false, false, 0.5F}, ButtonRef{{{0x30b7, 0x1001}, 3}, true, true, 0.4F}};
    t.secondaries[3] = {};   // none in this game
    GameTuning back;
    for (const auto& [key, value] : tuningLines(t)) EXPECT_EQ(applyTuningKey(back, key, value, 19), "") << key;
    EXPECT_EQ(back, t);
    EXPECT_TRUE(tuningLines(GameTuning{}).empty());
    EXPECT_TRUE(GameTuning{}.empty());
    EXPECT_FALSE(t.empty());
}

TEST(GameTuning, EffectTypesFallIntoGroups) {
    EXPECT_EQ(ffbGroupOf(FfbEffectType::Constant), FfbGroup::Constant);
    EXPECT_EQ(ffbGroupOf(FfbEffectType::Sine), FfbGroup::Periodic);
    EXPECT_EQ(ffbGroupOf(FfbEffectType::SawtoothDown), FfbGroup::Periodic);
    EXPECT_EQ(ffbGroupOf(FfbEffectType::Spring), FfbGroup::Spring);
    EXPECT_EQ(ffbGroupOf(FfbEffectType::Inertia), FfbGroup::Inertia);
}

// No game: the mapping's [ffb] as it is, every kind at 1.
TEST(GameTuning, ResolveWithoutAGameIsTheMapping) {
    FfbSafetyConfig base;
    base.masterGain = 0.7F;
    const GameFfb ffb = resolveGameFfb(base, true, false, nullptr, std::nullopt, nullptr);
    EXPECT_FLOAT_EQ(ffb.safety.masterGain, 0.7F);
    EXPECT_FLOAT_EQ(ffb.safety.maxMagnitude, 1.0F);
    for (const float gain : ffb.gains) EXPECT_FLOAT_EQ(gain, 1.0F);
    EXPECT_TRUE(ffb.invert);
    EXPECT_FALSE(ffb.effects);
}

// The profile's [ffb] gain (and the older [game_ffb_gain] over it) is for constant and periodic effects only; the
// tuning's per-kind gains have the last word.
TEST(GameTuning, ResolveLaysProfileLegacyAndTuningInOrder) {
    GameProfile game;
    game.ffbGain = 3.0F;
    const FfbSafetyConfig base;
    GameFfb ffb = resolveGameFfb(base, false, true, &game, std::nullopt, nullptr);
    EXPECT_FLOAT_EQ(ffb.gains[static_cast<std::size_t>(FfbGroup::Constant)], 3.0F);
    EXPECT_FLOAT_EQ(ffb.gains[static_cast<std::size_t>(FfbGroup::Periodic)], 3.0F);
    EXPECT_FLOAT_EQ(ffb.gains[static_cast<std::size_t>(FfbGroup::Spring)], 1.0F);
    ffb = resolveGameFfb(base, false, true, &game, 2.0F, nullptr);
    EXPECT_FLOAT_EQ(ffb.gains[static_cast<std::size_t>(FfbGroup::Constant)], 2.0F);
    GameTuning tuning;
    tuning.ffbGains[static_cast<std::size_t>(FfbGroup::Constant)] = 1.5F;
    tuning.ffbGains[static_cast<std::size_t>(FfbGroup::Spring)] = 0.0F;
    tuning.ffbStrength = 0.4F;
    tuning.ffbInvert = true;
    tuning.ffbEffects = false;
    tuning.ffbFadeSeconds = 0.0F;
    ffb = resolveGameFfb(base, false, true, &game, 2.0F, &tuning);
    EXPECT_FLOAT_EQ(ffb.gains[static_cast<std::size_t>(FfbGroup::Constant)], 1.5F);
    EXPECT_FLOAT_EQ(ffb.gains[static_cast<std::size_t>(FfbGroup::Periodic)], 2.0F);
    EXPECT_FLOAT_EQ(ffb.gains[static_cast<std::size_t>(FfbGroup::Spring)], 0.0F);
    EXPECT_FLOAT_EQ(ffb.safety.masterGain, 0.4F);
    EXPECT_FLOAT_EQ(ffb.safety.frozenForceSeconds, 0.0F);
    EXPECT_TRUE(ffb.invert);
    EXPECT_FALSE(ffb.effects);
}

// The safety limits only go down: a game's cap or slew above the mapping's is the mapping's, and out-of-range values
// (a hand-edited file, NaN) never get past the clamps.
TEST(GameTuning, ResolveNeverRaisesTheLimits) {
    FfbSafetyConfig base;
    base.maxMagnitude = 0.8F;
    base.maxSlewPerSecond = 50.0F;
    GameTuning tuning;
    tuning.ffbCap = 1.0F;
    tuning.ffbSlew = 500.0F;
    tuning.ffbStrength = 3.0F;
    tuning.ffbGains[0] = 99.0F;
    GameFfb ffb = resolveGameFfb(base, false, true, nullptr, std::nullopt, &tuning);
    EXPECT_FLOAT_EQ(ffb.safety.maxMagnitude, 0.8F);
    EXPECT_FLOAT_EQ(ffb.safety.maxSlewPerSecond, 50.0F);
    EXPECT_FLOAT_EQ(ffb.safety.masterGain, 1.0F);
    EXPECT_FLOAT_EQ(ffb.gains[0], kGameEffectGainMax);
    tuning.ffbCap = 0.5F;
    tuning.ffbSlew = 20.0F;
    ffb = resolveGameFfb(base, false, true, nullptr, std::nullopt, &tuning);
    EXPECT_FLOAT_EQ(ffb.safety.maxMagnitude, 0.5F);
    EXPECT_FLOAT_EQ(ffb.safety.maxSlewPerSecond, 20.0F);
    tuning.ffbCap = std::numeric_limits<float>::quiet_NaN();
    ffb = resolveGameFfb(base, false, true, nullptr, std::nullopt, &tuning);
    EXPECT_FALSE(ffb.safety.maxMagnitude > 0.8F);
    EXPECT_FALSE(std::isnan(ffb.safety.maxMagnitude));
}

TEST(GameTuning, UserGamesRoundTrip) {
    GameProfile heat;
    heat.id = "needforspeedheat";
    heat.label = "NFS Heat";
    heat.program = "needforspeedheat.exe";
    GameProfile excalibur;
    excalibur.id = "excalibur";
    excalibur.label = "Excalibur";
    excalibur.program = "excalibur.exe";
    const std::vector<GameProfile> games{heat, excalibur};
    const UserGamesResult back = parseUserGames(formatUserGames(games));
    EXPECT_TRUE(back.problems.empty());
    ASSERT_EQ(back.games.size(), 2U);
    for (const GameProfile& game : back.games) {
        EXPECT_TRUE(game.target.empty());
        EXPECT_TRUE(profileFitsTarget(game, "g25"));
        EXPECT_TRUE(profileFitsTarget(game, "G923"));
    }
}

TEST(GameTuning, UserGamesProblems) {
    const UserGamesResult result = parseUserGames(
        "[game.a]\nlabel = A\nprogram = A.EXE\n[game.b]\nlabel = B\nprogram = a.exe\n[other]\n[game.Bad]\n"
        "label = x\nprogram = x.exe\n[game.c]\nlabel = C\n[game.d]\nlabel = D\nprogram = d.exe\ncolour = red\n");
    ASSERT_EQ(result.games.size(), 2U);
    EXPECT_EQ(result.games[0].program, "a.exe");   // lower case, as the game is found
    EXPECT_EQ(result.games[1].id, "d");
    EXPECT_EQ(result.problems.size(), 5U);
}

TEST(GameTuning, GameIdForAProgram) {
    EXPECT_EQ(gameIdFor("NeedForSpeedHeat.exe"), "needforspeedheat");
    EXPECT_EQ(gameIdFor("D:\\Games\\My Game-2.exe"), "mygame2");
    EXPECT_EQ(gameIdFor("game_x"), "game_x");
    EXPECT_EQ(gameIdFor("---.exe"), "");
}

// The game's mapping: pedal invert, and buttons from the wheel or an accessory the mapping reads; one on a
// device it does not read stays as mapped and is said.
TEST(GameTuning, TunedMapper) {
    MapperConfig base;
    base.brake.sourceAxis = 5;
    base.accessories[0] = {0x30b7, 0x1001};
    GameTuning tuning;
    tuning.pedalInvert[1] = true;
    tuning.buttons[4] = {{}, 13};
    tuning.buttons[5] = {{0x30b7, 0x1001}, 2};
    tuning.buttons[6] = {{0x044f, 0xb696}, 1};   // not in the mapping
    tuning.buttons[7] = {{}, -1};                // nothing
    std::vector<std::string> problems;
    const MapperConfig out = tunedMapper(base, tuning, &problems);
    EXPECT_TRUE(out.brake.invert);
    EXPECT_FLOAT_EQ(out.brake.gamma, 1.0F);   // as mapped: the game's values never change how a pedal feels
    EXPECT_EQ(out.brake.sourceAxis, 5);
    EXPECT_FALSE(out.throttle.invert);
    EXPECT_EQ(out.buttonMap[4], 13);
    EXPECT_EQ(out.buttonDevice[4], 0);
    EXPECT_EQ(out.buttonMap[5], 2);
    EXPECT_EQ(out.buttonDevice[5], 1);
    EXPECT_EQ(out.buttonMap[6], 6);   // as mapped
    EXPECT_EQ(out.buttonMap[7], kUnmapped);
    ASSERT_EQ(problems.size(), 1U);
    EXPECT_NE(problems[0].find("button 6"), std::string::npos);
    EXPECT_TRUE(changesMapping(tuning));
    GameTuning ffbOnly;
    ffbOnly.ffbGains[0] = 2.0F;
    ffbOnly.pedalAxes[0] = ObjectGuid::Slider;
    EXPECT_FALSE(changesMapping(ffbOnly));
}

namespace {

GameProfile testGame(const std::string& id, const std::string& target) {
    GameProfile game;
    game.id = id;
    game.label = id;
    game.program = id + ".exe";
    game.target = target;
    return game;
}

}  // namespace

// The session's decision: a built-in profile only on its target; the user's values on every target; the pedals routed
// only for the virtual device, and only when the axes are the device's.
TEST(GameTuning, DecideGame) {
    GameProfile nfs = testGame("nfs2015", "g920");
    nfs.throttleAxis = ObjectGuid::Slider;
    nfs.brakeAxis = ObjectGuid::YAxis;
    nfs.clutchAxis = ObjectGuid::RzAxis;
    nfs.ffbGain = 2.0F;
    GameSessionConfig session;
    session.target = "g920";
    GameDecision d = decideGame(&nfs, true, session);
    EXPECT_EQ(d.route, (PedalRoute{{1, 2, 0}}));
    EXPECT_FLOAT_EQ(d.ffb.gains[0], 2.0F);
    EXPECT_EQ(d.tuning, nullptr);

    // Another target: the profile is not used, but the user's values for the game are.
    session.target = "g923";
    session.tunings["nfs2015"].ffbGains[2] = 0.5F;
    d = decideGame(&nfs, false, session);
    EXPECT_TRUE(d.route.identity());
    EXPECT_FLOAT_EQ(d.ffb.gains[0], 1.0F);
    EXPECT_FLOAT_EQ(d.ffb.gains[2], 0.5F);
    ASSERT_NE(d.tuning, nullptr);

    // A game the user added: its axes from the tuning.
    const GameProfile mine = testGame("mine", "");
    session.target = "g920";
    session.tunings["mine"].pedalAxes = {ObjectGuid::RzAxis, ObjectGuid::YAxis, ObjectGuid::Slider};
    d = decideGame(&mine, true, session);
    EXPECT_EQ(d.route, (PedalRoute{{1, 0, 2}}));
    // An axis the virtual device has not: not routed, said.
    session.tunings["mine"].pedalAxes[0] = ObjectGuid::ZAxis;
    d = decideGame(&mine, true, session);
    EXPECT_TRUE(d.unroutable);
    EXPECT_TRUE(d.route.identity());
    // The shim: never routed here (the shim lays out the axes itself).
    session.routePedals = false;
    d = decideGame(&mine, true, session);
    EXPECT_FALSE(d.unroutable);
    // No game: the mapping's force feedback.
    d = decideGame(nullptr, false, session);
    EXPECT_EQ(d.tuning, nullptr);
    for (const float gain : d.ffb.gains) EXPECT_FLOAT_EQ(gain, 1.0F);
}

// Every word for true and false the INI files take, and only those.
TEST(GameTuning, EveryWordForTrueAndFalse) {
    for (const char* yes : {"true", "on", "yes", "1"}) EXPECT_EQ(tuned("ffb_invert", yes).ffbInvert, true) << yes;
    for (const char* no : {"false", "off", "no", "0"}) EXPECT_EQ(tuned("ffb_invert", no).ffbInvert, false) << no;
    for (const char* other : {"True", "2", "", " on"}) {
        std::string why;
        EXPECT_FALSE(tuned("ffb_invert", other, &why).ffbInvert.has_value()) << other;
        EXPECT_EQ(why, "expected true or false");
    }
}

// A button key must be a whole, non-negative number below the target's button count; a target with no buttons
// says 0 to 0, never a wrapped count.
TEST(GameTuning, ButtonKeysAreWholeNumbersInRange) {
    std::string why;
    for (const char* key : {"button_3x", "button_-1", "button_", "button_+3", "button_99999999999"}) {
        EXPECT_TRUE(tuned(key, "2", &why).buttons.empty()) << key;
        EXPECT_EQ(why, "no such button on the target (0 to 18)") << key;
    }
    GameTuning none;
    EXPECT_EQ(applyTuningKey(none, "button_0", "2", 0), "no such button on the target (0 to 0)");
    EXPECT_TRUE(none.buttons.empty());
}

// A tuning holding a button number out of the mapping's range (a hand-edited app.ini) is skipped, never written past
// the mapping's buttons; without a problem list the device that is not read is skipped silently.
TEST(GameTuning, TunedMapperSkipsWhatItCannotPlace) {
    MapperConfig base;
    GameTuning tuning;
    tuning.buttons[-1] = {{}, 3};
    tuning.buttons[static_cast<int>(MapperConfig::kMaxTargetButtons)] = {{}, 3};
    tuning.buttons[2] = {{0x044f, 0xb696}, 1};   // a device the mapping does not read
    const MapperConfig out = tunedMapper(base, tuning, nullptr);
    EXPECT_EQ(out.buttonMap, base.buttonMap);
    EXPECT_EQ(out.buttonDevice, base.buttonDevice);
}

// button_N_or: the game's secondaries for a button, as [buttons] target_N_or writes them; -1 for none; at most 8;
// what is not a list of sources is refused and changes nothing.
TEST(GameTuning, SecondariesKeys) {
    std::string why;
    GameTuning t = tuned("button_4_or", "20, 30b7:1001/axis 3 inverted > 40%", &why);
    EXPECT_EQ(why, "");
    ASSERT_EQ(t.secondaries.at(4).size(), 2U);
    EXPECT_EQ(t.secondaries.at(4)[0], (ButtonRef{{{}, 20}, false, false, 0.5F}));
    EXPECT_EQ(t.secondaries.at(4)[1].source, (SourceRef{{0x30b7, 0x1001}, 3}));
    EXPECT_TRUE(t.secondaries.at(4)[1].axis);
    EXPECT_TRUE(t.secondaries.at(4)[1].invert);
    EXPECT_NEAR(t.secondaries.at(4)[1].threshold, 0.4F, 1e-4F);
    EXPECT_TRUE(t.buttons.empty());   // the main is left to the Mapping tab
    t = tuned("button_4_or", "-1", &why);
    EXPECT_EQ(why, "");
    EXPECT_TRUE(t.secondaries.at(4).empty());
    for (const char* bad : {"two", "1,,2", "-1, 4", "axis 3 > 150%", "64"}) {
        EXPECT_TRUE(tuned("button_4_or", bad, &why).empty()) << bad;
        EXPECT_FALSE(why.empty()) << bad;
    }
    EXPECT_TRUE(tuned("button_4_or", "1, 2, 3, 4, 5, 6, 7, 8, 9", &why).empty());
    EXPECT_EQ(why, "at most 8 secondary sources per button");
    EXPECT_TRUE(tuned("button_19_or", "2", &why).empty());
    EXPECT_EQ(why, "no such button on the target (0 to 18)");
    EXPECT_TRUE(tuned("button__or", "2", &why).empty());
    EXPECT_TRUE(tuned("button_or", "2", &why).empty());
}

// The game's secondaries replace the mapping's for that button (none: emptied); a list with a source on a device the
// mapping does not read is left out whole, and said; the other buttons keep the mapping's.
TEST(GameTuning, TunedMapperSecondaries) {
    MapperConfig base;
    base.accessories[0] = {0x30b7, 0x1001};
    base.buttonSecondary[1][0] = ButtonSource{0, 9, false, false, 0.5F};
    base.buttonSecondary[2][0] = ButtonSource{0, 10, false, false, 0.5F};
    base.buttonSecondary[3][0] = ButtonSource{0, 11, false, false, 0.5F};
    GameTuning tuning;
    tuning.secondaries[1] = {ButtonRef{{{}, 20}, false, false, 0.5F},
                             ButtonRef{{{0x30b7, 0x1001}, 3}, true, false, 0.3F}};
    tuning.secondaries[2] = {};
    tuning.secondaries[3] = {ButtonRef{{{0x044f, 0xb696}, 1}, false, false, 0.5F}};   // not in the mapping
    std::vector<std::string> problems;
    const MapperConfig out = tunedMapper(base, tuning, &problems);
    EXPECT_EQ(out.buttonSecondary[1][0], (ButtonSource{0, 20, false, false, 0.5F}));
    EXPECT_EQ(out.buttonSecondary[1][1], (ButtonSource{1, 3, true, false, 0.3F}));
    EXPECT_FALSE(out.buttonSecondary[1][2].mapped());
    EXPECT_FALSE(out.buttonSecondary[2][0].mapped());
    EXPECT_EQ(out.buttonSecondary[3], base.buttonSecondary[3]);
    ASSERT_EQ(problems.size(), 1U);
    EXPECT_NE(problems[0].find("button 3 or 044f:b696/1"), std::string::npos) << problems[0];
    EXPECT_EQ(out.buttonMap, base.buttonMap);   // the mains: the mapping's
    GameTuning only;
    only.secondaries[0] = {};
    EXPECT_TRUE(changesMapping(only));
}

// The old [game_ffb_gain] value counts for a game with a profile or values of its own, after the profile.
TEST(GameTuning, DecideGameTakesTheOldGain) {
    GameProfile nfs;
    nfs.id = "nfs2015";
    nfs.label = "NFS 2015";
    nfs.program = "nfs16.exe";
    nfs.target = "g920";
    GameSessionConfig session;
    session.target = "g920";
    session.legacyGains["nfs2015"] = 1.5F;
    EXPECT_FLOAT_EQ(decideGame(&nfs, true, session).ffb.gains[0], 1.5F);
    EXPECT_FLOAT_EQ(decideGame(&nfs, false, session).ffb.gains[0], 1.0F);   // not its target, no values of its own
}

// A user's game whose id is empty, or not as an id is written (upper case, a character past 'z'), is refused.
TEST(GameTuning, UserGameIdsAreChecked) {
    for (const char* section : {"[game.]", "[game.A]", "[game.a~]", "[game.a-b]"}) {
        const UserGamesResult result = parseUserGames(std::string(section) + "\nlabel = X\nprogram = x.exe\n");
        EXPECT_TRUE(result.games.empty()) << section;
        ASSERT_EQ(result.problems.size(), 1U) << section;
        EXPECT_NE(result.problems[0].find("the id must be"), std::string::npos) << result.problems[0];
    }
    const UserGamesResult noProgram = parseUserGames("[game.x]\nlabel = X\n");
    EXPECT_NE(noProgram.problems.at(0).find("needs a label and a program"), std::string::npos);
    const UserGamesResult noLabel = parseUserGames("[game.x]\nprogram = x.exe\n");
    EXPECT_NE(noLabel.problems.at(0).find("needs a label and a program"), std::string::npos);
    EXPECT_EQ(gameIdFor("Game~{|}.exe"), "game");
}
