#include "wheelbri/DiEffect.hpp"
#include "wheelbri/FfbController.hpp"
#include "wheelbri/FfbPacket.hpp"
#include "wheelbri/GameProfiles.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
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

// A packet of another version of the wire format (an older shim in a game) is not read as this one.
TEST(FfbPacket, RejectsAnotherVersionAndUnknownKinds) {
    auto bytes = encode(FfbPacket{});
    bytes[4] = static_cast<std::uint8_t>(FfbPacket::kVersion + 1);
    EXPECT_FALSE(decodeFfbPacket(bytes).has_value());
    bytes = encode(FfbPacket{});
    bytes[6] = 3;   // one past StopAll
    EXPECT_FALSE(decodeFfbPacket(bytes).has_value());
    bytes[6] = 2;
    ASSERT_TRUE(decodeFfbPacket(bytes).has_value());
    EXPECT_EQ(decodeFfbPacket(bytes)->kind, FfbPacket::Kind::StopAll);
    bytes = encode(FfbPacket{});
    std::vector<std::uint8_t> longer(bytes.begin(), bytes.end());
    longer.push_back(0);   // one byte too many
    EXPECT_FALSE(decodeFfbPacket(longer).has_value());
}

// Every field of every effect type survives the trip; infinities become 0 like NaN does.
TEST(FfbPacket, EveryFieldRoundTripsAndInfinitiesBecomeZero) {
    FfbPacket packet;
    packet.kind = FfbPacket::Kind::SlotState;
    packet.slot = 7;
    packet.sequence = 0xFFFFFFFFU;
    packet.effect.type = FfbEffectType::SawtoothDown;
    packet.effect.gain = 0.9F;
    packet.effect.constantLevel = -0.3F;
    packet.effect.condition = {0.1F, 0.2F, 0.3F, -0.4F, 0.5F, 0.6F};
    packet.effect.periodic = {0.7F, -0.8F, 120.0F, 359.0F};
    const auto decoded = decodeFfbPacket(encode(packet));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->sequence, 0xFFFFFFFFU);
    EXPECT_FALSE(decoded->playing);
    EXPECT_EQ(decoded->effect, packet.effect);

    packet.effect.periodic.magnitude = std::numeric_limits<float>::infinity();
    packet.effect.condition.center = -std::numeric_limits<float>::infinity();
    const auto infinite = decodeFfbPacket(encode(packet));
    ASSERT_TRUE(infinite.has_value());
    EXPECT_EQ(infinite->effect.periodic.magnitude, 0.0F);
    EXPECT_EQ(infinite->effect.condition.center, 0.0F);
}

// An effect without any coordinate flag (rglDirection unused): all of it acts on the wheel.
TEST(DiEffect, NoCoordinateSystemIsAllAlongX) {
    EXPECT_FLOAT_EQ(di::directionAlongX(0, 0), 1.0F);
    EXPECT_FLOAT_EQ(di::directionAlongX(0, 18000), 1.0F);
    EXPECT_NEAR(di::directionAlongX(di::kSpherical, 0), 1.0F, 1e-5F);
    EXPECT_NEAR(di::directionAlongX(di::kPolar, 4500), 0.70710678F, 1e-5F);   // north-east: part of it
}

// A periodic effect pointing right keeps its phase; phases of a turn or more wrap; a direction beyond -1..1 is
// clamped (not a stronger wave); one across the wheel (0) is no wave.
TEST(DiEffect, PeriodicDirectionAndPhase) {
    const FfbEffect right = di::periodic(FfbEffectType::Square, 5000, -2000, 9000, 20000, 1.0F, 5000);
    EXPECT_FLOAT_EQ(right.periodic.magnitude, 0.5F);
    EXPECT_FLOAT_EQ(right.periodic.offset, -0.2F);
    EXPECT_FLOAT_EQ(right.periodic.phaseDeg, 90.0F);
    EXPECT_FLOAT_EQ(right.periodic.periodMs, 20.0F);
    EXPECT_FLOAT_EQ(right.gain, 0.5F);
    EXPECT_EQ(right.type, FfbEffectType::Square);
    EXPECT_FLOAT_EQ(di::periodic(FfbEffectType::Sine, 5000, 0, 45000, 1000, 1.0F, 10000).periodic.phaseDeg, 90.0F);
    EXPECT_FLOAT_EQ(di::periodic(FfbEffectType::Sine, 5000, 0, 27000, 1000, -1.0F, 10000).periodic.phaseDeg, 90.0F);
    const FfbEffect strong = di::periodic(FfbEffectType::Sine, 20000, 30000, 0, 1000, 3.0F, 20000);
    EXPECT_FLOAT_EQ(strong.periodic.magnitude, 1.0F);
    EXPECT_FLOAT_EQ(strong.periodic.offset, 1.0F);
    EXPECT_FLOAT_EQ(strong.gain, 1.0F);
    const FfbEffect across = di::periodic(FfbEffectType::Sine, 8000, 1000, 0, 1000, 0.0F, 10000);
    EXPECT_FLOAT_EQ(across.periodic.magnitude, 0.0F);
    EXPECT_FLOAT_EQ(across.periodic.offset, 0.0F);
}

