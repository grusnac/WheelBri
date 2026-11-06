// SPDX-License-Identifier: GPL-3.0-or-later
// platform::WaitSet (service/src/Platform.hpp), the event loop's wait, with real sockets and events on 127.0.0.1.

#include "Platform.hpp"

#include "wheelbri/UdpLoopback.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace wheelbri::service {
namespace {

using platform::WaitSet;

constexpr std::uint64_t kMs = 1'000'000;

void sendOne(const std::uint16_t port) {
    const auto sender = UdpLoopbackSender::create(port);
    ASSERT_NE(sender, nullptr);
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    ASSERT_TRUE(sender->send(bytes));
}

bool readOne(UdpLoopbackListener& listener) {
    std::array<std::uint8_t, 64> buffer{};
    return listener.receive(buffer).has_value();
}

void drain(UdpLoopbackListener& listener) {
    while (readOne(listener)) {
    }
}

TEST(WaitSet, NothingToWaitForTimesOutAtTheDeadline) {
    WaitSet set;
    EXPECT_EQ(set.size(), 0);
    const std::uint64_t start = platform::nowNs();
    EXPECT_EQ(set.waitUntil(start + 20 * kMs), WaitSet::kTimedOut);
    const std::uint64_t waited = platform::nowNs() - start;
    EXPECT_GE(waited, 19 * kMs);
    EXPECT_LT(waited, 500 * kMs);
}

TEST(WaitSet, ADeadlineAlreadyPassedReturnsAtOnce) {
    WaitSet set;
    const auto listener = UdpLoopbackListener::create(0);
    ASSERT_NE(listener, nullptr);
    ASSERT_EQ(set.addSocket(listener->nativeSocket()), 0);
    const std::uint64_t start = platform::nowNs();
    EXPECT_EQ(set.waitUntil(0), WaitSet::kTimedOut);
    EXPECT_EQ(set.waitUntil(start), WaitSet::kTimedOut);
    EXPECT_LT(platform::nowNs() - start, 50 * kMs);
}

TEST(WaitSet, InvalidSourcesAreRefused) {
    WaitSet set;
#if defined(_WIN32)
    EXPECT_EQ(set.addEvent(0), -1);
    EXPECT_EQ(set.addEvent(reinterpret_cast<std::intptr_t>(INVALID_HANDLE_VALUE)), -1);
    EXPECT_EQ(set.addSocket(static_cast<std::intptr_t>(INVALID_SOCKET)), -1);
#else
    EXPECT_EQ(set.addEvent(-1), -1);
    EXPECT_EQ(set.addSocket(-1), -1);
#endif
    EXPECT_EQ(set.size(), 0);
}

// Each socket wakes with its own index; with several ready, the first, and the others are not lost by that.
TEST(WaitSet, ADatagramWakesWithItsSocketsIndex) {
    WaitSet set;
    // Ports the system chooses: tests running side by side never share one.
    const auto a = UdpLoopbackListener::create(0);
    ASSERT_NE(a, nullptr);
    const std::uint16_t portA = a->localPort();
    const auto b = UdpLoopbackListener::create(0);
    ASSERT_NE(b, nullptr);
    const std::uint16_t portB = b->localPort();
    ASSERT_EQ(set.addSocket(a->nativeSocket()), 0);
    ASSERT_EQ(set.addSocket(b->nativeSocket()), 1);
    EXPECT_EQ(set.size(), 2);

    sendOne(portB);
    EXPECT_EQ(set.waitUntil(platform::nowNs() + 1000 * kMs), 1);   // the second one
    drain(*b);

    sendOne(portA);
    EXPECT_EQ(set.waitUntil(platform::nowNs() + 1000 * kMs), 0);
    drain(*a);

    sendOne(portB);
    sendOne(portA);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(set.waitUntil(platform::nowNs() + 1000 * kMs), 0);
    drain(*a);
    EXPECT_EQ(set.waitUntil(platform::nowNs() + 1000 * kMs), 1);   // b's datagram was not lost by waking for a
    drain(*b);
}

TEST(WaitSet, ADatagramSentDuringTheWaitWakesIt) {
    WaitSet set;
    // A port the system chooses: tests running side by side never share one.
    const auto listener = UdpLoopbackListener::create(0);
    ASSERT_NE(listener, nullptr);
    const std::uint16_t portA = listener->localPort();
    ASSERT_EQ(set.addSocket(listener->nativeSocket()), 0);
    std::thread later([portA] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        sendOne(portA);
    });
    const std::uint64_t start = platform::nowNs();
    const int ready = set.waitUntil(start + 2000 * kMs);
    const std::uint64_t waited = platform::nowNs() - start;
    later.join();
    EXPECT_EQ(ready, 0);
    EXPECT_LT(waited, 1000 * kMs);   // woken by the datagram, not by the deadline
    drain(*listener);
}

TEST(WaitSet, NoWakeAfterEverythingWasRead) {
    WaitSet set;
    // A port the system chooses: tests running side by side never share one.
    const auto listener = UdpLoopbackListener::create(0);
    ASSERT_NE(listener, nullptr);
    const std::uint16_t portA = listener->localPort();
    ASSERT_EQ(set.addSocket(listener->nativeSocket()), 0);
    sendOne(portA);
    sendOne(portA);
    ASSERT_EQ(set.waitUntil(platform::nowNs() + 1000 * kMs), 0);
    drain(*listener);
    EXPECT_EQ(set.waitUntil(platform::nowNs() + 20 * kMs), WaitSet::kTimedOut);   // no stale wake-up
}

