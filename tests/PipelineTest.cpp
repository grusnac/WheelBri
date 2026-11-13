// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/Pipeline.hpp"

#include "wheelbri/AntiDeadZone.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

namespace wheelbri {
namespace {

class FakeSource final : public IInputSource {
public:
    std::string name() const override { return "fake"; }
    bool poll(RawInputState& state) override {
        if (!connected) {
            return false;
        }
        state = next;
        return true;
    }

    RawInputState next;
    bool connected = true;
};

class RecordingPublisher final : public IStatePublisher {
public:
    bool publish(const StatePacket& packet) override {
        packets.push_back(packet);
        return true;
    }

    std::vector<StatePacket> packets;
};

TEST(Pipeline, MapsEncodesAndPublishesEachTick) {
    FakeSource source;
    RecordingPublisher publisher;
    MapperConfig config;
    config.throttle.sourceAxis = 1;
    const Mapper mapper(config);
    const DeviceProfile profile(g920Spec());
    Pipeline pipeline(source, mapper, profile, publisher);

    source.next.axes[0] = 1.0F;      // full right
    source.next.axes[1] = 1.0F;      // throttle fully pressed
    ASSERT_TRUE(pipeline.tick());
    ASSERT_TRUE(pipeline.tick());

    ASSERT_EQ(publisher.packets.size(), 2U);
    EXPECT_EQ(publisher.packets[0].sequence, 1U);
    EXPECT_EQ(publisher.packets[1].sequence, 2U);
    EXPECT_EQ(publisher.packets[1].report.steering, 65535);
    EXPECT_EQ(publisher.packets[1].report.throttle, 0);   // G920 pedals: pressed = low end
    EXPECT_EQ(pipeline.lastReport(), publisher.packets[1].report);
}

// A game profile's pedal route moves the pedals of what is published; the window and telemetry keep the mapped ones.
TEST(Pipeline, PedalRouteMovesOnlyThePublishedPedals) {
    FakeSource source;
    RecordingPublisher publisher;
    MapperConfig config;
    config.throttle.sourceAxis = 1;
    config.brake.sourceAxis = 2;
    const Mapper mapper(config);
    const DeviceProfile profile(g920Spec());
    Pipeline pipeline(source, mapper, profile, publisher);
    EXPECT_TRUE(pipeline.pedalRoute().identity());   // the defaults until a game profile says otherwise

    source.next.axes[1] = 1.0F;    // throttle fully pressed
    source.next.axes[2] = -1.0F;   // brake released
    ASSERT_TRUE(pipeline.tick());
    EXPECT_EQ(publisher.packets.back().report, pipeline.lastReport());

    const PedalRoute nfs2015{{1, 2, 0}};   // Y carries the brake, Rz the clutch, the Slider the throttle
    pipeline.setPedalRoute(nfs2015);
    EXPECT_EQ(pipeline.pedalRoute(), nfs2015);
    ASSERT_TRUE(pipeline.tick());
    const TargetReport& sent = publisher.packets.back().report;
    const TargetReport& mapped = pipeline.lastReport();
    EXPECT_EQ(mapped.throttle, 0);                 // as mapped: the throttle pressed
    EXPECT_EQ(sent.clutch, mapped.throttle);       // NFS 2015 reads the throttle from the Slider (clutch field)
    EXPECT_EQ(sent.throttle, mapped.brake);        // and the brake from Y (throttle field)
    EXPECT_EQ(sent.brake, mapped.clutch);          // and the clutch from Rz (brake field)
    EXPECT_EQ(sent.steering, mapped.steering);

    pipeline.setPedalRoute(PedalRoute{});
    ASSERT_TRUE(pipeline.tick());
    EXPECT_EQ(publisher.packets.back().report, pipeline.lastReport());
}

// Combined pedals (Logitech's "Report Combined Pedals"): the published throttle carries the combined pedals, the brake
// rests in the middle, the clutch stays; the window and telemetry keep the mapped pedals. Before a game's pedal route.
// The anti-dead zone (AntiDeadZone.hpp): off at 0; the centre stays the centre; any turn starts at the percent and
// full lock stays full lock; the sign is kept; nonsense is off or clamped.
TEST(AntiDeadZone, StartsAtThePercentAndKeepsFullLock) {
    EXPECT_FLOAT_EQ(withAntiDeadZone(0.3F, 0.0F), 0.3F);
    EXPECT_FLOAT_EQ(withAntiDeadZone(0.0F, 10.0F), 0.0F);
    EXPECT_NEAR(withAntiDeadZone(0.001F, 10.0F), 0.1009F, 1e-4F);
    EXPECT_NEAR(withAntiDeadZone(-0.001F, 10.0F), -0.1009F, 1e-4F);
    EXPECT_NEAR(withAntiDeadZone(0.5F, 10.0F), 0.55F, 1e-6F);
    EXPECT_FLOAT_EQ(withAntiDeadZone(1.0F, 10.0F), 1.0F);
    EXPECT_FLOAT_EQ(withAntiDeadZone(-1.0F, 10.0F), -1.0F);
    EXPECT_FLOAT_EQ(withAntiDeadZone(0.5F, std::numeric_limits<float>::quiet_NaN()), 0.5F);
    EXPECT_NEAR(withAntiDeadZone(0.001F, 90.0F), 0.5005F, 1e-4F);   // clamped to the most, 50%
    EXPECT_TRUE(antiDeadZoneUsable(0.0F));
    EXPECT_TRUE(antiDeadZoneUsable(kAntiDeadZoneMaxPercent));
    EXPECT_FALSE(antiDeadZoneUsable(-1.0F));
    EXPECT_FALSE(antiDeadZoneUsable(std::numeric_limits<float>::quiet_NaN()));
}

// The pipeline publishes the steering with the anti-dead zone; lastState() and lastReport() stay as mapped.
TEST(Pipeline, AntiDeadZoneChangesOnlyWhatTheGameReads) {
    FakeSource source;
    RecordingPublisher publisher;
    const Mapper mapper{MapperConfig{}};
    const DeviceProfile profile(g25Spec());
    Pipeline pipeline(source, mapper, profile, publisher);
    EXPECT_EQ(pipeline.antiDeadZone(), 0.0F);

    source.next.axes[0] = 0.02F;   // a little to the right
    ASSERT_TRUE(pipeline.tick());
    const std::int32_t asMapped = pipeline.lastReport().steering;
    EXPECT_EQ(publisher.packets.back().report.steering, asMapped);   // off: as mapped

    pipeline.setAntiDeadZone(10.0F);
    ASSERT_TRUE(pipeline.tick());
    WheelState expected = pipeline.lastState();
    expected.steering = withAntiDeadZone(expected.steering, 10.0F);
    EXPECT_EQ(publisher.packets.back().report.steering, profile.encode(expected).steering);
    EXPECT_GT(publisher.packets.back().report.steering, asMapped);
    EXPECT_EQ(pipeline.lastReport().steering, asMapped);
    EXPECT_EQ(pipeline.lastPublished().steering, publisher.packets.back().report.steering);

    source.next.axes[0] = 0.0F;   // the centre stays the centre
    ASSERT_TRUE(pipeline.tick());
    EXPECT_EQ(publisher.packets.back().report.steering, pipeline.lastReport().steering);
}

TEST(Pipeline, CombinedPedalsPutThemOnTheThrottleAndCentreTheBrake) {
    FakeSource source;
    RecordingPublisher publisher;
    MapperConfig config;
    config.throttle.sourceAxis = 1;
    config.brake.sourceAxis = 2;
    config.clutch.sourceAxis = 3;
    const Mapper mapper(config);
    const DeviceProfile profile(g25Spec());
    Pipeline pipeline(source, mapper, profile, publisher);
    EXPECT_FALSE(pipeline.combinedPedals());

    source.next.axes[1] = -1.0F;   // all released
    source.next.axes[2] = -1.0F;
    source.next.axes[3] = -1.0F;
    pipeline.setCombinedPedals(true);
    ASSERT_TRUE(pipeline.tick());
    const TargetReport& released = publisher.packets.back().report;
    EXPECT_EQ(released.throttle, kPedalMiddle);   // released: the middle, so menus read no direction
    EXPECT_EQ(released.brake, kPedalMiddle);
    EXPECT_EQ(released.clutch, 65535);
    EXPECT_EQ(pipeline.lastReport().throttle, 65535);   // as mapped

    source.next.axes[1] = 1.0F;    // throttle fully pressed: the bottom
    ASSERT_TRUE(pipeline.tick());
    EXPECT_EQ(publisher.packets.back().report.throttle, 0);
    source.next.axes[1] = -1.0F;
    source.next.axes[2] = 1.0F;    // brake fully pressed: the top
    ASSERT_TRUE(pipeline.tick());
    EXPECT_EQ(publisher.packets.back().report.throttle, 65535);
    EXPECT_EQ(publisher.packets.back().report.brake, kPedalMiddle);
    EXPECT_EQ(publisher.packets.back().report.combined, 65535);   // Slider 0 as always

    // A game that reads the throttle from the Slider (a route) gets the combined pedals there.
    pipeline.setPedalRoute(PedalRoute{{1, 2, 0}});
    ASSERT_TRUE(pipeline.tick());
    EXPECT_EQ(publisher.packets.back().report.clutch, 65535);   // the combined pedals: brake pressed
    EXPECT_EQ(publisher.packets.back().report.throttle, kPedalMiddle);

    pipeline.setPedalRoute(PedalRoute{});
    pipeline.setCombinedPedals(false);
    ASSERT_TRUE(pipeline.tick());
    EXPECT_EQ(publisher.packets.back().report, pipeline.lastReport());
}

// [buttons] order = native: the G25's buttons are published in its own HID order; the window and telemetry keep them as
// named. A target without a native order (the G920) ignores it.
TEST(Pipeline, NativeButtonOrderMovesOnlyThePublishedButtons) {
    FakeSource source;
    RecordingPublisher publisher;
    MapperConfig config;
    config.buttonMap.fill(kUnmapped);
    config.buttonMap[17] = 0;   // Blk bottom <- source button 0
    config.buttonMap[0] = 1;    // Red 1 <- source button 1
    const Mapper mapper(config);
    const DeviceProfile g25(g25Spec());
    Pipeline pipeline(source, mapper, g25, publisher);
    EXPECT_FALSE(pipeline.nativeButtons());

    source.next.buttons = SourceButtons::fromBits(0b11);
    ASSERT_TRUE(pipeline.tick());
    EXPECT_EQ(publisher.packets.back().report.buttons, (1U << 17) | 1U);   // the driver's order, as named

    pipeline.setButtonOrder(ButtonOrder::Native);
    EXPECT_TRUE(pipeline.nativeButtons());
    ASSERT_TRUE(pipeline.tick());
    EXPECT_EQ(publisher.packets.back().report.buttons, 1U | (1U << 11));   // Blk bottom 0, Red 1 11
    EXPECT_EQ(pipeline.lastReport().buttons, (1U << 17) | 1U);

    const DeviceProfile g920(g920Spec());
    Pipeline other(source, mapper, g920, publisher);
    other.setButtonOrder(ButtonOrder::Native);
    EXPECT_FALSE(other.nativeButtons());
}

TEST(Pipeline, LimitedSendingPublishesOnlyChangesAndKeepAlives) {
    FakeSource source;
    RecordingPublisher publisher;
    const Mapper mapper(MapperConfig{});
    const DeviceProfile profile(g920Spec());
    Pipeline pipeline(source, mapper, profile, publisher);
    pipeline.limitSending(2'000'000);   // at most 500 a second

    constexpr std::uint64_t kMs = 1'000'000;
    ASSERT_TRUE(pipeline.tick(1 * kMs));                  // first state: sent
    ASSERT_TRUE(pipeline.tick(2 * kMs));                  // unchanged: not sent
    source.next.axes[0] = 0.5F;
    ASSERT_TRUE(pipeline.tick(2 * kMs));                  // changed, but within 2 ms: held
    EXPECT_NE(pipeline.lastReport(), publisher.packets.back().report);   // the UI still sees the newest
    ASSERT_TRUE(pipeline.tick(3 * kMs));                  // the held change goes out
    ASSERT_EQ(publisher.packets.size(), 2U);
    EXPECT_EQ(publisher.packets[1].sequence, 2U);         // numbered by packets sent, not by ticks
    EXPECT_EQ(publisher.packets[1].report, pipeline.lastReport());
    ASSERT_TRUE(pipeline.tick(103 * kMs));                // 100 ms unchanged: a keep-alive
    EXPECT_EQ(publisher.packets.size(), 3U);
    ASSERT_NE(pipeline.sendGate(), nullptr);
    EXPECT_EQ(pipeline.sendGate()->heldBack(), 1U);
}

TEST(Pipeline, StopsWhenSourceDisconnects) {
    FakeSource source;
    RecordingPublisher publisher;
    const Mapper mapper(MapperConfig{});
    const DeviceProfile profile(g920Spec());
    Pipeline pipeline(source, mapper, profile, publisher);

    source.connected = false;
    EXPECT_FALSE(pipeline.tick());
    EXPECT_TRUE(publisher.packets.empty());
}

TEST(Pipeline, ReadsAccessoriesAndStopsWhenOneIsLost) {
    FakeSource wheel;
    FakeSource pedals;
    RecordingPublisher publisher;
    MapperConfig config;
    config.accessories[0] = {0x044F, 0xB696};
    config.throttle.sourceAxis = 0;
    config.throttle.sourceDevice = 1;
    const Mapper mapper(config);
    const DeviceProfile profile(g920Spec());
    Pipeline pipeline(wheel, mapper, profile, publisher);
    pipeline.setAccessories({&pedals, nullptr, nullptr});

    pedals.next.axes[0] = 1.0F;
    ASSERT_TRUE(pipeline.tick());
    EXPECT_FLOAT_EQ(pipeline.lastState().throttle, 1.0F);
    EXPECT_FLOAT_EQ(pipeline.lastInputs().devices[1].axes[0], 1.0F);

    pedals.connected = false;
    EXPECT_FALSE(pipeline.tick());
    EXPECT_EQ(pipeline.lostDevice(), 1);
}

TEST(Pipeline, InputChangedSaysWhetherAnyDeviceReportedSomethingNew) {
    FakeSource wheel;
    FakeSource pedals;
    RecordingPublisher publisher;
    MapperConfig config;
    config.accessories[0] = {0x044F, 0xB696};
    const Mapper mapper(config);
    const DeviceProfile profile(g920Spec());
    Pipeline pipeline(wheel, mapper, profile, publisher);
    pipeline.setAccessories({&pedals, nullptr, nullptr});

    EXPECT_FALSE(pipeline.inputChanged());   // before the first tick
    ASSERT_TRUE(pipeline.tick());
    EXPECT_FALSE(pipeline.inputChanged());   // all at rest, as the state starts

    wheel.next.axes[0] = 0.25F;
    ASSERT_TRUE(pipeline.tick());
    EXPECT_TRUE(pipeline.inputChanged());    // an axis of the wheel
    ASSERT_TRUE(pipeline.tick());
    EXPECT_FALSE(pipeline.inputChanged());   // the same again

    wheel.next.buttons = SourceButtons::fromBits(0x4U);
    ASSERT_TRUE(pipeline.tick());
    EXPECT_TRUE(pipeline.inputChanged());    // a button

    pedals.next.axes[2] = -1.0F;
    ASSERT_TRUE(pipeline.tick());
    EXPECT_TRUE(pipeline.inputChanged());    // an accessory, mapped or not
    ASSERT_TRUE(pipeline.tick());
    EXPECT_FALSE(pipeline.inputChanged());
}

TEST(Pipeline, InputChangedIsFalseWhenTheSourceIsLost) {
    FakeSource wheel;
    RecordingPublisher publisher;
    const Mapper mapper(MapperConfig{});
    const DeviceProfile profile(g920Spec());
    Pipeline pipeline(wheel, mapper, profile, publisher);

    wheel.next.axes[0] = 1.0F;
    ASSERT_TRUE(pipeline.tick());
    EXPECT_TRUE(pipeline.inputChanged());
    wheel.connected = false;
    EXPECT_FALSE(pipeline.tick());
    EXPECT_FALSE(pipeline.inputChanged());
}

class FailingPublisher final : public IStatePublisher {
public:
    bool publish(const StatePacket& /*packet*/) override { return false; }
};

// The consumer's socket refuses the datagram (nobody listens, a full buffer): the tick goes on, the failure is counted
// and the packets are still numbered, so a consumer that comes back sees the gap.
TEST(Pipeline, PublishFailuresAreCountedAndTheLoopGoesOn) {
    FakeSource source;
    FailingPublisher publisher;
    const Mapper mapper(MapperConfig{});
    const DeviceProfile profile(g920Spec());
    Pipeline pipeline(source, mapper, profile, publisher);
    EXPECT_EQ(pipeline.sendGate(), nullptr);   // every tick publishes without limitSending
    ASSERT_TRUE(pipeline.tick());
    ASSERT_TRUE(pipeline.tick());
    EXPECT_EQ(pipeline.publishFailures(), 2U);
    EXPECT_EQ(pipeline.sequence(), 2U);
}

// Accessory slots in use with gaps (device 2 only): the empty ones are skipped, the used one is read.
TEST(Pipeline, AccessorySlotsWithGapsAreSkipped) {
    FakeSource wheel;
    FakeSource box;
    RecordingPublisher publisher;
    MapperConfig config;
    config.buttonMap[0] = 1;
    config.buttonDevice[0] = 2;
    const Mapper mapper(config);
    const DeviceProfile profile(g920Spec());
    Pipeline pipeline(wheel, mapper, profile, publisher);
    pipeline.setAccessories({nullptr, &box, nullptr});
    box.next.buttons = SourceButtons::fromBits(1U << 1);
    ASSERT_TRUE(pipeline.tick());
    EXPECT_EQ(pipeline.lastReport().buttons & 1U, 1U);
    EXPECT_EQ(pipeline.lastInputs().devices[2].buttons, SourceButtons::fromBits(1U << 1));
    EXPECT_EQ(pipeline.lastInputs().devices[1], RawInputState{});
}

// A game's own mapping (GameTuning.hpp, tunedMapper) is used while it is set, then the mapping's again.
TEST(Pipeline, GameMapperIsUsedWhileSet) {
    FakeSource source;
    RecordingPublisher publisher;
    MapperConfig config;
    config.throttle.sourceAxis = 1;
    const Mapper mapper(config);
    MapperConfig inverted = config;
    inverted.throttle.invert = true;
    const Mapper game(inverted);
    const DeviceProfile profile(g920Spec());
    Pipeline pipeline(source, mapper, profile, publisher);
    source.next.axes[1] = 1.0F;   // pressed for the mapping, released for the game's
    ASSERT_TRUE(pipeline.tick());
    EXPECT_FLOAT_EQ(pipeline.lastState().throttle, 1.0F);
    pipeline.setGameMapper(&game);
    EXPECT_EQ(pipeline.gameMapper(), &game);
    ASSERT_TRUE(pipeline.tick());
    EXPECT_FLOAT_EQ(pipeline.lastState().throttle, 0.0F);
    pipeline.setGameMapper(nullptr);
    ASSERT_TRUE(pipeline.tick());
    EXPECT_FLOAT_EQ(pipeline.lastState().throttle, 1.0F);
}

// The Mapping tab changed while the session runs (LiveMapping.hpp): the new mapping is used from the next tick, and a
// game's own mapping still wins while it is set.
TEST(Pipeline, NewMapperIsUsedFromTheNextTick) {
    FakeSource source;
    RecordingPublisher publisher;
    MapperConfig config;
    config.throttle.sourceAxis = 1;
    const Mapper mapper(config);
    MapperConfig inverted = config;
    inverted.throttle.invert = true;
    const Mapper changed(inverted);
    const Mapper game(config);
    const DeviceProfile profile(g920Spec());
    Pipeline pipeline(source, mapper, profile, publisher);
    source.next.axes[1] = 1.0F;
    ASSERT_TRUE(pipeline.tick());
    EXPECT_FLOAT_EQ(pipeline.lastState().throttle, 1.0F);
    pipeline.setMapper(changed);
    EXPECT_EQ(&pipeline.mapper(), &changed);
    ASSERT_TRUE(pipeline.tick());
    EXPECT_FLOAT_EQ(pipeline.lastState().throttle, 0.0F);
    pipeline.setGameMapper(&game);
    ASSERT_TRUE(pipeline.tick());
    EXPECT_FLOAT_EQ(pipeline.lastState().throttle, 1.0F);
}

}  // namespace
}  // namespace wheelbri
