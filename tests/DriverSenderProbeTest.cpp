// SPDX-License-Identifier: GPL-3.0-or-later
// The driver's sender check (driver/SenderProbe.hpp): which senders it lets through, looks up and logs, and, on
// Windows, what it finds out about one (a socket of this test program: not WheelBri, so dropped).

#include "../driver/SenderProbe.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <map>
#include <string>

#ifdef _WIN32
#include "wheelbri/UdpLoopback.hpp"

#include <array>
#include <chrono>
#include <optional>
#include <thread>
#endif

namespace wheelbri::driver {
namespace {

// A fake lookup: the verdict per port, and how often it was asked.
struct FakeLookUp {
    std::map<std::uint16_t, SenderVerdict> verdicts;
    int calls = 0;
    SenderLookup operator()(const std::uint16_t port) {
        ++calls;
        const auto it = verdicts.find(port);
        const SenderVerdict verdict = it == verdicts.end() ? SenderVerdict::Refuse : it->second;
        return {verdict, "port " + std::to_string(port)};
    }
};

// WheelBri is let through and looked up again only after kAcceptRecheckMs; it is logged once (as info), not at each
// recheck that finds the same.
TEST(DriverSenderProbe, WheelBriIsLetThroughAndLoggedOnce) {
    SenderCheck check;
    FakeLookUp look{{{50000, SenderVerdict::Accept}}};
    std::string log;
    bool warn = true;
    EXPECT_TRUE(check.allow(50000, 0, std::ref(look), log, warn));
    EXPECT_EQ(log, "port 50000");
    EXPECT_FALSE(warn);
    EXPECT_TRUE(check.allow(50000, SenderCheck::kAcceptRecheckMs - 1, std::ref(look), log, warn));
    EXPECT_EQ(look.calls, 1);
    EXPECT_TRUE(log.empty());
    EXPECT_TRUE(check.allow(50000, SenderCheck::kAcceptRecheckMs, std::ref(look), log, warn));
    EXPECT_EQ(look.calls, 2);
    EXPECT_TRUE(log.empty());   // the same answer again: nothing new to say
}

// Another program is dropped, with a warning, and asked about again after kRefuseRecheckMs (the port may be
// WheelBri's by then).
TEST(DriverSenderProbe, AnotherProgramIsDroppedWithAWarning) {
    SenderCheck check;
    FakeLookUp look;
    std::string log;
    bool warn = false;
    EXPECT_FALSE(check.allow(50001, 0, std::ref(look), log, warn));
    EXPECT_TRUE(warn);
    EXPECT_FALSE(log.empty());
    EXPECT_FALSE(check.allow(50001, 1, std::ref(look), log, warn));
    EXPECT_TRUE(log.empty());
    EXPECT_EQ(look.calls, 1);
    look.verdicts[50001] = SenderVerdict::Accept;
    EXPECT_TRUE(check.allow(50001, SenderCheck::kRefuseRecheckMs, std::ref(look), log, warn));
    EXPECT_FALSE(warn);
    EXPECT_FALSE(log.empty());   // a changed verdict is said
}

// Windows could not say: let through (the stream must not be lost to a failed lookup), with a warning each time.
TEST(DriverSenderProbe, AFailedLookupLetsThroughWithAWarning) {
    SenderCheck check;
    FakeLookUp look{{{50002, SenderVerdict::Unknown}}};
    std::string log;
    bool warn = false;
    EXPECT_TRUE(check.allow(50002, 0, std::ref(look), log, warn));
    EXPECT_TRUE(warn);
    EXPECT_TRUE(check.allow(50002, SenderCheck::kAcceptRecheckMs, std::ref(look), log, warn));
    EXPECT_TRUE(warn);
    EXPECT_FALSE(log.empty());
}

// A flood from ever new ports: at most kLookupsPerSecond lookups a second; a new sender past the cap is dropped, a
// known one keeps its verdict.
TEST(DriverSenderProbe, LookupsAreCappedPerSecond) {
    SenderCheck check;
    FakeLookUp look{{{40000, SenderVerdict::Accept}}};
    std::string log;
    bool warn = false;
    ASSERT_TRUE(check.allow(40000, 0, std::ref(look), log, warn));
    const std::int64_t later = SenderCheck::kAcceptRecheckMs;   // WheelBri's entry is due again
    for (std::uint16_t port = 1000; port < 1100; ++port) (void)check.allow(port, later, std::ref(look), log, warn);
    EXPECT_EQ(look.calls, 1 + static_cast<int>(SenderCheck::kLookupsPerSecond));
    EXPECT_FALSE(check.allow(2000, later, std::ref(look), log, warn));   // new, over the cap: dropped
    EXPECT_TRUE(check.allow(40000, later, std::ref(look), log, warn));   // known: still WheelBri
    EXPECT_EQ(look.calls, 1 + static_cast<int>(SenderCheck::kLookupsPerSecond));
}

// More senders than entries: the one used longest ago makes room, and is looked up again when it comes back.
TEST(DriverSenderProbe, TheOldestSenderMakesRoom) {
    SenderCheck check;
    FakeLookUp look;
    std::string log;
    bool warn = false;
    std::int64_t now = 0;
    for (std::uint16_t port = 0; port < SenderCheck::kEntries; ++port) {
        look.verdicts[static_cast<std::uint16_t>(3000 + port)] = SenderVerdict::Accept;
        (void)check.allow(static_cast<std::uint16_t>(3000 + port), now, std::ref(look), log, warn);
        now += 1000;   // its own second each: never capped
    }
    const int calls = look.calls;
    (void)check.allow(3000, now, std::ref(look), log, warn);   // still remembered
    EXPECT_EQ(look.calls, calls);
    (void)check.allow(4000, now + 1000, std::ref(look), log, warn);   // takes 3001's place (3000 was just used)
    (void)check.allow(3001, now + 2000, std::ref(look), log, warn);
    EXPECT_EQ(look.calls, calls + 2);
}

TEST(DriverSenderProbe, OnlyWheelBrisProgramsAreWheelBri) {
    EXPECT_TRUE(isWheelBriProgram(L"C:\\Program Files\\WheelBri\\wheelbri-service.exe"));
    EXPECT_TRUE(isWheelBriProgram(L"D:\\x\\WheelBri.EXE"));
    EXPECT_TRUE(isWheelBriProgram(L"wheelbri.exe"));
    EXPECT_FALSE(isWheelBriProgram(L"C:\\Games\\acc.exe"));
    EXPECT_FALSE(isWheelBriProgram(L"C:\\x\\wheelbri.exe.evil"));
    EXPECT_FALSE(isWheelBriProgram(L"C:\\x\\notwheelbri.exe"));
    EXPECT_FALSE(isWheelBriProgram(L""));
}

#ifdef _WIN32
// What the driver will log, from a normal process: this program owns the sending port, and it is not WheelBri.
TEST(DriverSenderProbe, ASenderOfThisProgramIsNamed) {
    const auto listener = UdpLoopbackListener::create(0);
    ASSERT_NE(listener, nullptr);
    const auto sender = UdpLoopbackSender::create(listener->localPort());
    ASSERT_NE(sender, nullptr);
    const std::array<std::uint8_t, 1> byte{1};
    ASSERT_TRUE(sender->send(byte));
    std::array<std::uint8_t, 4> buffer{};
    std::uint16_t from = 0;
    std::optional<std::size_t> size;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!size && std::chrono::steady_clock::now() < deadline) {
        size = listener->receive(buffer, &from);
        if (!size) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(size.has_value());
    const SenderLookup found = lookUpSender(from, "test");
    const std::string& line = found.line;
    EXPECT_EQ(found.verdict, SenderVerdict::Refuse);
    EXPECT_NE(line.find("pid " + std::to_string(GetCurrentProcessId())), std::string::npos) << line;
    EXPECT_NE(line.find("wheelbri_tests.exe"), std::string::npos) << line;
    EXPECT_NE(line.find("not WheelBri"), std::string::npos) << line;
    const SenderLookup nobody = lookUpSender(1, "test");
    EXPECT_EQ(nobody.verdict, SenderVerdict::Refuse);
    EXPECT_NE(nobody.line.find("no process owns that port"), std::string::npos);
}
#endif

}  // namespace
}  // namespace wheelbri::driver
