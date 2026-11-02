#include "wheelbri/WayNotices.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace wheelbri {
namespace {

using Programs = std::optional<std::vector<RunningProgram>>;

// A game that works only with the virtual device, one that works only with the shim, one that works both ways (for a
// G25 too: the way is the game's, whatever the target).
const std::vector<GameProfile>& table() {
    static const std::vector<GameProfile> profiles = [] {
        std::vector<GameProfile> list;
        list.push_back(*parseGameProfile("payback", "[game]\nlabel = NFS Payback\nprogram = payback.exe\n"
                                                    "target = g920\nways = virtual\nways_note = DirectX error\n")
                            .profile);
        list.push_back(*parseGameProfile("old", "[game]\nlabel = Old\nprogram = old.exe\ntarget = g25\n"
                                                "ways = shim\nways_note = no virtual wheel\n")
                            .profile);
        list.push_back(*parseGameProfile("both", "[game]\nlabel = Both\nprogram = both.exe\ntarget = g920\n")
                            .profile);
        return list;
    }();
    return profiles;
}

Programs running(const std::vector<std::string>& names) {
    std::vector<RunningProgram> programs{{1, "explorer.exe"}};
    std::uint32_t pid = 100;
    for (const std::string& name : names) programs.push_back({pid++, name});
    return programs;
}

struct Session {
    GameDetector detector{"g920", 1, table()};
    WayNotices notices;
    explicit Session(const bool shim) : notices(shim) {}
    void poll(const Programs& programs) { notices.update(detector.poll(programs), detector.state()); }
};

TEST(WayNotices, AShimSessionTellsOncePerStartOfAVirtualOnlyGame) {
    Session shim(true);
    shim.poll(running({}));
    EXPECT_FALSE(shim.notices.current());
    EXPECT_EQ(shim.notices.count(), 0U);

    shim.poll(running({"Payback.exe"}));
    ASSERT_TRUE(shim.notices.current());
    EXPECT_EQ(shim.notices.current()->title, "NFS Payback needs Mode Virtual");
    EXPECT_EQ(shim.notices.count(), 1U);
    shim.poll(running({"Payback.exe"}));   // still running: no new notice
    EXPECT_EQ(shim.notices.count(), 1U);
    EXPECT_TRUE(shim.notices.current());

    shim.poll(running({}));   // gone: the advice goes too
    EXPECT_FALSE(shim.notices.current());
    shim.poll(running({"payback.exe"}));   // started again: told again
    EXPECT_EQ(shim.notices.count(), 2U);
}

TEST(WayNotices, ARestartOfTheSameGameIsNotANewStart) {
    Session shim(true);
    shim.poll(running({"payback.exe"}));
    ASSERT_EQ(shim.notices.count(), 1U);
    shim.poll(Programs(std::vector<RunningProgram>{{1, "explorer.exe"}, {555, "payback.exe"}}));   // another pid
    EXPECT_EQ(shim.notices.count(), 1U);
    EXPECT_TRUE(shim.notices.current());
}

TEST(WayNotices, TheVirtualDeviceTellsAboutAShimOnlyGameWhateverItsTarget) {
    Session virtualDevice(false);
    virtualDevice.poll(running({"payback.exe"}));
    EXPECT_FALSE(virtualDevice.notices.current());   // it works with the virtual device
    EXPECT_EQ(virtualDevice.notices.count(), 0U);
    virtualDevice.poll(running({}));
    virtualDevice.poll(running({"old.exe"}));   // its profile is for the G25, the session's target the G920
    ASSERT_TRUE(virtualDevice.notices.current());
    EXPECT_EQ(virtualDevice.notices.current()->title, "Old needs Mode Shim");
    EXPECT_EQ(virtualDevice.notices.count(), 1U);
}

TEST(WayNotices, ASwitchToAnotherGameIsANewStart) {
    Session shim(true);
    shim.poll(running({"both.exe"}));
    EXPECT_FALSE(shim.notices.current());   // works both ways
    shim.poll(running({"payback.exe"}));    // both.exe gone, payback runs: switched
    ASSERT_TRUE(shim.notices.current());
    EXPECT_EQ(shim.notices.count(), 1U);
    shim.poll(running({"both.exe"}));       // and back: no advice for it
    EXPECT_FALSE(shim.notices.current());
    EXPECT_EQ(shim.notices.count(), 1U);
}

TEST(WayNotices, AFailedListingKeepsTheAdvice) {
    Session shim(true);
    shim.poll(running({"payback.exe"}));
    shim.poll(std::nullopt);   // the system could not list programs: nothing changes
    EXPECT_TRUE(shim.notices.current());
    EXPECT_EQ(shim.notices.count(), 1U);
}

TEST(WayNoticeFollower, ShowsEachNoticeOnce) {
    WayNoticeFollower follower;
    EXPECT_FALSE(follower.onFrame(0));
    EXPECT_TRUE(follower.onFrame(1));
    EXPECT_FALSE(follower.onFrame(1));
    EXPECT_TRUE(follower.onFrame(3));   // two at once (frames missed): shown once, the latest
    EXPECT_FALSE(follower.onFrame(3));
    follower.reset();                   // a new session
    EXPECT_TRUE(follower.onFrame(1));
}

TEST(WayNoticeFollower, ASmallerCountIsANewSessionNotANotice) {
    WayNoticeFollower follower;
    EXPECT_TRUE(follower.onFrame(2));
    EXPECT_FALSE(follower.onFrame(0));   // a new session's count (reset not called): nothing shown
    EXPECT_TRUE(follower.onFrame(1));    // and its first notice is shown
}

}  // namespace
}  // namespace wheelbri
