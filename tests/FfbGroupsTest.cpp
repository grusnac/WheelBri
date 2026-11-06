// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/FfbGroups.hpp"

#include "wheelbri/FfbController.hpp"
#include "wheelbri/FfbPacket.hpp"

#include <gtest/gtest.h>

namespace wheelbri {
namespace {

class NullSink final : public IFfbSink {
public:
    bool play(std::size_t /*slot*/, const FfbEffect& /*effect*/) override { return true; }
    void stop(std::size_t /*slot*/) override {}
    void stopAll() override {}
};

FfbPacket slotPacket(const std::uint8_t slot, const bool playing, const FfbEffect& effect) {
    FfbPacket packet;
    packet.kind = FfbPacket::Kind::SlotState;
    packet.slot = slot;
    packet.playing = playing;
    packet.effect = effect;
    return packet;
}

TEST(FfbGroups, EffectTypesFallIntoGroups) {
    EXPECT_EQ(ffbGroupOf(FfbEffectType::Constant), FfbGroup::Constant);
    EXPECT_EQ(ffbGroupOf(FfbEffectType::Sine), FfbGroup::Periodic);
    EXPECT_EQ(ffbGroupOf(FfbEffectType::SawtoothDown), FfbGroup::Periodic);
    EXPECT_EQ(ffbGroupOf(FfbEffectType::Spring), FfbGroup::Spring);
    EXPECT_EQ(ffbGroupOf(FfbEffectType::Inertia), FfbGroup::Inertia);
    for (const std::string_view name : kFfbGroupNames) EXPECT_LE(name.size(), 20U);
}

// Per kind: the strongest playing effect the game asks and the strongest the wheel gets; the first update of a push
// is held back by the slew limit, so the wheel gets less than asked.
TEST(FfbGroups, LevelsOfAskedAndSent) {
    NullSink sink;
    FfbController controller(sink, FfbSafetyConfig{});
    EXPECT_EQ(ffbGroupLevels(controller).asked, (std::array<float, kFfbGroupCount>{}));
    FfbEffect push;
    push.type = FfbEffectType::Constant;
    push.constantLevel = -0.6F;
    FfbEffect spring;
    spring.type = FfbEffectType::Spring;
    spring.condition.positiveCoefficient = 0.4F;
    spring.condition.negativeCoefficient = 0.4F;
    spring.condition.positiveSaturation = 1.0F;
    spring.condition.negativeSaturation = 1.0F;
    controller.onPacket(slotPacket(0, true, push), 0);
    controller.onPacket(slotPacket(1, true, spring), 0);
    controller.update(1'000'000);
    controller.update(6'000'000);   // 5 ms later: a quarter of full scale at 50/s
    const FfbGroupLevels levels = ffbGroupLevels(controller);
    const auto constant = static_cast<std::size_t>(FfbGroup::Constant);
    const auto springs = static_cast<std::size_t>(FfbGroup::Spring);
    EXPECT_NEAR(levels.asked[constant], 0.6F, 1e-4F);   // its size, either way
    EXPECT_GT(levels.sent[constant], 0.0F);
    EXPECT_LT(levels.sent[constant], levels.asked[constant]);   // slew: on its way up
    EXPECT_GT(levels.asked[springs], 0.0F);
    EXPECT_EQ(levels.asked[static_cast<std::size_t>(FfbGroup::Periodic)], 0.0F);
    // Stopped: nothing.
    controller.onPacket(slotPacket(0, false, push), 2'000'000);
    controller.onPacket(slotPacket(1, false, spring), 2'000'000);
    controller.update(3'000'000);
    EXPECT_EQ(ffbGroupLevels(controller).asked, (std::array<float, kFfbGroupCount>{}));
}

}  // namespace
}  // namespace wheelbri
