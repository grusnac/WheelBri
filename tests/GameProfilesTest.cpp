#include "wheelbri/GameProfiles.hpp"
#include "wheelbri/HidG920.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

namespace wheelbri {
namespace {

// Distinct pedal values, so every move shows (G920 pedals: 65535 = released, 0 = fully pressed).
TargetReport pedals() {
    TargetReport report;
    report.steering = 40000;
    report.throttle = 1000;    // gas pressed
    report.brake = 20000;      // brake half
    report.clutch = 65535;     // clutch released
    report.buttons = 0b101;
    report.hat = 2;
    return report;
}

const GameProfile& nfs2015() {
    const GameProfile* const profile = gameProfileFor("NFS16.exe");
    EXPECT_NE(profile, nullptr);
    return *profile;
}

TEST(GameProfiles, EveryBuiltInFileParsesCleanly) {
    EXPECT_TRUE(gameProfileProblems().empty()) << gameProfileProblems().front();
    ASSERT_FALSE(builtInGameProfileFiles().empty());
    EXPECT_EQ(gameProfiles().size(), builtInGameProfileFiles().size());
    for (const GameProfile& profile : gameProfiles()) {
        EXPECT_FALSE(profile.label.empty()) << profile.id;
        EXPECT_EQ(programName(profile.program), profile.program) << profile.id;
        EXPECT_EQ(gameProfileFor(profile.program), &profile) << profile.id;
        // A profile is the exception: it changes something, or it should not exist.
        EXPECT_FALSE(describeOverlay(profile).empty()) << profile.id;
    }
}

TEST(GameProfiles, Nfs2015HoldsOnlyItsPedalAxes) {
    const GameProfile& profile = nfs2015();
    EXPECT_EQ(profile.id, "nfs2015");
    EXPECT_EQ(profile.label, "NFS 2015");
    EXPECT_EQ(profile.target, "g920");
    EXPECT_EQ(profile.throttleAxis, ObjectGuid::Slider);
    EXPECT_EQ(profile.brakeAxis, ObjectGuid::YAxis);
    EXPECT_EQ(profile.clutchAxis, ObjectGuid::RzAxis);
    EXPECT_EQ(describeOverlay(profile), "throttle_axis = slider, brake_axis = y, clutch_axis = rz");
}

TEST(GameProfiles, TheSieveReplacesOnlyWhatTheProfileSets) {
    GameProfile onlyClutch;
    onlyClutch.clutchAxis = ObjectGuid::ZAxis;
    JoystickLayout base = layoutFor("g920", 19);
    base.invertPedals = true;
    const JoystickLayout result = overlay(base, onlyClutch);
    EXPECT_EQ(result.throttle, ObjectGuid::YAxis);   // kept
    EXPECT_EQ(result.brake, ObjectGuid::RzAxis);     // kept
    EXPECT_EQ(result.clutch, ObjectGuid::ZAxis);     // replaced
    EXPECT_TRUE(result.invertPedals);                // not a profile key: kept
    EXPECT_EQ(result.buttonCount, 19U);
    EXPECT_EQ(describeOverlay(onlyClutch), "clutch_axis = z");
    const JoystickLayout same = overlay(base, GameProfile{});
    EXPECT_EQ(same.throttle, base.throttle);
    EXPECT_EQ(same.brake, base.brake);
    EXPECT_EQ(same.clutch, base.clutch);
}

TEST(GameProfiles, StraightThroughByDefault) {
    EXPECT_TRUE(PedalRoute{}.identity());
    EXPECT_EQ(applyPedalRoute(pedals(), PedalRoute{}), pedals());
    const JoystickLayout device = layoutFor("g920", 0);
    EXPECT_EQ(routePedals(device, device), PedalRoute{});
}

// NFS 2015 reads a G920's brake from Y, its clutch from Rz and its throttle from the Slider (2026-10-09).
TEST(GameProfiles, Nfs2015OnTheVirtualG920) {
    const JoystickLayout device = layoutFor("g920", 0);
    const std::optional<PedalRoute> route = routePedals(device, overlay(device, nfs2015()));
    ASSERT_TRUE(route.has_value());
    const TargetReport moved = applyPedalRoute(pedals(), *route);
    EXPECT_EQ(moved.throttle, 20000);   // the field packed on Y: the brake
    EXPECT_EQ(moved.brake, 65535);      // on Rz: the clutch
    EXPECT_EQ(moved.clutch, 1000);      // on the Slider: the throttle
    EXPECT_EQ(moved.steering, 40000);   // the rest unchanged
    EXPECT_EQ(moved.buttons, 0b101U);
    EXPECT_EQ(moved.hat, 2);

    // Packed as the G920's input report: Y (bytes 3-4) carries the brake, Rz (5-6) the clutch, Slider (7-8) the gas.
    const auto bytes = packG920Input(moved);
    const auto axis = [&](const std::size_t at) {
        return static_cast<std::int16_t>(static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1] << 8U)));
    };
    EXPECT_EQ(axis(3), 20000 - 32768);
    EXPECT_EQ(axis(5), 65535 - 32768);
    EXPECT_EQ(axis(7), 1000 - 32768);
}

