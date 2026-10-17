#include "wheelbri/SenderGate.hpp"

#include <gtest/gtest.h>

#include <map>

namespace wheelbri {
namespace {

constexpr std::uint64_t kMs = 1'000'000;

struct FakeSystem {
    std::map<std::uint16_t, SenderVerdict> verdicts;
    int lookups = 0;
    SenderGate::Lookup lookup() {
        return [this](const std::uint16_t port) {
            ++lookups;
            const auto it = verdicts.find(port);
            return it == verdicts.end() ? SenderVerdict::Refuse : it->second;
        };
    }
};

TEST(SenderGate, LooksUpANewSenderOnceThenUsesTheCache) {
    FakeSystem system;
    system.verdicts[50000] = SenderVerdict::Accept;
    SenderGate gate(system.lookup());
    EXPECT_TRUE(gate.allow(50000, 0));
    EXPECT_TRUE(gate.allow(50000, 1 * kMs));
    EXPECT_TRUE(gate.allow(50000, 500 * kMs));
    EXPECT_EQ(system.lookups, 1);
    EXPECT_FALSE(gate.allow(50001, 600 * kMs));
    EXPECT_TRUE(gate.lastWasNewRefusal());
    EXPECT_FALSE(gate.allow(50001, 601 * kMs));
    EXPECT_FALSE(gate.lastWasNewRefusal());   // refused from the cache: no new log line
    EXPECT_EQ(system.lookups, 2);
    EXPECT_EQ(gate.stats().refused, 2U);
}

TEST(SenderGate, AnAcceptedPortIsCheckedAgainAfterItsTimeToLive) {
    FakeSystem system;
    system.verdicts[50000] = SenderVerdict::Accept;
    SenderGate gate(system.lookup());
    EXPECT_TRUE(gate.allow(50000, 0));
    system.verdicts[50000] = SenderVerdict::Refuse;   // the game closed the port, another program took it
    EXPECT_TRUE(gate.allow(50000, 1999 * kMs));
    EXPECT_FALSE(gate.allow(50000, 2000 * kMs));
    EXPECT_EQ(system.lookups, 2);
}

TEST(SenderGate, UnknownFollowsThePolicy) {
    FakeSystem system;
    system.verdicts[1] = SenderVerdict::Unknown;
    SenderGate strict(system.lookup());
    EXPECT_FALSE(strict.allow(1, 0));
    SenderGate::Settings lenient;
    lenient.acceptUnknown = true;
    SenderGate open(system.lookup(), lenient);
    EXPECT_TRUE(open.allow(1, 0));
    EXPECT_EQ(open.stats().unknown, 1U);
}

TEST(SenderGate, AFloodFromNewPortsIsCappedPerSecond) {
    FakeSystem system;
    SenderGate::Settings settings;
    settings.maxLookupsPerSecond = 4;
    SenderGate gate(system.lookup(), settings);
    for (std::uint16_t port = 1000; port < 1100; ++port) EXPECT_FALSE(gate.allow(port, 10 * kMs));
    EXPECT_EQ(system.lookups, 4);
    EXPECT_EQ(gate.stats().overLookupCap, 96U);
    system.verdicts[2000] = SenderVerdict::Accept;
    EXPECT_TRUE(gate.allow(2000, 1100 * kMs));   // the next second: looked up again
}

TEST(SenderGate, ManySendersReuseTheLeastRecentlyUsedEntry) {
    FakeSystem system;
    for (std::uint16_t port = 1; port <= 40; ++port) system.verdicts[port] = SenderVerdict::Accept;
    SenderGate::Settings settings;
    settings.maxLookupsPerSecond = 1000;
    SenderGate gate(system.lookup(), settings);
    for (std::uint16_t port = 1; port <= 40; ++port) EXPECT_TRUE(gate.allow(port, port * kMs));
    EXPECT_EQ(system.lookups, 40);
    EXPECT_TRUE(gate.allow(40, 100 * kMs));   // still cached
    EXPECT_EQ(system.lookups, 40);
    EXPECT_TRUE(gate.allow(1, 101 * kMs));    // pushed out long ago: looked up again
    EXPECT_EQ(system.lookups, 41);
}

TEST(SenderRules, KnownPrograms) {
    EXPECT_TRUE(isDriverHostImage("C:\\Windows\\System32\\WUDFHost.exe", "C:\\Windows\\System32"));
    EXPECT_TRUE(isDriverHostImage("c:\\windows\\system32\\wudfhost.EXE", "C:\\Windows\\System32\\"));
    EXPECT_FALSE(isDriverHostImage("C:\\Users\\me\\Downloads\\WUDFHost.exe", "C:\\Windows\\System32"));
    EXPECT_FALSE(isDriverHostImage("C:\\Windows\\System32\\WUDFHost.exe", ""));
    EXPECT_TRUE(isWheelBriImage("C:\\Program Files\\WheelBri\\wheelbri.exe"));
    EXPECT_TRUE(isWheelBriImage("D:/cpp-projects/WheelBri/build/service/WheelBri-Service.exe"));
    EXPECT_FALSE(isWheelBriImage("C:\\Games\\wheelbri.exe.bak"));
    EXPECT_FALSE(isWheelBriImage("C:\\Games\\notwheelbri.exe"));
    EXPECT_TRUE(sameFolder("C:\\Games\\NFS\\", "c:/games/nfs"));
    EXPECT_FALSE(sameFolder("C:\\Games\\NFS", "C:\\Games\\NFS2"));
}

// Paths as programs and configs write them: doubled separators inside, several at the end, a network share (whose
// leading "\\" is not a doubled separator), an empty path (never the same folder as anything).
TEST(SenderRules, FolderComparisonIgnoresDoubledAndTrailingSeparators) {
    EXPECT_TRUE(sameFolder("C:\\\\Games\\\\NFS", "C:\\Games\\NFS"));
    EXPECT_TRUE(sameFolder("C:\\Games\\NFS\\\\\\", "C:/Games/NFS"));
    EXPECT_TRUE(sameFolder("\\\\server\\share", "\\\\server\\share\\"));
    EXPECT_FALSE(sameFolder("\\\\server\\share", "\\server\\share"));   // a share is not a folder on this drive
    EXPECT_TRUE(sameFolder("//server/share", "\\\\server\\share"));     // a share written either way
    EXPECT_TRUE(sameFolder("\\", "/"));                                 // the root keeps its one separator
    EXPECT_FALSE(sameFolder("", "C:\\Games"));
    EXPECT_FALSE(sameFolder("C:\\Games", ""));
    EXPECT_FALSE(sameFolder("", ""));
}

// The program image as the system reports it: with or without a folder, any case.
TEST(SenderRules, ImagesWithoutAFolder) {
    EXPECT_TRUE(isWheelBriImage("WHEELBRI.EXE"));
    EXPECT_TRUE(isWheelBriImage("wheelbri-service.exe"));
    EXPECT_FALSE(isWheelBriImage(""));
    EXPECT_FALSE(isWheelBriImage("C:\\WheelBri\\"));
    EXPECT_FALSE(isDriverHostImage("WUDFHost.exe", "C:\\Windows\\System32"));
    EXPECT_TRUE(isDriverHostImage("C:\\\\Windows\\System32\\\\WUDFHost.exe", "c:/windows/system32"));
}

// No lookup at all (a platform without one): every sender is "unknown" and the policy decides.
TEST(SenderGate, WithoutALookupThePolicyDecides) {
    SenderGate strict(SenderGate::Lookup{});
    EXPECT_FALSE(strict.allow(1, 0));
    EXPECT_EQ(strict.stats().unknown, 1U);
    EXPECT_EQ(strict.stats().lookups, 1U);
    SenderGate::Settings lenient;
    lenient.acceptUnknown = true;
    SenderGate open(SenderGate::Lookup{}, lenient);
    EXPECT_TRUE(open.allow(1, 0));
    EXPECT_FALSE(open.lastWasNewRefusal());
}

// A refused sender is asked about again only after the (longer) refusal time to live: a program that later gets
// the port legitimately is let through then.
TEST(SenderGate, ARefusedPortIsCheckedAgainAfterItsTimeToLive) {
    FakeSystem system;
    SenderGate gate(system.lookup());
    EXPECT_FALSE(gate.allow(7, 0));
    system.verdicts[7] = SenderVerdict::Accept;
    EXPECT_FALSE(gate.allow(7, 9999 * kMs));
    EXPECT_EQ(system.lookups, 1);
    EXPECT_TRUE(gate.allow(7, 10000 * kMs));
    EXPECT_EQ(system.lookups, 2);
}

// A clock that goes back (a time from another source, a resumed machine): the lookup window starts again instead of
// staying exhausted until the clock catches up.
TEST(SenderGate, AClockThatGoesBackStartsANewLookupWindow) {
    FakeSystem system;
    SenderGate::Settings settings;
    settings.maxLookupsPerSecond = 1;
    SenderGate gate(system.lookup(), settings);
    EXPECT_FALSE(gate.allow(1, 5000 * kMs));
    EXPECT_FALSE(gate.allow(2, 5001 * kMs));   // over the cap
    EXPECT_EQ(gate.stats().overLookupCap, 1U);
    system.verdicts[3] = SenderVerdict::Accept;
    EXPECT_TRUE(gate.allow(3, 1000 * kMs));    // earlier than the window: a new one
    EXPECT_EQ(system.lookups, 2);
}

}  // namespace
}  // namespace wheelbri
