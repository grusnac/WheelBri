// RelistTimer: when to list the game controllers again. Every branch of due(), listed() and nextDueMs() is named in
// the test that covers it.

#include "wheelbri/RelistTimer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace wheelbri {
namespace {

RelistTimer::Settings settings(const std::uint64_t settle, const std::uint64_t followUp, const std::uint64_t safety) {
    RelistTimer::Settings s;
    s.settleMs = settle;
    s.followUpMs = followUp;
    s.safetyMs = safety;
    return s;
}

// --- the defaults ---

TEST(RelistTimer, DefaultsSettleFollowUpAndSafety) {
    const RelistTimer::Settings defaults;
    EXPECT_EQ(defaults.settleMs, 300U);
    EXPECT_EQ(defaults.followUpMs, 2'000U);
    EXPECT_EQ(defaults.safetyMs, 30'000U);
    const RelistTimer::Settings polled = RelistTimer::Settings::withoutNotifications();
    EXPECT_EQ(polled.settleMs, 300U);
    EXPECT_EQ(polled.followUpMs, 2'000U);
    EXPECT_EQ(polled.safetyMs, 2'000U);   // the old listing every 2 s
    EXPECT_EQ(RelistTimer(polled).settings().safetyMs, 2'000U);
}

// --- due(): never listed ---

TEST(RelistTimer, NeverListedIsDueAtOnceWhateverTheTimeOrCount) {
    RelistTimer timer;
    EXPECT_TRUE(timer.due(0, 0));
    EXPECT_TRUE(timer.due(123'456, 7));
    EXPECT_EQ(timer.nextDueMs(5'000, 3), 5'000U);
}

// --- due(): the safety listing ---

TEST(RelistTimer, WithoutChangesOnlyTheSafetyListing) {
    RelistTimer timer;
    timer.listed(0, 0);
    EXPECT_FALSE(timer.due(2'000, 0));    // no more listing every 2 s
    EXPECT_FALSE(timer.due(29'999, 0));   // one before the safety time
    EXPECT_EQ(timer.nextDueMs(1'000, 0), 30'000U);
    EXPECT_TRUE(timer.due(30'000, 0));    // exactly at it
    timer.listed(30'000, 0);
    EXPECT_FALSE(timer.due(31'000, 0));   // a safety listing brings no follow-up
    EXPECT_EQ(timer.nextDueMs(31'000, 0), 60'000U);
}

TEST(RelistTimer, WithoutNotificationsEveryTwoSecondsAsBefore) {
    RelistTimer timer(RelistTimer::Settings::withoutNotifications());
    timer.listed(0, 0);
    EXPECT_FALSE(timer.due(1'999, 0));
    EXPECT_TRUE(timer.due(2'000, 0));
}

TEST(RelistTimer, AZeroSafetyTimeListsAtEveryCall) {
    RelistTimer timer(settings(300, 2'000, 0));
    timer.listed(100, 0);
    EXPECT_TRUE(timer.due(100, 0));
}

// --- due(): a change ---

TEST(RelistTimer, AChangeIsListedOnceItSettled) {
    RelistTimer timer;
    timer.listed(0, 0);
    EXPECT_FALSE(timer.due(10'000, 3));   // three interfaces of one device just arrived: noticed now
    EXPECT_EQ(timer.nextDueMs(10'000, 3), 10'300U);
    EXPECT_FALSE(timer.due(10'299, 3));   // one before it settled
    EXPECT_TRUE(timer.due(10'300, 3));    // exactly settled
}

TEST(RelistTimer, MoreChangesWhileSettlingDoNotPushTheListingBack) {
    RelistTimer timer;
    timer.listed(0, 0);
    EXPECT_FALSE(timer.due(10'000, 1));
    EXPECT_FALSE(timer.due(10'200, 4));   // still arriving: the settle time runs from the first one seen
    EXPECT_EQ(timer.nextDueMs(10'200, 4), 10'300U);
    EXPECT_TRUE(timer.due(10'300, 4));
}

TEST(RelistTimer, AChangeNotYetSeenByDueIsPlannedFromNow) {
    // nextDueMs is asked before due() looked at the new count (the device watch's wait after its listing).
    RelistTimer timer;
    timer.listed(0, 0);
    EXPECT_EQ(timer.nextDueMs(5'000, 1), 5'300U);
    EXPECT_FALSE(timer.due(5'100, 1));   // seen now: it settles from here
    EXPECT_EQ(timer.nextDueMs(5'100, 1), 5'400U);
}

TEST(RelistTimer, AZeroSettleTimeListsAChangeAtOnce) {
    RelistTimer timer(settings(0, 2'000, 30'000));
    timer.listed(0, 0);
    EXPECT_TRUE(timer.due(500, 1));
}

TEST(RelistTimer, ACountThatMovesBackIsStillAChange) {
    // The count only ever grows; a different number (whatever it is) means a change, never a missed one.
    RelistTimer timer;
    timer.listed(0, 5);
    EXPECT_FALSE(timer.due(1'000, 4));
    EXPECT_TRUE(timer.due(1'300, 4));
}

// --- listed(): the follow-up ---

TEST(RelistTimer, AListingAChangeCausedIsFollowedOnce) {
    RelistTimer timer;
    timer.listed(0, 0);
    ASSERT_FALSE(timer.due(10'000, 1));
    ASSERT_TRUE(timer.due(10'300, 1));
    timer.listed(10'300, 1);
    EXPECT_FALSE(timer.due(11'000, 1));
    EXPECT_EQ(timer.nextDueMs(11'000, 1), 12'300U);
    EXPECT_FALSE(timer.due(12'299, 1));
    EXPECT_TRUE(timer.due(12'300, 1));    // DirectInput may list the device only now
    timer.listed(12'300, 1);
    EXPECT_FALSE(timer.due(14'300, 1));   // no follow-up of the follow-up
    EXPECT_EQ(timer.nextDueMs(14'300, 1), 42'300U);
}

TEST(RelistTimer, AForcedListingWithAChangeNotYetSeenIsFollowedToo) {
    // The Rig tab lists at once when a device stops answering (refreshDevices(true)), before due() saw the change.
    RelistTimer timer;
    timer.listed(0, 0);
    timer.listed(500, 1);
    EXPECT_EQ(timer.nextDueMs(600, 1), 2'500U);
    EXPECT_TRUE(timer.due(2'500, 1));
}

TEST(RelistTimer, AForcedListingWhileAChangeSettlesTakesItAndIsFollowed) {
    RelistTimer timer;
    timer.listed(0, 0);
    ASSERT_FALSE(timer.due(1'000, 1));    // settling
    timer.listed(1'100, 1);               // listed before it settled
    EXPECT_FALSE(timer.due(1'300, 1));    // the change is taken: no listing at its settle time
    EXPECT_TRUE(timer.due(3'100, 1));     // the follow-up
}

TEST(RelistTimer, AForcedListingWithoutAChangeIsNotFollowed) {
    RelistTimer timer;
    timer.listed(0, 0);
    timer.listed(500, 0);
    EXPECT_FALSE(timer.due(2'500, 0));
    EXPECT_EQ(timer.nextDueMs(2'500, 0), 30'500U);
}

TEST(RelistTimer, TheFirstListingIsNotFollowedEvenWithAChangeCount) {
    RelistTimer timer;
    timer.listed(0, 9);                   // the count was not 0 when the watch started
    EXPECT_FALSE(timer.due(2'000, 9));
}

TEST(RelistTimer, ANewChangeBeforeTheFollowUpIsSettledFirst) {
    RelistTimer timer;
    timer.listed(0, 0);
    ASSERT_FALSE(timer.due(1'000, 1));
    timer.listed(1'300, 1);               // follow-up planned at 3'300
    EXPECT_FALSE(timer.due(1'500, 2));    // unplugged again
    EXPECT_EQ(timer.nextDueMs(1'500, 2), 1'800U);
    EXPECT_TRUE(timer.due(1'800, 2));
    timer.listed(1'800, 2);
    EXPECT_EQ(timer.nextDueMs(1'900, 2), 3'800U);   // the follow-up moved with it
}

// --- nextDueMs(): the earliest of what is planned, never in the past ---

TEST(RelistTimer, TheNextTimeIsTheEarliestOfSettleFollowUpAndSafety) {
    RelistTimer timer(settings(300, 2'000, 1'000));   // safety before the follow-up
    timer.listed(0, 0);
    ASSERT_FALSE(timer.due(100, 1));
    EXPECT_EQ(timer.nextDueMs(100, 1), 400U);          // settle first
    timer.listed(400, 1);
    EXPECT_EQ(timer.nextDueMs(500, 1), 1'400U);        // the safety listing before the follow-up at 2'400
}

TEST(RelistTimer, AnOverdueTimeIsNowNotThePast) {
    RelistTimer timer;
    timer.listed(0, 0);
    EXPECT_EQ(timer.nextDueMs(45'000, 0), 45'000U);   // the safety listing was due at 30'000
    ASSERT_FALSE(timer.due(1'000, 1));
    EXPECT_EQ(timer.nextDueMs(9'000, 1), 9'000U);     // settled long ago
}

// --- a clock that goes back (it should not) ---

TEST(RelistTimer, AClockThatWentBackListsRatherThanWaitingForEver) {
    RelistTimer timer;
    timer.listed(50'000, 0);
    EXPECT_TRUE(timer.due(1'000, 0));
    EXPECT_EQ(timer.nextDueMs(1'000, 0), 1'000U);
}

TEST(RelistTimer, AClockThatWentBackAfterAChangeWasSeenStillLists) {
    RelistTimer timer;
    timer.listed(0, 0);
    ASSERT_FALSE(timer.due(5'000, 1));   // seen at 5'000
    EXPECT_TRUE(timer.due(3'000, 1));    // the clock went back to 3'000: listed, not stuck until 5'300
}

// --- the edges of the number range ---

TEST(RelistTimer, TimesNearTheEndOfTheRangeDoNotWrap) {
    const std::uint64_t late = std::numeric_limits<std::uint64_t>::max() - 100'000;
    RelistTimer timer;
    timer.listed(late, 0);
    EXPECT_FALSE(timer.due(late + 1'000, 0));
    EXPECT_TRUE(timer.due(late + 30'000, 0));
    EXPECT_EQ(timer.nextDueMs(late + 1'000, 0), late + 30'000);
}

}  // namespace
}  // namespace wheelbri
