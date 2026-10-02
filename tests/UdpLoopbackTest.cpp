#include "wheelemu/UdpLoopback.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <thread>
#include <vector>

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

TEST(UdpLoopback, RawDatagramsArriveInOrder) {
    constexpr std::uint16_t kPort = 41998;
    const auto listener = UdpLoopbackListener::create(kPort);
    const auto sender = UdpLoopbackSender::create(kPort);
    ASSERT_NE(listener, nullptr);
    ASSERT_NE(sender, nullptr);

    const std::array<std::uint8_t, 3> first = {1, 2, 3};
    const std::array<std::uint8_t, 2> second = {9, 8};
    ASSERT_TRUE(sender->send(first));
    ASSERT_TRUE(sender->send(second));

    std::array<std::uint8_t, 16> buffer{};
    std::vector<std::size_t> sizes;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (sizes.size() < 2 && std::chrono::steady_clock::now() < deadline) {
        if (const auto size = listener->receive(buffer)) {
            sizes.push_back(*size);
            if (sizes.size() == 2) {
                EXPECT_EQ(buffer[0], 9);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    ASSERT_EQ(sizes.size(), 2U);
    EXPECT_EQ(sizes[0], 3U);
    EXPECT_EQ(sizes[1], 2U);
    EXPECT_FALSE(listener->receive(buffer).has_value());
}

}  // namespace
}  // namespace wheelemu
