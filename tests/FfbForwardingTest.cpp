// SPDX-License-Identifier: GPL-3.0-or-later
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
    push.constantLevel = 0.99F;   // above the cap (a full push alone would be GRID's broken value: FfbLoneFullPush)
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

// --- several senders (the driver's HID PID, HID++ and TrueForce; a shim; a restarted game) ---

// Two senders using the same slot number each get a slot of their own; one alone keeps its numbers.
TEST(FfbController, SendersWithTheSameSlotGetSlotsOfTheirOwn) {
    RecordingSink sink;
    FfbController controller(sink, safety());
    FfbEffect spring;
    spring.type = FfbEffectType::Spring;
    spring.condition.positiveCoefficient = 0.2F;
    FfbEffect damper;
    damper.type = FfbEffectType::Damper;
    damper.condition.positiveCoefficient = 0.1F;
    controller.onPacket(slotPacket(7, true, spring), 0, 50001);
    controller.onPacket(slotPacket(7, true, damper), 0, 50002);
    controller.update(0);

    ASSERT_EQ(sink.plays.size(), 2U);
    EXPECT_EQ(sink.plays[0].slot, 0U);   // the second sender's 7, in the first slot free
    EXPECT_EQ(sink.plays[1].slot, 7U);   // the first sender's own 7
    EXPECT_EQ(sink.plays[0].effect.type, FfbEffectType::Damper);
    EXPECT_EQ(sink.plays[1].effect.type, FfbEffectType::Spring);
    EXPECT_EQ(controller.stats().activeSlots, 2U);
}

// One sender's "stop all" stops its own effects, not another sender's.
TEST(FfbController, ASendersStopAllStopsOnlyItsOwn) {
    RecordingSink sink;
    FfbController controller(sink, safety());
    FfbEffect spring;
    spring.type = FfbEffectType::Spring;
    spring.condition.positiveCoefficient = 0.2F;
    controller.onPacket(slotPacket(0, true, spring), 0, 1);
    controller.onPacket(slotPacket(1, true, spring), 0, 2);
    controller.update(0);
    FfbPacket stopAll;
    stopAll.kind = FfbPacket::Kind::StopAll;
    controller.onPacket(stopAll, 5 * kMs, 1);
    controller.update(5 * kMs);

    ASSERT_EQ(sink.stops.size(), 1U);
    EXPECT_EQ(sink.stops[0], 0U);
    EXPECT_TRUE(controller.slot(1).wanted);
}

// A sender that goes quiet (its game crashed) has its forces stopped even while another keeps sending keep-alives:
// their keep-alives no longer feed its watchdog.
TEST(FfbController, ASenderGoneQuietIsStoppedWhileAnotherStillSends) {
    RecordingSink sink;
    FfbController controller(sink, safety(), {100 * kMs, 4 * kMs});
    FfbEffect push;
    push.constantLevel = 0.3F;
    controller.onPacket(slotPacket(0, true, push), 0, 1);       // the force, then nothing from it
    FfbEffect spring;
    spring.type = FfbEffectType::Spring;
    spring.condition.positiveCoefficient = 0.2F;
    controller.onPacket(slotPacket(3, true, spring), 0, 2);
    controller.update(0);
    for (std::uint64_t t = 50; t <= 300; t += 50) {
        controller.onPacket(FfbPacket{}, t * kMs, 2);            // the other keeps sending
        controller.update(t * kMs);
    }

    ASSERT_EQ(sink.stops.size(), 1U);
    EXPECT_EQ(sink.stops[0], 0U);
    EXPECT_FALSE(controller.slot(0).wanted);
    EXPECT_TRUE(controller.slot(3).wanted);
    EXPECT_EQ(controller.stats().watchdogStops, 1U);
    EXPECT_FALSE(controller.watchdogTripped());   // not all of them: the other still sends
}