// DICONDITION values out of DirectInput's ranges are clamped: a negative dead band is none, one past 10000 is all.
TEST(DiEffect, ConditionValuesOutOfRangeAreClamped) {
    const FfbEffect wild =
        di::condition(FfbEffectType::Friction, -30000, 20000, -20000, 40000, 40000, -500, 40000U);
    EXPECT_EQ(wild.type, FfbEffectType::Friction);
    EXPECT_FLOAT_EQ(wild.condition.center, -1.0F);
    EXPECT_FLOAT_EQ(wild.condition.positiveCoefficient, 1.0F);
    EXPECT_FLOAT_EQ(wild.condition.negativeCoefficient, -1.0F);
    EXPECT_FLOAT_EQ(wild.condition.positiveSaturation, 1.0F);
    EXPECT_FLOAT_EQ(wild.condition.negativeSaturation, 1.0F);
    EXPECT_FLOAT_EQ(wild.condition.deadband, 0.0F);
    EXPECT_FLOAT_EQ(wild.gain, 1.0F);
    EXPECT_FLOAT_EQ(di::condition(FfbEffectType::Inertia, 0, 0, 0, 0, 0, 20000, 0).condition.deadband, 1.0F);
}

// GRID (2008) passes lMagnitude = -2147483648 while racing: garbage, not a force. It and any value far outside
// DirectInput's ranges give no force instead of a full push; a little over full scale is still clamped.
TEST(DiEffect, ValuesFarOutOfRangeGiveNoForce) {
    constexpr auto kMin = std::numeric_limits<std::int32_t>::min();
    constexpr auto kMax = std::numeric_limits<std::int32_t>::max();
    EXPECT_TRUE(di::isGarbage(kMin));
    EXPECT_TRUE(di::isGarbage(100001));
    EXPECT_FALSE(di::isGarbage(-100000));
    EXPECT_FALSE(di::isGarbage(10000));
    EXPECT_FLOAT_EQ(di::constantForce(kMin, 1.0F, 10000).constantLevel, 0.0F);
    EXPECT_FLOAT_EQ(di::constantForce(kMin, -1.0F, 10000).constantLevel, 0.0F);
    EXPECT_FLOAT_EQ(di::constantForce(kMax, 1.0F, 10000).constantLevel, 0.0F);
    EXPECT_FLOAT_EQ(di::constantForce(-12000, 1.0F, 10000).constantLevel, -1.0F);
    EXPECT_FLOAT_EQ(di::constantForce(5000, 1.0F, 0xFFFFFFFFU).gain, 0.0F);
    const FfbEffect wave = di::periodic(FfbEffectType::Sine, 0x80000000U, kMin, 0, 100000, 1.0F, 10000);
    EXPECT_FLOAT_EQ(wave.periodic.magnitude, 0.0F);
    EXPECT_FLOAT_EQ(wave.periodic.offset, 0.0F);
    const FfbEffect spring = di::condition(FfbEffectType::Spring, kMin, kMax, kMin, 0xFFFFFFFFU, 10000, 0, 10000);
    EXPECT_FLOAT_EQ(spring.condition.center, 0.0F);
    EXPECT_FLOAT_EQ(spring.condition.positiveCoefficient, 0.0F);
    EXPECT_FLOAT_EQ(spring.condition.negativeCoefficient, 0.0F);
    EXPECT_FLOAT_EQ(spring.condition.positiveSaturation, 0.0F);
}