TEST(GameProfiles, Nfs2015InTheShim) {
    // The shim's layout: defaults, its ini, then the profile; the same axes the virtual device ends up with.
    const JoystickLayout layout = overlay(layoutFor("g920", 19), nfs2015());
    EXPECT_EQ(layout.throttle, ObjectGuid::Slider);
    EXPECT_EQ(layout.brake, ObjectGuid::YAxis);
    EXPECT_EQ(layout.clutch, ObjectGuid::RzAxis);
    EXPECT_TRUE(isValid(layout));
}

TEST(GameProfiles, AVirtualDeviceCannotUseAxesItDoesNotHave) {
    const JoystickLayout device = layoutFor("g920", 0);
    GameProfile onZ;
    onZ.clutchAxis = ObjectGuid::ZAxis;   // the virtual G920 has no Z: the Slider would carry nothing
    EXPECT_FALSE(routePedals(device, overlay(device, onZ)).has_value());
    GameProfile noClutch;
    noClutch.clutchAxis = ObjectGuid::Any;
    EXPECT_FALSE(routePedals(device, overlay(device, noClutch)).has_value());
    // Not a route at all: straight through rather than garbage.
    EXPECT_EQ(applyPedalRoute(pedals(), PedalRoute{{3, 0, 1}}), pedals());
}

TEST(GameProfiles, ParseOnlyWhatChanges) {
    const ParsedGameProfile parsed = parseGameProfile("racer", "[game]\r\nlabel = Racer\r\nprogram = Racer.EXE\r\n"
                                                               "target = G920\r\n[pedals]\r\nclutch_axis = none\r\n");
    ASSERT_TRUE(parsed.profile.has_value()) << (parsed.problems.empty() ? "" : parsed.problems.front());
    EXPECT_EQ(parsed.profile->id, "racer");
    EXPECT_EQ(parsed.profile->program, "racer.exe");   // lower case
    EXPECT_EQ(parsed.profile->target, "g920");
    EXPECT_FALSE(parsed.profile->throttleAxis.has_value());
    EXPECT_FALSE(parsed.profile->brakeAxis.has_value());
    EXPECT_EQ(parsed.profile->clutchAxis, ObjectGuid::Any);
    EXPECT_TRUE(profileFitsTarget(*parsed.profile, "G920"));
    EXPECT_FALSE(profileFitsTarget(*parsed.profile, "g29"));
}

TEST(GameProfiles, AProfileWithProblemsIsNotUsedAndSaysEveryProblem) {
    const ParsedGameProfile parsed = parseGameProfile(
        "bad", "[game]\nprogram = C:\\games\\bad.exe\n[pedals]\nthrottle_axis = sideways\nhorn_axis = y\n"
               "[extra]\nkey = 1\nnot a line\n");
    EXPECT_FALSE(parsed.profile.has_value());
    const std::string all = [&] {
        std::string text;
        for (const std::string& problem : parsed.problems) text += problem + "\n";
        return text;
    }();
    EXPECT_NE(all.find("expected key = value"), std::string::npos) << all;
    EXPECT_NE(all.find("throttle_axis = 'sideways'"), std::string::npos) << all;
    EXPECT_NE(all.find("[pedals] horn_axis: unknown key"), std::string::npos) << all;
    EXPECT_NE(all.find("[extra]: unknown section"), std::string::npos) << all;
    EXPECT_NE(all.find("label is missing"), std::string::npos) << all;
    EXPECT_NE(all.find("program must be"), std::string::npos) << all;   // a path, not a file name
    EXPECT_NE(all.find("target is missing"), std::string::npos) << all;
}

