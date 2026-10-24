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

// A profile exists because its game needs something changed, or a target it does not support said (NFS Heat): every
// built-in one does at least one of them.
TEST(GameTab, TheBuiltInProfilesChangeSomething) {
    for (const GameProfile& game : gameProfiles()) {
        const bool changes = !describeOverlay(game).empty() || game.rotationDeg.has_value() ||
                             game.ffbGain.has_value() || !game.settings.empty() || !game.notFor.empty();
        EXPECT_TRUE(changes) << game.id << " changes nothing and advises nothing: why a profile?";
    }
}

}  // namespace
}  // namespace wheelbri