// A sender beyond kMaxSources takes the place of the one quiet longest, whose effects are given up.
TEST(FfbController, TooManySendersGiveUpTheQuietestOne) {
    RecordingSink sink;
    FfbController controller(sink, safety());
    FfbEffect spring;
    spring.type = FfbEffectType::Spring;
    spring.condition.positiveCoefficient = 0.2F;
    for (std::uint32_t s = 0; s < FfbController::kMaxSources; ++s) {
        controller.onPacket(slotPacket(0, true, spring), s * kMs, 100 + s);
    }
    controller.update(10 * kMs);
    EXPECT_EQ(controller.stats().activeSlots, FfbController::kMaxSources);
    controller.onPacket(slotPacket(0, true, spring), 20 * kMs, 999);
    controller.update(20 * kMs);
    EXPECT_EQ(controller.stats().activeSlots, FfbController::kMaxSources);
    // Sender 100 (quiet longest) gave its place and its slot up; 999 plays in it now.
    EXPECT_TRUE(controller.slot(0).wanted);
    EXPECT_FLOAT_EQ(controller.slot(0).requested.condition.positiveCoefficient, 0.2F);
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
    push.constantLevel = 0.99F;
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

// Every field of every effect type survives the trip; NaN and infinities become 0.
TEST(FfbPacket, EveryFieldRoundTripsAndNonFiniteNumbersBecomeZero) {
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
    packet.effect.constantLevel = std::numeric_limits<float>::quiet_NaN();
    const auto infinite = decodeFfbPacket(encode(packet));
    ASSERT_TRUE(infinite.has_value());
    EXPECT_EQ(infinite->effect.periodic.magnitude, 0.0F);
    EXPECT_EQ(infinite->effect.condition.center, 0.0F);
    EXPECT_EQ(infinite->effect.constantLevel, 0.0F);
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

// A small push that comes to none, or changes side, reaches the wheel: within closeTo's step it would stay playing
// (ACC at a standstill: 0.0009 left on the wheel, 2026-11-03). The same small push again is still not re-sent.
TEST(FfbController, ASmallPushThatEndsOrChangesSideReachesTheWheel) {
    RecordingSink sink;
    FfbController controller(sink, safety());
    FfbEffect push;
    std::uint64_t now = 0;
    const auto send = [&](const float level) {
        push.constantLevel = level;
        now += 10 * kMs;
        controller.onPacket(slotPacket(0, true, push), now);
        const std::size_t before = sink.plays.size();
        controller.update(now);
        return sink.plays.size() - before;
    };
    send(0.0009F);
    send(0.0009F);   // past the soft start
    ASSERT_FALSE(sink.plays.empty());
    EXPECT_NEAR(sink.plays.back().effect.constantLevel, 0.0009F, 1e-6F);
    EXPECT_EQ(send(0.0009F), 0U);
    EXPECT_EQ(send(0.0F), 1U);
    EXPECT_FLOAT_EQ(sink.plays.back().effect.constantLevel, 0.0F);
    EXPECT_EQ(send(0.0F), 0U);
    send(0.0004F);
    EXPECT_EQ(send(-0.0003F), 1U);
    EXPECT_FLOAT_EQ(sink.plays.back().effect.constantLevel, -0.0003F);
    EXPECT_EQ(send(-0.0005F), 0U);   // the same side, within the step: as before
}

// A playing effect replaced by one of another kind is sent at once, even inside the minimum update interval, and
// the new one is limited on its own: it ramps up from none as a new effect does, to the cap on its magnitude.
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
    EXPECT_FLOAT_EQ(sink.plays[1].effect.periodic.magnitude, 0.0F);
    controller.update(21 * kMs);   // 20 ms at 50/s: up to the cap
    EXPECT_FLOAT_EQ(sink.plays.back().effect.periodic.magnitude, 0.5F);
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
    EXPECT_NEAR(sentTo(controller, 0).constantLevel, 0.83F, 1e-4F);   // all it asks (under the cap), not faded
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
    controller.update(2'820 * kMs);   // a new effect ramps up from none
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

// The game's effect gain counts: an 8% push at effect gain 0.5 is a 4% push, below what counts as frozen.
TEST(FfbFrozenPush, ThePushIsMeasuredAsItWouldReachTheWheel) {
    FfbEffect push;
    RecordingSink other;
    FfbController halved(other, nfsSafety(), kPatientWatchdog);
    push.constantLevel = 0.08F;
    push.gain = 0.5F;   // 4%
    halved.onPacket(slotPacket(0, true, push), 0);
    halved.update(3'000 * kMs);
    EXPECT_EQ(halved.frozenFade(), 1.0F);
}

// --- a lone full push (Race Driver: GRID, 2026-10-21 18:46: VSync off at 300 Hz, Mode Virtual) -------------------

// GRID's stream as the driver got it: nothing but 0 and -100% (its -2147483648, clamped by Windows' PID driver), a
// packet every 11 ms for a minute. Not one full push reaches the wheel, and the stream is said to be ignored.
TEST(FfbLoneFullPush, GridsBrokenStreamPlaysAsNone) {
    RecordingSink sink;
    FfbController controller(sink, nfsSafety(), kPatientWatchdog);
    FfbEffect push;
    for (std::uint64_t now = 0; now <= 60'000 * kMs; now += 11 * kMs) {
        push.constantLevel = (now / (500 * kMs)) % 3 == 0 ? 0.0F : -1.0F;
        controller.onPacket(slotPacket(0, true, push), now);
        controller.update(now);
        ASSERT_EQ(sentTo(controller, 0).constantLevel, 0.0F) << now / kMs << " ms";
    }
    EXPECT_TRUE(controller.ignoringLoneFullPush());
    EXPECT_GT(controller.stats().loneFullPushes, 0U);
    EXPECT_EQ(controller.slot(0).requested.constantLevel, 0.0F);   // what it keeps: no push
}

// GRID with VSync on: real forces all the way, and now and then a full one among them (a wall, a kerb). Those play.
TEST(FfbLoneFullPush, AFullPushAmongRealForcesPlays) {
    RecordingSink sink;
    FfbController controller(sink, nfsSafety(), kPatientWatchdog);
    FfbEffect push;
    for (std::uint64_t now = 0; now <= 1'000 * kMs; now += 11 * kMs) {
        push.constantLevel = -0.2F - 0.001F * static_cast<float>(now / kMs % 50);
        controller.onPacket(slotPacket(0, true, push), now);
        controller.update(now);
    }
    push.constantLevel = 1.0F;
    controller.onPacket(slotPacket(0, true, push), 1'011 * kMs);
    controller.update(1'011 * kMs);
    EXPECT_FLOAT_EQ(sentTo(controller, 0).constantLevel, 1.0F);
    push.constantLevel = -1.0F;
    controller.onPacket(slotPacket(0, true, push), 2'900 * kMs);   // 1.9 s after the last real force: still believed
    controller.update(2'900 * kMs);
    EXPECT_FLOAT_EQ(sentTo(controller, 0).constantLevel, -1.0F);
    EXPECT_FALSE(controller.ignoringLoneFullPush());
    EXPECT_EQ(controller.stats().loneFullPushes, 0U);
}

// A full push more than 2 s after the last real force is lone; the next real force believes full pushes again.
TEST(FfbLoneFullPush, RealForcesBringFullPushesBack) {
    RecordingSink sink;
    FfbController controller(sink, nfsSafety(), kPatientWatchdog);
    FfbEffect push;
    push.constantLevel = 0.3F;
    controller.onPacket(slotPacket(0, true, push), 0);
    push.constantLevel = 1.0F;
    controller.onPacket(slotPacket(0, true, push), 2'001 * kMs);
    controller.update(2'001 * kMs);
    EXPECT_EQ(sentTo(controller, 0).constantLevel, 0.0F);
    EXPECT_TRUE(controller.ignoringLoneFullPush());
    push.constantLevel = 0.4F;
    controller.onPacket(slotPacket(0, true, push), 2'100 * kMs);
    push.constantLevel = 1.0F;
    controller.onPacket(slotPacket(0, true, push), 2'111 * kMs);
    controller.update(2'111 * kMs);
    EXPECT_FALSE(controller.ignoringLoneFullPush());
    EXPECT_FLOAT_EQ(sentTo(controller, 0).constantLevel, 1.0F);
}

// Only a push at full scale (a constant force, or a periodic effect's offset): 99.9% is a real force, and a wave at
// full strength with no offset is left alone.
TEST(FfbLoneFullPush, OnlyAPushAtFullScaleIsScreened) {
    FfbEffect push;
    push.constantLevel = -0.999F;
    EXPECT_FLOAT_EQ(FfbController::screened(push, false).constantLevel, -0.999F);
    push.constantLevel = -1.0F;
    EXPECT_EQ(FfbController::screened(push, false).constantLevel, 0.0F);
    EXPECT_FLOAT_EQ(FfbController::screened(push, true).constantLevel, -1.0F);
    FfbEffect sine;
    sine.type = FfbEffectType::Sine;
    sine.periodic.magnitude = 1.0F;
    sine.constantLevel = 1.0F;   // not its force: a periodic effect's is its magnitude and offset
    EXPECT_EQ(FfbController::screened(sine, false), sine);
    FfbEffect square;
    square.type = FfbEffectType::Square;
    square.periodic.offset = -1.0F;   // a full push with no wave around it
    EXPECT_EQ(FfbController::screened(square, false).periodic.offset, 0.0F);
    EXPECT_EQ(FfbController::screened(square, true).periodic.offset, -1.0F);
}

// A periodic effect whose offset is a full push, with no real force around it, plays without the push: the same
// screen as a constant force's.
TEST(FfbLoneFullPush, APeriodicOffsetAtFullScaleIsScreenedToo) {
    RecordingSink sink;
    FfbController controller(sink, nfsSafety(), kPatientWatchdog);
    FfbEffect square;
    square.type = FfbEffectType::Square;
    square.periodic = {0.0F, 1.0F, 65535.0F, 0.0F};
    controller.onPacket(slotPacket(0, true, square), 0);
    controller.update(0);
    controller.update(100 * kMs);
    EXPECT_EQ(sentTo(controller, 0).periodic.offset, 0.0F);
    EXPECT_TRUE(controller.ignoringLoneFullPush());
}

// ignore_lone_full_push = false: GRID's stream plays as the game sends it.
TEST(FfbLoneFullPush, SwitchedOffEveryFullPushPlays) {
    RecordingSink sink;
    FfbSafetyConfig safety = nfsSafety();
    safety.ignoreLoneFullPush = false;
    FfbController controller(sink, safety, kPatientWatchdog);
    FfbEffect push;
    push.constantLevel = -1.0F;
    controller.onPacket(slotPacket(0, true, push), 0);
    controller.update(0);
    controller.update(100 * kMs);   // past the soft start
    EXPECT_FLOAT_EQ(sentTo(controller, 0).constantLevel, -1.0F);
    EXPECT_FALSE(controller.ignoringLoneFullPush());
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

// The game changing only its effect's gain is a new push: the frozen-push clock starts again.
TEST(FfbFrozenPush, AChangedGainIsANewPush) {
    RecordingSink sink;
    FfbController controller(sink, nfsSafety(), kPatientWatchdog);
    FfbEffect push;
    push.constantLevel = 0.7F;
    controller.onPacket(slotPacket(0, true, push), 0);
    controller.update(2'500 * kMs);
    EXPECT_EQ(controller.frozenFade(), 0.0F);
    push.gain = 0.9F;
    controller.onPacket(slotPacket(0, true, push), 2'600 * kMs);
    controller.update(2'600 * kMs);
    EXPECT_EQ(controller.frozenFade(), 1.0F);
}

// Two pushes frozen at once: the fade follows the one frozen first, whichever slot it is in.
TEST(FfbFrozenPush, TheFirstFrozenPushSetsTheFade) {
    for (const bool firstInSlotZero : {true, false}) {
        RecordingSink sink;
        FfbController controller(sink, nfsSafety(), kPatientWatchdog);
        FfbEffect push;
        push.constantLevel = 0.7F;
        controller.onPacket(slotPacket(firstInSlotZero ? 0 : 1, true, push), 0);              // frozen at 2.0 s
        controller.onPacket(slotPacket(firstInSlotZero ? 1 : 0, true, push), 1'000 * kMs);    // frozen at 3.0 s
        controller.update(3'100 * kMs);   // the first: 1.1 s past, faded out; the second only 0.1 s (0.8)
        EXPECT_EQ(controller.frozenFade(), 0.0F) << firstInSlotZero;
    }
}

// A centring spring that is not a number is off, never a spring of undefined strength.
TEST(FfbController, ANonsenseCentringSpringIsOff) {
    RecordingSink sink;
    FfbController controller(sink, safety());
    controller.setCentringSpring(30.0F);
    controller.setCentringSpring(std::numeric_limits<float>::quiet_NaN());
    EXPECT_EQ(controller.centringSpring(), 0.0F);
    controller.setCentringSpring(std::numeric_limits<float>::infinity());
    EXPECT_EQ(controller.centringSpring(), 0.0F);
}

// --- periodic pushes and the cap on all effects together --------------------------------------------------------

// A periodic effect's offset is a push: held unchanged past frozen_force_s it fades out as a constant push does.
TEST(FfbFrozenPush, AFrozenPeriodicOffsetFadesOut) {
    RecordingSink sink;
    FfbController controller(sink, nfsSafety(), kPatientWatchdog);
    FfbEffect square;
    square.type = FfbEffectType::Square;
    square.periodic = {0.1F, 0.6F, 65535.0F, 0.0F};   // a long square wave: a push that never changes
    for (std::uint64_t now = 0; now <= 2'600 * kMs; now += 10 * kMs) {
        controller.onPacket(slotPacket(0, true, square), now);
        controller.update(now);
    }
    EXPECT_EQ(controller.frozenFade(), 0.0F);
    EXPECT_EQ(sentTo(controller, 0).periodic.offset, 0.0F);
    EXPECT_EQ(sentTo(controller, 0).periodic.magnitude, 0.0F);
    square.periodic.offset = 0.5F;   // the game moves it: forces back
    controller.onPacket(slotPacket(0, true, square), 2'610 * kMs);
    controller.update(2'610 * kMs);
    EXPECT_EQ(controller.frozenFade(), 1.0F);
}

// A wave with no offset (a rumble) is no push, however long it plays.
TEST(FfbFrozenPush, AWaveWithNoOffsetIsNeverFaded) {
    RecordingSink sink;
    FfbController controller(sink, nfsSafety(), kPatientWatchdog);
    FfbEffect rumble;
    rumble.type = FfbEffectType::Sine;
    rumble.periodic = {0.4F, 0.0F, 30.0F, 0.0F};
    for (std::uint64_t now = 0; now <= 10'000 * kMs; now += 10 * kMs) {
        controller.onPacket(slotPacket(0, true, rumble), now);
        controller.update(now);
    }
    EXPECT_EQ(controller.frozenFade(), 1.0F);
    EXPECT_FLOAT_EQ(sentTo(controller, 0).periodic.magnitude, 0.4F);
}

// Two constant forces at the cap each: together they get the cap, half each, not twice it.
TEST(FfbController, TheCapHoldsForAllSlotsTogether) {
    RecordingSink sink;
    FfbController controller(sink, safety());   // cap 0.5
    FfbEffect push;
    push.constantLevel = 0.5F;
    controller.onPacket(slotPacket(0, true, push), 0);
    controller.onPacket(slotPacket(1, true, push), 0);
    controller.update(0);
    controller.update(100 * kMs);
    EXPECT_NEAR(sentTo(controller, 0).constantLevel, 0.25F, 1e-5F);
    EXPECT_NEAR(sentTo(controller, 1).constantLevel, 0.25F, 1e-5F);

    // Opposite pushes still count in full (the cap is on the most the effects can put on the wheel).
    FfbEffect pull;
    pull.constantLevel = -0.5F;
    controller.onPacket(slotPacket(1, true, pull), 110 * kMs);
    controller.update(200 * kMs);
    EXPECT_NEAR(sentTo(controller, 1).constantLevel, -0.25F, 1e-5F);

    // One stops: the other gets all of the cap again.
    controller.onPacket(slotPacket(1, false, pull), 210 * kMs);
    controller.update(300 * kMs);
    EXPECT_NEAR(sentTo(controller, 0).constantLevel, 0.5F, 1e-5F);
}

// No cap (1, the default): the wheel is the limit, and a push with a rumble on top plays as the game mixed it.
TEST(FfbController, WithoutACapTheGamesMixPlaysAsAsked) {
    RecordingSink sink;
    FfbSafetyConfig full = safety();
    full.maxMagnitude = 1.0F;
    FfbController controller(sink, full);
    FfbEffect push;
    push.constantLevel = 0.8F;
    FfbEffect rumble;
    rumble.type = FfbEffectType::Sine;
    rumble.periodic = {0.3F, 0.0F, 30.0F, 0.0F};
    controller.onPacket(slotPacket(0, true, push), 0);
    controller.onPacket(slotPacket(1, true, rumble), 0);
    controller.update(0);
    controller.update(100 * kMs);
    EXPECT_NEAR(sentTo(controller, 0).constantLevel, 0.8F, 1e-5F);
    EXPECT_NEAR(sentTo(controller, 1).periodic.magnitude, 0.3F, 1e-5F);
}

// A slot held back by the minimum update interval still comes down at once when another effect starts and the cap
// is shared: a lower force never waits.
TEST(FfbController, ASharedCapLowersAHeldBackSlotAtOnce) {
    RecordingSink sink;
    FfbController controller(sink, safety());   // cap 0.5, minimum update 4 ms
    FfbEffect push;
    push.constantLevel = 0.5F;
    controller.onPacket(slotPacket(0, true, push), 0);
    controller.update(0);
    controller.update(10 * kMs);
    EXPECT_NEAR(sentTo(controller, 0).constantLevel, 0.5F, 1e-5F);
    FfbEffect square;
    square.type = FfbEffectType::Square;
    square.periodic = {0.5F, 0.0F, 40.0F, 0.0F};
    controller.onPacket(slotPacket(1, true, square), 11 * kMs);
    controller.update(11 * kMs);   // starts at none: nothing to share yet
    controller.update(12 * kMs);   // 1 ms after slot 0's last update: held back, but the square has risen
    const float together = sentTo(controller, 0).constantLevel + sentTo(controller, 1).periodic.magnitude;
    EXPECT_LE(together, 0.5F + 1e-5F);
    controller.update(30 * kMs);
    EXPECT_NEAR(sentTo(controller, 0).constantLevel, 0.25F, 1e-5F);
    EXPECT_NEAR(sentTo(controller, 1).periodic.magnitude, 0.25F, 1e-5F);
}

}  // namespace
}  // namespace wheelbri
