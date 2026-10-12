#include "wheelbri/Pipeline.hpp"

#include <gtest/gtest.h>

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

TEST(Pipeline, LimitedSendingPublishesOnlyChangesAndKeepAlives) {
    FakeSource source;
    RecordingPublisher publisher;
    const Mapper mapper(MapperConfig{});
    const DeviceProfile profile(g920Spec());
    Pipeline pipeline(source, mapper, profile, publisher);
    pipeline.limitSending(2'000'000);   // at most 500 a second

    constexpr std::uint64_t kMs = 1'000'000;
    ASSERT_TRUE(pipeline.tick(std::nullopt, 1 * kMs));    // first state: sent
    ASSERT_TRUE(pipeline.tick(std::nullopt, 2 * kMs));    // unchanged: not sent
    source.next.axes[0] = 0.5F;
    ASSERT_TRUE(pipeline.tick(std::nullopt, 2 * kMs));    // changed, but within 2 ms: held
    EXPECT_NE(pipeline.lastReport(), publisher.packets.back().report);   // the UI still sees the newest
    ASSERT_TRUE(pipeline.tick(std::nullopt, 3 * kMs));    // the held change goes out
    ASSERT_EQ(publisher.packets.size(), 2U);
    EXPECT_EQ(publisher.packets[1].sequence, 2U);         // numbered by packets sent, not by ticks
    EXPECT_EQ(publisher.packets[1].report, pipeline.lastReport());
    ASSERT_TRUE(pipeline.tick(std::nullopt, 103 * kMs));  // 100 ms unchanged: a keep-alive
    EXPECT_EQ(publisher.packets.size(), 3U);
    ASSERT_NE(pipeline.sendGate(), nullptr);
    EXPECT_EQ(pipeline.sendGate()->heldBack(), 1U);
}

TEST(Pipeline, TestClutchReplacesTheMappedClutch) {
    FakeSource source;
    RecordingPublisher publisher;
    const Mapper mapper(MapperConfig{});
    const DeviceProfile profile(g920Spec());
    Pipeline pipeline(source, mapper, profile, publisher);

    ASSERT_TRUE(pipeline.tick(1.0F));
    EXPECT_FLOAT_EQ(pipeline.lastState().clutch, 1.0F);
    EXPECT_EQ(pipeline.lastReport().clutch, 0);          // pressed = low end, like the other pedals
    ASSERT_TRUE(pipeline.tick());
    EXPECT_FLOAT_EQ(pipeline.lastState().clutch, 0.0F);  // no override: the mapped (absent) clutch
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

    wheel.next.buttons = 0x4U;
    ASSERT_TRUE(pipeline.tick());
    EXPECT_TRUE(pipeline.inputChanged());    // a button

    pedals.next.axes[2] = -1.0F;
    ASSERT_TRUE(pipeline.tick());
    EXPECT_TRUE(pipeline.inputChanged());    // an accessory, mapped or not
    ASSERT_TRUE(pipeline.tick());
    EXPECT_FALSE(pipeline.inputChanged());
}

TEST(Pipeline, InputChangedIgnoresTheTestClutch) {
    FakeSource wheel;
    RecordingPublisher publisher;
    const Mapper mapper(MapperConfig{});
    const DeviceProfile profile(g920Spec());
    Pipeline pipeline(wheel, mapper, profile, publisher);

    ASSERT_TRUE(pipeline.tick());
    ASSERT_TRUE(pipeline.tick(0.5F));
    EXPECT_FALSE(pipeline.inputChanged());   // the clutch override comes over UDP, not from a device
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

}  // namespace
}  // namespace wheelbri
