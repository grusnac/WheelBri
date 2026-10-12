#include "wheelbri/FfbController.hpp"
#include "wheelbri/FfbUsage.hpp"

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace wheelbri {
namespace {

constexpr std::uint64_t kMs = 1'000'000;

FfbEffect constant(const float level) {
    FfbEffect e;
    e.type = FfbEffectType::Constant;
    e.constantLevel = level;
    return e;
}

FfbEffect spring(const float coefficient) {
    FfbEffect e;
    e.type = FfbEffectType::Spring;
    e.condition.positiveCoefficient = coefficient;
    e.condition.negativeCoefficient = coefficient;
    return e;
}

FfbSafetyConfig safety(const float gain = 1.0F, const float max = 0.8F) {
    FfbSafetyConfig config;
    config.masterGain = gain;
    config.maxMagnitude = max;
    return config;
}

TEST(FfbUsage, CountsStartsTimeAndStrengthPerType) {
    FfbUsage usage(safety());
    std::array<FfbUsage::SlotSample, 2> slots{};
    slots[0] = {true, constant(0.5F), constant(0.5F)};
    slots[1] = {true, spring(0.3F), spring(0.3F)};
    for (int i = 0; i < 10; ++i) {
        usage.record(10 * kMs, true, slots);
    }
    slots[0] = {true, constant(-0.7F), constant(-0.7F)};
    for (int i = 0; i < 10; ++i) {
        usage.record(10 * kMs, true, slots);
    }

    const auto& c = usage.type(FfbEffectType::Constant);
    EXPECT_EQ(c.starts, 1U);                          // the level changed, not a new start
    EXPECT_EQ(c.playNs, 200 * kMs);
    EXPECT_NEAR(c.askedAverage(), 0.6F, 1e-4F);
    EXPECT_FLOAT_EQ(c.askedPeak, 0.7F);
    EXPECT_EQ(c.cappedNs, 0U);
    EXPECT_EQ(usage.type(FfbEffectType::Spring).starts, 1U);
    EXPECT_EQ(usage.type(FfbEffectType::Damper).playNs, 0U);
    EXPECT_EQ(usage.gameNs(), 200 * kMs);
    EXPECT_EQ(usage.anyForceNs(), 200 * kMs);

    const auto used = usage.usedTypes();
    ASSERT_EQ(used.size(), 2U);
    EXPECT_EQ(used[0], FfbEffectType::Constant);      // equal time: kept in type order
    EXPECT_EQ(used[1], FfbEffectType::Spring);
}

TEST(FfbUsage, StopAndRestartCountsTwoStarts) {
    FfbUsage usage(safety());
    std::array<FfbUsage::SlotSample, 1> slots{};
    slots[0] = {true, constant(0.2F), constant(0.2F)};
    usage.record(kMs, true, slots);
    slots[0].playing = false;
    usage.record(kMs, true, slots);
    slots[0].playing = true;
    usage.record(kMs, true, slots);
    EXPECT_EQ(usage.type(FfbEffectType::Constant).starts, 2U);
    EXPECT_EQ(usage.type(FfbEffectType::Constant).playNs, 2 * kMs);
    EXPECT_EQ(usage.gameNs(), 3 * kMs);
}

TEST(FfbUsage, TwoSlotsOfOneTypeCountTimeOnce) {
    FfbUsage usage(safety());
    std::array<FfbUsage::SlotSample, 2> slots{};
    slots[0] = {true, spring(0.2F), spring(0.2F)};
    slots[1] = {true, spring(0.6F), spring(0.6F)};
    usage.record(10 * kMs, true, slots);
    EXPECT_EQ(usage.type(FfbEffectType::Spring).starts, 2U);
    EXPECT_EQ(usage.type(FfbEffectType::Spring).playNs, 10 * kMs);
    EXPECT_FLOAT_EQ(usage.type(FfbEffectType::Spring).askedPeak, 0.6F);
}

TEST(FfbUsage, CapAndSlewAreCounted) {
    FfbUsage usage(safety(1.0F, 0.8F));
    std::array<FfbUsage::SlotSample, 1> slots{};
    slots[0] = {true, constant(0.9F), constant(0.8F)};   // capped, the wheel is at the cap
    usage.record(10 * kMs, true, slots);
    slots[0] = {true, constant(0.5F), constant(0.3F)};   // not capped, still slewing towards 0.5
    usage.record(10 * kMs, true, slots);
    const auto& c = usage.type(FfbEffectType::Constant);
    EXPECT_EQ(c.cappedNs, 10 * kMs);
    EXPECT_EQ(c.slewNs, 10 * kMs);
    EXPECT_FLOAT_EQ(c.wheelPeak, 0.8F);
}

TEST(FfbUsage, HistogramOfConstantStrength) {
    FfbUsage usage(safety());
    std::array<FfbUsage::SlotSample, 1> slots{};
    slots[0] = {true, constant(0.05F), constant(0.05F)};
    usage.record(30 * kMs, true, slots);
    slots[0] = {true, constant(-0.95F), constant(-0.8F)};
    usage.record(10 * kMs, true, slots);
    slots[0] = {true, constant(1.0F), constant(0.8F)};
    usage.record(10 * kMs, true, slots);
    const auto& h = usage.constantHistogramNs();
    EXPECT_EQ(h[0], 30 * kMs);
    EXPECT_EQ(h[9], 20 * kMs);   // 95% and 100% both in the last bin
    EXPECT_NE(usage.histogramLine().find("0-10% 60%"), std::string::npos);
}

TEST(FfbUsage, LongGapsDoNotCountAsPlayTime) {
    FfbUsage usage(safety());
    std::array<FfbUsage::SlotSample, 1> slots{};
    slots[0] = {true, constant(0.2F), constant(0.2F)};
    usage.record(5'000 * kMs, true, slots);   // the PC slept
    EXPECT_EQ(usage.type(FfbEffectType::Constant).playNs, 100 * kMs);
}

TEST(FfbUsage, TableRowsFormat) {
    FfbUsage usage(safety());
    std::array<FfbUsage::SlotSample, 1> slots{};
    slots[0] = {true, constant(0.5F), constant(0.5F)};
    for (int i = 0; i < 20; ++i) {
        usage.record(100 * kMs, true, slots);   // 2 s
    }
    const std::string header = FfbUsage::tableHeader();
    const std::string row = usage.tableRow(FfbEffectType::Constant);
    EXPECT_EQ(header.size(), row.size());
    EXPECT_LE(row.size(), 68U);
    EXPECT_NE(row.find("0:00:02"), std::string::npos);
    EXPECT_NE(row.find("50% / 50%"), std::string::npos);
    EXPECT_NE(row.find("100%"), std::string::npos);   // share of the game's time
}

// Through the controller: what the sink plays is what the usage sees.
class NullSink : public IFfbSink {
public:
    bool play(std::size_t, const FfbEffect&) override { return true; }
    void stop(std::size_t) override {}
    void stopAll() override {}
};

TEST(FfbUsage, ControllerRecordsWhatPlays) {
    NullSink sink;
    FfbController controller(sink, safety(1.0F, 0.8F));
    FfbPacket packet;
    packet.kind = FfbPacket::Kind::SlotState;
    packet.slot = 0;
    packet.playing = true;
    packet.effect = constant(0.4F);
    std::uint64_t now = 1'000 * kMs;
    controller.onPacket(packet, now);
    for (int i = 0; i < 100; ++i) {
        controller.update(now);
        now += kMs;
    }
    const auto& usage = controller.usage();
    EXPECT_EQ(usage.type(FfbEffectType::Constant).starts, 1U);
    EXPECT_EQ(usage.type(FfbEffectType::Constant).playNs, 99 * kMs);   // from the second tick on
    EXPECT_FLOAT_EQ(usage.type(FfbEffectType::Constant).askedPeak, 0.4F);
    EXPECT_GT(usage.gameNs(), 0U);
}

}  // namespace
}  // namespace wheelbri
