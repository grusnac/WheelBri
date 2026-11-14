// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/Telemetry.hpp"
#include "wheelbri/LogFiles.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#ifndef _WIN32
#include <csignal>
#include <cstdlib>
#include <sys/resource.h>
#endif

namespace wheelbri {
namespace {

namespace fs = std::filesystem;

constexpr std::uint64_t kMs = 1'000'000;
constexpr std::uint64_t kSecond = 1000 * kMs;

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
        logfiles::forgetRun();   // each test is a new run: its files are -01
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
    EXPECT_EQ(recorder->samplesPath().filename(), "telemetry-2026-09-30-01.log");
    EXPECT_EQ(recorder->ffbPath().filename(), "ffb-2026-09-30-01.log");
    recorder.reset();

    const auto all = lines(dir_ / "telemetry-2026-09-30-01.log", true);
    EXPECT_EQ(count(all, "wheelbri telemetry format 4"), 1U);
    EXPECT_EQ(count(all, "session started 2026-09-30 14:25:01"), 1U);
    EXPECT_EQ(count(all, "source: test wheel"), 1U);
    // Every line in the other logs' format.
    for (const auto& line : all) {
        EXPECT_EQ(line.rfind("[2026-09-30 14:25:01.000] [info ] [telemetry     ] ", 0), 0U) << line;
    }
    EXPECT_TRUE(lines(dir_ / "telemetry-2026-09-30-01.log").empty());   // no rows yet
}

TEST_F(TelemetryTest, SessionsOfTheSameDayAppendWithOneColumnRow) {
    for (const int hour : {9, 20}) {
        auto recorder = open(at(hour, 0, 0));
        ASSERT_NE(recorder, nullptr);
        TelemetrySample sample;
        sample.nowNs = 5 * kMs;
        recorder->writeSample(sample);
    }
    const auto all = lines(dir_ / "telemetry-2026-09-30-01.log", true);
    EXPECT_EQ(count(all, "wheelbri telemetry format"), 1U);   // what the keys mean: once, in a new file
    EXPECT_EQ(count(all, "session started 2026-09-30 09:00:00"), 1U);
    EXPECT_EQ(count(all, "session started 2026-09-30 20:00:00"), 1U);
    const auto data = lines(dir_ / "telemetry-2026-09-30-01.log");
    ASSERT_EQ(data.size(), 2U);
    EXPECT_EQ(data[0].rfind("[2026-09-30 09:00:00.000] [info ] [telemetry     ] t_ms=0.0 ", 0), 0U) << data[0];
    EXPECT_EQ(data[1].rfind("[2026-09-30 20:00:00.000] [info ] [telemetry     ] t_ms=0.0 ", 0), 0U) << data[1];
    EXPECT_TRUE(lines(dir_ / "ffb-2026-09-30-01.log").empty());
}

TEST_F(TelemetryTest, RowsHaveTheirKeysWithAndWithoutFfb) {
    auto recorder = open(at(10, 0, 0));
    ASSERT_NE(recorder, nullptr);

    TelemetrySample sample;
    sample.nowNs = 1000 * kMs;
    sample.raw.axes[2] = -0.5F;
    sample.raw.buttons = SourceButtons::fromBits((1ULL << 35) | 1);
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
    sample.raw.buttons.set(119);   // past 63: the column grows to 128 bits, still bit n = button n
    recorder->writeSample(sample);
    recorder.reset();

    const auto data = lines(dir_ / "telemetry-2026-09-30-01.log");
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
    EXPECT_EQ(value(data[1], "raw_buttons"), "80000000000000" "0000000800000001");
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
    EXPECT_EQ(recorder->samplesPath().filename(), "telemetry-2026-10-01-01.log");
    recorder.reset();

    const auto before = lines(dir_ / "telemetry-2026-09-30-01.log");
    const auto after = lines(dir_ / "telemetry-2026-10-01-01.log");
    ASSERT_EQ(before.size(), 1U);
    ASSERT_EQ(after.size(), 1U);
    EXPECT_EQ(before[0].rfind("[2026-09-30 23:59:59.900] [info ] [telemetry     ] t_ms=0.0 ", 0), 0U) << before[0];
    EXPECT_EQ(after[0].rfind("[2026-10-01 00:00:00.050] [info ] [telemetry     ] t_ms=150.0 ", 0), 0U) << after[0];
    const auto afterAll = lines(dir_ / "telemetry-2026-10-01-01.log", true);
    EXPECT_EQ(count(afterAll, "session started 2026-09-30 23:59:59, continued"), 1U);
    EXPECT_EQ(count(afterAll, "wheelbri telemetry format"), 1U);
    EXPECT_EQ(lines(dir_ / "ffb-2026-09-30-01.log").size(), 1U);
    EXPECT_EQ(lines(dir_ / "ffb-2026-10-01-01.log").size(), 1U);
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
    const auto data = lines(dir_ / "ffb-2026-09-30-01.log");
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

    const auto data = lines(dir_ / "ffb-2026-09-30-01.log");
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

    const auto data = lines(dir_ / "ffb-2026-09-30-01.log");
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

    const auto data = lines(dir_ / "telemetry-2026-09-30-01.log");
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

TEST_F(TelemetryTest, ANewRunOfTheProgramStartsTheNextNumberedFiles) {
    auto recorder = open(at(10, 0, 0));
    ASSERT_NE(recorder, nullptr);
    EXPECT_EQ(recorder->samplesPath().filename(), "telemetry-2026-09-30-01.log");
    recorder.reset();
    recorder = open(at(11, 0, 0));   // another session of the same run: the same files
    ASSERT_NE(recorder, nullptr);
    EXPECT_EQ(recorder->samplesPath().filename(), "telemetry-2026-09-30-01.log");
    recorder.reset();
    logfiles::forgetRun();           // the program started again
    recorder = open(at(12, 0, 0));
    ASSERT_NE(recorder, nullptr);
    EXPECT_EQ(recorder->samplesPath().filename(), "telemetry-2026-09-30-02.log");
    EXPECT_EQ(recorder->ffbPath().filename(), "ffb-2026-09-30-02.log");
}

TEST_F(TelemetryTest, NoFileCanBeMadeIsAnErrorNotACrash) {
    std::ofstream(dir_.string()) << "a file where the folder should be";
    TelemetryConfig config;
    std::string error;
    EXPECT_EQ(TelemetryRecorder::open(config, dir_, at(10, 0, 0), {}, error), nullptr);
    EXPECT_NE(error, "");
    fs::remove(dir_);
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
    const auto path = dir_ / "telemetry-2026-09-30-01.log";
    EXPECT_LE(fs::file_size(path), 1024U * 1024U + 128U);
    EXPECT_EQ(count(lines(path, true), "stopped: max_file_mb reached"), 1U);
}

// The day's numbers are used up (a file numbered 99999 is there): no file is made, and open says why.
TEST_F(TelemetryTest, NoFreeNumberForTheDayIsAnError) {
    fs::create_directories(dir_);
    std::ofstream(dir_ / "telemetry-2026-09-30-99999.log") << "the last number";
    std::string error;
    EXPECT_EQ(TelemetryRecorder::open(TelemetryConfig{}, dir_, at(10, 0, 0), {}, error), nullptr);
    EXPECT_NE(error.find("no free number"), std::string::npos) << error;
    EXPECT_FALSE(fs::exists(dir_ / "ffb-2026-09-30-01.log"));
}

// The keys telemetry_summary.sh reads by name: in their order, and every key a row carries is one of them.
TEST_F(TelemetryTest, TheColumnsListEveryKeyInOrder) {
    const std::vector<std::string> columns = telemetryColumns();
    ASSERT_EQ(columns.size(), 3U + RawInputState::kMaxAxes + 21U + IFfbSink::kMaxSlots * 4U + 2U);
    EXPECT_EQ(columns[0], "time");
    EXPECT_EQ(columns[1], "t_ms");
    EXPECT_EQ(columns[2], "seq");
    EXPECT_EQ(columns[3], "raw_a0");
    EXPECT_EQ(columns[3 + RawInputState::kMaxAxes], "raw_buttons");
    EXPECT_EQ(columns[columns.size() - 3], "s" + std::to_string(IFfbSink::kMaxSlots - 1) + "_out");
    EXPECT_EQ(columns[columns.size() - 2], "period_us");
    EXPECT_EQ(columns.back(), "work_us");

    auto recorder = open(at(10, 0, 0));
    ASSERT_NE(recorder, nullptr);
    NullSink sink;
    FfbController controller(sink, FfbSafetyConfig{});
    FfbPacket packet;
    packet.kind = FfbPacket::Kind::SlotState;
    packet.slot = 0;
    packet.playing = true;
    controller.onPacket(packet, 0);
    controller.update(0);
    TelemetrySample sample;
    sample.ffb = &controller;
    recorder->writeSample(sample);
    recorder.reset();
    const auto data = lines(dir_ / "telemetry-2026-09-30-01.log");
    ASSERT_EQ(data.size(), 1U);
    std::istringstream words(message(data[0]));
    for (std::string word; words >> word;) {
        const std::string key = word.substr(0, word.find('='));
        EXPECT_NE(std::find(columns.begin(), columns.end(), key), columns.end()) << key;
    }
}

// Each mapped pedal and each value the game sees makes a row on its own, even when nothing else moved.
TEST_F(TelemetryTest, EachMappedAndReportedValueMakesARow) {
    auto recorder = open(at(10, 0, 0));
    ASSERT_NE(recorder, nullptr);
    TelemetrySample sample;
    std::uint64_t t = 0;
    const auto next = [&] {
        t += 20 * kMs;
        sample.nowNs = t;
        recorder->writeSample(sample);
    };
    next();                                  // the first row
    sample.state.throttle = 0.5F;
    next();
    sample.state.brake = 0.5F;
    next();
    sample.state.clutch = 0.5F;
    next();
    sample.report.steering = 1000;           // only what the game sees: a mapping change
    next();
    sample.report.throttle = 1000;
    next();
    sample.report.brake = 1000;
    next();
    sample.report.clutch = 1000;
    next();
    sample.report.clutch = 1002;             // two counts: below the threshold (min_change on 16 bits)
    next();
    EXPECT_EQ(recorder->rowsWritten(), 8U);
    EXPECT_EQ(recorder->rowsSkipped(), 1U);
}

// Force feedback slots as they are: wanted but not playing yet (nothing sent to the wheel: no sN_out), the watchdog
// tripped after the game went quiet, and no packet at all yet (no ffb_age_ms).
TEST_F(TelemetryTest, SlotsAndTheWatchdogAsTheyAre) {
    auto recorder = open(at(10, 0, 0));
    ASSERT_NE(recorder, nullptr);
    NullSink sink;
    FfbController controller(sink, FfbSafetyConfig{});
    TelemetrySample sample;
    sample.ffb = &controller;
    recorder->writeSample(sample);           // no packet yet

    FfbPacket packet;
    packet.kind = FfbPacket::Kind::SlotState;
    packet.slot = 2;
    packet.playing = true;
    packet.effect.type = FfbEffectType::Spring;
    controller.onPacket(packet, 100 * kMs);  // not updated yet: wanted, not playing
    sample.nowNs = 100 * kMs;
    recorder->writeSample(sample);

    controller.update(100 * kMs);
    controller.update(2000 * kMs);           // 1.9 s without a packet: the watchdog stops everything
    sample.nowNs = 2000 * kMs;
    recorder->writeSample(sample);
    recorder.reset();

    const auto data = lines(dir_ / "telemetry-2026-09-30-01.log");
    ASSERT_EQ(data.size(), 3U);
    EXPECT_EQ(value(data[0], "ffb_packets"), "0");
    EXPECT_EQ(value(data[0], "ffb_age_ms"), "");
    EXPECT_EQ(value(data[0], "ffb_watchdog"), "0");
    EXPECT_EQ(value(data[1], "s2_type"), "spring");
    EXPECT_EQ(value(data[1], "s2_state"), "w");
    EXPECT_EQ(value(data[1], "s2_out"), "");
    EXPECT_EQ(value(data[1], "ffb_age_ms"), "0");
    EXPECT_EQ(value(data[2], "ffb_watchdog"), "1");
    EXPECT_EQ(value(data[2], "ffb_age_ms"), "1900");
}

// heartbeat_s = 0: nothing that has not changed is ever written again.
TEST_F(TelemetryTest, NoHeartbeatWhenItIsZero) {
    TelemetryConfig config;
    config.heartbeatSeconds = 0;
    auto recorder = open(at(10, 0, 0), config);
    ASSERT_NE(recorder, nullptr);
    TelemetrySample sample;
    for (int i = 0; i <= 200; ++i) {   // 4 s still
        sample.nowNs = static_cast<std::uint64_t>(i) * 20 * kMs;
        recorder->writeSample(sample);
    }
    EXPECT_EQ(recorder->rowsWritten(), 1U);
}

// A packet naming a slot past the last one (a damaged or newer sender) is written each time, never indexed.
TEST_F(TelemetryTest, APacketForASlotPastTheLastIsWrittenEachTime) {
    auto recorder = open(at(10, 0, 0));
    ASSERT_NE(recorder, nullptr);
    FfbPacket packet;
    packet.kind = FfbPacket::Kind::SlotState;
    packet.slot = static_cast<std::uint8_t>(IFfbSink::kMaxSlots);
    recorder->writeFfbPacket(0, packet);
    recorder->writeFfbPacket(kMs, packet);
    EXPECT_EQ(recorder->ffbRowsWritten(), 2U);
    recorder.reset();
    const auto data = lines(dir_ / "ffb-2026-09-30-01.log");
    ASSERT_EQ(data.size(), 2U);
    EXPECT_EQ(value(data[0], "slot"), std::to_string(IFfbSink::kMaxSlots));
}

// A stop the wheel refused: kind wheel_stop with its slot, ok=0 and no effect type.
TEST_F(TelemetryTest, ARefusedStopIsWrittenAsSuch) {
    auto recorder = open(at(10, 0, 0));
    ASSERT_NE(recorder, nullptr);
    AsyncFfbSink::Applied applied;
    applied.kind = AsyncFfbSink::Applied::Kind::Stop;
    applied.slot = 4;
    applied.ok = false;
    recorder->writeFfbApplied(kMs, applied);
    recorder.reset();
    const auto data = lines(dir_ / "ffb-2026-09-30-01.log");
    ASSERT_EQ(data.size(), 1U);
    EXPECT_EQ(value(data[0], "kind"), "wheel_stop");
    EXPECT_EQ(value(data[0], "slot"), "4");
    EXPECT_EQ(value(data[0], "ok"), "0");
    EXPECT_EQ(value(data[0], "type"), "");
}

// A stray program flooding the force feedback port: one malformed line a second at most, saying how many were left
// out before it.
TEST_F(TelemetryTest, MalformedPacketsAreNotedOnceASecond) {
    auto recorder = open(at(10, 0, 0));
    ASSERT_NE(recorder, nullptr);
    recorder->noteMalformedFfb(1 * kMs, 3);
    for (int i = 0; i < 50; ++i) recorder->noteMalformedFfb((2 + static_cast<std::uint64_t>(i)) * kMs, 3);
    recorder->noteMalformedFfb(1500 * kMs, 70000);
    recorder.reset();
    const auto data = lines(dir_ / "ffb-2026-09-30-01.log");
    ASSERT_EQ(data.size(), 2U);
    EXPECT_EQ(value(data[0], "skipped_before"), "0");
    EXPECT_EQ(value(data[1], "skipped_before"), "50");
    EXPECT_EQ(value(data[1], "bytes"), "70000");
}

// A time stamp earlier than the session's first (a packet stamped before the first row was written): its t_ms is 0,
// never a wrapped-around unsigned number.
TEST_F(TelemetryTest, ATimeBeforeTheFirstIsZeroNotWrapped) {
    auto recorder = open(at(10, 0, 0));
    ASSERT_NE(recorder, nullptr);
    TelemetrySample sample;
    sample.nowNs = 100 * kMs;
    recorder->writeSample(sample);
    FfbPacket packet;
    packet.kind = FfbPacket::Kind::SlotState;
    recorder->writeFfbPacket(50 * kMs, packet);
    recorder.reset();
    const auto data = lines(dir_ / "ffb-2026-09-30-01.log");
    ASSERT_EQ(data.size(), 1U);
    EXPECT_EQ(value(data[0], "t_ms"), "0.0") << data[0];
    EXPECT_EQ(data[0].rfind("[2026-09-30 10:00:00.000] ", 0), 0U) << data[0];
}

// The folder is gone at midnight (a file in its place): the new day's rows are dropped, retried once a minute, and
// written again once the folder can be made; the earlier day's file keeps its rows.
TEST_F(TelemetryTest, ANewDayThatCannotBeOpenedIsRetriedEveryMinute) {
    auto recorder = open(at(23, 59, 59, 900));
    ASSERT_NE(recorder, nullptr);
    TelemetrySample sample;
    sample.nowNs = 0;
    recorder->writeSample(sample);
    recorder->writeFfbPacket(10 * kMs, FfbPacket{});
    const fs::path oldDay = recorder->samplesPath();
    const fs::path aside = dir_.string() + "-aside";
    fs::remove_all(aside);
    std::error_code moved;
    fs::rename(dir_, aside, moved);   // Windows refuses to move a folder whose files are open (the old day's are)
    if (moved) {
        recorder.reset();
        GTEST_SKIP() << "the log folder cannot be moved while its files are open here: " << moved.message();
    }
    std::ofstream(dir_.string()) << "a file where the folder should be";

    sample.raw.axes[0] = 0.5F;
    sample.nowNs = 1 * kSecond;                 // 00:00:00.900: the new day, its files cannot be made
    recorder->writeSample(sample);
    FfbPacket packet;
    packet.kind = FfbPacket::Kind::StopAll;
    recorder->writeFfbPacket(sample.nowNs, packet);
    sample.raw.axes[0] = 0.6F;
    sample.nowNs = 30 * kSecond;                // half a minute later: not tried again yet
    recorder->writeSample(sample);
    sample.raw.axes[0] = 0.7F;
    sample.nowNs = 62 * kSecond;                // a minute later: tried again, still a file there
    recorder->writeSample(sample);
    EXPECT_TRUE(fs::is_regular_file(dir_));

    fs::remove(dir_);
    sample.raw.axes[0] = 0.8F;
    sample.nowNs = 90 * kSecond;                // not yet a minute after the last try
    recorder->writeSample(sample);
    sample.raw.axes[0] = 0.9F;
    sample.nowNs = 125 * kSecond;               // 00:02:04.900: written again
    recorder->writeSample(sample);
    EXPECT_EQ(recorder->day(), "2026-10-01");
    EXPECT_EQ(recorder->samplesPath().parent_path(), dir_);
    const fs::path newDay = recorder->samplesPath();
    recorder.reset();

    const auto before = lines(aside / oldDay.filename());
    ASSERT_EQ(before.size(), 1U);
    const auto after = lines(newDay);
    ASSERT_EQ(after.size(), 1U);
    EXPECT_EQ(value(after[0], "raw_a0"), "0.9000");
    EXPECT_EQ(after[0].rfind("[2026-10-01 00:02:04.900] ", 0), 0U) << after[0];
    EXPECT_EQ(count(lines(newDay, true), "session started 2026-09-30 23:59:59, continued"), 1U);
    std::error_code ec;
    fs::remove_all(aside, ec);
}

// The mapped steering alone moving (the physical wheel's reading the same, a game profile's curve, say) is a change:
// a row, like any other value.
TEST_F(TelemetryTest, TheMappedValuesAloneMakeRows) {
    auto recorder = open(at(10, 0, 0));
    ASSERT_NE(recorder, nullptr);
    TelemetrySample sample;
    recorder->writeSample(sample);
    for (float* value : {&sample.state.steering, &sample.state.throttle, &sample.state.brake, &sample.state.clutch}) {
        sample.nowNs += 20 * kMs;
        *value = 0.5F;
        recorder->writeSample(sample);
    }
    EXPECT_EQ(recorder->rowsWritten(), 5U);
    EXPECT_EQ(recorder->rowsSkipped(), 0U);
}

// A slot the game stopped that still plays on the wheel (until the next update) is shown as playing only ("p"); a
// sample taken before the last packet's time has an age of 0, never a wrapped number.
TEST_F(TelemetryTest, AStoppedSlotStillPlayingAndAPacketFromTheFuture) {
    auto recorder = open(at(10, 0, 0));
    ASSERT_NE(recorder, nullptr);
    NullSink sink;
    FfbController controller(sink, FfbSafetyConfig{});
    FfbPacket packet;
    packet.kind = FfbPacket::Kind::SlotState;
    packet.slot = 1;
    packet.playing = true;
    packet.effect.type = FfbEffectType::Constant;
    controller.onPacket(packet, 10 * kMs);
    controller.update(10 * kMs);                 // playing on the wheel
    packet.playing = false;
    controller.onPacket(packet, 200 * kMs);      // the game stops it; not applied yet
    TelemetrySample sample;
    sample.ffb = &controller;
    sample.nowNs = 100 * kMs;                    // before the last packet's time
    recorder->writeSample(sample);
    recorder.reset();
    const auto data = lines(dir_ / "telemetry-2026-09-30-01.log");
    ASSERT_EQ(data.size(), 1U);
    EXPECT_EQ(value(data[0], "s1_state"), "p") << data[0];
    EXPECT_EQ(value(data[0], "ffb_age_ms"), "0") << data[0];
}

// The samples' file got its number but the force feedback file has none left for the day: open says why, and no
// half-open recorder is handed out.
TEST_F(TelemetryTest, NoNumberForTheFfbFileIsAnError) {
    fs::create_directories(dir_);
    std::ofstream(dir_ / "telemetry-2026-09-30-99998.log") << "an earlier run";
    std::ofstream(dir_ / "ffb-2026-09-30-99999.log") << "the last number";
    std::string error;
    EXPECT_EQ(TelemetryRecorder::open(TelemetryConfig{}, dir_, at(10, 0, 0), {}, error), nullptr);
    EXPECT_NE(error.find("no free number for ffb-2026-09-30"), std::string::npos) << error;
}

// The run's file is there but cannot be opened for writing (made read-only meanwhile): open says which file and
// why, rather than recording into nothing.
TEST_F(TelemetryTest, ARunFileThatCannotBeOpenedIsAnError) {
    auto first = open(at(10, 0, 0));
    ASSERT_NE(first, nullptr);
    const fs::path samples = first->samplesPath();
    first.reset();
    fs::permissions(samples, fs::perms::owner_read);
    if (std::ofstream(samples, std::ios::app).good()) {   // Administrator / root: still writable
        fs::permissions(samples, fs::perms::owner_all);
        GTEST_SKIP() << "the file stays writable here";
    }
    std::string error;
    EXPECT_EQ(TelemetryRecorder::open(TelemetryConfig{}, dir_, at(11, 0, 0), {}, error), nullptr);   // same run
    fs::permissions(samples, fs::perms::owner_all);
    EXPECT_NE(error.find("cannot open"), std::string::npos) << error;
    EXPECT_NE(error.find(samples.filename().string()), std::string::npos) << error;
}

// The PC slept over two midnights while the folder could not be written: the missing day is skipped (never written
// under the wrong date) and the rows go to the day they belong to once the folder is back.
TEST_F(TelemetryTest, TwoMidnightsWithoutAFolderLandOnTheRightDay) {
    auto recorder = open(at(23, 59, 59, 900));
    ASSERT_NE(recorder, nullptr);
    TelemetrySample sample;
    recorder->writeSample(sample);
    const fs::path aside = dir_.string() + "-aside";
    fs::remove_all(aside);
    std::error_code moved;
    fs::rename(dir_, aside, moved);
    if (moved) {
        recorder.reset();
        GTEST_SKIP() << "the log folder cannot be moved while its files are open here: " << moved.message();
    }
    std::ofstream(dir_.string()) << "a file where the folder should be";
    constexpr std::uint64_t kDay = 24ULL * 3600 * kSecond;
    sample.raw.axes[0] = 0.5F;
    sample.nowNs = kDay + kSecond;              // 2026-10-02 00:00:00.900: dropped
    recorder->writeSample(sample);
    fs::remove(dir_);
    sample.raw.axes[0] = 0.6F;
    sample.nowNs = kDay + 125 * kSecond;        // two minutes later: the folder is back
    recorder->writeSample(sample);
    EXPECT_EQ(recorder->day(), "2026-10-02");
    const fs::path newDay = recorder->samplesPath();
    recorder.reset();
    EXPECT_FALSE(fs::exists(dir_ / "telemetry-2026-10-01-01.log"));
    const auto rows = lines(newDay);
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(value(rows[0], "raw_a0"), "0.6000");
    fs::remove_all(aside);
}

#ifndef _WIN32
// The disk fills up (here: the process may not write files past a size): writing stops on that file for the day,
// with an error in the log; the service goes on, and nothing past the limit is written. Run in a child process,
// whose limit can be lowered without touching the tests'.
void recordUntilTheDiskIsFull(const fs::path& dir, const rlim_t bytes) {
    std::signal(SIGXFSZ, SIG_IGN);   // a write past the limit fails (EFBIG) instead of ending the process
    rlimit before{};
    ::getrlimit(RLIMIT_FSIZE, &before);
    rlimit small = before;
    small.rlim_cur = bytes;
    ::setrlimit(RLIMIT_FSIZE, &small);
    std::string error;
    TelemetryConfig config;
    config.onlyChanges = false;
    auto recorder = TelemetryRecorder::open(config, dir, at(10, 0, 0), {"source: test wheel"}, error);
    bool fine = recorder != nullptr;
    if (recorder) {
        TelemetrySample sample;
        FfbPacket packet;
        packet.kind = FfbPacket::Kind::SlotState;
        for (int i = 0; i < 2000; ++i) {   // ~400 bytes a row: far past the limit, and flushed every second
            sample.nowNs = static_cast<std::uint64_t>(i) * 20 * kMs;
            sample.raw.axes[0] = static_cast<float>(i % 2);
            recorder->writeSample(sample);
            packet.effect.constantLevel = static_cast<float>(i % 2);
            recorder->writeFfbPacket(sample.nowNs, packet);
        }
        const fs::path samples = recorder->samplesPath();
        const fs::path ffb = recorder->ffbPath();
        recorder.reset();
        fine = fs::file_size(samples) <= bytes && fs::file_size(ffb) <= bytes;
    }
    ::setrlimit(RLIMIT_FSIZE, &before);   // back, for whatever the process writes as it exits
    std::exit(fine ? 0 : 1);
}

TEST_F(TelemetryTest, AFullDiskStopsTheFileNotTheService) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(recordUntilTheDiskIsFull(dir_, 6000), ::testing::ExitedWithCode(0), "");      // during the rows
    fs::remove_all(dir_);
    logfiles::forgetRun();
    EXPECT_EXIT(recordUntilTheDiskIsFull(dir_, 100), ::testing::ExitedWithCode(0), "");       // in the header
}
#endif


// pedals= and out_combined=: what the game gets; switching the pedals makes a row of its own (with only_changes).
TEST_F(TelemetryTest, RowsSayWhichPedalsTheGameGets) {
    auto recorder = open(at(10, 0, 0));
    ASSERT_NE(recorder, nullptr);
    TelemetrySample sample;
    sample.nowNs = 1000 * kMs;
    sample.report.throttle = 65535;
    sample.report.brake = 65535;
    sample.report.combined = 32767;
    recorder->writeSample(sample);
    sample.nowNs = 1020 * kMs;
    recorder->writeSample(sample);   // nothing changed: no row
    sample.nowNs = 1040 * kMs;
    sample.combinedPedals = true;
    recorder->writeSample(sample);
    recorder.reset();

    const auto data = lines(dir_ / "telemetry-2026-09-30-01.log");
    ASSERT_EQ(data.size(), 2U);
    EXPECT_EQ(value(data[0], "pedals"), "separate");
    EXPECT_EQ(value(data[0], "out_combined"), "32767");
    EXPECT_EQ(value(data[1], "pedals"), "combined");
    EXPECT_EQ(value(data[1], "out_combined"), "32767");
}

}  // namespace
}  // namespace wheelbri
