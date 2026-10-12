#include "wheelbri/UdpLoopback.hpp"

#include "wheelbri/LogLevelPacket.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <optional>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <poll.h>
#endif

namespace wheelbri {
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

TEST(UdpLoopback, PollReadsAtMostItsCapAndTheRestWaits) {
    constexpr std::uint16_t kPort = 41997;
    const auto receiver = UdpLoopbackReceiver::create(kPort);
    const auto publisher = UdpLoopbackPublisher::create(kPort);
    ASSERT_NE(receiver, nullptr);
    ASSERT_NE(publisher, nullptr);
    constexpr std::uint32_t kSent = 160;   // well within the socket buffer (Linux keeps about 270 of these)
    for (std::uint32_t sequence = 1; sequence <= kSent; ++sequence) {
        StatePacket packet;
        packet.sequence = sequence;
        ASSERT_TRUE(publisher->publish(packet));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto first = receiver->pollLatest();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->sequence, UdpLoopbackReceiver::kMaxPerPoll);   // one call stops after its share
    EXPECT_EQ(receiver->lastPoll().read, UdpLoopbackReceiver::kMaxPerPoll);
    EXPECT_TRUE(receiver->lastPoll().capped);   // behind: newer packets still wait
    std::optional<StatePacket> last;
    for (int i = 0; i < 20; ++i) {
        if (auto packet = receiver->pollLatest()) last = packet;
    }
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last->sequence, kSent);   // the later calls get the rest
    EXPECT_EQ(receiver->lastPoll().read, 0U);
    EXPECT_FALSE(receiver->lastPoll().capped);
}

TEST(UdpLoopback, SenderFilterDropsRefusedSenders) {
    constexpr std::uint16_t kPort = 41996;
    const auto receiver = UdpLoopbackReceiver::create(kPort);
    const auto publisher = UdpLoopbackPublisher::create(kPort);
    ASSERT_NE(receiver, nullptr);
    ASSERT_NE(publisher, nullptr);
    std::vector<std::uint16_t> senders;
    receiver->setSenderFilter([&](const std::uint16_t port) {
        senders.push_back(port);
        return false;
    });
    ASSERT_TRUE(publisher->publish(StatePacket{}));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(receiver->pollLatest().has_value());
    ASSERT_EQ(senders.size(), 1U);
    EXPECT_NE(senders[0], 0);
    receiver->setSenderFilter([](std::uint16_t) { return true; });
    ASSERT_TRUE(publisher->publish(StatePacket{}));
    EXPECT_TRUE(pollWithTimeout(*receiver).has_value());
}

TEST(UdpLoopback, TheReceiverTakesTheServicesLogLevelBesideTheStates) {
    constexpr std::uint16_t kPort = 41994;
    const auto receiver = UdpLoopbackReceiver::create(kPort);
    const auto publisher = UdpLoopbackPublisher::create(kPort);
    const auto sender = UdpLoopbackSender::create(kPort);
    ASSERT_NE(receiver, nullptr);
    ASSERT_NE(publisher, nullptr);
    ASSERT_NE(sender, nullptr);
    EXPECT_FALSE(receiver->takeLogLevel().has_value());
    // From the state's own socket, as the service sends it.
    ASSERT_TRUE(publisher->publishLogLevel(LogLevel::Info));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(receiver->pollLatest().has_value());
    EXPECT_EQ(receiver->takeLogLevel(), LogLevel::Info);
    StatePacket state;
    state.sequence = 7;
    ASSERT_TRUE(publisher->publish(state));
    ASSERT_TRUE(sender->send(encode(LogLevelPacket{LogLevel::Info})));
    ASSERT_TRUE(sender->send(encode(LogLevelPacket{LogLevel::Debug})));   // the newest one counts
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto latest = pollWithTimeout(*receiver);
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->sequence, 7U);   // a level is not a state
    EXPECT_EQ(receiver->takeLogLevel(), LogLevel::Debug);
    EXPECT_FALSE(receiver->takeLogLevel().has_value());   // taken
    // A level alone: no state from it.
    ASSERT_TRUE(sender->send(encode(LogLevelPacket{LogLevel::Trace})));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(receiver->pollLatest().has_value());
    EXPECT_EQ(receiver->takeLogLevel(), LogLevel::Trace);
}

