#include "wheelbri/TrueForceFfb.hpp"

#include "wheelbri/TrueForce.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace wheelbri {
namespace {

std::vector<std::uint8_t> stream(const std::uint16_t cur) {
    std::vector<std::uint8_t> r(64, 0);
    r[0] = 0x01;
    r[4] = 0x01;
    r[6] = r[8] = static_cast<std::uint8_t>(cur);
    r[7] = r[9] = static_cast<std::uint8_t>(cur >> 8);
    return r;
}

std::vector<std::uint8_t> command(const std::uint8_t type) {
    std::vector<std::uint8_t> r(64, 0);
    r[0] = 0x01;
    r[4] = type;
    return r;
}

std::vector<std::uint8_t> damping(const float value) {
    std::vector<std::uint8_t> r = command(0x0C);
    std::memcpy(r.data() + 6, &value, sizeof(value));
    return r;
}

struct Ffb {
    std::vector<FfbPacket> sent;
    TrueForceFfb ffb{[this](const FfbPacket& p) { sent.push_back(p); }};
};

TEST(TrueForceFfb, LevelFromTheStreamsMotorTarget) {
    EXPECT_FLOAT_EQ(*TrueForceFfb::level(stream(0x8000)), 0.0F);
    EXPECT_FLOAT_EQ(*TrueForceFfb::level(stream(0x0000)), 1.0F);
    EXPECT_NEAR(*TrueForceFfb::level(stream(0xFFFF)), -1.0F, 1e-4);
    EXPECT_FLOAT_EQ(*TrueForceFfb::level(stream(0x4000)), 0.5F);
    EXPECT_FALSE(TrueForceFfb::level(command(0x03)));
    EXPECT_FALSE(TrueForceFfb::level(std::vector<std::uint8_t>{0x01, 0, 0, 0, 0x01}));   // too short
    auto other = stream(0x4000);
    other[0] = 0x02;
    EXPECT_FALSE(TrueForceFfb::level(other));
}

// The target is the mean of the two words, which lean opposite ways (ACC, 2026-11-01: a real G923's stream at about
// zero torque, 6..7 = 0x9600, 8..9 = 0x6a00): no force, where 6..7 alone said -0.17.
TEST(TrueForceFfb, LevelIsTheMeanOfTheTwoWords) {
    auto r = stream(0x8000);
    r[6] = 0x00;
    r[7] = 0x96;
    r[8] = 0x00;
    r[9] = 0x6A;
    EXPECT_FLOAT_EQ(*TrueForceFfb::level(r), 0.0F);
    r[9] = 0x4A;   // 0x9600 and 0x4a00: 0x7000
    EXPECT_FLOAT_EQ(*TrueForceFfb::level(r), 0.125F);
    EXPECT_EQ(streamTarget(r), 0x7000);
}

TEST(TrueForceFfb, PlaysTheStreamInItsSlotAtMostEvery2Ms) {
    Ffb f;
    f.ffb.output(stream(0x4000), 1000);
    ASSERT_EQ(f.sent.size(), 1U);
    EXPECT_EQ(f.sent[0].kind, FfbPacket::Kind::SlotState);
    EXPECT_EQ(f.sent[0].slot, TrueForceFfb::kSlot);
    EXPECT_TRUE(f.sent[0].playing);
    EXPECT_EQ(f.sent[0].effect.type, FfbEffectType::Constant);
    EXPECT_FLOAT_EQ(f.sent[0].effect.constantLevel, 0.5F);
    f.ffb.output(stream(0x4000), 1500);      // the same: nothing
    f.ffb.output(stream(0xC000), 2000);      // 1 ms later: held back
    EXPECT_EQ(f.sent.size(), 1U);
    f.ffb.tick(2500);
    EXPECT_EQ(f.sent.size(), 1U);
    f.ffb.tick(3000);                        // 2 ms after the last one
    ASSERT_EQ(f.sent.size(), 2U);
    EXPECT_FLOAT_EQ(f.sent[1].effect.constantLevel, -0.5F);
    EXPECT_GT(f.sent[1].sequence, f.sent[0].sequence);
    f.ffb.output(stream(0x8000), 5000);      // 2 ms on: at once
    ASSERT_EQ(f.sent.size(), 3U);
    EXPECT_FLOAT_EQ(f.sent[2].effect.constantLevel, 0.0F);
    EXPECT_TRUE(f.sent[2].playing);
}

TEST(TrueForceFfb, KeepsAliveWhilePlaying) {
    Ffb f;
    f.ffb.output(stream(0x6000), 0);
    for (std::int64_t t = 1000; t < TrueForceFfb::kKeepAliveUs; t += 1000) {
        f.ffb.output(stream(0x6000), t);
        f.ffb.tick(t);
    }
    EXPECT_EQ(f.sent.size(), 1U);
    f.ffb.output(stream(0x6000), TrueForceFfb::kKeepAliveUs);
    f.ffb.tick(TrueForceFfb::kKeepAliveUs);
    ASSERT_EQ(f.sent.size(), 2U);
    EXPECT_TRUE(f.sent[1].playing);
    EXPECT_FLOAT_EQ(f.sent[1].effect.constantLevel, 0.25F);
}

// A stop sets the force to 0 but keeps it playing: the SDK stops and starts again whenever a game re-opens the wheel.
// Quiet only without a stream.
TEST(TrueForceFfb, AStopZeroesTheForceAndOnlyNoStreamQuietsIt) {
    Ffb f;
    f.ffb.output(stream(0x2000), 0);
    f.ffb.output(command(0x04), 1000);
    ASSERT_EQ(f.sent.size(), 2U);
    EXPECT_TRUE(f.sent[1].playing);
    EXPECT_FLOAT_EQ(f.sent[1].effect.constantLevel, 0.0F);
    EXPECT_TRUE(f.ffb.playing());
    f.ffb.output(command(0x03), 1500);       // started again at once: nothing
    f.ffb.output(stream(0x8000), 2000);      // still no force: nothing
    EXPECT_EQ(f.sent.size(), 2U);
    f.ffb.tick(2000 + TrueForceFfb::kQuietUs);
    ASSERT_EQ(f.sent.size(), 3U);
    EXPECT_FALSE(f.sent[2].playing);
    f.ffb.tick(500'000);                     // quiet stays quiet: no keep-alives
    EXPECT_EQ(f.sent.size(), 3U);

    f.ffb.output(stream(0x2000), 600'000);   // plays again
    ASSERT_EQ(f.sent.size(), 4U);
    EXPECT_TRUE(f.sent[3].playing);
    f.ffb.tick(600'000 + TrueForceFfb::kQuietUs - 1);   // a keep-alive, still playing
    ASSERT_EQ(f.sent.size(), 5U);
    EXPECT_TRUE(f.sent[4].playing);
    f.ffb.tick(600'000 + TrueForceFfb::kQuietUs);       // no stream for kQuietUs
    ASSERT_EQ(f.sent.size(), 6U);
    EXPECT_FALSE(f.sent[5].playing);

    f.ffb.output(stream(0x2000), 900'000);
    f.ffb.stop(901'000);
    ASSERT_EQ(f.sent.size(), 8U);
    EXPECT_FALSE(f.sent[7].playing);
    f.ffb.stop(902'000);                     // already quiet
    EXPECT_EQ(f.sent.size(), 8U);
}

// The game's damping (0x0c) plays as a damper next to the force: damping / 0.3, with the force, quiet with it.
TEST(TrueForceFfb, DampingPlaysAsADamperWithTheForce) {
    Ffb f;
    f.ffb.output(damping(0.27F), 0);         // no force yet: nothing
    EXPECT_TRUE(f.sent.empty());
    f.ffb.output(stream(0x8000), 1000);
    ASSERT_EQ(f.sent.size(), 2U);
    EXPECT_EQ(f.sent[0].slot, TrueForceFfb::kSlot);
    EXPECT_EQ(f.sent[1].slot, TrueForceFfb::kDamperSlot);
    EXPECT_TRUE(f.sent[1].playing);
    EXPECT_EQ(f.sent[1].effect.type, FfbEffectType::Damper);
    EXPECT_NEAR(f.sent[1].effect.condition.positiveCoefficient, 0.9F, 1e-5);
    EXPECT_NEAR(f.sent[1].effect.condition.negativeCoefficient, 0.9F, 1e-5);

    f.ffb.output(damping(0.03F), 2000);      // a change goes out at once
    ASSERT_EQ(f.sent.size(), 3U);
    EXPECT_NEAR(f.sent[2].effect.condition.positiveCoefficient, 0.1F, 1e-5);
    f.ffb.output(damping(9.0F), 3000);       // clamped
    ASSERT_EQ(f.sent.size(), 4U);
    EXPECT_FLOAT_EQ(f.sent[3].effect.condition.positiveCoefficient, 1.0F);

    f.ffb.output(stream(0x8000), 50'000);
    f.ffb.tick(3000 + TrueForceFfb::kKeepAliveUs);   // the force's keep-alive brings the damper's
    ASSERT_EQ(f.sent.size(), 6U);
    EXPECT_EQ(f.sent[4].slot, TrueForceFfb::kSlot);
    EXPECT_EQ(f.sent[5].slot, TrueForceFfb::kDamperSlot);

    f.ffb.tick(50'000 + TrueForceFfb::kQuietUs);   // no stream: both quiet
    ASSERT_EQ(f.sent.size(), 8U);
    EXPECT_FALSE(f.sent[6].playing);
    EXPECT_FALSE(f.sent[7].playing);
    EXPECT_EQ(f.sent[7].slot, TrueForceFfb::kDamperSlot);

    f.ffb.output(stream(0x8000), 300'000);   // plays again, damper too
    ASSERT_EQ(f.sent.size(), 10U);
    f.ffb.output(damping(0.0F), 301'000);    // no damping: the damper stops, the force plays on
    ASSERT_EQ(f.sent.size(), 11U);
    EXPECT_EQ(f.sent[10].slot, TrueForceFfb::kDamperSlot);
    EXPECT_FALSE(f.sent[10].playing);
    EXPECT_TRUE(f.ffb.playing());
}

TEST(TrueForceFfb, OtherWritesChangeNothing) {
    Ffb f;
    f.ffb.output(command(0x03), 0);
    f.ffb.output(command(0x04), 0);          // a stop while quiet
    f.ffb.output(command(0x05), 0);
    f.ffb.output(std::vector<std::uint8_t>{}, 0);
    f.ffb.tick(1'000'000);
    EXPECT_TRUE(f.sent.empty());
}

}  // namespace
}  // namespace wheelbri
