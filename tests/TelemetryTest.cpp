#include "wheelbri/Telemetry.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace wheelbri {
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
               ("wheelbri-telemetry-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
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

    // Every line, or only the rows (the lines whose message starts with t_ms=).
    static std::vector<std::string> lines(const fs::path& path, const bool all = false) {
        std::ifstream file(path);
        std::vector<std::string> result;
        for (std::string line; std::getline(file, line);) {
            if (all || message(line).rfind("t_ms=", 0) == 0) {
                result.push_back(line);
            }
        }
        return result;
    }

    // The part after "[date time] [level] [component] ".
    static std::string message(const std::string& line) {
        std::size_t at = 0;
        for (int i = 0; i < 3; ++i) {
            at = line.find("] ", at);
            if (at == std::string::npos) return {};
            at += 2;
        }
        return line.substr(at);
    }

    // The value of `key` in a row ("" when the row has no such key).
    static std::string value(const std::string& line, const std::string& key) {
        const std::string text = " " + message(line);
        const std::size_t at = text.find(" " + key + "=");
        if (at == std::string::npos) return {};
        const std::size_t start = at + key.size() + 2;
        return text.substr(start, text.find(' ', start) - start);
    }

    static std::size_t count(const std::vector<std::string>& all, const std::string& part) {
        return static_cast<std::size_t>(std::count_if(
            all.begin(), all.end(), [&](const std::string& line) { return message(line).rfind(part, 0) == 0; }));
    }

    fs::path dir_;
};

TEST_F(TelemetryTest, OneFilePerDayNamedByDate) {
    auto recorder = open(at(14, 25, 1));
    ASSERT_NE(recorder, nullptr);
    EXPECT_EQ(recorder->samplesPath().filename(), "telemetry-2026-09-30.log");
    EXPECT_EQ(recorder->ffbPath().filename(), "ffb-2026-09-30.log");
    recorder.reset();

    const auto all = lines(dir_ / "telemetry-2026-09-30.log", true);
    EXPECT_EQ(count(all, "wheelbri telemetry format 3"), 1U);
    EXPECT_EQ(count(all, "session started 2026-09-30 14:25:01"), 1U);
    EXPECT_EQ(count(all, "source: test wheel"), 1U);
    // Every line in the other logs' format.
    for (const auto& line : all) {
        EXPECT_EQ(line.rfind("[2026-09-30 14:25:01.000] [info ] [telemetry     ] ", 0), 0U) << line;
    }
    EXPECT_TRUE(lines(dir_ / "telemetry-2026-09-30.log").empty());   // no rows yet
}

TEST_F(TelemetryTest, SessionsOfTheSameDayAppendWithOneColumnRow) {
    for (const int hour : {9, 20}) {
        auto recorder = open(at(hour, 0, 0));
        ASSERT_NE(recorder, nullptr);
        TelemetrySample sample;
        sample.nowNs = 5 * kMs;
        recorder->writeSample(sample);
    }
    const auto all = lines(dir_ / "telemetry-2026-09-30.log", true);
    EXPECT_EQ(count(all, "wheelbri telemetry format"), 1U);   // what the keys mean: once, in a new file
    EXPECT_EQ(count(all, "session started 2026-09-30 09:00:00"), 1U);
    EXPECT_EQ(count(all, "session started 2026-09-30 20:00:00"), 1U);
    const auto data = lines(dir_ / "telemetry-2026-09-30.log");
    ASSERT_EQ(data.size(), 2U);
    EXPECT_EQ(data[0].rfind("[2026-09-30 09:00:00.000] [info ] [telemetry     ] t_ms=0.0 ", 0), 0U) << data[0];
    EXPECT_EQ(data[1].rfind("[2026-09-30 20:00:00.000] [info ] [telemetry     ] t_ms=0.0 ", 0), 0U) << data[1];
    EXPECT_TRUE(lines(dir_ / "ffb-2026-09-30.log").empty());
}

