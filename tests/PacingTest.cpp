#include "wheelbri/Pacing.hpp"

#include <gtest/gtest.h>

namespace wheelbri {
namespace {

constexpr std::uint64_t kMs = 1'000'000;

TargetReport report(const std::int32_t steering) {
    TargetReport r;
    r.steering = steering;
    return r;
}

TEST(StateSendGate, SendsTheFirstStateAndEveryChange) {
    StateSendGate gate(0);
    EXPECT_TRUE(gate.shouldSend(report(0), 1 * kMs));
    EXPECT_FALSE(gate.shouldSend(report(0), 2 * kMs));
    EXPECT_TRUE(gate.shouldSend(report(5), 3 * kMs));
    EXPECT_FALSE(gate.shouldSend(report(5), 4 * kMs));
    EXPECT_EQ(gate.sent(), 2U);
    EXPECT_EQ(gate.unchanged(), 2U);
}

TEST(StateSendGate, KeepsReceiversAliveWithoutChanges) {
    StateSendGate gate(0, 100 * kMs);
    EXPECT_TRUE(gate.shouldSend(report(1), 0));
    int sent = 0;
    for (std::uint64_t t = 1; t <= 1000; ++t) {
        if (gate.shouldSend(report(1), t * kMs)) ++sent;
    }
    EXPECT_EQ(sent, 10);   // one every 100 ms for a second
}

TEST(StateSendGate, HoldsAChangeForTheIntervalAndSendsItThen) {
    StateSendGate gate(2 * kMs);   // 500 a second
    EXPECT_TRUE(gate.shouldSend(report(1), 10 * kMs));
    EXPECT_FALSE(gate.shouldSend(report(2), 11 * kMs));   // too soon
    EXPECT_EQ(gate.heldBack(), 1U);
    EXPECT_TRUE(gate.shouldSend(report(2), 12 * kMs));    // the held change, not lost
    EXPECT_FALSE(gate.shouldSend(report(2), 13 * kMs));
}

TEST(StateSendGate, AtMostOncePerIntervalWhileEverythingChanges) {
    StateSendGate gate(2 * kMs);
    int sent = 0;
    for (std::int32_t t = 0; t < 1000; ++t) {
        if (gate.shouldSend(report(t), static_cast<std::uint64_t>(t) * kMs)) ++sent;
    }
    EXPECT_EQ(sent, 500);
}

TEST(StateSendGate, ButtonsAndHatCountAsChanges) {
    StateSendGate gate(0);
    TargetReport r = report(0);
    EXPECT_TRUE(gate.shouldSend(r, 0));
    r.buttons = 1;
    EXPECT_TRUE(gate.shouldSend(r, 1 * kMs));
    r.hat = 2;
    EXPECT_TRUE(gate.shouldSend(r, 2 * kMs));
}

}  // namespace
}  // namespace wheelbri
