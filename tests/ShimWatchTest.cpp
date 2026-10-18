// Mode Shim's check that the shim works (core/src/ShimWatch.cpp): a game running two minutes without a word from the
// shim warns once per start; the shim heard once, never.

#include "wheelbri/ShimWatch.hpp"

#include <gtest/gtest.h>

namespace wheelbri {
namespace {

constexpr std::uint64_t kQuiet = ShimWatch::kQuietMs;

TEST(ShimWatch, AGameWithoutTheShimWarnsOnceAfterTwoMinutes) {
    ShimWatch watch;
    EXPECT_FALSE(watch.update(0, "", 0, false));               // no game: nothing to wait for
    EXPECT_FALSE(watch.update(1000, "nfs2015", 1, false));     // it starts
    EXPECT_FALSE(watch.update(1000 + kQuiet - 1, "nfs2015", 1, false));
    const auto due = watch.update(1000 + kQuiet, "nfs2015", 1, false);
    ASSERT_TRUE(due);
    EXPECT_EQ(*due, "nfs2015");
    EXPECT_FALSE(watch.update(1000 + 2 * kQuiet, "nfs2015", 1, false));   // once
    EXPECT_FALSE(watch.heard());
}

TEST(ShimWatch, AStartAgainWaitsAgain) {
    ShimWatch watch;
    watch.update(0, "nfs2015", 1, false);
    ASSERT_TRUE(watch.update(kQuiet, "nfs2015", 1, false));
    watch.update(kQuiet + 10, "", 1, false);                       // closed
    EXPECT_FALSE(watch.update(kQuiet + 20, "nfs2015", 2, false));  // started again: a new wait
    EXPECT_FALSE(watch.update(2 * kQuiet + 19, "nfs2015", 2, false));
    EXPECT_TRUE(watch.update(2 * kQuiet + 20, "nfs2015", 2, false));
    // Another game: its own wait.
    EXPECT_FALSE(watch.update(2 * kQuiet + 30, "nfsunbound", 3, false));
    EXPECT_TRUE(watch.update(3 * kQuiet + 30, "nfsunbound", 3, false));
}

TEST(ShimWatch, HeardOnceNeverWarns) {
    ShimWatch watch;
    watch.update(0, "nfs2015", 1, false);
    EXPECT_FALSE(watch.update(1000, "nfs2015", 1, true));
    EXPECT_TRUE(watch.heard());
    EXPECT_FALSE(watch.update(10 * kQuiet, "nfs2015", 1, false));   // heard stays heard
    watch.reset();
    EXPECT_FALSE(watch.heard());
    watch.update(0, "nfs2015", 1, false);
    EXPECT_TRUE(watch.update(kQuiet, "nfs2015", 1, false));
}

}  // namespace
}  // namespace wheelbri
