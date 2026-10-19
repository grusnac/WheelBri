#include "wheelbri/FfbController.hpp"
#include "wheelbri/FfbUsage.hpp"

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <string>
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

// Coefficients beyond full scale (a game asking for more than there is) count as capped, on either side.
TEST(FfbUsage, ConditionCoefficientsBeyondTheRangeCountAsCapped) {
    FfbUsage usage(safety());
    std::array<FfbUsage::SlotSample, 1> slots{};
    FfbEffect strong = spring(0.5F);
    strong.condition.positiveCoefficient = 1.5F;
    slots[0] = {true, strong, spring(1.0F)};
    usage.record(10 * kMs, true, slots);
    strong.condition.positiveCoefficient = 0.5F;
    strong.condition.negativeCoefficient = -1.5F;
    slots[0] = {true, strong, spring(1.0F)};
    usage.record(10 * kMs, true, slots);
    slots[0] = {true, spring(0.9F), spring(0.9F)};   // within the range: not capped
    usage.record(10 * kMs, true, slots);
    EXPECT_EQ(usage.type(FfbEffectType::Spring).cappedNs, 20 * kMs);
    EXPECT_EQ(usage.type(FfbEffectType::Spring).slewNs, 0U);   // only a constant force slews
}

// A periodic effect whose offset would take it past the cap, or whose magnitude alone does, counts as capped.
TEST(FfbUsage, PeriodicOffsetOrMagnitudePastTheCapCountsAsCapped) {
    FfbUsage usage(safety(1.0F, 0.8F));
    std::array<FfbUsage::SlotSample, 1> slots{};
    FfbEffect sine;
    sine.type = FfbEffectType::Sine;
    sine.periodic.magnitude = 0.5F;
    sine.periodic.offset = -0.4F;   // 0.9 in all
    slots[0] = {true, sine, sine};
    usage.record(10 * kMs, true, slots);
    sine.periodic.magnitude = 0.9F;
    sine.periodic.offset = 0.0F;
    slots[0] = {true, sine, sine};
    usage.record(10 * kMs, true, slots);
    sine.periodic.magnitude = 0.5F;
    sine.periodic.offset = 0.2F;
    slots[0] = {true, sine, sine};
    usage.record(10 * kMs, true, slots);
    EXPECT_EQ(usage.type(FfbEffectType::Sine).cappedNs, 20 * kMs);
}

// A config that is not a number is counted against the limiter's defaults, the cap the wheel really had.
TEST(FfbUsage, NotANumberConfigCountsAgainstTheDefaultCap) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    FfbUsage usage(safety(nan, nan));
    std::array<FfbUsage::SlotSample, 1> slots{};
    slots[0] = {true, constant(1.5F), constant(1.0F)};   // above the default cap 1
    usage.record(10 * kMs, true, slots);
    slots[0] = {true, constant(0.7F), constant(0.7F)};
    usage.record(10 * kMs, true, slots);
    EXPECT_EQ(usage.type(FfbEffectType::Constant).cappedNs, 10 * kMs);
    EXPECT_EQ(usage.type(FfbEffectType::Constant).slewNs, 0U);
}

// Nothing played yet: averages are 0, the table shows dashes, the histogram says so.
TEST(FfbUsage, NothingPlayedYet) {
    const FfbUsage usage(safety());
    EXPECT_TRUE(usage.empty());
    EXPECT_TRUE(usage.usedTypes().empty());
    EXPECT_EQ(usage.type(FfbEffectType::Damper).askedAverage(), 0.0F);
    EXPECT_EQ(usage.type(FfbEffectType::Damper).wheelAverage(), 0.0F);
    const std::string row = usage.tableRow(FfbEffectType::Constant);
    EXPECT_NE(row.find("0:00:00"), std::string::npos);
    EXPECT_NE(row.find(" 0%"), std::string::npos);    // share of no game time
    EXPECT_EQ(row.substr(row.size() - 1), "-");       // no slew to tell
    EXPECT_EQ(usage.histogramLine(), "constant force strength the game asked for: none yet");
}

