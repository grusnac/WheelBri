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
    // A port the system chooses: tests running side by side never share one.
    const auto receiver = UdpLoopbackReceiver::create(0);
    ASSERT_NE(receiver, nullptr);
    const std::uint16_t port = receiver->localPort();
    const auto publisher = UdpLoopbackPublisher::create(port);
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
    // A port the system chooses: tests running side by side never share one.
    const auto listener = UdpLoopbackListener::create(0);
    ASSERT_NE(listener, nullptr);
    const std::uint16_t port = listener->localPort();
    const auto sender = UdpLoopbackSender::create(port);
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
    // A port the system chooses: tests running side by side never share one.
    const auto receiver = UdpLoopbackReceiver::create(0);
    ASSERT_NE(receiver, nullptr);
    const std::uint16_t port = receiver->localPort();
    const auto publisher = UdpLoopbackPublisher::create(port);
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
    // A port the system chooses: tests running side by side never share one.
    const auto receiver = UdpLoopbackReceiver::create(0);
    ASSERT_NE(receiver, nullptr);
    const std::uint16_t port = receiver->localPort();
    const auto publisher = UdpLoopbackPublisher::create(port);
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
    // A port the system chooses: tests running side by side never share one.
    const auto receiver = UdpLoopbackReceiver::create(0);
    ASSERT_NE(receiver, nullptr);
    const std::uint16_t port = receiver->localPort();
    const auto publisher = UdpLoopbackPublisher::create(port);
    const auto sender = UdpLoopbackSender::create(port);
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
    // A port the system chooses: tests running side by side never share one.
    const auto listener = UdpLoopbackListener::create(0);
    ASSERT_NE(listener, nullptr);
    const std::uint16_t port = listener->localPort();
    const auto sender = UdpLoopbackSender::create(port);
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
    EXPECT_NE(from, port);
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
    // A port the system chooses: tests running side by side never share one.
    const auto receiver = UdpLoopbackReceiver::create(0);
    ASSERT_NE(receiver, nullptr);
    const std::uint16_t port = receiver->localPort();
    const auto publisher = UdpLoopbackPublisher::create(port);
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
    // A port the system chooses: tests running side by side never share one.
    const auto receiver = UdpLoopbackReceiver::create(0);
    ASSERT_NE(receiver, nullptr);
    const std::uint16_t port = receiver->localPort();
    const auto sender = UdpLoopbackSender::create(port);
    const auto publisher = UdpLoopbackPublisher::create(port);
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

// Damaged states from a stray program or an older build: cut short, one byte too long, the wrong magic, far too
// large for the receiver's buffer (read cut), a log level packet cut or padded. None is taken as a state or a
// level, each is read out of the socket, and the good state after them gets through.
TEST(UdpLoopback, ShortLongAndForeignDatagramsAreNeverStates) {
    // A port the system chooses: tests running side by side never share one.
    const auto receiver = UdpLoopbackReceiver::create(0);
    ASSERT_NE(receiver, nullptr);
    const std::uint16_t port = receiver->localPort();
    const auto sender = UdpLoopbackSender::create(port);
    ASSERT_NE(receiver, nullptr);
    ASSERT_NE(sender, nullptr);
    const auto good = encode(withSequence(9));
    const std::vector<std::uint8_t> whole(good.begin(), good.end());
    std::vector<std::uint8_t> shortOne(whole.begin(), whole.end() - 1);
    std::vector<std::uint8_t> longOne = whole;
    longOne.push_back(0);
    std::vector<std::uint8_t> wrongMagic = whole;
    wrongMagic[0] = 'X';
    std::vector<std::uint8_t> huge = whole;
    huge.resize(1000, 0xAB);                  // the first 33 bytes are a good state: still not one
    const auto level = encode(LogLevelPacket{LogLevel::Debug});
    std::vector<std::uint8_t> levelShort(level.begin(), level.end() - 1);
    std::vector<std::uint8_t> levelLong(level.begin(), level.end());
    levelLong.push_back(0);
    std::vector<std::uint8_t> levelWrongMagic(level.begin(), level.end());
    levelWrongMagic[0] = 'X';
    std::vector<std::uint8_t> empty;
    for (const auto* bytes : {&shortOne, &longOne, &wrongMagic, &huge, &levelShort, &levelLong, &levelWrongMagic,
                              &empty}) {
        ASSERT_TRUE(sender->send(*bytes));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(receiver->pollLatest().has_value());
#ifdef _WIN32
    EXPECT_EQ(receiver->lastPoll().read, 7U);   // Windows drops the huge one (WSAEMSGSIZE, skipped, not read)
#else
    EXPECT_EQ(receiver->lastPoll().read, 8U);   // elsewhere it is read cut short
#endif
    EXPECT_FALSE(receiver->lastPoll().capped);
    EXPECT_FALSE(receiver->takeLogLevel().has_value());

    ASSERT_TRUE(sender->send(whole));
    const auto latest = pollWithTimeout(*receiver);
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->sequence, 9U);
}

