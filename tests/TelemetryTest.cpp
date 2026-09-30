#include "wheelemu/Telemetry.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace wheelemu {
namespace {

namespace fs = std::filesystem;

constexpr std::uint64_t kMs = 1'000'000;

class NullSink : public IFfbSink {
public:
    bool play(std::size_t, const FfbEffect&) override { return true; }
    void stop(std::size_t) override {}
    void stopAll() override {}
};

// Local wall-clock time on 2026-09-30 (the test works in any time zone).
WallClock::time_point at(const int hour, const int minute, const int second, const int ms = 0) {
    return *startOfDay("2026-09-30") + std::chrono::hours(hour) + std::chrono::minutes(minute) +
           std::chrono::seconds(second) + std::chrono::milliseconds(ms);
}

class TelemetryTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("wheelemu-telemetry-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    std::unique_ptr<TelemetryRecorder> open(const WallClock::time_point start, TelemetryConfig config = {}) {
        std::string error;
        auto recorder = TelemetryRecorder::open(config, dir_, start, {"source: test wheel"}, error);
        EXPECT_NE(recorder, nullptr) << error;
        return recorder;
    }

    static std::vector<std::string> lines(const fs::path& path, const bool withComments = false) {
        std::ifstream file(path);
        std::vector<std::string> result;
        for (std::string line; std::getline(file, line);) {
            if (withComments || (!line.empty() && line[0] != '#')) {
                result.push_back(line);
            }
        }
        return result;
    }

    static std::size_t fields(const std::string& line) {
        return static_cast<std::size_t>(std::count(line.begin(), line.end(), ',')) + 1;
    }

    static std::size_t count(const std::vector<std::string>& all, const std::string& prefix) {
        return static_cast<std::size_t>(std::count_if(
            all.begin(), all.end(), [&](const std::string& line) { return line.rfind(prefix, 0) == 0; }));
    }

    fs::path dir_;
};

TEST_F(TelemetryTest, OneFilePerDayNamedByDate) {
    auto recorder = open(at(14, 25, 1));
    ASSERT_NE(recorder, nullptr);
    EXPECT_EQ(recorder->samplesPath().filename(), "telemetry-2026-09-30.csv");
    EXPECT_EQ(recorder->ffbPath().filename(), "ffb-2026-09-30.csv");
    recorder.reset();

    const auto all = lines(dir_ / "telemetry-2026-09-30.csv", true);
    EXPECT_EQ(count(all, "# wheelemu telemetry format 2"), 1U);
    EXPECT_EQ(count(all, "# session started 2026-09-30 14:25:01"), 1U);
    EXPECT_EQ(count(all, "# source: test wheel"), 1U);
    const auto data = lines(dir_ / "telemetry-2026-09-30.csv");
    ASSERT_EQ(data.size(), 1U);
    EXPECT_EQ(data[0].rfind("time,t_ms,seq,raw_a0", 0), 0U);
    EXPECT_EQ(fields(data[0]), telemetryColumns().size());
}

TEST_F(TelemetryTest, SessionsOfTheSameDayAppendWithOneColumnRow) {
    for (const int hour : {9, 20}) {
        auto recorder = open(at(hour, 0, 0));
        ASSERT_NE(recorder, nullptr);
        TelemetrySample sample;
        sample.nowNs = 5 * kMs;
        recorder->writeSample(sample);
    }
    const auto all = lines(dir_ / "telemetry-2026-09-30.csv", true);
    EXPECT_EQ(count(all, "time,"), 1U);
    EXPECT_EQ(count(all, "# session started 2026-09-30 09:00:00"), 1U);
    EXPECT_EQ(count(all, "# session started 2026-09-30 20:00:00"), 1U);
    const auto data = lines(dir_ / "telemetry-2026-09-30.csv");
    ASSERT_EQ(data.size(), 3U);
    EXPECT_EQ(data[1].rfind("09:00:00.000,0.0,", 0), 0U);
    EXPECT_EQ(data[2].rfind("20:00:00.000,0.0,", 0), 0U);
    EXPECT_EQ(lines(dir_ / "ffb-2026-09-30.csv").size(), 1U);  // just the column row
}

TEST_F(TelemetryTest, RowsHaveOneFieldPerColumnWithAndWithoutFfb) {
    auto recorder = open(at(10, 0, 0));
    ASSERT_NE(recorder, nullptr);

    TelemetrySample sample;
    sample.nowNs = 1000 * kMs;
    sample.raw.axes[2] = -0.5F;
    sample.raw.buttons = (1ULL << 35) | 1;
    sample.state.throttle = 0.25F;
    sample.report.throttle = 49151;
    recorder->writeSample(sample);

    NullSink sink;
    FfbController controller(sink, FfbSafetyConfig{});
    FfbPacket packet;
    packet.kind = FfbPacket::Kind::SlotState;
    packet.slot = 1;
    packet.playing = true;
    packet.effect.constantLevel = 0.5F;
    controller.onPacket(packet, 1010 * kMs);
    controller.update(1010 * kMs);
    sample.ffb = &controller;
    sample.nowNs = 1020 * kMs;
    recorder->writeSample(sample);
    recorder.reset();

    const auto data = lines(dir_ / "telemetry-2026-09-30.csv");
    ASSERT_EQ(data.size(), 3U);
    EXPECT_EQ(fields(data[1]), telemetryColumns().size());
    EXPECT_EQ(fields(data[2]), telemetryColumns().size());
    EXPECT_EQ(data[1].rfind("10:00:00.000,0.0,", 0), 0U);
    EXPECT_NE(data[1].find(",800000001,"), std::string::npos);  // raw buttons in hex
    EXPECT_NE(data[1].find(",49151,"), std::string::npos);
    EXPECT_EQ(data[2].rfind("10:00:00.020,20.0,", 0), 0U);
    EXPECT_NE(data[2].find(",constant,wp,0.5000,"), std::string::npos);
}