// A game changing any one parameter of an effect while it plays (a spring's centre, a sine's period): the change
// reaches the wheel, not only changes of the level.
TEST(FfbController, AChangeToAnyOneParameterReachesTheWheel) {
    RecordingSink sink;
    FfbController controller(sink, safety());
    FfbEffect spring;
    spring.type = FfbEffectType::Spring;
    spring.condition = {0.0F, 0.1F, 0.3F, 0.3F, 0.2F, 0.2F};
    std::uint64_t now = 0;
    controller.onPacket(slotPacket(0, true, spring), now);
    controller.update(now);
    const auto changeAndCount = [&](FfbEffect& effect, float& field, const float value) {
        field = value;
        now += 10 * kMs;
        controller.onPacket(slotPacket(0, true, effect), now);
        const std::size_t before = sink.plays.size();
        controller.update(now);
        return sink.plays.size() - before;
    };
    EXPECT_EQ(changeAndCount(spring, spring.condition.center, 0.2F), 1U);
    EXPECT_EQ(changeAndCount(spring, spring.condition.deadband, 0.3F), 1U);
    EXPECT_EQ(changeAndCount(spring, spring.condition.positiveCoefficient, 0.5F), 1U);
    EXPECT_EQ(changeAndCount(spring, spring.condition.negativeCoefficient, 0.6F), 1U);
    EXPECT_EQ(changeAndCount(spring, spring.condition.positiveSaturation, 0.3F), 1U);
    EXPECT_EQ(changeAndCount(spring, spring.condition.negativeSaturation, 0.4F), 1U);
    EXPECT_EQ(changeAndCount(spring, spring.condition.negativeSaturation, 0.4F), 0U);   // the same again: not re-sent
    EXPECT_FLOAT_EQ(sink.plays.back().effect.condition.center, 0.2F);
    EXPECT_FLOAT_EQ(sink.plays.back().effect.condition.negativeSaturation, 0.4F);

    FfbEffect sine;
    sine.type = FfbEffectType::Sine;
    sine.periodic = {0.2F, 0.0F, 100.0F, 0.0F};
    controller.onPacket(slotPacket(0, true, sine), now);
    controller.update(now);   // another type: sent at once
    EXPECT_EQ(changeAndCount(sine, sine.periodic.magnitude, 0.3F), 1U);
    EXPECT_EQ(changeAndCount(sine, sine.periodic.offset, 0.1F), 1U);
    EXPECT_EQ(changeAndCount(sine, sine.periodic.periodMs, 50.0F), 1U);
    EXPECT_EQ(changeAndCount(sine, sine.periodic.phaseDeg, 90.0F), 1U);
    EXPECT_EQ(changeAndCount(sine, sine.periodic.phaseDeg, 90.0F), 0U);
    EXPECT_FLOAT_EQ(sink.plays.back().effect.periodic.phaseDeg, 90.0F);
}

// A playing effect replaced by one of another kind is sent at once, even inside the minimum update interval, and
// the new one is limited on its own (a sine after a spring gets the cap on its magnitude).
TEST(FfbController, ATypeChangeIsSentAtOnce) {
    RecordingSink sink;
    FfbController controller(sink, safety());   // minimum update 4 ms
    FfbEffect spring;
    spring.type = FfbEffectType::Spring;
    spring.condition.positiveCoefficient = 0.5F;
    controller.onPacket(slotPacket(1, true, spring), 0);
    controller.update(0);
    FfbEffect square;
    square.type = FfbEffectType::Square;
    square.periodic = {0.9F, 0.0F, 40.0F, 0.0F};
    controller.onPacket(slotPacket(1, true, square), 1 * kMs);
    controller.update(1 * kMs);
    ASSERT_EQ(sink.plays.size(), 2U);
    EXPECT_EQ(sink.plays[1].effect.type, FfbEffectType::Square);
    EXPECT_FLOAT_EQ(sink.plays[1].effect.periodic.magnitude, 0.5F);
    EXPECT_TRUE(sink.stops.empty());   // replaced, not stopped first
}

