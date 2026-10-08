#include "wheelbri/RelistTimer.hpp"

#include <gtest/gtest.h>

namespace wheelbri {
namespace {

TEST(RelistTimer, ListsAtOnceTheFirstTime) {
    RelistTimer timer;
    EXPECT_TRUE(timer.due(5'000, 0));
    EXPECT_EQ(timer.nextDueMs(5'000, 0), 5'000U);
}

TEST(RelistTimer, WithoutChangesOnlyTheSafetyListing) {
    RelistTimer timer;
    timer.listed(0, 0);
    EXPECT_FALSE(timer.due(2'000, 0));    // no more listing every 2 s
    EXPECT_FALSE(timer.due(29'999, 0));
    EXPECT_EQ(timer.nextDueMs(1'000, 0), 30'000U);
    EXPECT_TRUE(timer.due(30'000, 0));
    timer.listed(30'000, 0);
    EXPECT_FALSE(timer.due(31'000, 0));   // a safety listing brings no follow-up
}

TEST(RelistTimer, AChangeIsListedOnceItSettled) {
    RelistTimer timer;
    timer.listed(0, 0);
    EXPECT_FALSE(timer.due(10'000, 3));   // three interfaces of one device just arrived
    EXPECT_EQ(timer.nextDueMs(10'000, 3), 10'300U);
    EXPECT_FALSE(timer.due(10'200, 4));   // still arriving: the settle time runs from the first one
    EXPECT_TRUE(timer.due(10'300, 4));
}

TEST(RelistTimer, AListingAChangeCausedIsFollowedOnce) {
    RelistTimer timer;
    timer.listed(0, 0);
    ASSERT_FALSE(timer.due(10'000, 1));
    ASSERT_TRUE(timer.due(10'300, 1));
    timer.listed(10'300, 1);
    EXPECT_FALSE(timer.due(11'000, 1));
    EXPECT_EQ(timer.nextDueMs(11'000, 1), 12'300U);
    EXPECT_TRUE(timer.due(12'300, 1));    // DirectInput may list the device only now
    timer.listed(12'300, 1);
    EXPECT_FALSE(timer.due(14'300, 1));   // no follow-up of the follow-up
    EXPECT_EQ(timer.nextDueMs(14'300, 1), 42'300U);
}

TEST(RelistTimer, ANewChangeBeforeTheFollowUpIsSettledFirst) {
    RelistTimer timer;
    timer.listed(0, 0);
    ASSERT_FALSE(timer.due(1'000, 1));
    timer.listed(1'300, 1);
    EXPECT_FALSE(timer.due(1'500, 2));    // unplugged again
    EXPECT_TRUE(timer.due(1'800, 2));
}

TEST(RelistTimer, WithoutNotificationsEveryTwoSecondsAsBefore) {
    RelistTimer timer(RelistTimer::Settings::withoutNotifications());
    timer.listed(0, 0);
    EXPECT_FALSE(timer.due(1'999, 0));
    EXPECT_TRUE(timer.due(2'000, 0));
}

TEST(RelistTimer, AClockThatWentBackListsRatherThanWaitingForEver) {
    RelistTimer timer;
    timer.listed(50'000, 0);
    EXPECT_TRUE(timer.due(1'000, 0));
}

}  // namespace
}  // namespace wheelbri
