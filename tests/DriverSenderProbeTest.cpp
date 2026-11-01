// The driver's sender probe (driver/SenderProbe.hpp): which senders it looks up, and, on Windows, what it finds out
// about one (a socket of this test program: not WheelBri).

#include "../driver/SenderProbe.hpp"

#include <gtest/gtest.h>

#ifdef _WIN32
#include "wheelbri/UdpLoopback.hpp"

#include <array>
#include <chrono>
#include <optional>
#include <thread>
#endif

namespace wheelbri::driver {
namespace {

// A new sender is looked up once, then not again until kRecheckMs have gone by.
TEST(DriverSenderProbe, EachSenderIsLookedUpOnceAWhile) {
    SenderSeen seen;
    EXPECT_TRUE(seen.shouldLookUp(50000, 0));
    EXPECT_FALSE(seen.shouldLookUp(50000, 1));
    EXPECT_FALSE(seen.shouldLookUp(50000, SenderSeen::kRecheckMs - 1));
    EXPECT_TRUE(seen.shouldLookUp(50001, 2));   // another sender
    EXPECT_TRUE(seen.shouldLookUp(50000, SenderSeen::kRecheckMs));
    EXPECT_FALSE(seen.shouldLookUp(50000, SenderSeen::kRecheckMs + 1));
}

// A flood from ever new ports: at most kLookupsPerSecond a second, the rest wait for the next second.
TEST(DriverSenderProbe, LookupsAreCappedPerSecond) {
    SenderSeen seen;
    std::uint32_t lookups = 0;
    for (std::uint16_t port = 1000; port < 1100; ++port) {
        if (seen.shouldLookUp(port, 10)) ++lookups;
    }
    EXPECT_EQ(lookups, SenderSeen::kLookupsPerSecond);
    EXPECT_TRUE(seen.shouldLookUp(2000, 1010));
}

// More senders than entries: the one seen longest ago makes room, and is looked up again when it comes back.
TEST(DriverSenderProbe, TheOldestSenderMakesRoom) {
    SenderSeen seen;
    std::int64_t now = 0;
    for (std::uint16_t port = 0; port < SenderSeen::kEntries; ++port) {
        ASSERT_TRUE(seen.shouldLookUp(static_cast<std::uint16_t>(3000 + port), now));
        now += 1000;   // its own second each: never capped
    }
    EXPECT_FALSE(seen.shouldLookUp(3000, now));   // still remembered
    EXPECT_TRUE(seen.shouldLookUp(4000, now + 1000));   // takes 3000's place
    EXPECT_TRUE(seen.shouldLookUp(3000, now + 2000));
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
    const std::string line = lookUpSender(from, "test");
    EXPECT_NE(line.find("pid " + std::to_string(GetCurrentProcessId())), std::string::npos) << line;
    EXPECT_NE(line.find("wheelbri_tests.exe"), std::string::npos) << line;
    EXPECT_NE(line.find("not WheelBri"), std::string::npos) << line;
    EXPECT_NE(lookUpSender(1, "test").find("no process owns that port"), std::string::npos);
}
#endif

}  // namespace
}  // namespace wheelbri::driver
