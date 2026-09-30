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

class RecordingSink : public IFfbSink {
public:
    bool play(std::size_t, const FfbEffect&) override { return true; }
    void stop(std::size_t) override {}
    void stopAll() override {}
};

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

    std::unique_ptr<TelemetryRecorder> open(TelemetryConfig config = {}) {
        std::string error;
        auto recorder = TelemetryRecorder::open(config, dir_, "20260930-120000", {"source: test wheel"}, error);
        EXPECT_NE(recorder, nullptr) << error;
        return recorder;
    }

    static std::vector<std::string> dataLines(const fs::path& path) {
        std::ifstream file(path);
        std::vector<std::string> lines;
        for (std::string line; std::getline(file, line);) {
            if (!line.empty() && line[0] != '#') {
                lines.push_back(line);
            }
        }
        return lines;
    }

    static std::string readAll(const fs::path& path) {
        std::ifstream file(path);
        std::stringstream text;
        text << file.rdbuf();
        return text.str();
    }

    static std::size_t fields(const std::string& line) {
        return static_cast<std::size_t>(std::count(line.begin(), line.end(), ',')) + 1;
    }

    fs::path dir_;
};

TEST_F(TelemetryTest, WritesHeaderCommentsAndColumns) {
    auto recorder = open();
    ASSERT_NE(recorder, nullptr);
    recorder.reset();  // closes the files

    const std::string text = readAll(dir_ / "telemetry-20260930-120000.csv");
    EXPECT_NE(text.find("# source: test wheel"), std::string::npos);
    const auto lines = dataLines(dir_ / "telemetry-20260930-120000.csv");
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_EQ(lines[0].rfind("t_ms,seq,raw_a0", 0), 0U);
    EXPECT_EQ(fields(lines[0]), telemetryColumns().size());
    EXPECT_TRUE(fs::exists(dir_ / "ffb-20260930-120000.csv"));
}

TEST_F(TelemetryTest, RowsHaveOneFieldPerColumnWithAndWithoutFfb) {
    auto recorder = open();
    ASSERT_NE(recorder, nullptr);

    TelemetrySample sample;
    sample.nowNs = 1000 * kMs;
    sample.raw.axes[2] = -0.5F;
    sample.raw.buttons = (1ULL << 35) | 1;
    sample.state.throttle = 0.25F;
    sample.report.throttle = 49151;
    recorder->writeSample(sample);

    RecordingSink sink;
    FfbController controller(sink, FfbSafetyConfig{});
    FfbPacket packet;
    packet.kind = FfbPacket::Kind::SlotState;
    packet.slot = 1;
    packet.playing = true;
    packet.effect.type = FfbEffectType::Constant;
    packet.effect.constantLevel = 0.5F;
    controller.onPacket(packet, 1010 * kMs);
    controller.update(1010 * kMs);
    sample.ffb = &controller;
    sample.nowNs = 1020 * kMs;
    recorder->writeSample(sample);
    recorder.reset();

    const auto lines = dataLines(dir_ / "telemetry-20260930-120000.csv");
    ASSERT_EQ(lines.size(), 3U);
    const std::size_t columns = telemetryColumns().size();
    EXPECT_EQ(fields(lines[1]), columns);
    EXPECT_EQ(fields(lines[2]), columns);
    EXPECT_EQ(lines[1].rfind("0.0,", 0), 0U);
    EXPECT_NE(lines[1].find(",800000001,"), std::string::npos);  // raw buttons in hex
    EXPECT_NE(lines[1].find(",49151,"), std::string::npos);
    EXPECT_EQ(lines[2].rfind("20.0,", 0), 0U);
    EXPECT_NE(lines[2].find(",constant,wp,0.5000,"), std::string::npos);
}

TEST_F(TelemetryTest, DueFollowsTheRate) {
    TelemetryConfig config;
    config.rateHz = 100;  // every 10 ms
    auto recorder = open(config);
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
    auto recorder = open();
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

    const auto lines = dataLines(dir_ / "ffb-20260930-120000.csv");
    ASSERT_EQ(lines.size(), 4U);  // header + 2 packets + malformed note
    const std::size_t columns = fields(lines[0]);
    for (const auto& line : lines) {
        EXPECT_EQ(fields(line), columns) << line;
    }
    EXPECT_EQ(lines[1].rfind("2.0,slot,3,42,1,saw_up,", 0), 0U);
    EXPECT_NE(lines[1].find(",0.3000,"), std::string::npos);
    EXPECT_NE(lines[2].find(",stopall,"), std::string::npos);
    EXPECT_NE(lines[3].find("malformed_12_bytes"), std::string::npos);
}

TEST_F(TelemetryTest, StopsAtMaxFileSize) {
    TelemetryConfig config;
    config.maxFileMb = 1;
    auto recorder = open(config);
    ASSERT_NE(recorder, nullptr);
    TelemetrySample sample;
    for (int i = 0; i < 10000; ++i) {  // ~150 bytes a row: well past 1 MB
        sample.nowNs = static_cast<std::uint64_t>(i) * 20 * kMs;
        recorder->writeSample(sample);
    }
    recorder.reset();
    const auto size = fs::file_size(dir_ / "telemetry-20260930-120000.csv");
    EXPECT_LE(size, 1024U * 1024U + 64U);
    EXPECT_NE(readAll(dir_ / "telemetry-20260930-120000.csv").find("# stopped: max_file_mb reached"),
              std::string::npos);
}

TEST_F(TelemetryTest, PruneKeepsNewestSessions) {
    fs::create_directories(dir_);
    for (const char* stamp : {"20260101-000000", "20260102-000000", "20260103-000000", "20260104-000000"}) {
        std::ofstream(dir_ / (std::string("telemetry-") + stamp + ".csv")) << "x";
        std::ofstream(dir_ / (std::string("ffb-") + stamp + ".csv")) << "x";
    }
    std::ofstream(dir_ / "wheelemu-service.log") << "keep me";

    EXPECT_EQ(pruneTelemetry(dir_, 2), 4U);
    EXPECT_FALSE(fs::exists(dir_ / "telemetry-20260101-000000.csv"));
    EXPECT_FALSE(fs::exists(dir_ / "ffb-20260102-000000.csv"));
    EXPECT_TRUE(fs::exists(dir_ / "telemetry-20260103-000000.csv"));
    EXPECT_TRUE(fs::exists(dir_ / "ffb-20260104-000000.csv"));
    EXPECT_TRUE(fs::exists(dir_ / "wheelemu-service.log"));
    EXPECT_EQ(pruneTelemetry(dir_, 2), 0U);
}

TEST(TelemetryStamp, SortableDateTime) {
    const std::string stamp = telemetryStamp(std::time(nullptr));
    ASSERT_EQ(stamp.size(), 15U);
    EXPECT_EQ(stamp[8], '-');
}

}  // namespace
}  // namespace wheelemu
