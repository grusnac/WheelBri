#include "wheelbri/GameDetector.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <optional>
#include <string>
#include <vector>

namespace wheelbri {
namespace {

using Event = GameDetector::Event;
using Programs = std::optional<std::vector<RunningProgram>>;

// Two games with profiles, so the two-game branches are covered while the built-in table has one.
const std::vector<GameProfile>& table() {
    static const std::vector<GameProfile> profiles = [] {
        std::vector<GameProfile> list;
        for (const auto& [id, program] : {std::pair{"nfs2015", "nfs16.exe"}, std::pair{"racer", "racer.exe"}}) {
            const std::string label = std::string(id) == "nfs2015" ? "NFS 2015" : "Racer";
            list.push_back(*parseGameProfile(id, "[game]\nlabel = " + label + "\nprogram = " + program +
                                                     "\ntarget = g920\n[pedals]\nthrottle_axis = slider\n"
                                                     "brake_axis = y\nclutch_axis = rz\n")
                                .profile);
        }
        return list;
    }();
    return profiles;
}

Programs only(std::vector<RunningProgram> programs) {
    programs.push_back({1, "explorer.exe"});
    programs.push_back({2, "steam.exe"});
    programs.push_back({3, "Z:\\games\\NeedForSpeedHeat.exe"});   // works as a real G920: never named
    return programs;
}
const Programs kNothing = only({});
const Programs kNfs = only({{100, "NFS16.exe"}});
const Programs kRacer = only({{200, "Z:\\games\\Racer.exe"}});
const Programs kBoth = only({{200, "Racer.exe"}, {100, "NFS16.exe"}});
const Programs kFailed = std::nullopt;

GameDetector detectorFor(const char* target = "g920", const int pollsToEnd = 2) {
    return GameDetector(target, pollsToEnd, table());
}

std::vector<Event> events(GameDetector& detector, const Programs& programs) {
    return detector.poll(programs).events;
}

TEST(GameDetector, NothingRunningKeepsARealG920) {
    GameDetector detector = detectorFor();
    EXPECT_TRUE(events(detector, kNothing).empty());
    EXPECT_EQ(detector.state().game, nullptr);
    EXPECT_TRUE(detector.state().listing);
}

TEST(GameDetector, Nfs2015StartsAtOnceWithItsPedals) {
    GameDetector detector = detectorFor();
    EXPECT_EQ(events(detector, kNfs), std::vector<Event>{Event::Started});
    EXPECT_EQ(detector.state().game->label, "NFS 2015");
    EXPECT_EQ(detector.state().pid, 100U);
    EXPECT_EQ(detector.state().program, "NFS16.exe");
    EXPECT_TRUE(detector.state().profileApplies);
    EXPECT_TRUE(events(detector, kNfs).empty());   // still running: nothing new
}

TEST(GameDetector, AGameThatWorksAsARealG920IsNeverNamed) {
    GameDetector detector = detectorFor();
    EXPECT_TRUE(events(detector, kNothing).empty());   // kNothing holds NFS Heat: not in any table
    EXPECT_EQ(detector.state().game, nullptr);
    GameDetector real("g920");   // the real table: NFS 2015, NFS Payback, NFS Unbound
    EXPECT_TRUE(real.poll(kNothing).events.empty());
    EXPECT_EQ(real.poll(kNfs).events, std::vector<Event>{Event::Started});
}

TEST(GameDetector, AnyCaseTarget) {
    GameDetector detector = detectorFor("G920");
    events(detector, kNfs);
    EXPECT_TRUE(detector.state().profileApplies);
}

TEST(GameDetector, EndsOnlyAfterTwoMissesInARow) {
    GameDetector detector = detectorFor();
    events(detector, kNfs);
    EXPECT_TRUE(events(detector, kNothing).empty());   // one miss: a process list taken while it restarts
    const GameDetector::Update ended = detector.poll(kNothing);
    EXPECT_EQ(ended.events, std::vector<Event>{Event::Ended});
    EXPECT_EQ(ended.previous->label, "NFS 2015");
    EXPECT_EQ(detector.state().game, nullptr);
    EXPECT_EQ(detector.state().pid, 0U);
}

TEST(GameDetector, AMissThenBackStartsCountingAgain) {
    GameDetector detector = detectorFor();
    events(detector, kNfs);
    events(detector, kNothing);
    EXPECT_TRUE(events(detector, kNfs).empty());
    EXPECT_TRUE(events(detector, kNothing).empty());   // the count started again: one miss only
}

TEST(GameDetector, ARestartKeepsTheProfile) {
    GameDetector detector = detectorFor();
    events(detector, kNfs);
    events(detector, kNothing);
    EXPECT_EQ(events(detector, only({{300, "NFS16.exe"}})), std::vector<Event>{Event::Restarted});
    EXPECT_EQ(detector.state().pid, 300U);
    // A restart seen without a miss in between (the launcher swapped processes between two looks).
    EXPECT_EQ(events(detector, only({{301, "NFS16.exe"}})), std::vector<Event>{Event::Restarted});
}

TEST(GameDetector, OnePollToEndWhenAskedAndNeverZero) {
    GameDetector once = detectorFor("g920", 1);
    events(once, kNfs);
    EXPECT_EQ(events(once, kNothing), std::vector<Event>{Event::Ended});
    GameDetector zero = detectorFor("g920", 0);   // taken as 1
    events(zero, kNfs);
    EXPECT_EQ(events(zero, kNothing), std::vector<Event>{Event::Ended});
}

TEST(GameDetector, TwoGamesTheFollowedOneStays) {
    GameDetector detector = detectorFor();
    events(detector, kRacer);
    EXPECT_EQ(events(detector, kBoth), std::vector<Event>{Event::Others});
    EXPECT_EQ(detector.state().game->label, "Racer");   // already followed: not taken over by table order
    ASSERT_EQ(detector.state().others.size(), 1U);
    EXPECT_EQ(detector.state().others[0].game->label, "NFS 2015");
    EXPECT_TRUE(events(detector, kBoth).empty());   // the same others: said once
    EXPECT_EQ(events(detector, kRacer), std::vector<Event>{Event::Others});   // gone again
    EXPECT_TRUE(detector.state().others.empty());
}

TEST(GameDetector, TwoGamesStartingTogetherTableOrder) {
    GameDetector detector = detectorFor();
    EXPECT_EQ(events(detector, kBoth), (std::vector<Event>{Event::Started, Event::Others}));
    EXPECT_EQ(detector.state().game->label, "NFS 2015");
    EXPECT_EQ(detector.state().others[0].game->label, "Racer");
}

TEST(GameDetector, SwitchesToTheOtherGameWhenTheFollowedOneEnds) {
    GameDetector detector = detectorFor();
    events(detector, kBoth);   // follows NFS 2015
    EXPECT_TRUE(events(detector, kRacer).empty());   // one miss, the same other: nothing yet
    EXPECT_EQ(detector.state().game->label, "NFS 2015");   // one miss: still followed, the other racer still an other
    const GameDetector::Update switched = detector.poll(kRacer);
    EXPECT_EQ(switched.events, (std::vector<Event>{Event::Switched, Event::Others}));
    EXPECT_EQ(switched.previous->label, "NFS 2015");
    EXPECT_EQ(detector.state().game->label, "Racer");
    EXPECT_EQ(detector.state().pid, 200U);
    EXPECT_TRUE(detector.state().others.empty());
}

TEST(GameDetector, AFailedListingNeverMovesThePedals) {
    GameDetector detector = detectorFor();
    events(detector, kNfs);
    EXPECT_EQ(events(detector, kFailed), std::vector<Event>{Event::ListFailed});
    EXPECT_FALSE(detector.state().listing);
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(events(detector, kFailed).empty());   // said once; no misses counted
    }
    EXPECT_EQ(events(detector, kNfs), std::vector<Event>{Event::ListBack});
    EXPECT_TRUE(detector.state().listing);
}

TEST(GameDetector, AFailedListingFromTheStartKeepsARealG920) {
    GameDetector detector = detectorFor();
    EXPECT_EQ(events(detector, kFailed), std::vector<Event>{Event::ListFailed});
    EXPECT_EQ(detector.state().game, nullptr);
    EXPECT_EQ(events(detector, kNfs), (std::vector<Event>{Event::ListBack, Event::Started}));
}

TEST(GameDetector, AnEmptyListIsAListWithNoGame) {
    GameDetector detector = detectorFor();
    events(detector, kNfs);
    events(detector, std::vector<RunningProgram>{});
    EXPECT_EQ(events(detector, std::vector<RunningProgram>{}), std::vector<Event>{Event::Ended});
}

TEST(GameDetector, AnotherTargetNamesTheGameButLeavesThePedals) {
    GameDetector detector = detectorFor("g29");
    EXPECT_EQ(events(detector, kNfs), std::vector<Event>{Event::Started});
    EXPECT_EQ(detector.state().game->label, "NFS 2015");
    EXPECT_FALSE(detector.state().profileApplies);
}

TEST(GameDetector, LookAlikesAreNotTheGame) {
    GameDetector detector = detectorFor();
    EXPECT_TRUE(events(detector, only({{5, "nfs16.exe.bak"}, {6, "notnfs16.exe"}, {7, "C:\\nfs16.exe\\x.exe"}}))
                    .empty());
    EXPECT_EQ(detector.state().game, nullptr);
}

TEST(GameDetector, EventNames) {
    EXPECT_EQ(eventName(Event::Started), "started");
    EXPECT_EQ(eventName(Event::ListBack), "list back");
}

}  // namespace
}  // namespace wheelbri