// A datagram larger than UDP allows cannot be sent: send says so, nothing arrives, and the next one goes through.
TEST(UdpLoopback, AnOversizedDatagramIsRefusedBySend) {
    // A port the system chooses: tests running side by side never share one.
    const auto listener = UdpLoopbackListener::create(0);
    ASSERT_NE(listener, nullptr);
    const std::uint16_t port = listener->localPort();
    const auto sender = UdpLoopbackSender::create(port);
    ASSERT_NE(listener, nullptr);
    ASSERT_NE(sender, nullptr);
    const std::vector<std::uint8_t> tooLarge(70000, 1);
    EXPECT_FALSE(sender->send(tooLarge));
    EXPECT_FALSE(sender->send(tooLarge));   // again: rate-limited warning, same answer
    std::array<std::uint8_t, 16> buffer{};
    EXPECT_FALSE(listener->receive(buffer).has_value());
    const std::array<std::uint8_t, 2> small{4, 2};
    ASSERT_TRUE(sender->send(small));
    std::optional<std::size_t> size;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!size && std::chrono::steady_clock::now() < deadline) {
        size = listener->receive(buffer);
        if (!size) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(size.has_value());
    EXPECT_EQ(*size, 2U);
    EXPECT_EQ(buffer[0], 4);
}

// Several gaps in a row (a busy PC dropping datagrams): every newer state is still handed over, in order.
TEST(UdpLoopback, RepeatedGapsAreFollowed) {
    // A port the system chooses: tests running side by side never share one.
    const auto receiver = UdpLoopbackReceiver::create(0);
    ASSERT_NE(receiver, nullptr);
    const std::uint16_t port = receiver->localPort();
    const auto publisher = UdpLoopbackPublisher::create(port);
    ASSERT_NE(receiver, nullptr);
    ASSERT_NE(publisher, nullptr);
    for (const std::uint32_t sequence : {1U, 5U, 9U, 20U, 21U}) {
        ASSERT_TRUE(publisher->publish(withSequence(sequence)));
        const auto packet = pollWithTimeout(*receiver);
        ASSERT_TRUE(packet.has_value());
        EXPECT_EQ(packet->sequence, sequence);
        EXPECT_EQ(packet->report.steering, static_cast<std::int32_t>(sequence));
    }
}

TEST(UdpLoopback, ASecondReceiverOnTheSamePortIsRefused) {
    // A port the system chooses: tests running side by side never share one.
    const auto first = UdpLoopbackReceiver::create(0);
    ASSERT_NE(first, nullptr);
    const std::uint16_t port = first->localPort();
    EXPECT_EQ(UdpLoopbackReceiver::create(port), nullptr);   // a second wheelbri-uhid, or the shim in a game
    EXPECT_EQ(UdpLoopbackListener::create(port), nullptr);
}

TEST(UdpLoopback, TheReceiversSocketCanBeWaitedOn) {
    // A port the system chooses: tests running side by side never share one.
    const auto receiver = UdpLoopbackReceiver::create(0);
    ASSERT_NE(receiver, nullptr);
#ifdef _WIN32
    EXPECT_NE(receiver->nativeSocket(), static_cast<std::intptr_t>(~static_cast<std::uintptr_t>(0)));
#else
    const std::uint16_t port = receiver->localPort();
    const int fd = static_cast<int>(receiver->nativeSocket());
    ASSERT_GE(fd, 0);
    pollfd pfd{fd, POLLIN, 0};
    EXPECT_EQ(::poll(&pfd, 1, 0), 0);   // nothing yet
    const auto publisher = UdpLoopbackPublisher::create(port);
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