TEST(UdpLoopback, ListenerTellsTheSendersPort) {
    constexpr std::uint16_t kPort = 41995;
    const auto listener = UdpLoopbackListener::create(kPort);
    const auto sender = UdpLoopbackSender::create(kPort);
    ASSERT_NE(listener, nullptr);
    ASSERT_NE(sender, nullptr);
    const std::array<std::uint8_t, 1> byte = {7};
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
    EXPECT_NE(from, 0);
    EXPECT_NE(from, kPort);
}
StatePacket withSequence(const std::uint32_t sequence) {
    StatePacket packet;
    packet.sequence = sequence;
    packet.report.steering = static_cast<std::int32_t>(sequence);
    return packet;
}

// A restarted service counts from 1 again, and packets may be lost: the receiver hands over what came last, in
// arrival order, never holding on to the old, higher numbers.
TEST(UdpLoopback, ARestartedServiceAndLostPacketsAreFollowed) {
    constexpr std::uint16_t kPort = 41993;
    const auto receiver = UdpLoopbackReceiver::create(kPort);
    const auto publisher = UdpLoopbackPublisher::create(kPort);
    ASSERT_NE(receiver, nullptr);
    ASSERT_NE(publisher, nullptr);
    ASSERT_TRUE(publisher->publish(withSequence(100)));
    EXPECT_EQ(pollWithTimeout(*receiver)->sequence, 100U);
    ASSERT_TRUE(publisher->publish(withSequence(1)));   // restarted
    EXPECT_EQ(pollWithTimeout(*receiver)->sequence, 1U);
    ASSERT_TRUE(publisher->publish(withSequence(1)));   // the same number again (two services)
    EXPECT_EQ(pollWithTimeout(*receiver)->sequence, 1U);
    ASSERT_TRUE(publisher->publish(withSequence(50)));  // 48 lost
    EXPECT_EQ(pollWithTimeout(*receiver)->sequence, 50U);
}

TEST(UdpLoopback, MalformedDatagramsAreDroppedAndTheGoodOnesKept) {
    constexpr std::uint16_t kPort = 41992;
    const auto receiver = UdpLoopbackReceiver::create(kPort);
    const auto sender = UdpLoopbackSender::create(kPort);
    const auto publisher = UdpLoopbackPublisher::create(kPort);
    ASSERT_NE(receiver, nullptr);
    ASSERT_NE(sender, nullptr);
    ASSERT_NE(publisher, nullptr);
    const std::array<std::uint8_t, 5> garbage{1, 2, 3, 4, 5};
    auto wrongVersion = encode(withSequence(7));
    wrongVersion[4] = 0x7F;   // the version field
    ASSERT_TRUE(sender->send(garbage));
    ASSERT_TRUE(sender->send(wrongVersion));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(receiver->pollLatest().has_value());
    EXPECT_EQ(receiver->lastPoll().read, 2U);   // both read (and dropped), not left in the socket

    ASSERT_TRUE(sender->send(garbage));
    ASSERT_TRUE(publisher->publish(withSequence(8)));
    ASSERT_TRUE(sender->send(garbage));
    const auto latest = pollWithTimeout(*receiver);
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->sequence, 8U);
}

TEST(UdpLoopback, ASecondReceiverOnTheSamePortIsRefused) {
    constexpr std::uint16_t kPort = 41991;
    const auto first = UdpLoopbackReceiver::create(kPort);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(UdpLoopbackReceiver::create(kPort), nullptr);   // a second wheelbri-uhid, or the shim in a game
    EXPECT_EQ(UdpLoopbackListener::create(kPort), nullptr);
}

TEST(UdpLoopback, TheReceiversSocketCanBeWaitedOn) {
    constexpr std::uint16_t kPort = 41990;
    const auto receiver = UdpLoopbackReceiver::create(kPort);
    ASSERT_NE(receiver, nullptr);
#ifdef _WIN32
    EXPECT_NE(receiver->nativeSocket(), static_cast<std::intptr_t>(~static_cast<std::uintptr_t>(0)));
#else
    const int fd = static_cast<int>(receiver->nativeSocket());
    ASSERT_GE(fd, 0);
    pollfd pfd{fd, POLLIN, 0};
    EXPECT_EQ(::poll(&pfd, 1, 0), 0);   // nothing yet
    const auto publisher = UdpLoopbackPublisher::create(kPort);
    ASSERT_NE(publisher, nullptr);
    ASSERT_TRUE(publisher->publish(withSequence(1)));
    pfd.revents = 0;
    EXPECT_EQ(::poll(&pfd, 1, 1000), 1);   // wakes for the state (wheelbri-uhid's loop)
    EXPECT_NE(pfd.revents & POLLIN, 0);
    EXPECT_TRUE(receiver->pollLatest().has_value());
    pfd.revents = 0;
    EXPECT_EQ(::poll(&pfd, 1, 0), 0);   // read: quiet again
#endif
}

}  // namespace
}  // namespace wheelbri
