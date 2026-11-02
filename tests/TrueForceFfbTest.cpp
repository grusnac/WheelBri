#include "wheelbri/TrueForceFfb.hpp"

#include "wheelbri/TrueForce.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
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

// A stream packet at no torque whose window ends in `fresh` samples (oldest first), each as both its words:
// 0x8000 - value * 32768, so a value of +1 is 0x0000 as the torque's.
std::vector<std::uint8_t> textured(const std::vector<float>& fresh) {
    std::vector<std::uint8_t> r = stream(0x8000);
    r[10] = static_cast<std::uint8_t>(fresh.size());
    r[11] = 0x0D;
    for (std::size_t slot = 0; slot < 13; ++slot) {   // older slots: no texture
        r[12 + slot * 4] = r[14 + slot * 4] = 0x00;
        r[13 + slot * 4] = r[15 + slot * 4] = 0x80;
    }
    for (std::size_t i = 0; i < fresh.size(); ++i) {
        const auto raw = static_cast<std::uint16_t>(std::lround(0x8000 - fresh[i] * 32768.0F));
        const std::size_t at = 12 + (13 - fresh.size() + i) * 4;
        r[at] = r[at + 2] = static_cast<std::uint8_t>(raw);
        r[at + 1] = r[at + 3] = static_cast<std::uint8_t>(raw >> 8);
    }
    return r;
}