TEST(GameProfiles, LabelsAreShortAndSayNfs) {
    const auto problems = [](const std::string& label) {
        return parseGameProfile("x", "[game]\nlabel = " + label + "\nprogram = x.exe\ntarget = g920\n[pedals]\n"
                                     "clutch_axis = none\n")
            .problems;
    };
    EXPECT_TRUE(problems("NFS 2015").empty());
    EXPECT_TRUE(problems("NFS Hot Pursuit RM").empty());           // 18 characters: fine
    EXPECT_FALSE(problems("Need for Speed 2015").empty());          // NFS, not Need for Speed
    EXPECT_FALSE(problems("need for speed heat").empty());          // in any case
    EXPECT_FALSE(problems("NFS Most Wanted 2012 Remaster").empty());   // longer than 20
}

TEST(GameProfiles, InvalidLayoutsAreRefused) {
    const auto problems = [](const char* pedalsSection) {
        return parseGameProfile("x", std::string("[game]\nlabel = X\nprogram = x.exe\ntarget = g920\n[pedals]\n") +
                                         pedalsSection)
            .problems;
    };
    EXPECT_FALSE(problems("throttle_axis = rz\n").empty());     // brake is on rz already
    EXPECT_FALSE(problems("brake_axis = x\n").empty());         // x is the wheel
    EXPECT_FALSE(problems("throttle_axis = none\n").empty());   // only the clutch may be absent
    EXPECT_TRUE(problems("throttle_axis = rz\nbrake_axis = y\n").empty());   // a swap is fine
}

TEST(GameProfiles, ProgramMatching) {
    EXPECT_EQ(programName("NFS16.exe"), "nfs16.exe");
    EXPECT_EQ(programName("D:\\Steam\\steamapps\\common\\Need for Speed\\NFS16.exe"), "nfs16.exe");
    EXPECT_EQ(programName("/usr/bin/bash"), "bash");
    EXPECT_EQ(programName("Spi\xC3\x89l.exe"), "spi\xC3\x89l.exe");   // UTF-8 left as it is
    EXPECT_EQ(programName("C:\\Games\\"), "");
    EXPECT_EQ(gameProfileFor("Z:\\home\\deck\\NFS16.exe"), &nfs2015());
    for (const char* other : {"", "nfs16", "nfs16.exe.bak", "notnfs16.exe", "nfs16.exe ", "C:\\nfs16.exe\\x.exe",
                              "NeedForSpeedUnbound.exe"}) {
        EXPECT_EQ(gameProfileFor(other), nullptr) << other;   // Unbound works with the defaults: no profile
    }
}

TEST(GameProfiles, RunningGamesInProfileOrderOncePerGame) {
    const std::vector<GameProfile> table = {
        *parseGameProfile("a", "[game]\nlabel = A\nprogram = a.exe\ntarget = g920\n[pedals]\nclutch_axis = none\n")
             .profile,
        *parseGameProfile("b", "[game]\nlabel = B\nprogram = b.exe\ntarget = g920\n[pedals]\nclutch_axis = none\n")
             .profile,
    };
    const std::vector<RunningProgram> running = {{4, "explorer.exe"}, {7, "B.exe"}, {9, "A.exe"}, {11, "a.exe"}};
    const std::vector<RunningGame> games = findRunningGames(running, table);
    ASSERT_EQ(games.size(), 2U);
    EXPECT_EQ(games[0].game->label, "A");   // profile order, not list order
    EXPECT_EQ(games[0].pid, 9U);           // its first process
    EXPECT_EQ(games[0].program, "A.exe");
    EXPECT_EQ(games[1].game->label, "B");
    EXPECT_TRUE(findRunningGames({}).empty());
    EXPECT_EQ(findRunningGames(std::vector<RunningProgram>{{1, "NFS16.exe"}, {2, "NeedForSpeedUnbound.exe"}}).size(),
              1U);
}

}  // namespace
}  // namespace wheelbri