TEST_F(TelemetryTest, SessionContinuesInTheNextDaysFileAfterMidnight) {
    auto recorder = open(at(23, 59, 59, 900));
    ASSERT_NE(recorder, nullptr);
    TelemetrySample sample;
    sample.nowNs = 0;
    recorder->writeSample(sample);
    FfbPacket packet;
    packet.kind = FfbPacket::Kind::SlotState;
    recorder->writeFfbPacket(50 * kMs, packet);
    sample.nowNs = 150 * kMs;  // 00:00:00.050 on the next day
    recorder->writeSample(sample);
    recorder->writeFfbPacket(160 * kMs, packet);
    EXPECT_EQ(recorder->day(), "2026-10-01");
    EXPECT_EQ(recorder->samplesPath().filename(), "telemetry-2026-10-01.csv");
    recorder.reset();

    const auto before = lines(dir_ / "telemetry-2026-09-30.csv");
    const auto after = lines(dir_ / "telemetry-2026-10-01.csv");
    ASSERT_EQ(before.size(), 2U);
    ASSERT_EQ(after.size(), 2U);
    EXPECT_EQ(before[1].rfind("23:59:59.900,0.0,", 0), 0U);
    EXPECT_EQ(after[0].rfind("time,", 0), 0U);
    EXPECT_EQ(after[1].rfind("00:00:00.050,150.0,", 0), 0U);
    EXPECT_EQ(count(lines(dir_ / "telemetry-2026-10-01.csv", true), "# session started 2026-09-30 23:59:59, continued"),
              1U);
    EXPECT_EQ(lines(dir_ / "ffb-2026-09-30.csv").size(), 2U);
    EXPECT_EQ(lines(dir_ / "ffb-2026-10-01.csv").size(), 2U);
}

TEST_F(TelemetryTest, DueFollowsTheRate) {
    TelemetryConfig config;
    config.rateHz = 100;  // every 10 ms
    auto recorder = open(at(10, 0, 0), config);
    ASSERT_NE(recorder, nullptr);
    EXPECT_TRUE(recorder->due(0));

    TelemetrySample sample;
    sample.nowNs = 5 * kMs;
    recorder->writeSample(sample);
    EXPECT_FALSE(recorder->due(9 * kMs));
    EXPECT_TRUE(recorder->due(15 * kMs));
    sample.nowNs = 16 * kMs;  // a late row does not shift the grid
    recorder->writeSample(sample);
    EXPECT_TRUE(recorder->due(25 * kMs));
}

TEST_F(TelemetryTest, FfbEventsSkipKeepAlives) {
    auto recorder = open(at(10, 0, 0));
    ASSERT_NE(recorder, nullptr);
    FfbPacket packet;
    packet.kind = FfbPacket::Kind::KeepAlive;
    recorder->writeFfbPacket(0, packet);
    packet.kind = FfbPacket::Kind::SlotState;
    packet.slot = 3;
    packet.sequence = 42;
    packet.playing = true;
    packet.effect.type = FfbEffectType::SawtoothUp;
    packet.effect.periodic.magnitude = 0.3F;
    recorder->writeFfbPacket(2 * kMs, packet);
    packet.kind = FfbPacket::Kind::StopAll;
    recorder->writeFfbPacket(3 * kMs, packet);
    recorder->noteMalformedFfb(4 * kMs, 12);
    EXPECT_EQ(recorder->ffbRowsWritten(), 2U);
    recorder.reset();

    const auto data = lines(dir_ / "ffb-2026-09-30.csv");
    ASSERT_EQ(data.size(), 4U);  // column row + 2 packets + malformed note
    const std::size_t columns = fields(data[0]);
    for (const auto& line : data) {
        EXPECT_EQ(fields(line), columns) << line;
    }
    EXPECT_EQ(data[1].rfind("10:00:00.002,2.0,slot,3,42,1,saw_up,", 0), 0U);
    EXPECT_NE(data[1].find(",0.3000,"), std::string::npos);
    EXPECT_NE(data[2].find(",stopall,"), std::string::npos);
    EXPECT_NE(data[3].find("malformed_12_bytes"), std::string::npos);
}

TEST_F(TelemetryTest, StopsAtMaxFileSize) {
    TelemetryConfig config;
    config.maxFileMb = 1;
    auto recorder = open(at(10, 0, 0), config);
    ASSERT_NE(recorder, nullptr);
    TelemetrySample sample;
    for (int i = 0; i < 10000; ++i) {  // ~170 bytes a row: well past 1 MB
        sample.nowNs = static_cast<std::uint64_t>(i) * 20 * kMs;
        recorder->writeSample(sample);
    }
    recorder.reset();
    const auto path = dir_ / "telemetry-2026-09-30.csv";
    EXPECT_LE(fs::file_size(path), 1024U * 1024U + 64U);
    EXPECT_EQ(count(lines(path, true), "# stopped: max_file_mb reached"), 1U);
}

}  // namespace
}  // namespace wheelemu