TEST_F(TelemetryTest, RowsHaveTheirKeysWithAndWithoutFfb) {
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

    const auto data = lines(dir_ / "telemetry-2026-09-30.log");
    ASSERT_EQ(data.size(), 2U);
    EXPECT_EQ(data[0].rfind("[2026-09-30 10:00:00.000] ", 0), 0U);
    EXPECT_EQ(value(data[0], "t_ms"), "0.0");
    EXPECT_EQ(value(data[0], "raw_a2"), "-0.5000");
    EXPECT_EQ(value(data[0], "raw_buttons"), "800000001");   // in hex
    EXPECT_EQ(value(data[0], "out_throttle"), "49151");
    EXPECT_EQ(value(data[0], "ffb_packets"), "");            // force feedback off: no ffb_* keys
    EXPECT_EQ(value(data[0], "s1_type"), "");
    EXPECT_EQ(value(data[0], "period_us"), "0");
    EXPECT_EQ(data[1].rfind("[2026-09-30 10:00:00.020] ", 0), 0U);
    EXPECT_EQ(value(data[1], "t_ms"), "20.0");
    EXPECT_EQ(value(data[1], "s1_type"), "constant");
    EXPECT_EQ(value(data[1], "s1_state"), "wp");
    EXPECT_EQ(value(data[1], "s1_req"), "0.5000");
    EXPECT_EQ(value(data[1], "s0_type"), "");                 // an idle slot is left out
    EXPECT_EQ(value(data[1], "ffb_watchdog"), "0");
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
    EXPECT_EQ(recorder->samplesPath().filename(), "telemetry-2026-10-01.log");
    recorder.reset();

    const auto before = lines(dir_ / "telemetry-2026-09-30.log");
    const auto after = lines(dir_ / "telemetry-2026-10-01.log");
    ASSERT_EQ(before.size(), 1U);
    ASSERT_EQ(after.size(), 1U);
    EXPECT_EQ(before[0].rfind("[2026-09-30 23:59:59.900] [info ] [telemetry     ] t_ms=0.0 ", 0), 0U) << before[0];
    EXPECT_EQ(after[0].rfind("[2026-10-01 00:00:00.050] [info ] [telemetry     ] t_ms=150.0 ", 0), 0U) << after[0];
    const auto afterAll = lines(dir_ / "telemetry-2026-10-01.log", true);
    EXPECT_EQ(count(afterAll, "session started 2026-09-30 23:59:59, continued"), 1U);
    EXPECT_EQ(count(afterAll, "wheelbri telemetry format"), 1U);
    EXPECT_EQ(lines(dir_ / "ffb-2026-09-30.log").size(), 1U);
    EXPECT_EQ(lines(dir_ / "ffb-2026-10-01.log").size(), 1U);
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

TEST_F(TelemetryTest, ExtremeNumbersAreWrittenInFull) {
    auto recorder = open(at(10, 0, 0));
    ASSERT_NE(recorder, nullptr);
    FfbPacket packet;
    packet.kind = FfbPacket::Kind::SlotState;
    packet.slot = 1;
    packet.playing = true;
    packet.effect.type = FfbEffectType::Spring;
    packet.effect.condition.center = 3.0e38F;   // finite, so the decoder lets it through; prints as ~45 characters
    packet.effect.condition.deadband = -3.0e38F;
    packet.effect.condition.positiveCoefficient = 3.0e38F;
    packet.effect.condition.negativeCoefficient = -3.0e38F;
    packet.effect.condition.positiveSaturation = 3.0e38F;
    packet.effect.condition.negativeSaturation = -3.0e38F;
    recorder->writeFfbPacket(0, packet);
    recorder.reset();
    const auto data = lines(dir_ / "ffb-2026-09-30.log");
    ASSERT_EQ(data.size(), 1U);
    EXPECT_GT(value(data[0], "center").size(), 30U) << data[0];   // not cut
    EXPECT_EQ(value(data[0], "phase_deg"), "0.0") << data[0];      // and the keys after it are still there
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

    const auto data = lines(dir_ / "ffb-2026-09-30.log");
    ASSERT_EQ(data.size(), 3U);  // 2 packets + malformed note
    EXPECT_EQ(data[0].rfind("[2026-09-30 10:00:00.002] [info ] [ffb           ] t_ms=2.0 kind=slot seq=42 slot=3 "
                            "playing=1 type=saw_up ", 0), 0U) << data[0];
    EXPECT_EQ(value(data[0], "magnitude"), "0.3000");
    EXPECT_EQ(value(data[1], "kind"), "stopall");
    EXPECT_EQ(value(data[1], "type"), "");   // nothing of an effect for stopall
    EXPECT_EQ(value(data[2], "kind"), "malformed");
    EXPECT_EQ(value(data[2], "bytes"), "12");
}

TEST_F(TelemetryTest, CallsToTheWheelHaveTheirDelay) {
    auto recorder = open(at(10, 0, 0));
    ASSERT_NE(recorder, nullptr);
    AsyncFfbSink::Applied applied;
    applied.kind = AsyncFfbSink::Applied::Kind::Play;
    applied.slot = 2;
    applied.type = FfbEffectType::Spring;
    applied.waitUs = 120;
    applied.callUs = 4100;
    recorder->writeFfbApplied(5 * kMs, applied);
    applied.kind = AsyncFfbSink::Applied::Kind::StopAll;
    recorder->writeFfbApplied(6 * kMs, applied);
    recorder.reset();

    const auto data = lines(dir_ / "ffb-2026-09-30.log");
    ASSERT_EQ(data.size(), 2U);
    EXPECT_NE(data[0].find(" kind=wheel_play slot=2 ok=1 type=spring wait_us=120 call_us=4100"), std::string::npos)
        << data[0];
    EXPECT_EQ(value(data[1], "kind"), "wheel_stopall");
    EXPECT_EQ(value(data[1], "slot"), "");
}

TEST_F(TelemetryTest, WritesRowsOnlyWhenAValueChanges) {
    auto recorder = open(at(10, 0, 0));
    ASSERT_NE(recorder, nullptr);
    TelemetrySample sample;
    for (int i = 0; i < 10; ++i) {  // nothing moves: one row
        sample.nowNs = static_cast<std::uint64_t>(i) * 20 * kMs;
        sample.periodUs = 1000 + i;  // timing and sequence changes do not count
        sample.sequence = static_cast<std::uint32_t>(i);
        recorder->writeSample(sample);
    }
    sample.nowNs = 200 * kMs;
    sample.raw.axes[0] = 0.25F;  // the wheel moves: a row
    sample.state.steering = 0.25F;
    recorder->writeSample(sample);
    sample.nowNs = 220 * kMs;
    recorder->writeSample(sample);  // still: skipped
    sample.nowNs = 240 * kMs;
    sample.raw.axes[0] = 0.2502F;   // sensor noise below min_change: skipped
    sample.state.steering = 0.2502F;
    sample.report.steering += 3;
    recorder->writeSample(sample);
    EXPECT_EQ(recorder->rowsWritten(), 2U);
    EXPECT_EQ(recorder->rowsSkipped(), 11U);
    recorder.reset();

    const auto data = lines(dir_ / "telemetry-2026-09-30.log");
    ASSERT_EQ(data.size(), 2U);
    EXPECT_EQ(value(data[0], "t_ms"), "0.0");
    EXPECT_EQ(value(data[1], "t_ms"), "200.0");
    EXPECT_EQ(value(data[1], "raw_a0"), "0.2500");
}

TEST_F(TelemetryTest, HeartbeatRowWhenNothingChanges) {
    TelemetryConfig config;
    config.heartbeatSeconds = 1;
    auto recorder = open(at(10, 0, 0), config);
    ASSERT_NE(recorder, nullptr);
    TelemetrySample sample;
    for (int i = 0; i <= 125; ++i) {  // 2.5 s without a change at 50 rows/s
        sample.nowNs = static_cast<std::uint64_t>(i) * 20 * kMs;
        recorder->writeSample(sample);
    }
    EXPECT_EQ(recorder->rowsWritten(), 3U);  // start, +1 s, +2 s
}

TEST_F(TelemetryTest, EveryRowWhenOnlyChangesIsOff) {
    TelemetryConfig config;
    config.onlyChanges = false;
    auto recorder = open(at(10, 0, 0), config);
    ASSERT_NE(recorder, nullptr);
    TelemetrySample sample;
    for (int i = 0; i < 5; ++i) {
        sample.nowNs = static_cast<std::uint64_t>(i) * 20 * kMs;
        recorder->writeSample(sample);
    }
    EXPECT_EQ(recorder->rowsWritten(), 5U);
}

TEST_F(TelemetryTest, FfbEventsOnlyForRealChanges) {
    auto recorder = open(at(10, 0, 0));
    ASSERT_NE(recorder, nullptr);
    FfbPacket packet;
    packet.kind = FfbPacket::Kind::SlotState;
    packet.slot = 0;
    packet.playing = true;
    packet.effect.constantLevel = 0.2F;
    recorder->writeFfbPacket(0, packet);
    recorder->writeFfbPacket(250 * kMs, packet);  // periodic re-send: skipped
    packet.effect.constantLevel = 0.3F;
    recorder->writeFfbPacket(300 * kMs, packet);
    packet.slot = 1;
    recorder->writeFfbPacket(310 * kMs, packet);  // same effect, other slot: a change
    packet.kind = FfbPacket::Kind::StopAll;
    recorder->writeFfbPacket(320 * kMs, packet);
    packet.kind = FfbPacket::Kind::SlotState;
    recorder->writeFfbPacket(330 * kMs, packet);  // after StopAll the slot starts again
    EXPECT_EQ(recorder->ffbRowsWritten(), 5U);
}

TEST_F(TelemetryTest, StopsAtMaxFileSize) {
    TelemetryConfig config;
    config.maxFileMb = 1;
    config.onlyChanges = false;
    auto recorder = open(at(10, 0, 0), config);
    ASSERT_NE(recorder, nullptr);
    TelemetrySample sample;
    for (int i = 0; i < 10000; ++i) {  // ~400 bytes a row: well past 1 MB
        sample.nowNs = static_cast<std::uint64_t>(i) * 20 * kMs;
        recorder->writeSample(sample);
    }
    recorder.reset();
    const auto path = dir_ / "telemetry-2026-09-30.log";
    EXPECT_LE(fs::file_size(path), 1024U * 1024U + 128U);
    EXPECT_EQ(count(lines(path, true), "stopped: max_file_mb reached"), 1U);
}

}  // namespace
}  // namespace wheelbri
