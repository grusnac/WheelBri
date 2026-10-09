#include "wheelbri/HidG920.hpp"
#include "wheelbri/KnownGames.hpp"
#include "wheelbri/PedalLayout.hpp"

#include <gtest/gtest.h>

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

TEST(PedalLayout, LogitechLeavesThePedalsWhereARealG920HasThem) {
    EXPECT_EQ(applyPedalLayout(pedals(), PedalLayout::Logitech), pedals());
}

// NFS 2015 reads a G920's brake from Y, its clutch from Rz and its throttle from the Slider (2026-10-09).
TEST(PedalLayout, Nfs2015GetsBrakeOnYClutchOnRzThrottleOnSlider) {
    const TargetReport moved = applyPedalLayout(pedals(), PedalLayout::Nfs2015);
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

TEST(PedalLayout, Names) {
    EXPECT_EQ(pedalLayoutName(PedalLayout::Logitech), "Logitech G920");
    EXPECT_EQ(pedalLayoutName(PedalLayout::Nfs2015), "NFS 2015");
}

TEST(KnownGames, ProgramNameFromANameOrAPath) {
    EXPECT_EQ(programName("NFS16.exe"), "nfs16.exe");
    EXPECT_EQ(programName("D:\\Steam\\steamapps\\common\\Need for Speed\\NFS16.exe"), "nfs16.exe");
    EXPECT_EQ(programName("Z:\\home\\deck\\games\\NeedForSpeedUnbound.exe"), "needforspeedunbound.exe");
    EXPECT_EQ(programName("/usr/bin/bash"), "bash");
    EXPECT_EQ(programName(""), "");
}

TEST(KnownGames, Nfs2015IsTheOneWithItsOwnPedals) {
    const std::vector<std::string> running = {"explorer.exe", "steam.exe", "NFS16.exe"};
    const KnownGame* const game = findRunningGame(running);
    ASSERT_NE(game, nullptr);
    EXPECT_EQ(game->name, "NFS 2015");
    EXPECT_EQ(game->pedals, PedalLayout::Nfs2015);
    for (const KnownGame& known : knownGames()) {
        EXPECT_EQ(known.pedals, known.program == "nfs16.exe" ? PedalLayout::Nfs2015 : PedalLayout::Logitech)
            << known.name;
        EXPECT_EQ(programName(known.program), known.program) << "program names are kept lower case";
    }
}

TEST(KnownGames, NewerGamesReadARealG920) {
    const std::vector<std::string> running = {"Z:\\games\\Excalibur\\NeedForSpeedUnbound.exe"};
    const KnownGame* const game = findRunningGame(running);
    ASSERT_NE(game, nullptr);
    EXPECT_EQ(game->name, "NFS Unbound");
    EXPECT_EQ(game->pedals, PedalLayout::Logitech);
}

TEST(KnownGames, NoKnownGameRunning) {
    EXPECT_EQ(findRunningGame(std::vector<std::string>{}), nullptr);
    const std::vector<std::string> running = {"explorer.exe", "nfs16.exe.bak", "notnfs16.exe"};
    EXPECT_EQ(findRunningGame(running), nullptr);
}

}  // namespace
}  // namespace wheelbri