// Slew is only a constant force's: another type's row has a dash there, its capped share a number.
TEST(FfbUsage, OnlyTheConstantRowHasASlewShare) {
    FfbUsage usage(safety());
    std::array<FfbUsage::SlotSample, 1> slots{};
    slots[0] = {true, spring(0.5F), spring(0.5F)};
    usage.record(10 * kMs, false, slots);   // the game not (yet) counted as active: share 0
    const std::string row = usage.tableRow(FfbEffectType::Spring);
    EXPECT_EQ(row.substr(row.size() - 1), "-");
    EXPECT_NE(row.find("0%"), std::string::npos);
    EXPECT_EQ(usage.gameNs(), 0U);
    EXPECT_EQ(usage.anyForceNs(), 10 * kMs);
}

// More slots than the controller has (a caller with a bigger array): their time counts, their starts cannot be told.
TEST(FfbUsage, SlotsBeyondTheControllersCountTimeButNotStarts) {
    FfbUsage usage(safety());
    std::array<FfbUsage::SlotSample, IFfbSink::kMaxSlots + 2> slots{};
    slots[IFfbSink::kMaxSlots] = {true, spring(0.4F), spring(0.4F)};
    usage.record(10 * kMs, true, slots);
    slots[IFfbSink::kMaxSlots].playing = false;
    usage.record(10 * kMs, true, slots);
    EXPECT_EQ(usage.type(FfbEffectType::Spring).starts, 0U);
    EXPECT_EQ(usage.type(FfbEffectType::Spring).playNs, 10 * kMs);
    const auto used = usage.usedTypes();
    ASSERT_EQ(used.size(), 1U);
    EXPECT_EQ(used[0], FfbEffectType::Spring);
}

// A sample of a type this build does not know is left out (no index past the table).
TEST(FfbUsage, AnUnknownTypeIsLeftOut) {
    FfbUsage usage(safety());
    std::array<FfbUsage::SlotSample, 1> slots{};
    FfbEffect unknown = constant(0.5F);
    unknown.type = static_cast<FfbEffectType>(FfbUsage::kTypes + 3);
    slots[0] = {true, unknown, unknown};
    usage.record(10 * kMs, true, slots);
    EXPECT_TRUE(usage.empty());
    EXPECT_TRUE(usage.usedTypes().empty());
    EXPECT_EQ(usage.gameNs(), 10 * kMs);
}

// A constant force that is not a number (never meant) does not land in the histogram or the peaks.
TEST(FfbUsage, NotANumberStrengthCountsAsNone) {
    FfbUsage usage(safety());
    std::array<FfbUsage::SlotSample, 1> slots{};
    slots[0] = {true, constant(std::numeric_limits<float>::quiet_NaN()), constant(0.0F)};
    usage.record(10 * kMs, true, slots);
    EXPECT_EQ(usage.type(FfbEffectType::Constant).askedPeak, 0.0F);
    EXPECT_EQ(usage.type(FfbEffectType::Constant).playNs, 10 * kMs);
}

// Two slots of one type, the first already capped: the type counts as capped for that time once.
TEST(FfbUsage, OneCappedSlotOfATypeIsEnough) {
    FfbUsage usage(safety(1.0F, 0.8F));
    std::array<FfbUsage::SlotSample, 2> slots{};
    slots[0] = {true, constant(0.95F), constant(0.8F)};
    slots[1] = {true, constant(0.2F), constant(0.2F)};
    usage.record(10 * kMs, true, slots);
    EXPECT_EQ(usage.type(FfbEffectType::Constant).cappedNs, 10 * kMs);
    EXPECT_EQ(usage.type(FfbEffectType::Constant).playNs, 10 * kMs);
}

}  // namespace
}  // namespace wheelbri
