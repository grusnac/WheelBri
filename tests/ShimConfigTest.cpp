// SPDX-License-Identifier: GPL-3.0-or-later
// The settings of the dinput8.dll shim (wheelbri-shim.ini, shim/src/ShimConfig.cpp: parseShimConfig, layout,
// matchesSource) and of wheelbri-logi.dll (wheelbri-logi.ini, logi/src/Settings.cpp: parseSettings).
// Windows only (MinGW: run under Wine).

#include "Settings.hpp"
#include "ShimConfig.hpp"

#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/GameProfiles.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace wheelbri::shim {
namespace {

bool mentions(const std::vector<std::string>& problems, const std::string& part) {
    return std::any_of(problems.begin(), problems.end(),
                       [&](const std::string& problem) { return problem.find(part) != std::string::npos; });
}

ShimConfig parsed(const std::string& text, std::vector<std::string>& problems) {
    problems.clear();
    return parseShimConfig(text, problems);
}

// No file (or an empty one): every default, no problem.
TEST(ShimConfigFile, EmptyKeepsTheDefaults) {
    std::vector<std::string> problems;
    const ShimConfig config = parsed("", problems);
    EXPECT_TRUE(problems.empty());
    EXPECT_TRUE(config.emulate);
    EXPECT_FALSE(config.logiLog);
    EXPECT_EQ(config.sourceVendorId, 0x346E);
    EXPECT_FALSE(config.sourceProductId);
    EXPECT_EQ(config.targetProfile, "g920");
    EXPECT_EQ(config.statePort, kDefaultStatePort);
    EXPECT_EQ(config.staleAfterMs, 1500);
    EXPECT_EQ(config.forceFeedback, FfbMode::Service);
    EXPECT_EQ(config.ffbPort, 41330);
    EXPECT_FALSE(config.invertPedals);
    EXPECT_FALSE(config.gameAxesFromApp);
    EXPECT_EQ(config.gameProfile, nullptr);
    EXPECT_TRUE(config.chain.empty());
}

// Every key set to a value it takes.
TEST(ShimConfigFile, EveryKeyRead) {
    std::vector<std::string> problems;
    const ShimConfig config = parsed("[shim]\n"
                                     "emulate = no\n"
                                     "logi_log = true\n"
                                     "source_vid = 046d\n"
                                     "source_pid = c262\n"
                                     "target_profile = g25\n"
                                     "state_port = 50000\n"
                                     "stale_after_ms = 20\n"
                                     "force_feedback = passthrough\n"
                                     "ffb_port = 65535\n"
                                     "throttle_axis = rx\n"
                                     "brake_axis = ry\n"
                                     "clutch_axis = none\n"
                                     "invert_pedals = yes\n"
                                     "game_axes = wheelbri\n"
                                     "chain = dinput8-chain.dll\n",
                                     problems);
    EXPECT_TRUE(problems.empty()) << (problems.empty() ? "" : problems.front());
    EXPECT_FALSE(config.emulate);
    EXPECT_TRUE(config.logiLog);
    EXPECT_EQ(config.sourceVendorId, 0x046D);
    EXPECT_EQ(config.sourceProductId, 0xC262);
    EXPECT_EQ(config.targetProfile, "g25");
    EXPECT_EQ(config.statePort, 50000);
    EXPECT_EQ(config.staleAfterMs, 20);
    EXPECT_EQ(config.forceFeedback, FfbMode::Passthrough);
    EXPECT_EQ(config.ffbPort, 65535);
    EXPECT_EQ(config.throttleAxis, ObjectGuid::RxAxis);
    EXPECT_EQ(config.brakeAxis, ObjectGuid::RyAxis);
    EXPECT_TRUE(config.clutchAxis.has_value());
    EXPECT_TRUE(config.invertPedals);
    EXPECT_TRUE(config.gameAxesFromApp);
    EXPECT_EQ(config.chain, "dinput8-chain.dll");
}

// A chain that is not a bare DLL name next to the shim is refused: DirectInput goes to Windows' own.
TEST(ShimConfigFile, AChainThatIsNoDllNameIsRefused) {
    for (const char* bad : {"..\\x.dll", "C:\\mods\\x.dll", "dinput8.dll", "x.exe"}) {
        std::vector<std::string> problems;
        const ShimConfig config = parsed(std::string("[shim]\nchain = ") + bad + "\n", problems);
        EXPECT_EQ(problems.size(), 1U) << bad;
        EXPECT_TRUE(config.chain.empty()) << bad;
    }
    std::vector<std::string> problems;
    EXPECT_TRUE(parsed("[shim]\nchain =\n", problems).chain.empty());
    EXPECT_TRUE(problems.empty());
}

// The other spellings each switch takes.
TEST(ShimConfigFile, BooleansAndModesInEverySpelling) {
    std::vector<std::string> problems;
    for (const char* yes : {"true", "1", "yes"}) {
        EXPECT_TRUE(parsed(std::string("[shim]\ninvert_pedals = ") + yes + "\n", problems).invertPedals) << yes;
    }
    for (const char* no : {"false", "0", "no"}) {
        EXPECT_TRUE(parsed(std::string("[shim]\nemulate = ") + no + "\n", problems).emulate == false) << no;
    }
    EXPECT_EQ(parsed("[shim]\nforce_feedback = service\n", problems).forceFeedback, FfbMode::Service);
    EXPECT_EQ(parsed("[shim]\nforce_feedback = true\n", problems).forceFeedback, FfbMode::Service);
    EXPECT_EQ(parsed("[shim]\nforce_feedback = off\n", problems).forceFeedback, FfbMode::Off);
    EXPECT_EQ(parsed("[shim]\nforce_feedback = false\n", problems).forceFeedback, FfbMode::Off);
    EXPECT_EQ(parsed("[shim]\nsource_pid = any\n", problems).sourceProductId, std::nullopt);
    EXPECT_TRUE(problems.empty());
    EXPECT_STREQ(toString(FfbMode::Service), "service");
    EXPECT_STREQ(toString(FfbMode::Passthrough), "passthrough");
    EXPECT_STREQ(toString(FfbMode::Off), "off");
    EXPECT_STREQ(toString(static_cast<FfbMode>(9)), "?");
}

// A value it cannot use is a problem that names the key, and the default stays.
TEST(ShimConfigFile, BadValuesKeepTheDefaults) {
    std::vector<std::string> problems;
    const ShimConfig config = parsed("[shim]\n"
                                     "emulate = maybe\n"
                                     "force_feedback = loud\n"
                                     "ffb_port = 1023\n"
                                     "game_axes = mine\n"
                                     "throttle_axis = w\n"
                                     "source_vid = 0x346e\n"
                                     "source_pid = 10000\n"
                                     "target_profile = g29\n"
                                     "state_port = 65536\n"
                                     "stale_after_ms = 19\n",
                                     problems);
    for (const char* key : {"emulate", "force_feedback", "ffb_port", "game_axes", "throttle_axis", "source_vid",
                            "source_pid", "target_profile", "state_port", "stale_after_ms"}) {
        EXPECT_TRUE(mentions(problems, key)) << key;
    }
    EXPECT_TRUE(config.emulate);
    EXPECT_EQ(config.forceFeedback, FfbMode::Service);
    EXPECT_EQ(config.ffbPort, 41330);
    EXPECT_FALSE(config.gameAxesFromApp);
    EXPECT_FALSE(config.throttleAxis);
    EXPECT_EQ(config.sourceVendorId, 0x346E);
    EXPECT_FALSE(config.sourceProductId);
    EXPECT_EQ(config.targetProfile, "g920");
    EXPECT_EQ(config.statePort, kDefaultStatePort);
    EXPECT_EQ(config.staleAfterMs, 1500);
}

// Numbers: trailing text, an empty value and each end of a range.
TEST(ShimConfigFile, NumbersAtTheirLimits) {
    std::vector<std::string> problems;
    EXPECT_EQ(parsed("[shim]\nffb_port = 1024\n", problems).ffbPort, 1024);
    EXPECT_EQ(parsed("[shim]\nffb_port = 4000x\n", problems).ffbPort, 41330);
    EXPECT_EQ(parsed("[shim]\nffb_port = 65536\n", problems).ffbPort, 41330);
    EXPECT_EQ(parsed("[shim]\nstate_port = 1023\n", problems).statePort, kDefaultStatePort);
    EXPECT_EQ(parsed("[shim]\nstate_port = 1024\n", problems).statePort, 1024);
    EXPECT_EQ(parsed("[shim]\nstate_port = 2000 \n", problems).statePort, 2000);   // the reader trims
    EXPECT_EQ(parsed("[shim]\nstate_port = 2x\n", problems).statePort, kDefaultStatePort);
    EXPECT_EQ(parsed("[shim]\nstale_after_ms = 5000\n", problems).staleAfterMs, 5000);
    EXPECT_EQ(parsed("[shim]\nstale_after_ms = 5001\n", problems).staleAfterMs, 1500);
    EXPECT_EQ(parsed("[shim]\nstale_after_ms = 9q\n", problems).staleAfterMs, 1500);
    EXPECT_EQ(parsed("[shim]\nsource_vid = ffff\n", problems).sourceVendorId, 0xFFFF);
    EXPECT_EQ(parsed("[shim]\nsource_vid = \n", problems).sourceVendorId, 0x346E);
    EXPECT_EQ(parsed("[shim]\nsource_vid = 12zz\n", problems).sourceVendorId, 0x346E);
}

// A file the INI reader cannot read in part: its errors are problems too, what it could read is used.
TEST(ShimConfigFile, ReaderErrorsAreProblems) {
    std::vector<std::string> problems{"kept"};
    const ShimConfig config = parseShimConfig("[shim\nemulate = false\n[shim]\nffb_port = 2000\n", problems);
    EXPECT_GE(problems.size(), 2U);
    EXPECT_EQ(problems.front(), "kept");   // added to, never cleared
    EXPECT_EQ(config.ffbPort, 2000);
}

// Pedal axes that do not make a layout (two pedals on one axis, a pedal on x, throttle none) are all dropped.
TEST(ShimConfigFile, AnInvalidLayoutDropsEveryPedalAxis) {
    std::vector<std::string> problems;
    ShimConfig config = parsed("[shim]\nthrottle_axis = y\nbrake_axis = y\nclutch_axis = z\n", problems);
    EXPECT_TRUE(mentions(problems, "two pedals share an axis"));
    EXPECT_FALSE(config.throttleAxis);
    EXPECT_FALSE(config.brakeAxis);
    EXPECT_FALSE(config.clutchAxis);
    config = parsed("[shim]\nthrottle_axis = x\n", problems);
    EXPECT_FALSE(config.throttleAxis);
    config = parsed("[shim]\nthrottle_axis = none\n", problems);
    EXPECT_FALSE(config.throttleAxis);
}

// The layout: the target's, this file's axes over it, and a game profile over both.
TEST(ShimConfigFile, LayoutIsASieveOfThreeLayers) {
    std::vector<std::string> problems;
    ShimConfig config = parsed("[shim]\nthrottle_axis = rx\ninvert_pedals = true\n", problems);
    JoystickLayout layout = config.layout(19);
    EXPECT_EQ(layout.throttle, ObjectGuid::RxAxis);
    EXPECT_EQ(layout.brake, layoutFor("g920", 19).brake);
    EXPECT_TRUE(layout.invertPedals);
    GameProfile game;
    game.brakeAxis = ObjectGuid::ZAxis;
    config.gameProfile = &game;
    layout = config.layout(19);
    EXPECT_EQ(layout.throttle, ObjectGuid::RxAxis);
    EXPECT_EQ(layout.brake, ObjectGuid::ZAxis);
}

// The physical wheel to replace: its vendor id, and its product id only when one is set; nothing when not emulating.
TEST(ShimConfigFile, MatchesTheSourceWheel) {
    std::vector<std::string> problems;
    ShimConfig config = parsed("", problems);
    EXPECT_TRUE(config.matchesSource(0x346E, 0x0005));
    EXPECT_FALSE(config.matchesSource(0x046D, 0x0005));
    config.sourceProductId = 0x0006;
    EXPECT_FALSE(config.matchesSource(0x346E, 0x0005));
    EXPECT_TRUE(config.matchesSource(0x346E, 0x0006));
    config.emulate = false;
    EXPECT_FALSE(config.matchesSource(0x346E, 0x0006));
}

// The game's profile is laid over the shim's layout only when there is one, the app did not already write the game's
// axes, it is for this target, and with this file's axes no two pedals share an axis (nor a pedal x).
TEST(ShimConfigGame, WhenTheGameProfileIsUsed) {
    const GameProfile* const nfs2015 = gameProfileFor("D:\\Games\\Need for Speed\\NFS16.exe");
    const GameProfile* const payback = gameProfileFor("NeedForSpeedPayback.exe");
    ASSERT_NE(nfs2015, nullptr);
    ASSERT_NE(payback, nullptr);
    ShimConfig config;
    EXPECT_EQ(gameProfileUse(config, nullptr), GameProfileUse::NoProfile);
    EXPECT_EQ(gameProfileUse(config, nfs2015), GameProfileUse::Used);
    config.gameAxesFromApp = true;
    EXPECT_EQ(gameProfileUse(config, nfs2015), GameProfileUse::AxesFromApp);
    config.gameAxesFromApp = false;
    config.targetProfile = "g25";
    EXPECT_EQ(gameProfileUse(config, nfs2015), GameProfileUse::OtherTarget);
    config.targetProfile = "g920";
    config.throttleAxis = ObjectGuid::YAxis;   // the brake's: Payback's profile changes no pedal, so they clash
    config.brakeAxis = ObjectGuid::YAxis;
    EXPECT_EQ(gameProfileUse(config, payback), GameProfileUse::Clashes);
    EXPECT_EQ(gameProfileUse(config, nfs2015), GameProfileUse::Used);   // its own three pedals replace both
}

}  // namespace
}  // namespace wheelbri::shim

