#include "wheelbri/DiEffect.hpp"
#include "wheelbri/FfbController.hpp"
#include "wheelbri/FfbPacket.hpp"
#include "wheelbri/GameProfiles.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <vector>

namespace wheelbri {
namespace {

constexpr std::uint64_t kMs = 1'000'000;

// --- wire format -----------------------------------------------------------------------------------

TEST(FfbPacket, RoundTrip) {
    FfbPacket packet;
    packet.kind = FfbPacket::Kind::SlotState;
    packet.slot = 3;
    packet.sequence = 77;
    packet.playing = true;
    packet.effect.type = FfbEffectType::Damper;
    packet.effect.gain = 0.5F;
    packet.effect.condition.positiveCoefficient = 0.25F;
    packet.effect.condition.negativeSaturation = 0.75F;
    packet.effect.periodic.phaseDeg = 90.0F;

    const auto bytes = encode(packet);
    const auto decoded = decodeFfbPacket(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->kind, FfbPacket::Kind::SlotState);
    EXPECT_EQ(decoded->slot, 3);
    EXPECT_EQ(decoded->sequence, 77U);
    EXPECT_TRUE(decoded->playing);
    EXPECT_EQ(decoded->effect.type, FfbEffectType::Damper);
    EXPECT_FLOAT_EQ(decoded->effect.gain, 0.5F);
    EXPECT_FLOAT_EQ(decoded->effect.condition.positiveCoefficient, 0.25F);
    EXPECT_FLOAT_EQ(decoded->effect.condition.negativeSaturation, 0.75F);
    EXPECT_FLOAT_EQ(decoded->effect.periodic.phaseDeg, 90.0F);
}

TEST(FfbPacket, RejectsGarbage) {
    auto bytes = encode(FfbPacket{});
    EXPECT_FALSE(decodeFfbPacket(std::span<const std::uint8_t>(bytes.data(), 10)).has_value());
    bytes[0] ^= 0xFF;
    EXPECT_FALSE(decodeFfbPacket(bytes).has_value());
    bytes = encode(FfbPacket{});
    bytes[13] = 200;   // effect type out of range
    EXPECT_FALSE(decodeFfbPacket(bytes).has_value());
}

TEST(FfbPacket, NonFiniteNumbersBecomeZero) {
    FfbPacket packet;
    packet.effect.constantLevel = std::numeric_limits<float>::quiet_NaN();
    const auto decoded = decodeFfbPacket(encode(packet));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->effect.constantLevel, 0.0F);
}

// --- DirectInput parameters ------------------------------------------------------------------------

TEST(DiEffect, DirectionAlongX) {
    EXPECT_FLOAT_EQ(di::directionAlongX(di::kCartesian, 1), 1.0F);
    EXPECT_FLOAT_EQ(di::directionAlongX(di::kCartesian, -5), -1.0F);
    EXPECT_FLOAT_EQ(di::directionAlongX(di::kCartesian, 0), 1.0F);
    EXPECT_NEAR(di::directionAlongX(di::kPolar, 9000), 1.0F, 1e-5F);     // east
    EXPECT_NEAR(di::directionAlongX(di::kPolar, 27000), -1.0F, 1e-5F);   // west
    EXPECT_NEAR(di::directionAlongX(di::kPolar, 0), 0.0F, 1e-5F);        // north: nothing on X
    EXPECT_NEAR(di::directionAlongX(di::kSpherical, 18000), -1.0F, 1e-5F);
}

TEST(DiEffect, ConstantForce) {
    const FfbEffect effect = di::constantForce(-5000, -1.0F, 8000);
    EXPECT_EQ(effect.type, FfbEffectType::Constant);
    EXPECT_FLOAT_EQ(effect.constantLevel, 0.5F);
    EXPECT_FLOAT_EQ(effect.gain, 0.8F);
    EXPECT_FLOAT_EQ(di::constantForce(20000, 1.0F, 10000).constantLevel, 1.0F);   // clamped
}

TEST(DiEffect, Condition) {
    const FfbEffect effect = di::condition(FfbEffectType::Spring, 1000, 4000, -2000, 10000, 5000, 500, 10000);
    EXPECT_EQ(effect.type, FfbEffectType::Spring);
    EXPECT_FLOAT_EQ(effect.condition.center, 0.1F);
    EXPECT_FLOAT_EQ(effect.condition.positiveCoefficient, 0.4F);
    EXPECT_FLOAT_EQ(effect.condition.negativeCoefficient, -0.2F);
    EXPECT_FLOAT_EQ(effect.condition.positiveSaturation, 1.0F);
    EXPECT_FLOAT_EQ(effect.condition.negativeSaturation, 0.5F);
    EXPECT_FLOAT_EQ(effect.condition.deadband, 0.05F);
}

TEST(DiEffect, PeriodicReversedByDirection) {
    const FfbEffect effect = di::periodic(FfbEffectType::Sine, 6000, 1000, 9000, 50000, -1.0F, 10000);
    EXPECT_FLOAT_EQ(effect.periodic.magnitude, 0.6F);
    EXPECT_FLOAT_EQ(effect.periodic.offset, -0.1F);
    EXPECT_FLOAT_EQ(effect.periodic.periodMs, 50.0F);
    EXPECT_FLOAT_EQ(effect.periodic.phaseDeg, 270.0F);
}

// --- service controller ----------------------------------------------------------------------------

class RecordingSink final : public IFfbSink {
public:
    bool play(std::size_t slot, const FfbEffect& effect) override {
        plays.push_back({slot, effect});
        return accept;
    }
    void stop(std::size_t slot) override { stops.push_back(slot); }
    void stopAll() override { ++stopAlls; }