// Before the game ever sent anything: nothing plays, the watchdog has nothing to watch, no game time is counted.
TEST(FfbController, NothingHappensBeforeTheFirstPacket) {
    RecordingSink sink;
    FfbController controller(sink, safety(), {100 * kMs, 4 * kMs});
    controller.update(0);
    controller.update(10'000 * kMs);
    EXPECT_TRUE(sink.plays.empty());
    EXPECT_TRUE(sink.stops.empty());
    EXPECT_FALSE(controller.watchdogTripped());
    EXPECT_FALSE(controller.lastPacketNs().has_value());
    EXPECT_EQ(controller.usage().gameNs(), 0U);
}

// Two updates at the same moment (the loop ran twice within the clock's resolution) count no extra play time.
TEST(FfbController, UpdatesAtTheSameMomentCountNoTime) {
    RecordingSink sink;
    FfbController controller(sink, safety());
    FfbEffect push;
    push.constantLevel = 0.2F;
    controller.onPacket(slotPacket(0, true, push), 5 * kMs);
    controller.update(5 * kMs);
    controller.update(6 * kMs);
    controller.update(6 * kMs);
    EXPECT_EQ(controller.usage().type(FfbEffectType::Constant).playNs, 1 * kMs);
}

// Setting the gain a game profile already has changes nothing (the service sets it every tick).
TEST(FfbController, TheSameGameGainAgainChangesNothing) {
    RecordingSink sink;
    FfbController controller(sink, safety());
    controller.setGameGain(2.0F);
    controller.setGameGain(2.0F);
    EXPECT_FLOAT_EQ(controller.gameGain(), 2.0F);
}

// The game gain leaves an effect of a type this build does not know as it is (the limiter then gives it no force).
TEST(FfbController, GameGainLeavesAnUnknownTypeAlone) {
    FfbEffect unknown;
    unknown.type = static_cast<FfbEffectType>(42);
    unknown.constantLevel = 0.3F;
    unknown.periodic.magnitude = 0.3F;
    EXPECT_EQ(FfbController::withGameGain(unknown, 3.0F), unknown);
}

// An effect type this build does not know, from a newer or broken sender: what reaches the wheel is no force.
TEST(FfbController, AnUnknownEffectTypeReachesTheWheelAsNoForce) {
    RecordingSink sink;
    FfbController controller(sink, safety());
    FfbEffect unknown;
    unknown.type = static_cast<FfbEffectType>(42);
    unknown.constantLevel = 5.0F;
    unknown.periodic = {5.0F, 5.0F, 10.0F, 0.0F};
    controller.onPacket(slotPacket(2, true, unknown), 0);
    controller.update(0);
    ASSERT_EQ(sink.plays.size(), 1U);
    EXPECT_EQ(sink.plays[0].effect.constantLevel, 0.0F);
    EXPECT_EQ(sink.plays[0].effect.periodic.magnitude, 0.0F);
    EXPECT_EQ(sink.plays[0].effect.periodic.offset, 0.0F);
}

// The first slot number past the last one (8 of 8) is ignored like any other invalid one; the last one plays.
TEST(FfbController, OnlySlotsZeroToSevenPlay) {
    RecordingSink sink;
    FfbController controller(sink, safety());
    FfbEffect push;
    push.constantLevel = 0.2F;
    controller.onPacket(slotPacket(static_cast<std::uint8_t>(IFfbSink::kMaxSlots), true, push), 0);
    controller.onPacket(slotPacket(static_cast<std::uint8_t>(IFfbSink::kMaxSlots - 1), true, push), 0);
    controller.update(0);
    ASSERT_EQ(sink.plays.size(), 1U);
    EXPECT_EQ(sink.plays[0].slot, IFfbSink::kMaxSlots - 1);
    EXPECT_EQ(controller.stats().packets, 2U);
}

// The watchdog trips when the game goes quiet even if nothing was playing: nothing to stop, nothing counted, and
// the effects it had loaded are forgotten.
TEST(FfbController, TheWatchdogWithNothingPlayingStopsNothing) {
    RecordingSink sink;
    FfbController controller(sink, safety(), {100 * kMs, 4 * kMs});
    FfbEffect spring;
    spring.type = FfbEffectType::Spring;
    controller.onPacket(slotPacket(0, false, spring), 0);
    controller.update(200 * kMs);
    EXPECT_TRUE(controller.watchdogTripped());
    EXPECT_EQ(controller.stats().watchdogStops, 0U);
    EXPECT_TRUE(sink.stops.empty());
    EXPECT_FALSE(controller.slot(0).loaded);
}

// Every effect type a game can play reaches the sink, and stops when the game stops it.
TEST(FfbController, EveryTypePlaysAndStops) {
    for (int t = 0; t < 10; ++t) {
        const auto type = static_cast<FfbEffectType>(t);
        RecordingSink sink;
        FfbController controller(sink, safety());
        FfbEffect effect;
        effect.type = type;
        controller.onPacket(slotPacket(3, true, effect), 0);
        controller.update(0);
        ASSERT_EQ(sink.plays.size(), 1U) << ffbTypeName(type);
        EXPECT_EQ(sink.plays[0].effect.type, type);
        controller.onPacket(slotPacket(3, false, effect), 10 * kMs);
        controller.update(10 * kMs);
        ASSERT_EQ(sink.stops.size(), 1U) << ffbTypeName(type);
        EXPECT_EQ(sink.stops[0], 3U);
    }
}

// --- a push the game leaves frozen (NFS 2015, 2026-10-16 19:39:44: a loading screen) -----------------------------

// The effects NFS 2015 holds in a race: a constant push, its centring spring, a damper and a rumble.
struct NfsEffects {
    FfbEffect push;
    FfbEffect spring;
    FfbEffect damper;
    FfbEffect rumble;
    NfsEffects() {
        push.constantLevel = 0.83F;
        spring.type = FfbEffectType::Spring;
        spring.condition.positiveCoefficient = 0.9F;
        spring.condition.negativeCoefficient = 0.9F;
        spring.condition.positiveSaturation = 0.41F;
        spring.condition.negativeSaturation = 0.41F;
        damper.type = FfbEffectType::Damper;
        damper.condition.positiveCoefficient = 0.6F;
        damper.condition.negativeCoefficient = 0.6F;
        rumble.type = FfbEffectType::Square;
        rumble.periodic.magnitude = 0.36F;
        rumble.periodic.periodMs = 75.0F;
    }
};

// What reached the wheel for `slot` (nothing: an empty effect).
FfbEffect sentTo(const FfbController& controller, const std::size_t slot) {
    return controller.slot(slot).sent.value_or(FfbEffect{});
}

// For tests that leave seconds between packets: a watchdog that does not trip in between (the game is "there").
constexpr FfbController::Timing kPatientWatchdog{3'600'000 * kMs, 4 * kMs};

FfbSafetyConfig nfsSafety() {
    FfbSafetyConfig config;   // the defaults: frozen_force_s 2
    config.masterGain = 1.0F;
    config.maxSlewPerSecond = 1000.0F;
    return config;
}

// The game as the logs show it from 19:39:44: it keeps sending every 10 ms (no watchdog, no "quiet" from the
// driver), the push stays at 83% and the rumble kicks on and off twice a second, for 21 s. From 2 s on the push and
// the rumble fade out over half a second and stay out; the spring and the damper stay as they are.
TEST(FfbFrozenPush, ALoadingScreenThatKeepsSendingItsLastPushFadesOut) {
    RecordingSink sink;
    FfbController controller(sink, nfsSafety(), kPatientWatchdog);
    const NfsEffects nfs;
    const auto sendAll = [&](const std::uint64_t now) {
        controller.onPacket(slotPacket(0, true, nfs.push), now);
        controller.onPacket(slotPacket(1, true, nfs.spring), now);
        controller.onPacket(slotPacket(2, true, nfs.damper), now);
        controller.onPacket(slotPacket(3, (now / (250 * kMs)) % 2 == 0, nfs.rumble), now);   // the kicks
    };
    for (std::uint64_t now = 0; now <= 1'900 * kMs; now += 10 * kMs) {
        sendAll(now);
        controller.update(now);
    }
    EXPECT_FLOAT_EQ(controller.frozenFade(), 1.0F);
    EXPECT_NEAR(sentTo(controller, 0).constantLevel, 0.8F, 1e-4F);   // the cap, not the fade
    for (std::uint64_t now = 1'910 * kMs; now <= 2'250 * kMs; now += 10 * kMs) {
        sendAll(now);
        controller.update(now);
    }
    EXPECT_NEAR(controller.frozenFade(), 0.5F, 0.02F);                 // half way at 2.25 s
    EXPECT_NEAR(sentTo(controller, 0).constantLevel, 0.415F, 0.02F);
    for (std::uint64_t now = 2'260 * kMs; now <= 21'000 * kMs; now += 10 * kMs) {
        sendAll(now);
        controller.update(now);
        if (now >= 2'500 * kMs) {
            ASSERT_EQ(sentTo(controller, 0).constantLevel, 0.0F) << now / kMs << " ms";
            if (controller.slot(3).playing) {
                ASSERT_EQ(sentTo(controller, 3).periodic.magnitude, 0.0F) << now / kMs << " ms";
            }
        }
    }
    EXPECT_EQ(controller.frozenFade(), 0.0F);
    EXPECT_FLOAT_EQ(sentTo(controller, 1).condition.positiveSaturation, 0.41F);   // the spring still centres
    EXPECT_FLOAT_EQ(sentTo(controller, 2).condition.positiveCoefficient, 0.6F);   // the damper still damps

    // The race starts again: the game moves its push, and every force is back at once.
    FfbEffect moved = nfs.push;
    moved.constantLevel = 0.4F;
    controller.onPacket(slotPacket(0, true, moved), 21'010 * kMs);
    controller.onPacket(slotPacket(3, true, nfs.rumble), 21'010 * kMs);
    controller.update(21'010 * kMs);
    EXPECT_FLOAT_EQ(controller.frozenFade(), 1.0F);
    EXPECT_FLOAT_EQ(sentTo(controller, 0).constantLevel, 0.4F);
    EXPECT_FLOAT_EQ(sentTo(controller, 3).periodic.magnitude, 0.36F);
}

// Driving: NFS moves its push every 30 ms or so (in two hours of logs never left for more than 1.5 s). A minute of
// that is never faded.
TEST(FfbFrozenPush, DrivingIsNeverFaded) {
    RecordingSink sink;
    FfbController controller(sink, nfsSafety(), kPatientWatchdog);
    FfbEffect push;
    for (std::uint64_t now = 0; now <= 60'000 * kMs; now += 10 * kMs) {
        if (now % (30 * kMs) == 0) {
            push.constantLevel = 0.6F + 0.01F * static_cast<float>((now / (30 * kMs)) % 3);   // 0.60, 0.61, 0.62
            controller.onPacket(slotPacket(0, true, push), now);
        }
        controller.update(now);
        ASSERT_EQ(controller.frozenFade(), 1.0F) << now / kMs << " ms";
    }
}

// A long but legitimate hold just under 2 s, then a change: never faded; held 2 s exactly: the fade starts.
TEST(FfbFrozenPush, TheFadeStartsAfterTheSetTimeAndNotBefore) {
    RecordingSink sink;
    FfbController controller(sink, nfsSafety(), kPatientWatchdog);
    FfbEffect push;
    push.constantLevel = 0.5F;
    controller.onPacket(slotPacket(0, true, push), 0);
    controller.update(1'990 * kMs);
    EXPECT_EQ(controller.frozenFade(), 1.0F);
    push.constantLevel = 0.51F;
    controller.onPacket(slotPacket(0, true, push), 1'995 * kMs);
    controller.update(3'990 * kMs);
    EXPECT_EQ(controller.frozenFade(), 1.0F);   // 1.995 s since the change
    controller.update(4'095 * kMs);
    EXPECT_NEAR(controller.frozenFade(), 0.8F, 1e-3F);   // 0.1 s into the half-second fade
}

// A tiny push (5% or less, e.g. NFS's -3% at rest) left alone is no kick: not faded. Nor is a push that stops.
TEST(FfbFrozenPush, ATinyPushOrAStoppedOneIsLeftAlone) {
    RecordingSink sink;
    FfbController controller(sink, nfsSafety(), kPatientWatchdog);
    FfbEffect push;
    push.constantLevel = -0.03F;
    controller.onPacket(slotPacket(0, true, push), 0);
    controller.update(30'000 * kMs);
    EXPECT_EQ(controller.frozenFade(), 1.0F);
    push.constantLevel = 0.8F;
    controller.onPacket(slotPacket(0, false, push), 30'000 * kMs);
    controller.update(60'000 * kMs);
    EXPECT_EQ(controller.frozenFade(), 1.0F);
}

// The game restarting the same push (stop, start, same level) is still the same frozen push: the fade carries on.
// A push in another slot, or of another type in the same slot, is new.
TEST(FfbFrozenPush, ARestartWithTheSamePushIsNoChange) {
    RecordingSink sink;
    FfbController controller(sink, nfsSafety(), kPatientWatchdog);
    FfbEffect push;
    push.constantLevel = 0.7F;
    controller.onPacket(slotPacket(0, true, push), 0);
    controller.update(2'500 * kMs);
    EXPECT_EQ(controller.frozenFade(), 0.0F);
    controller.onPacket(slotPacket(0, false, push), 2'600 * kMs);
    controller.onPacket(slotPacket(0, true, push), 2'700 * kMs);
    controller.update(2'700 * kMs);
    EXPECT_EQ(controller.frozenFade(), 0.0F);
    EXPECT_EQ(sentTo(controller, 0).constantLevel, 0.0F);

    FfbEffect sine;
    sine.type = FfbEffectType::Sine;
    sine.periodic.magnitude = 0.3F;
    controller.onPacket(slotPacket(0, true, sine), 2'800 * kMs);   // the slot's push is gone: nothing frozen
    controller.update(2'800 * kMs);
    EXPECT_EQ(controller.frozenFade(), 1.0F);
    EXPECT_FLOAT_EQ(sentTo(controller, 0).periodic.magnitude, 0.3F);
}

// After "stop all" or the watchdog, the game's next push is a new one, even at the same level.
TEST(FfbFrozenPush, StopAllMakesTheNextPushANewOne) {
    RecordingSink sink;
    FfbController controller(sink, nfsSafety(), kPatientWatchdog);
    FfbEffect push;
    push.constantLevel = 0.7F;
    controller.onPacket(slotPacket(0, true, push), 0);
    controller.update(2'600 * kMs);
    EXPECT_EQ(controller.frozenFade(), 0.0F);
    FfbPacket stopAll;
    stopAll.kind = FfbPacket::Kind::StopAll;
    controller.onPacket(stopAll, 2'600 * kMs);
    controller.onPacket(slotPacket(0, true, push), 2'610 * kMs);
    controller.update(2'610 * kMs);
    EXPECT_EQ(controller.frozenFade(), 1.0F);
    EXPECT_FLOAT_EQ(sentTo(controller, 0).constantLevel, 0.7F);
}

// frozen_force_s = 0: never faded. Not a number: the default (2 s). Negative: never.
TEST(FfbFrozenPush, ZeroSwitchesItOffAndNonsenseIsHandled) {
    for (const float seconds : {0.0F, -1.0F, std::numeric_limits<float>::quiet_NaN()}) {
        RecordingSink sink;
        FfbSafetyConfig config = nfsSafety();
        config.frozenForceSeconds = seconds;
        FfbController controller(sink, config, kPatientWatchdog);
        FfbEffect push;
        push.constantLevel = 0.7F;
        controller.onPacket(slotPacket(0, true, push), 0);
        controller.update(10'000 * kMs);
        EXPECT_EQ(controller.frozenFade(), std::isnan(seconds) ? 0.0F : 1.0F) << seconds;
    }
}

// The game gain counts: a 4% push at gain 2 is an 8% push (frozen), and the game's effect gain too.
TEST(FfbFrozenPush, ThePushIsMeasuredAsItWouldReachTheWheel) {
    RecordingSink sink;
    FfbController controller(sink, nfsSafety(), kPatientWatchdog);
    controller.setGameGain(2.0F);
    FfbEffect push;
    push.constantLevel = 0.04F;
    controller.onPacket(slotPacket(0, true, push), 0);
    controller.update(3'000 * kMs);
    EXPECT_EQ(controller.frozenFade(), 0.0F);

    RecordingSink other;
    FfbController halved(other, nfsSafety(), kPatientWatchdog);
    push.constantLevel = 0.08F;
    push.gain = 0.5F;   // 4%
    halved.onPacket(slotPacket(0, true, push), 0);
    halved.update(3'000 * kMs);
    EXPECT_EQ(halved.frozenFade(), 1.0F);
}

// A clock that goes back (another time source after a restart) is no frozen push.
TEST(FfbFrozenPush, AClockGoingBackIsNoFreeze) {
    RecordingSink sink;
    FfbController controller(sink, nfsSafety(), kPatientWatchdog);
    FfbEffect push;
    push.constantLevel = 0.7F;
    controller.onPacket(slotPacket(0, true, push), 5'000 * kMs);
    controller.update(1'000 * kMs);
    EXPECT_EQ(controller.frozenFade(), 1.0F);
}

// A game's own values (GameTuning.hpp): each kind of effect times its own gain, conditions too, all still capped.
TEST(FfbController, GameTuningScalesEachKindOfEffect) {
    RecordingSink sink;
    FfbController controller(sink, safety());   // gain 1, cap 0.5, fast slew
    std::array<float, kFfbGroupCount> gains{};
    gains.fill(1.0F);
    gains[static_cast<std::size_t>(FfbGroup::Constant)] = 2.0F;
    gains[static_cast<std::size_t>(FfbGroup::Periodic)] = 0.0F;   // vibration off
    gains[static_cast<std::size_t>(FfbGroup::Damper)] = 4.0F;
    controller.setGameTuning(gains, safety(), false, true);
    FfbEffect push;
    push.constantLevel = 0.1F;
    FfbEffect rumble;
    rumble.type = FfbEffectType::Sine;
    rumble.periodic.magnitude = 0.2F;
    rumble.periodic.periodMs = 50.0F;
    FfbEffect damper;
    damper.type = FfbEffectType::Damper;
    damper.condition.positiveCoefficient = 0.1F;
    damper.condition.negativeCoefficient = 0.1F;
    damper.condition.positiveSaturation = 1.0F;
    damper.condition.negativeSaturation = 1.0F;
    controller.onPacket(slotPacket(0, true, push), 0);
    controller.onPacket(slotPacket(1, true, rumble), 0);
    controller.onPacket(slotPacket(2, true, damper), 0);
    controller.update(0);
    controller.update(100 * kMs);
    EXPECT_NEAR(controller.slot(0).sent->constantLevel, 0.2F, 1e-4F);
    EXPECT_NEAR(controller.slot(1).sent->periodic.magnitude, 0.0F, 1e-6F);
    EXPECT_NEAR(controller.slot(2).sent->condition.positiveCoefficient, 0.4F, 1e-4F);
    // Four times a strong damper: the limiter's cap still holds.
    damper.condition.positiveCoefficient = 0.9F;
    damper.condition.negativeCoefficient = 0.9F;
    controller.onPacket(slotPacket(2, true, damper), 110 * kMs);
    controller.update(200 * kMs);
    EXPECT_LE(controller.slot(2).sent->condition.positiveCoefficient, 1.0F);
    EXPECT_LE(controller.slot(2).sent->condition.positiveSaturation, 0.5F + 1e-5F);   // the force: at most the cap
    EXPECT_LE(std::abs(controller.slot(0).sent->constantLevel), 0.5F);
}

// The limits only go down: a cap above the mapping's (or not a number) is the mapping's; a lower one holds.
TEST(FfbController, GameTuningNeverRaisesTheLimits) {
    RecordingSink sink;
    FfbController controller(sink, safety());   // cap 0.5
    std::array<float, kFfbGroupCount> gains{};
    gains.fill(1.0F);
    FfbSafetyConfig wanted = safety();
    wanted.maxMagnitude = 1.0F;
    wanted.maxSlewPerSecond = 1e9F;
    controller.setGameTuning(gains, wanted, false, true);
    EXPECT_FLOAT_EQ(controller.safety().maxMagnitude, 0.5F);
    EXPECT_FLOAT_EQ(controller.safety().maxSlewPerSecond, 1000.0F);
    wanted.maxMagnitude = std::numeric_limits<float>::quiet_NaN();
    wanted.masterGain = std::numeric_limits<float>::quiet_NaN();
    controller.setGameTuning(gains, wanted, false, true);
    EXPECT_FLOAT_EQ(controller.safety().maxMagnitude, 0.5F);
    EXPECT_FLOAT_EQ(controller.safety().masterGain, 1.0F);
    FfbEffect push;
    push.constantLevel = 0.9F;
    controller.onPacket(slotPacket(0, true, push), 0);
    controller.update(0);
    controller.update(100 * kMs);
    EXPECT_FLOAT_EQ(controller.slot(0).sent->constantLevel, 0.5F);
    wanted.maxMagnitude = 0.25F;   // lower: holds
    wanted.masterGain = 1.0F;
    controller.setGameTuning(gains, wanted, false, true);
    controller.update(200 * kMs);
    EXPECT_FLOAT_EQ(controller.slot(0).sent->constantLevel, 0.25F);
    // Gains out of range are clamped to 0..4 (NaN: as asked).
    gains[0] = 99.0F;
    gains[1] = std::numeric_limits<float>::quiet_NaN();
    controller.setGameTuning(gains, wanted, false, true);
    EXPECT_FLOAT_EQ(controller.groupGains()[0], kGameEffectGainMax);
    EXPECT_FLOAT_EQ(controller.groupGains()[1], 1.0F);
}

// Turned the other way: pushes, offsets and a spring's centre flip; effects off stops everything and on brings back
// what the game still wants.
TEST(FfbController, GameTuningFlipsAndTurnsOff) {
    RecordingSink sink;
    FfbController controller(sink, safety());
    std::array<float, kFfbGroupCount> gains{};
    gains.fill(1.0F);
    FfbEffect push;
    push.constantLevel = 0.2F;
    FfbEffect spring;
    spring.type = FfbEffectType::Spring;
    spring.condition.center = 0.1F;
    spring.condition.positiveCoefficient = 0.3F;
    spring.condition.negativeCoefficient = 0.1F;
    spring.condition.positiveSaturation = 0.4F;
    spring.condition.negativeSaturation = 0.2F;
    controller.setGameTuning(gains, safety(), true, true);
    EXPECT_TRUE(controller.flipped());
    controller.onPacket(slotPacket(0, true, push), 0);
    controller.onPacket(slotPacket(1, true, spring), 0);
    controller.update(0);
    controller.update(100 * kMs);
    EXPECT_NEAR(controller.slot(0).sent->constantLevel, -0.2F, 1e-4F);
    EXPECT_NEAR(controller.slot(1).sent->condition.center, -0.1F, 1e-4F);
    EXPECT_NEAR(controller.slot(1).sent->condition.positiveCoefficient, 0.1F, 1e-4F);
    EXPECT_NEAR(controller.slot(1).sent->condition.negativeCoefficient, 0.3F, 1e-4F);

    controller.setGameTuning(gains, safety(), false, false);
    controller.update(200 * kMs);
    EXPECT_FALSE(controller.slot(0).playing);
    EXPECT_FALSE(controller.slot(1).playing);
    EXPECT_TRUE(controller.slot(0).wanted);   // the game still wants it
    FfbPacket keepAlive;
    keepAlive.kind = FfbPacket::Kind::KeepAlive;
    controller.onPacket(keepAlive, 250 * kMs);
    controller.setGameTuning(gains, safety(), false, true);
    controller.update(300 * kMs);
    EXPECT_TRUE(controller.slot(0).playing);
    controller.update(400 * kMs);   // a restarted push ramps up from 0, as a new one does
    EXPECT_NEAR(controller.slot(0).sent->constantLevel, 0.2F, 1e-4F);
}

}  // namespace
}  // namespace wheelbri
