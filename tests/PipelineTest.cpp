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
    const DeviceProfile profile(g29Spec());
    Pipeline pipeline(source, mapper, profile, publisher);

    source.next.axes[0] = 1.0F;      // full right
    source.next.axes[1] = 1.0F;      // throttle fully pressed
    ASSERT_TRUE(pipeline.tick());
    ASSERT_TRUE(pipeline.tick());

    ASSERT_EQ(publisher.packets.size(), 2U);
    EXPECT_EQ(publisher.packets[0].sequence, 1U);
    EXPECT_EQ(publisher.packets[1].sequence, 2U);
    EXPECT_EQ(publisher.packets[1].report.steering, 65535);
    EXPECT_EQ(publisher.packets[1].report.throttle, 0);   // G29 pedals: pressed = low end
    EXPECT_EQ(pipeline.lastReport(), publisher.packets[1].report);
}

TEST(Pipeline, TestClutchReplacesTheMappedClutch) {
    FakeSource source;
    RecordingPublisher publisher;
    const Mapper mapper(MapperConfig{});
    const DeviceProfile profile(g29Spec());
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
    const DeviceProfile profile(g29Spec());
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
    const DeviceProfile profile(g29Spec());
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

}  // namespace
}  // namespace wheelbri