TEST(WaitSet, ADatagramLeftUnreadWakesAgain) {
    WaitSet set;
    // A port the system chooses: tests running side by side never share one.
    const auto listener = UdpLoopbackListener::create(0);
    ASSERT_NE(listener, nullptr);
    const std::uint16_t portA = listener->localPort();
    ASSERT_EQ(set.addSocket(listener->nativeSocket()), 0);
    sendOne(portA);
    sendOne(portA);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));   // both queued
    ASSERT_EQ(set.waitUntil(platform::nowNs() + 1000 * kMs), 0);
    ASSERT_TRUE(readOne(*listener));                               // only one of the two
    EXPECT_EQ(set.waitUntil(platform::nowNs() + 1000 * kMs), 0);   // the other is still there
    ASSERT_TRUE(readOne(*listener));
    EXPECT_EQ(set.waitUntil(platform::nowNs() + 20 * kMs), WaitSet::kTimedOut);
}

TEST(WaitSet, ASignalledEventWakesWithItsIndex) {
    WaitSet set;
    const auto listener = UdpLoopbackListener::create(0);
    ASSERT_NE(listener, nullptr);
    ASSERT_EQ(set.addSocket(listener->nativeSocket()), 0);
#if defined(_WIN32)
    // Like DirectInput's notification: an auto-reset event.
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ASSERT_NE(event, nullptr);
    ASSERT_EQ(set.addEvent(reinterpret_cast<std::intptr_t>(event)), 1);
    EXPECT_EQ(set.waitUntil(platform::nowNs() + 20 * kMs), WaitSet::kTimedOut);
    ASSERT_TRUE(SetEvent(event));
    EXPECT_EQ(set.waitUntil(platform::nowNs() + 1000 * kMs), 1);
    EXPECT_EQ(set.waitUntil(platform::nowNs() + 20 * kMs), WaitSet::kTimedOut);   // auto-reset by the wait
    CloseHandle(event);
#else
    // Like evdev: a file descriptor that becomes readable.
    std::array<int, 2> pipeFds{};
    ASSERT_EQ(::pipe(pipeFds.data()), 0);
    ASSERT_EQ(set.addEvent(pipeFds[0]), 1);
    EXPECT_EQ(set.waitUntil(platform::nowNs() + 20 * kMs), WaitSet::kTimedOut);
    const char byte = 'x';
    ASSERT_EQ(::write(pipeFds[1], &byte, 1), 1);
    EXPECT_EQ(set.waitUntil(platform::nowNs() + 1000 * kMs), 1);
    char read = 0;
    ASSERT_EQ(::read(pipeFds[0], &read, 1), 1);
    EXPECT_EQ(set.waitUntil(platform::nowNs() + 20 * kMs), WaitSet::kTimedOut);
    ::close(pipeFds[0]);
    ::close(pipeFds[1]);
#endif
}

TEST(WaitSet, AnEmptyDatagramWakesAndDoesNotHideTheOnesBehindIt) {
    // Any local program can send an empty datagram to the force feedback port: it must not hide a real one.
    WaitSet set;
    // A port the system chooses: tests running side by side never share one.
    const auto listener = UdpLoopbackListener::create(0);
    ASSERT_NE(listener, nullptr);
    const std::uint16_t portA = listener->localPort();
    ASSERT_EQ(set.addSocket(listener->nativeSocket()), 0);
    {
        const auto sender = UdpLoopbackSender::create(portA);
        ASSERT_NE(sender, nullptr);
        const std::array<std::uint8_t, 1> none{};
        ASSERT_TRUE(sender->send(std::span<const std::uint8_t>(none).first(0)));
    }
    sendOne(portA);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const std::uint64_t start = platform::nowNs();
    EXPECT_EQ(set.waitUntil(start + 1000 * kMs), 0);
    EXPECT_LT(platform::nowNs() - start, 500 * kMs);   // at once, not at the deadline
    std::array<std::uint8_t, 64> buffer{};
    const auto first = listener->receive(buffer);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 0U);                                         // the empty one
    EXPECT_EQ(set.waitUntil(platform::nowNs() + 1000 * kMs), 0);   // the real one is still announced
    const auto second = listener->receive(buffer);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*second, 4U);
    EXPECT_EQ(set.waitUntil(platform::nowNs() + 20 * kMs), WaitSet::kTimedOut);
}

#if !defined(_WIN32)
TEST(WaitSet, AClosedDescriptorMakesTheWaitFail) {
    WaitSet set;
    std::array<int, 2> pipeFds{};
    ASSERT_EQ(::pipe(pipeFds.data()), 0);
    ASSERT_EQ(set.addEvent(pipeFds[0]), 0);
    ::close(pipeFds[0]);
    ::close(pipeFds[1]);
    // poll reports a closed descriptor as POLLNVAL at once, every time: a failure (the loop then runs fixed), not a
    // source with news, which would wake the loop without pause.
    EXPECT_EQ(set.waitUntil(platform::nowNs() + 20 * kMs), WaitSet::kFailed);
    EXPECT_FALSE(set.failureText().empty());
}
#endif

}  // namespace
}  // namespace wheelbri::service