namespace wheelbri::logi {
namespace {

bool mentions(const std::vector<std::string>& problems, const std::string& part) {
    return std::any_of(problems.begin(), problems.end(),
                       [&](const std::string& problem) { return problem.find(part) != std::string::npos; });
}

// wheelbri-logi.ini: its level and real library; anything else is a problem, never silently used.
TEST(LogiSettings, ReadsLevelAndLibrary) {
    Settings settings = parseSettings("[logi]\nlog_level = debug\nreal_library = C:\\G HUB\\logi.dll\n");
    EXPECT_TRUE(settings.problems.empty());
    EXPECT_EQ(settings.logLevel, "debug");
    EXPECT_EQ(settings.realLibrary, L"C:\\G HUB\\logi.dll");

    settings = parseSettings("[logi]\nreal_library = C:\\Pr\xc3\xb6gram\\x.dll\n");   // UTF-8 in the file
    EXPECT_EQ(settings.realLibrary, L"C:\\Pr\u00f6gram\\x.dll");

    settings = parseSettings("");
    EXPECT_TRUE(settings.problems.empty());
    EXPECT_TRUE(settings.logLevel.empty());
    EXPECT_TRUE(settings.realLibrary.empty());
}

TEST(LogiSettings, ProblemsNameWhatIsWrong) {
    const Settings settings = parseSettings("[logi]\nlog_level = loud\ncolour = red\n[other]\nx = 1\n[logi\n");
    EXPECT_TRUE(mentions(settings.problems, "log_level = 'loud'"));
    EXPECT_TRUE(mentions(settings.problems, "[logi] colour: unknown key"));
    EXPECT_TRUE(mentions(settings.problems, "[other]: unknown section"));
    EXPECT_GE(settings.problems.size(), 4U);   // the reader's own error about "[logi" too
    EXPECT_TRUE(settings.logLevel.empty());
}

}  // namespace
}  // namespace wheelbri::logi
