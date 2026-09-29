#include "wheelemu/UdpLoopback.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace wheelemu {
namespace {

constexpr std::uint16_t kTestPort = 41999;

std::optional<StatePacket> pollWithTimeout(IStateReceiver& receiver) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto packet = receiver.pollLatest()) {
            return packet;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::nullopt;
}

TEST(UdpLoopback, ReceiverGetsNewestPacket) {
    const auto receiver = UdpLoopbackReceiver::create(kTestPort);
    const auto publisher = UdpLoopbackPublisher::create(kTestPort);
    ASSERT_NE(receiver, nullptr);
    ASSERT_NE(publisher, nullptr);

    for (std::uint32_t sequence = 1; sequence <= 5; ++sequence) {
        StatePacket packet;
        packet.sequence = sequence;
        packet.report.steering = static_cast<std::int32_t>(sequence * 100);
        ASSERT_TRUE(publisher->publish(packet));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const auto latest = pollWithTimeout(*receiver);
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->sequence, 5U);
    EXPECT_EQ(latest->report.steering, 500);
    EXPECT_FALSE(receiver->pollLatest().has_value());   // queue drained
}

TEST(UdpLoopback, PublishingWithoutReceiverIsHarmless) {
    const auto publisher = UdpLoopbackPublisher::create(kTestPort + 1);
    ASSERT_NE(publisher, nullptr);
    (void)publisher->publish(StatePacket{});
}

}  // namespace
}  // namespace wheelemu
