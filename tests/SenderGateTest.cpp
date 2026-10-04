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

}  // namespace
}  // namespace wheelbri
