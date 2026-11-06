// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/InputReadStats.hpp"

#include <gtest/gtest.h>

using namespace wheelbri;

namespace {
constexpr std::uint64_t kMs = 1'000'000;
}

TEST(InputReadStats, CountsReadsGapsPacketsAndShimTime) {
    InputReadStats stats;
    stats.addPackets(11, false);
    stats.recordRead(100 * kMs, 100 * kMs + 5'000);   // 5 us in the shim
    stats.addPackets(4, false);                        // a Poll() before the read took some
    stats.addPackets(7, false);
    stats.recordRead(111 * kMs, 111 * kMs + 15'000);
    stats.recordRead(160 * kMs, 160 * kMs + 1'000);   // nothing new: the service stopped sending?
    EXPECT_EQ(stats.reads(), 3U);
    const std::string line = stats.summary(160 * kMs);
    EXPECT_NE(line.find("3 reads"), std::string::npos) << line;
    EXPECT_NE(line.find("every 30.0 ms on average, longest gap 49.0 ms"), std::string::npos) << line;
    EXPECT_NE(line.find("7.3 packets per read"), std::string::npos) << line;   // 22 / 3
    EXPECT_NE(line.find("1 reads without new state"), std::string::npos) << line;
    EXPECT_NE(line.find("0 reads behind"), std::string::npos) << line;
    EXPECT_NE(line.find("avg 7 us / max 15 us"), std::string::npos) << line;
}

TEST(InputReadStats, ACappedPollMeansTheReadWasBehind) {
    InputReadStats stats;
    stats.addPackets(64, true);
    stats.recordRead(0, 1'000);
    stats.addPackets(20, false);
    stats.recordRead(10 * kMs, 10 * kMs + 1'000);
    EXPECT_NE(stats.summary(10 * kMs).find("1 reads behind"), std::string::npos);
}

TEST(InputReadStats, AMinuteMakesALineAndANewWindow) {
    InputReadStats stats;
    EXPECT_FALSE(stats.due(0));
    stats.recordRead(1 * kMs, 1 * kMs);
    EXPECT_FALSE(stats.due(30'000 * kMs));
    EXPECT_TRUE(stats.due(61'000 * kMs));
    stats.restart(61'000 * kMs);
    EXPECT_EQ(stats.reads(), 0U);
    EXPECT_FALSE(stats.due(61'001 * kMs));
    stats.recordRead(61'011 * kMs, 61'011 * kMs);   // the gap to the last read before the restart still counts
    EXPECT_NE(stats.summary(61'011 * kMs).find("longest gap 61010.0 ms"), std::string::npos)
        << stats.summary(61'011 * kMs);
}

// A read timed just before the window was restarted (the game's thread took its time, then logReads() on the
// release restarted the window first): not a minute later, so no summary line of one read.
TEST(InputReadStats, AReadTimedBeforeTheWindowIsNotDue) {
    InputReadStats stats;
    stats.restart(1000 * kMs);
    stats.recordRead(990 * kMs, 999 * kMs);
    EXPECT_EQ(stats.reads(), 1U);
    EXPECT_FALSE(stats.due(999 * kMs));
    EXPECT_TRUE(stats.due(61'000 * kMs));
    // The summary of such a window: no time has passed yet, and a read that ended before it started took no time.
    const std::string line = stats.summary(999 * kMs);
    EXPECT_NE(line.find("1 reads in 0.0 s"), std::string::npos) << line;
    EXPECT_NE(line.find("max 9000 us"), std::string::npos) << line;
}

// Nothing read yet: the summary says so, and reads that end before they start (clocks of two threads) count no time.
TEST(InputReadStats, NoReadsAndBackwardTimes) {
    InputReadStats stats;
    EXPECT_EQ(stats.summary(0), "input: no reads");
    stats.addPackets(0, true);
    stats.addPackets(3, false);   // a capped poll earlier still marks the read as behind
    stats.recordRead(20 * kMs, 10 * kMs);
    stats.recordRead(15 * kMs, 15 * kMs);   // started before the last one: no gap counted
    const std::string line = stats.summary(5 * kMs);   // before the window: 0 s
    EXPECT_NE(line.find("2 reads in 0.0 s (every 0.0 ms on average, longest gap 0.0 ms)"), std::string::npos) << line;
    EXPECT_NE(line.find("1.5 packets per read"), std::string::npos) << line;
    EXPECT_NE(line.find("1 reads without new state, 1 reads behind"), std::string::npos) << line;
    EXPECT_NE(line.find("avg 0 us / max 0 us"), std::string::npos) << line;
}
