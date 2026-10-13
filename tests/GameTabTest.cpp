#include "wheelbri/GameTab.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace wheelbri {
namespace {

TEST(GameTabFollower, OpensOncePerStart) {
    GameTabFollower follower;
    EXPECT_FALSE(follower.onFrame(0, ""));                 // nothing started yet
    EXPECT_EQ(follower.onFrame(1, "nfsunbound"), "nfsunbound");
    EXPECT_FALSE(follower.onFrame(1, "nfsunbound"));       // the same start: the user may leave the tab
    EXPECT_FALSE(follower.onFrame(1, ""));                 // it ended
    EXPECT_EQ(follower.onFrame(2, "nfspayback"), "nfspayback");   // another start, another game
    EXPECT_EQ(follower.onFrame(3, "nfspayback"), "nfspayback");   // the same game started again
}

TEST(GameTabFollower, AStartAlreadyOverIsNotShown) {
    GameTabFollower follower;
    EXPECT_FALSE(follower.onFrame(1, ""));   // started and ended between two frames
    EXPECT_FALSE(follower.onFrame(1, ""));
    EXPECT_EQ(follower.onFrame(3, "nfs2015"), "nfs2015");   // starts missed in between: the one now
}

TEST(GameTabFollower, ANewSessionCountsAgain) {
    GameTabFollower follower;
    EXPECT_EQ(follower.onFrame(2, "nfs2015"), "nfs2015");
    follower.reset();
    EXPECT_EQ(follower.onFrame(1, "nfs2015"), "nfs2015");   // the next session's first start
    follower.reset();
    EXPECT_FALSE(follower.onFrame(0, "nfs2015"));             // its count is still 0: no start seen
}

GameProfile parsed(const std::string& text) {
    const ParsedGameProfile result = parseGameProfile("test", text);
    EXPECT_TRUE(result.problems.empty()) << (result.problems.empty() ? "" : result.problems.front());
    return result.profile.value_or(GameProfile{});
}

TEST(GameProfileChanges, NothingForAProfileThatChangesNothing) {
    EXPECT_TRUE(describeGameProfile(parsed("[game]\nlabel = Plain\nprogram = plain.exe\ntarget = g920\n")).empty());
}

TEST(GameProfileChanges, EveryKindInOrder) {
    const GameProfile game = parsed(
        "[game]\nlabel = All\nprogram = all.exe\ntarget = g920\n"
        "[pedals]\nthrottle_axis = slider\nbrake_axis = y\nclutch_axis = rz\n"
        "[steering]\nrotation_deg = 180\nnote = turns in slowly\n"
        "[ffb]\ngain = 3\nnote = too light\n"
        "[game_settings]\nfile = Game/settings/PROFILEOPTIONS\nnote = hidden ones\n"
        "spring = WheelSpringMagnitude 0.8\nrotation = WheelMaxRotation steering\n");
    const std::vector<GameProfileChange> changes = describeGameProfile(game);
    ASSERT_EQ(changes.size(), 4U);
    EXPECT_EQ(changes[0].what, "Pedals");
    EXPECT_NE(changes[0].value.find("throttle_axis = slider"), std::string::npos) << changes[0].value;
    EXPECT_EQ(changes[1].what, "Steering rotation");
    EXPECT_EQ(changes[1].value, "180 deg (set your wheel's software to it too)");
    EXPECT_EQ(changes[1].why, "turns in slowly");
    EXPECT_EQ(changes[2].what, "Force feedback gain");
    EXPECT_NE(changes[2].value.find("x3"), std::string::npos) << changes[2].value;
    EXPECT_EQ(changes[2].why, "too light");
    EXPECT_EQ(changes[3].what, "Game's own settings");
    EXPECT_EQ(changes[3].value, "rotation = the steering rotation, spring = 0.8");   // by id
    EXPECT_EQ(changes[3].why, "hidden ones");
}

TEST(GameProfileChanges, TheBuiltInProfilesSaySomething) {
    for (const GameProfile& game : gameProfiles()) {
        EXPECT_FALSE(describeGameProfile(game).empty()) << game.id << " changes nothing: why a profile?";
    }
}

}  // namespace
}  // namespace wheelbri
