#include "wheelbri/LoopStats.hpp"

#include <gtest/gtest.h>

namespace wheelbri {
namespace {

TEST(LoopStats, AveragesMaximaAndOverruns) {
    LoopStats stats(1'000'000);                          // 1 ms target
    EXPECT_FALSE(stats.record(1'000'000, 100'000));
    EXPECT_FALSE(stats.record(2'000'000, 300'000));      // exactly 2x is not an overrun
    EXPECT_TRUE(stats.record(3'000'000, 200'000));

    const auto snapshot = stats.snapshot();
    EXPECT_EQ(snapshot.ticks, 3U);
    EXPECT_EQ(snapshot.overruns, 1U);
    EXPECT_DOUBLE_EQ(snapshot.averagePeriodUs, 2000.0);
    EXPECT_DOUBLE_EQ(snapshot.maxPeriodUs, 3000.0);
    EXPECT_DOUBLE_EQ(snapshot.averageWorkUs, 200.0);
    EXPECT_DOUBLE_EQ(snapshot.maxWorkUs, 300.0);
}

TEST(LoopStats, ResetStartsANewWindow) {
    LoopStats stats(1'000'000);
    (void)stats.record(5'000'000, 1'000);
    stats.reset();
    const auto snapshot = stats.snapshot();
    EXPECT_EQ(snapshot.ticks, 0U);
    EXPECT_EQ(snapshot.overruns, 0U);
    EXPECT_DOUBLE_EQ(snapshot.averagePeriodUs, 0.0);
    EXPECT_DOUBLE_EQ(snapshot.maxPeriodUs, 0.0);
}

}  // namespace
}  // namespace wheelbri