    struct Play {
        std::size_t slot;
        FfbEffect effect;
    };
    std::vector<Play> plays;
    std::vector<std::size_t> stops;
    int stopAlls = 0;
    bool accept = true;
};

FfbPacket slotPacket(std::uint8_t slot, bool playing, FfbEffect effect) {
    FfbPacket packet;
    packet.kind = FfbPacket::Kind::SlotState;
    packet.slot = slot;
    packet.playing = playing;
    packet.effect = effect;
    return packet;
}

FfbSafetyConfig safety() {
    FfbSafetyConfig config;
    config.masterGain = 1.0F;
    config.maxMagnitude = 0.5F;
    config.maxSlewPerSecond = 1000.0F;
    return config;
}

TEST(FfbController, PlaysThroughTheLimiter) {
    RecordingSink sink;
    FfbController controller(sink, safety());
    FfbEffect spring;
    spring.type = FfbEffectType::Spring;
    spring.condition.positiveCoefficient = 1.0F;
    spring.condition.negativeCoefficient = 1.0F;
    controller.onPacket(slotPacket(1, true, spring), 0);
    controller.update(0);

    ASSERT_EQ(sink.plays.size(), 1U);
    EXPECT_EQ(sink.plays[0].slot, 1U);
    EXPECT_FLOAT_EQ(sink.plays[0].effect.condition.positiveSaturation, 0.5F);   // capped
    controller.update(10 * kMs);
    EXPECT_EQ(sink.plays.size(), 1U);   // nothing changed, nothing re-sent
}

TEST(FfbController, ConstantForceRampsAndIsRateLimited) {
    RecordingSink sink;
    FfbSafetyConfig config = safety();
    config.maxSlewPerSecond = 10.0F;   // full scale in 100 ms
    FfbController controller(sink, config);
    FfbEffect push;
    push.constantLevel = 1.0F;
    controller.onPacket(slotPacket(0, true, push), 0);
    controller.update(0);
    ASSERT_EQ(sink.plays.size(), 1U);
    EXPECT_FLOAT_EQ(sink.plays[0].effect.constantLevel, 0.0F);   // soft start

    controller.update(1 * kMs);   // inside minUpdate: skipped
    EXPECT_EQ(sink.plays.size(), 1U);
    controller.update(10 * kMs);
    ASSERT_EQ(sink.plays.size(), 2U);
    EXPECT_NEAR(sink.plays[1].effect.constantLevel, 0.1F, 1e-4F);
    controller.update(200 * kMs);
    EXPECT_FLOAT_EQ(sink.plays.back().effect.constantLevel, 0.5F);   // capped at max_magnitude
}

// A game profile's gain (NFS Unbound: 3) makes the pushing forces stronger, never past the cap; conditions stay.
TEST(FfbController, GameGainScalesPushingForcesUpToTheCap) {
    RecordingSink sink;
    FfbController controller(sink, safety());   // gain 1, cap 0.5, fast slew
    controller.setGameGain(3.0F);
    EXPECT_FLOAT_EQ(controller.gameGain(), 3.0F);
    FfbEffect push;
    push.constantLevel = -0.1F;
    FfbEffect rumble;
    rumble.type = FfbEffectType::Triangle;
    rumble.periodic.magnitude = 0.05F;
    rumble.periodic.periodMs = 64.0F;
    FfbEffect damper;
    damper.type = FfbEffectType::Damper;
    damper.condition.positiveCoefficient = 0.25F;
    damper.condition.negativeCoefficient = 0.25F;
    damper.condition.positiveSaturation = 0.25F;
    damper.condition.negativeSaturation = 0.25F;
    controller.onPacket(slotPacket(0, true, push), 0);
    controller.onPacket(slotPacket(1, true, rumble), 0);
    controller.onPacket(slotPacket(2, true, damper), 0);
    controller.update(0);
    controller.update(100 * kMs);
    EXPECT_NEAR(controller.slot(0).sent->constantLevel, -0.3F, 1e-4F);
    EXPECT_NEAR(controller.slot(1).sent->periodic.magnitude, 0.15F, 1e-4F);
    EXPECT_FLOAT_EQ(controller.slot(2).sent->condition.positiveCoefficient, 0.25F);   // untouched
    EXPECT_FLOAT_EQ(controller.slot(0).requested.constantLevel, -0.1F);              // the game's own, for telemetry

    push.constantLevel = 0.4F;   // 1.2 asked: the cap still holds
    controller.onPacket(slotPacket(0, true, push), 110 * kMs);
    controller.update(300 * kMs);
    EXPECT_FLOAT_EQ(controller.slot(0).sent->constantLevel, 0.5F);

    controller.setGameGain(100.0F);   // out of range: clamped
    EXPECT_FLOAT_EQ(controller.gameGain(), kFfbGainMax);
    controller.setGameGain(1.0F);     // the game ended: as asked again
    controller.update(400 * kMs);
    EXPECT_FLOAT_EQ(controller.slot(0).sent->constantLevel, 0.4F);
}

TEST(FfbController, StopsWhenTheGameSaysSo) {
    RecordingSink sink;
    FfbController controller(sink, safety());
    FfbEffect damper;
    damper.type = FfbEffectType::Damper;
    controller.onPacket(slotPacket(2, true, damper), 0);
    controller.update(0);
    controller.onPacket(slotPacket(2, false, damper), 5 * kMs);
    controller.update(5 * kMs);
    ASSERT_EQ(sink.stops.size(), 1U);
    EXPECT_EQ(sink.stops[0], 2U);

    controller.onPacket(slotPacket(2, true, damper), 6 * kMs);
    controller.update(6 * kMs);
    FfbPacket stopAll;
    stopAll.kind = FfbPacket::Kind::StopAll;
    controller.onPacket(stopAll, 7 * kMs);
    controller.update(7 * kMs);
    EXPECT_EQ(sink.stops.size(), 2U);
}

TEST(FfbController, WatchdogStopsEverythingWhenTheGameGoesQuiet) {
    RecordingSink sink;
    FfbController controller(sink, safety(), {100 * kMs, 4 * kMs});
    FfbEffect push;
    push.constantLevel = 0.3F;
    controller.onPacket(slotPacket(0, true, push), 0);
    controller.update(0);
    FfbPacket keepAlive;
    controller.onPacket(keepAlive, 90 * kMs);
    controller.update(150 * kMs);
    EXPECT_TRUE(sink.stops.empty());   // keep-alive at 90 ms keeps it going

    controller.update(191 * kMs);
    ASSERT_EQ(sink.stops.size(), 1U);
    EXPECT_EQ(controller.stats().watchdogStops, 1U);

    controller.update(300 * kMs);   // stays stopped without new packets
    EXPECT_EQ(sink.stops.size(), 1U);
}

// After the watchdog stopped everything, the game's next packets bring the forces back.
TEST(FfbController, ForcesComeBackWhenTheGameSendsAgain) {
    RecordingSink sink;
    FfbController controller(sink, safety(), {100 * kMs, 4 * kMs});
    FfbEffect push;
    push.constantLevel = 0.3F;
    controller.onPacket(slotPacket(0, true, push), 0);
    controller.update(0);
    controller.update(150 * kMs);
    ASSERT_TRUE(controller.watchdogTripped());
    EXPECT_FALSE(controller.slot(0).playing);
    controller.onPacket(slotPacket(0, true, push), 200 * kMs);
    EXPECT_FALSE(controller.watchdogTripped());
    controller.update(200 * kMs);
    EXPECT_TRUE(controller.slot(0).playing);
}

// The wheel refusing an effect: counted, and not retried every tick (the game sends it again if it cares).
TEST(FfbController, AnEffectTheWheelRefusesIsNotRetriedEveryTick) {
    RecordingSink sink;
    sink.accept = false;
    FfbController controller(sink, safety());
    FfbEffect push;
    push.constantLevel = 0.3F;
    controller.onPacket(slotPacket(0, true, push), 0);
    controller.update(0);
    controller.update(10 * kMs);
    controller.update(20 * kMs);
    EXPECT_EQ(sink.plays.size(), 1U);
    EXPECT_EQ(controller.stats().playFailures, 1U);
    EXPECT_FALSE(controller.slot(0).playing);
}

TEST(FfbController, StopAllEndsEverythingAtOnce) {
    RecordingSink sink;
    FfbController controller(sink, safety());
    FfbEffect push;
    push.constantLevel = 0.3F;
    controller.onPacket(slotPacket(0, true, push), 0);
    controller.onPacket(slotPacket(3, true, push), 0);
    controller.update(0);
    controller.stopAll();
    EXPECT_EQ(sink.stopAlls, 1);
    EXPECT_FALSE(controller.slot(0).playing);
    EXPECT_FALSE(controller.slot(3).wanted);
    controller.update(10 * kMs);
    EXPECT_EQ(sink.plays.size(), 2U);   // nothing started again
}

// A game gain that is not a number is the game's forces as asked.
TEST(FfbController, NonFiniteGameGainIsOne) {
    RecordingSink sink;
    FfbController controller(sink, safety());
    controller.setGameGain(3.0F);
    controller.setGameGain(std::numeric_limits<float>::quiet_NaN());
    EXPECT_FLOAT_EQ(controller.gameGain(), 1.0F);
    controller.setGameGain(-2.0F);
    EXPECT_FLOAT_EQ(controller.gameGain(), kFfbGainMin);
}

TEST(FfbController, OutOfRangeSlotIsIgnored) {
    RecordingSink sink;
    FfbController controller(sink, safety());
    controller.onPacket(slotPacket(200, true, FfbEffect{}), 0);
    controller.update(0);
    EXPECT_TRUE(sink.plays.empty());
}

TEST(FfbController, ATypeChangeOnAPlayingSlotStartsSoftAgain) {
    RecordingSink sink;
    FfbSafetyConfig config = safety();
    config.maxSlewPerSecond = 10.0F;
    FfbController controller(sink, config);
    FfbEffect push;
    push.constantLevel = 1.0F;
    controller.onPacket(slotPacket(0, true, push), 0);
    controller.update(0);
    controller.update(200 * kMs);
    ASSERT_FLOAT_EQ(sink.plays.back().effect.constantLevel, 0.5F);   // ramped up to the cap
    FfbEffect sine;
    sine.type = FfbEffectType::Sine;
    controller.onPacket(slotPacket(0, true, sine), 210 * kMs);
    controller.update(210 * kMs);
    controller.onPacket(slotPacket(0, true, push), 220 * kMs);       // constant again, on the same slot
    controller.update(220 * kMs);
    EXPECT_FLOAT_EQ(sink.plays.back().effect.constantLevel, 0.0F);   // from zero, not from where it was
}

TEST(FfbController, SlotViewOutOfRangeIsEmpty) {
    RecordingSink sink;
    FfbController controller(sink, safety());
    const FfbController::SlotView view = controller.slot(controller.slotCount() + 5);
    EXPECT_FALSE(view.wanted);
    EXPECT_FALSE(view.playing);
    EXPECT_FALSE(view.sent.has_value());
}

TEST(FfbController, CountsTheLastingEffectsLoadedAndWanted) {
    RecordingSink sink;
    FfbController controller(sink, safety(), {100 * kMs, 4 * kMs});
    EXPECT_EQ(controller.lastingLoaded(), 0U);
    FfbEffect constant;   // Constant by default
    FfbEffect spring;
    spring.type = FfbEffectType::Spring;
    FfbEffect sine;
    sine.type = FfbEffectType::Sine;
    // NFS 2015 at the start of a race: everything created, nothing started.
    controller.onPacket(slotPacket(0, false, constant), 0);
    controller.onPacket(slotPacket(1, false, spring), 0);
    controller.onPacket(slotPacket(2, true, sine), 0);   // a short effect playing does not count
    EXPECT_EQ(controller.lastingLoaded(), 2U);
    EXPECT_EQ(controller.lastingWanted(), 0U);
    EXPECT_TRUE(controller.slot(1).loaded);
    EXPECT_FALSE(controller.slot(3).loaded);
    controller.onPacket(slotPacket(1, true, spring), 1 * kMs);
    EXPECT_EQ(controller.lastingWanted(), 1U);

    FfbPacket stopAll;
    stopAll.kind = FfbPacket::Kind::StopAll;
    controller.onPacket(stopAll, 2 * kMs);   // stop all / reset: a new set of effects comes next
    EXPECT_EQ(controller.lastingLoaded(), 0U);
    EXPECT_EQ(controller.lastingWanted(), 0U);

    controller.onPacket(slotPacket(4, false, spring), 3 * kMs);
    EXPECT_EQ(controller.lastingLoaded(), 1U);
    controller.update(200 * kMs);   // the game went quiet: nothing is known any more
    EXPECT_TRUE(controller.watchdogTripped());
    EXPECT_EQ(controller.lastingLoaded(), 0U);
}

TEST(FfbController, OnlyConstantAndConditionsAreLasting) {
    EXPECT_TRUE(FfbController::lasting(FfbEffectType::Constant));
    EXPECT_TRUE(FfbController::lasting(FfbEffectType::Spring));
    EXPECT_TRUE(FfbController::lasting(FfbEffectType::Damper));
    EXPECT_TRUE(FfbController::lasting(FfbEffectType::Friction));
    EXPECT_TRUE(FfbController::lasting(FfbEffectType::Inertia));
    EXPECT_FALSE(FfbController::lasting(FfbEffectType::Sine));
    EXPECT_FALSE(FfbController::lasting(FfbEffectType::Square));
    EXPECT_FALSE(FfbController::lasting(FfbEffectType::Triangle));
    EXPECT_FALSE(FfbController::lasting(FfbEffectType::SawtoothUp));
    EXPECT_FALSE(FfbController::lasting(FfbEffectType::SawtoothDown));
    EXPECT_FALSE(FfbController::lasting(static_cast<FfbEffectType>(200)));
}

TEST(FfbController, CopyingKeepsTheState) {
    RecordingSink sink;
    FfbController controller(sink, safety());
    FfbEffect push;
    push.constantLevel = 0.2F;
    controller.onPacket(slotPacket(3, true, push), 0);
    controller.update(0);
    const FfbController copy(controller);   // the window's frame
    EXPECT_TRUE(copy.slot(3).playing);
    EXPECT_EQ(copy.stats().plays, controller.stats().plays);
}

}  // namespace
}  // namespace wheelbri
