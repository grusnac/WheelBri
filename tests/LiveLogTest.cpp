// SPDX-License-Identifier: GPL-3.0-or-later
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

// Since the app's Level button (2026-10-08) the drivers write their level in brackets, as spdlog does.
TEST(LiveLog, ParsesTheDriversLevelledLines) {
    const auto debug = parseLogLine("[2026-10-08 21:14:05.120] [debug] pid: start block 1 (constant, slot 0)", 1);
    EXPECT_EQ(debug.time, "21:14:05.120");
    EXPECT_EQ(debug.level, LiveLogLevel::Debug);
    EXPECT_TRUE(debug.logger.empty());
    EXPECT_EQ(debug.message, "pid: start block 1 (constant, slot 0)");
    const auto warning =
        parseLogLine("[2026-10-08 21:14:05.120] [warning] receiving from wheelbri-service failed (10054)", 1);
    EXPECT_EQ(warning.level, LiveLogLevel::Warn);
    EXPECT_EQ(warning.message, "receiving from wheelbri-service failed (10054)");
    EXPECT_EQ(parseLogLine("[2026-10-08 21:14:05.120] [error] WSAStartup failed (10091)", 1).level,
              LiveLogLevel::Error);
    EXPECT_EQ(parseLogLine("[2026-10-08 21:14:05.120] [trace] pid trace +3 out 1A 0101FF", 1).level,
              LiveLogLevel::Trace);
    // A message in brackets after the level is not taken for a component and lost.
    EXPECT_EQ(parseLogLine("[2026-10-08 21:14:05.120] [info] stopped: 12 reports", 1).message, "stopped: 12 reports");
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

// Lines that only look like log lines: an unclosed or empty bracket, a bracket that is not a time, a level the
// parser does not know (read as a driver line), a thread bracket that is not one, a source location not at the end.
TEST(LiveLog, LinesThatOnlyLookLikeLogLines) {
    EXPECT_EQ(parseLogLine("[2026-10-02 10:00:00.000 no close", 0).message, "[2026-10-02 10:00:00.000 no close");
    EXPECT_EQ(parseLogLine("[] empty", 0).message, "[] empty");
    EXPECT_EQ(parseLogLine("[12:00] short stamp", 0).time, "");
    EXPECT_EQ(parseLogLine("[2026/10/02 10:00:00.000] slashes", 0).time, "");
    EXPECT_EQ(parseLogLine("\t [2026-10-02 10:00:00.000] tabs \t", 0).message, "tabs");
    EXPECT_EQ(parseLogLine("", 0).message, "");

    const auto odd = parseLogLine("[2026-10-02 10:00:00.000] [notice] a warning here", 2);
    EXPECT_EQ(odd.level, LiveLogLevel::Warn);   // no level known: read from the words
    EXPECT_EQ(odd.message, "[notice] a warning here");
    EXPECT_EQ(parseLogLine("[2026-10-02 10:00:00.000] Open FAILED: 5", 0).level, LiveLogLevel::Error);
    EXPECT_EQ(parseLogLine("[2026-10-02 10:00:00.000] [critical] [svc] [tid 1 ] gone", 0).level, LiveLogLevel::Error);
    EXPECT_EQ(parseLogLine("[2026-10-02 10:00:00.000] [info ] [svc] [tid 1 ] x  (A.cpp:1)", 0).message, "x");

    const auto noThread = parseLogLine("[2026-10-02 10:00:00.000] [info ] [svc] [not a thread] text", 0);
    EXPECT_EQ(noThread.logger, "svc");
    EXPECT_EQ(noThread.message, "[not a thread] text");
    const auto noLogger = parseLogLine("[2026-10-02 10:00:00.000] [info ] plain", 0);
    EXPECT_TRUE(noLogger.logger.empty());
    EXPECT_EQ(noLogger.message, "plain");
    const auto noSource = parseLogLine("[2026-10-02 10:00:00.000] [info ] [svc] [tid 1 ] see  (manual) later", 0);
    EXPECT_EQ(noSource.message, "see  (manual) later");   // not at the end: part of the message
}

// A file that becomes shorter (cut, or replaced by a new one) is read again from its tail; blank lines are skipped.
TEST_F(LiveLogFiles, AFileThatShrinksIsReadAgain) {
    append("a.log", "[2026-10-02 10:00:00.000] first of a long line\n");
    LiveLog log;
    log.setFile(0, dir_ / "a.log");
    ASSERT_TRUE(log.poll());
    std::ofstream(dir_ / "a.log", std::ios::binary | std::ios::trunc) << "\n\n[2026-10-02 11:00:00.000] new\n";
    ASSERT_TRUE(log.poll());
    ASSERT_EQ(log.lines().size(), 2U);
    EXPECT_EQ(log.lines().back().message, "new");
    std::filesystem::remove(dir_ / "a.log");
    EXPECT_FALSE(log.poll());   // gone: nothing, no error
}

// Following another file (the next day's) starts over at its tail; the same path again changes nothing; an empty
// path stops following.
TEST_F(LiveLogFiles, TheFileFollowedCanChangeOrStop) {
    append("day1.log", "[2026-10-02 23:59:59.000] day one\n");
    append("day2.log", "[2026-10-03 00:00:01.000] day two\n");
    LiveLog log;
    log.setFile(0, dir_ / "day1.log");
    ASSERT_TRUE(log.poll());
    log.setFile(0, dir_ / "day1.log");     // the same: nothing read again
    EXPECT_FALSE(log.poll());
    log.setFile(0, dir_ / "day2.log");
    ASSERT_TRUE(log.poll());
    EXPECT_EQ(log.lines().back().message, "day two");
    log.setFile(0, {});
    log.setFile(5, {});                    // never followed: nothing to stop
    append("day2.log", "[2026-10-03 00:00:02.000] unseen\n");
    EXPECT_FALSE(log.poll());
    EXPECT_EQ(log.lines().size(), 2U);
}

// Only the newest kMaxLines are kept; added() still counts every line.
TEST_F(LiveLogFiles, OnlyTheNewestLinesAreKept) {
    LiveLog log;
    log.setFile(0, dir_ / "many.log");
    append("many.log", "start\n");
    ASSERT_TRUE(log.poll());
    std::string text;
    for (std::size_t i = 0; i < LiveLog::kMaxLines + 10; ++i) text += "l" + std::to_string(i) + "\n";
    append("many.log", text);
    ASSERT_TRUE(log.poll());
    EXPECT_EQ(log.lines().size(), LiveLog::kMaxLines);
    EXPECT_EQ(log.lines().front().message, "l10");
    EXPECT_EQ(log.added(), LiveLog::kMaxLines + 11);
}

// A big file whose tail has no line end at all (a binary file): nothing from the cut start is shown, and what
// comes after the next line end is.
TEST_F(LiveLogFiles, ATailWithoutALineEndShowsNothingUntilOne) {
    append("binary.log", std::string(LiveLog::kInitialTailBytes + 1000, 'x'));
    LiveLog log;
    log.setFile(0, dir_ / "binary.log");
    EXPECT_FALSE(log.poll());
    append("binary.log", "\n[2026-10-02 10:00:00.000] after\n");
    ASSERT_TRUE(log.poll());
    ASSERT_EQ(log.lines().size(), 1U);
    EXPECT_EQ(log.lines().front().message, "after");
}

// More than kMaxReadBytes written at once is read over several polls, none of it lost.
TEST_F(LiveLogFiles, AHugeWriteIsReadOverSeveralPolls) {
    LiveLog log;
    log.setFile(0, dir_ / "huge.log");
    append("huge.log", "start\n");
    ASSERT_TRUE(log.poll());
    const std::string line(1000, 'y');
    std::string text;
    for (int i = 0; i < 1200; ++i) text += line + "\n";   // about 1.2 MB
    append("huge.log", text);
    int polls = 0;
    while (log.poll()) ++polls;
    EXPECT_GE(polls, 3);
    EXPECT_EQ(log.added(), 1201U);
}

// A stamp is taken only with every separator in place: each one wrong on its own makes it a plain line; a logger
// with no thread after it is still the logger.
TEST(LiveLog, EverySeparatorOfTheStampCounts) {
    for (const char* stamp : {"[2026-10/02 10:00:00.000] x", "[2026-10-02_10:00:00.000] x",
                              "[2026-10-02 10.00:00.000] x", "[2026-10-02 10:00.00.000] x"}) {
        const LiveLogLine line = parseLogLine(stamp, 0);
        EXPECT_EQ(line.time, "") << stamp;
        EXPECT_EQ(line.message, stamp);
    }
    const LiveLogLine bare = parseLogLine("[2026-10-02 10:00:00.000] [warn] [svc] last words", 0);
    EXPECT_EQ(bare.logger, "svc");
    EXPECT_EQ(bare.level, LiveLogLevel::Warn);
    EXPECT_EQ(bare.message, "last words");
}

// A file that cannot be opened (no permission) is passed over without an error, the others still read; once it can
// be read again, it is.
TEST_F(LiveLogFiles, AFileThatCannotBeOpenedIsPassedOver) {
    append("a.log", "[2026-10-02 10:00:01.000] a1\n");
    append("b.log", "[2026-10-02 10:00:02.000] b1\n");
    std::filesystem::permissions(dir_ / "a.log", std::filesystem::perms::none);
    if (std::ifstream(dir_ / "a.log").good()) {
        std::filesystem::permissions(dir_ / "a.log", std::filesystem::perms::owner_all);
        GTEST_SKIP() << "the file stays readable here";
    }
    LiveLog log;
    log.setFile(0, dir_ / "a.log");
    log.setFile(1, dir_ / "b.log");
    ASSERT_TRUE(log.poll());
    ASSERT_EQ(log.lines().size(), 1U);
    EXPECT_EQ(log.lines()[0].message, "b1");
    std::filesystem::permissions(dir_ / "a.log", std::filesystem::perms::owner_all);
    ASSERT_TRUE(log.poll());
    EXPECT_EQ(log.lines().back().message, "a1");
}

}  // namespace
}  // namespace wheelbri
