#include "wheelbri/LiveLog.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace wheelbri {
namespace {

TEST(LiveLog, ParsesSpdlogLines) {
    const auto line = parseLogLine(
        "[2026-10-02 22:49:33.488] [warn ] [service.ffb   ] [tid 23616 ] game silent for 500 ms  (main.cpp:40)\r", 3);
    EXPECT_EQ(line.source, 3);
    EXPECT_EQ(line.time, "22:49:33.488");
    EXPECT_EQ(line.level, LiveLogLevel::Warn);
    EXPECT_EQ(line.logger, "service.ffb");
    EXPECT_EQ(line.message, "game silent for 500 ms");
}

TEST(LiveLog, ParsesDriverAndBridgeLines) {
    const auto info = parseLogLine("[2026-10-02 08:01:02.003] link: state packets on 41329", 1);
    EXPECT_EQ(info.time, "08:01:02.003");
    EXPECT_EQ(info.level, LiveLogLevel::Info);
    EXPECT_TRUE(info.logger.empty());
    EXPECT_EQ(info.message, "link: state packets on 41329");
    EXPECT_EQ(parseLogLine("[2026-10-02 08:01:02.003] ERROR: bind failed", 1).level, LiveLogLevel::Error);
}

TEST(LiveLog, KeepsOtherLinesWhole) {
    const auto line = parseLogLine("   at Program.Main()", 0);
    EXPECT_TRUE(line.time.empty());
    EXPECT_EQ(line.message, "at Program.Main()");
}

class LiveLogFiles : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("wheelbri-livelog-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    void append(const std::string& name, const std::string& text) {
        std::ofstream(dir_ / name, std::ios::binary | std::ios::app) << text;
    }

    std::filesystem::path dir_;
};

TEST_F(LiveLogFiles, MergesFilesInTimeOrderThenFollowsThem) {
    append("a.log", "[2026-10-02 10:00:01.000] a1\n[2026-10-02 10:00:03.000] a2\n");
    append("b.log", "[2026-10-02 10:00:02.000] b1\n");
    LiveLog log;
    log.setFile(0, dir_ / "a.log");
    log.setFile(1, dir_ / "b.log");
    log.setFile(2, dir_ / "missing.log");
    ASSERT_TRUE(log.poll());
    ASSERT_EQ(log.lines().size(), 3U);
    EXPECT_EQ(log.lines()[0].message, "a1");
    EXPECT_EQ(log.lines()[1].message, "b1");
    EXPECT_EQ(log.lines()[2].message, "a2");
    EXPECT_FALSE(log.poll());

    // A line written in two parts shows once it is complete.
    append("b.log", "[2026-10-02 10:00:04.000] b2 par");
    EXPECT_FALSE(log.poll());
    append("b.log", "t two\n");
    ASSERT_TRUE(log.poll());
    EXPECT_EQ(log.lines().back().message, "b2 part two");
    EXPECT_EQ(log.lines().back().source, 1);
    EXPECT_EQ(log.added(), 4U);
}

TEST_F(LiveLogFiles, ALineThatNeverEndsIsShownCut) {
    LiveLog log;
    log.setFile(0, dir_ / "endless.log");
    append("endless.log", "[2026-10-02 10:00:00.000] first\n");
    ASSERT_TRUE(log.poll());
    append("endless.log", std::string(LiveLog::kMaxLineBytes + 100, 'x'));
    ASSERT_TRUE(log.poll());
    EXPECT_EQ(log.lines().back().message.size(), LiveLog::kMaxLineBytes);
    append("endless.log", "\n[2026-10-02 10:00:01.000] next\n");
    ASSERT_TRUE(log.poll());
    EXPECT_EQ(log.lines().back().message, "next");
}

TEST_F(LiveLogFiles, ReadsOnlyTheTailOfABigFile) {
    std::string text;
    for (int i = 0; i < 4000; ++i) {
        text += "[2026-10-02 10:00:00.000] line " + std::to_string(i) + " with some padding to make it longer\n";
    }
    append("big.log", text);
    LiveLog log;
    log.setFile(0, dir_ / "big.log");
    ASSERT_TRUE(log.poll());
    EXPECT_LT(log.lines().size(), 4000U);
    EXPECT_EQ(log.lines().back().message, "line 3999 with some padding to make it longer");
    EXPECT_EQ(log.lines().front().message.rfind("line ", 0), 0U);   // the cut first line was dropped
}

}  // namespace
}  // namespace wheelbri