// `ms` packets of a sine of `amplitude` at `hz`, 4 samples a packet, one packet a millisecond from `startUs`.
template <typename Ffb>
void playSine(Ffb& f, const float amplitude, const float hz, const int ms, const std::int64_t startUs = 0) {
    constexpr float kPi = 3.14159265F;
    for (int p = 0; p < ms; ++p) {
        std::vector<float> fresh;
        for (int k = 0; k < 4; ++k) {
            const float t = static_cast<float>(p * 4 + k) / TrueForceFfb::kTextureSampleRateHz;
            fresh.push_back(amplitude * std::sin(2.0F * kPi * hz * t));
        }
        f.ffb.output(textured(fresh), startUs + p * 1000);
    }
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

// The window's new samples, oldest first, each the mean of its two words in the torque's sign; none while it is not
// valid (byte 11) or the packet is not the stream.
TEST(TrueForceFfb, TextureSamplesFromTheWindowsEnd) {
    std::array<float, 13> out{};
    auto r = textured({0.5F, -0.25F, 0.0F});
    ASSERT_EQ(TrueForceFfb::texture(r, out), 3U);
    EXPECT_FLOAT_EQ(out[0], 0.5F);
    EXPECT_FLOAT_EQ(out[1], -0.25F);
    EXPECT_FLOAT_EQ(out[2], 0.0F);
    r[12 + 12 * 4 + 1] = 0x90;   // the newest's first word 0x9000, its second 0x8000: their mean, -1/16
    ASSERT_EQ(TrueForceFfb::texture(r, out), 3U);
    EXPECT_FLOAT_EQ(out[2], -0.0625F);
    std::array<float, 2> small{};
    EXPECT_EQ(TrueForceFfb::texture(r, small), 2U);   // no more than fits
    r[11] = 0x00;
    EXPECT_EQ(TrueForceFfb::texture(r, out), 0U);
    r = textured({0.5F});
    r[10] = 40;   // more than the window has: its 13
    EXPECT_EQ(TrueForceFfb::texture(r, out), 13U);
    EXPECT_EQ(TrueForceFfb::texture(command(0x05), out), 0U);
    EXPECT_EQ(TrueForceFfb::texture(std::vector<std::uint8_t>(20, 0x01), out), 0U);   // too short
}

// A 250 Hz texture of 0.05 becomes a sine in its own slot, after one window (40 ms), with the force still playing.
TEST(TrueForceFfb, TexturePlaysAsASineOfItsSizeAndFrequency) {
    Ffb f;
    playSine(f, 0.05F, 250.0F, 39);
    for (const FfbPacket& p : f.sent) EXPECT_NE(p.slot, TrueForceFfb::kTextureSlot);   // not a window yet
    playSine(f, 0.05F, 250.0F, 1, 39'000);
    ASSERT_FALSE(f.sent.empty());
    const FfbPacket& t = f.sent.back();
    EXPECT_EQ(t.slot, TrueForceFfb::kTextureSlot);
    EXPECT_EQ(t.kind, FfbPacket::Kind::SlotState);
    EXPECT_TRUE(t.playing);
    EXPECT_EQ(t.effect.type, FfbEffectType::Sine);
    EXPECT_NEAR(t.effect.periodic.magnitude, 0.05F, 0.003F);
    EXPECT_NEAR(t.effect.periodic.periodMs, 4.0F, 0.4F);
    EXPECT_FLOAT_EQ(t.effect.periodic.offset, 0.0F);
    EXPECT_TRUE(f.ffb.playing());
}

TEST(TrueForceFfb, TextureFrequencyFollowsTheSignal) {
    Ffb f;
    playSine(f, 0.05F, 100.0F, 40);
    ASSERT_EQ(f.sent.back().slot, TrueForceFfb::kTextureSlot);
    EXPECT_NEAR(f.sent.back().effect.periodic.periodMs, 10.0F, 1.0F);
}

TEST(TrueForceFfb, TextureIsCappedAndKeptInItsRange) {
    Ffb f;
    playSine(f, 0.9F, 1000.0F, 40);   // too strong, and faster than the rig is sent: the fastest period
    ASSERT_EQ(f.sent.back().slot, TrueForceFfb::kTextureSlot);
    EXPECT_FLOAT_EQ(f.sent.back().effect.periodic.magnitude, TrueForceFfb::kTextureMax);
    EXPECT_GE(f.sent.back().effect.periodic.periodMs, TrueForceFfb::kTextureMinPeriodMs);
    Ffb slow;
    playSine(slow, 0.05F, 2.0F, 40);   // a 2 Hz swell crosses zero once in 40 ms: the slowest period
    ASSERT_EQ(slow.sent.back().slot, TrueForceFfb::kTextureSlot);
    EXPECT_FLOAT_EQ(slow.sent.back().effect.periodic.periodMs, TrueForceFfb::kTextureMaxPeriodMs);
}

TEST(TrueForceFfb, AFaintTextureIsNoneAndAFadingOneStops) {
    Ffb f;
    playSine(f, 0.001F, 250.0F, 80);
    for (const FfbPacket& p : f.sent) EXPECT_NE(p.slot, TrueForceFfb::kTextureSlot);
    playSine(f, 0.05F, 250.0F, 40, 80'000);
    ASSERT_EQ(f.sent.back().slot, TrueForceFfb::kTextureSlot);
    ASSERT_TRUE(f.sent.back().playing);
    playSine(f, 0.0F, 250.0F, 40, 120'000);   // a smooth road
    EXPECT_EQ(f.sent.back().slot, TrueForceFfb::kTextureSlot);
    EXPECT_FALSE(f.sent.back().playing);
    EXPECT_TRUE(f.ffb.playing());             // the force plays on
}

// Small changes are not sent again (a periodic effect sent anew may restart its phase); a big one is, and a playing
// texture again every kKeepAliveUs with the force's keep-alives.
TEST(TrueForceFfb, TextureIsSentWhenItChangesEnoughAndKeptAlive) {
    Ffb f;
    const auto textures = [&f] {
        std::size_t n = 0;
        for (const FfbPacket& p : f.sent) n += p.slot == TrueForceFfb::kTextureSlot ? 1 : 0;
        return n;
    };
    playSine(f, 0.05F, 250.0F, 40);
    ASSERT_EQ(textures(), 1U);
    playSine(f, 0.052F, 250.0F, 40, 40'000);   // 4% more: the same to feel
    EXPECT_EQ(textures(), 1U);
    playSine(f, 0.08F, 250.0F, 40, 80'000);    // 60% more: sent
    ASSERT_EQ(textures(), 2U);
    EXPECT_NEAR(f.sent.back().effect.periodic.magnitude, 0.08F, 0.005F);
    playSine(f, 0.08F, 125.0F, 40, 120'000);   // half the frequency: sent
    ASSERT_EQ(textures(), 3U);
    EXPECT_NEAR(f.sent.back().effect.periodic.periodMs, 8.0F, 0.8F);
    playSine(f, 0.08F, 125.0F, 120, 160'000);  // the same for more than kKeepAliveUs: sent again
    EXPECT_GE(textures(), 4U);
}

TEST(TrueForceFfb, TextureStopsWithAStopAndWhenTheStreamGoesQuiet) {
    Ffb f;
    playSine(f, 0.05F, 250.0F, 40);
    ASSERT_EQ(f.sent.back().slot, TrueForceFfb::kTextureSlot);
    f.ffb.output(command(0x04), 40'000);
    ASSERT_EQ(f.sent.back().slot, TrueForceFfb::kTextureSlot);
    EXPECT_FALSE(f.sent.back().playing);
    EXPECT_FLOAT_EQ(f.sent.back().effect.periodic.magnitude, 0.0F);

    playSine(f, 0.05F, 250.0F, 40, 50'000);
    ASSERT_TRUE(f.sent.back().playing);
    f.ffb.tick(90'000 + TrueForceFfb::kQuietUs);   // no stream: the force and its texture go quiet
    bool textureStopped = false;
    for (const FfbPacket& p : f.sent) {
        if (p.slot == TrueForceFfb::kTextureSlot && !p.playing) textureStopped = true;
    }
    EXPECT_TRUE(textureStopped);
    EXPECT_FALSE(f.sent.back().playing);
    EXPECT_FALSE(f.ffb.playing());

    std::size_t before = f.sent.size();
    f.ffb.stop(400'000);   // already quiet: nothing more
    EXPECT_EQ(f.sent.size(), before);
    playSine(f, 0.05F, 250.0F, 40, 500'000);
    f.ffb.stop(540'000);   // the interface closed: the texture stops too
    bool stoppedAgain = false;
    for (std::size_t i = before; i < f.sent.size(); ++i) {
        if (f.sent[i].slot == TrueForceFfb::kTextureSlot && !f.sent[i].playing) stoppedAgain = true;
    }
    EXPECT_TRUE(stoppedAgain);
}

}  // namespace
}  // namespace wheelbri
