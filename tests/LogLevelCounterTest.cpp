// LogLevelCounter: the warnings and errors in one log file, read a part at a time as the file grows.

#include "wheelbri/LogLevelCounter.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace wheelbri {
namespace {

namespace fs = std::filesystem;

class TempFile {
public:
    TempFile() {
        path_ = fs::temp_directory_path() /
                ("wheelbri-levels-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 ".log");
    }
    ~TempFile() {
        std::error_code ignored;
        fs::remove(path_, ignored);
    }
    void append(const std::string& text) const { std::ofstream(path_, std::ios::binary | std::ios::app) << text; }
    void replace(const std::string& text) const { std::ofstream(path_, std::ios::binary | std::ios::trunc) << text; }
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

std::string line(const char* level, const std::string& message, const char* time = "14:03:21.120") {
    return std::string("[2026-10-08 ") + time + "] [" + level + "] [helper.pipe   ] [tid 18692 ] " + message + "\r\n";
}

TEST(LogLevelCounter, CountsWarningsAndErrorsAndKeepsTheLatest) {
    TempFile file;
    file.append(line("info ", "helper: listening"));
    file.append(line("warning", "helper: refused a caller", "09:00:00.000"));
    file.append(line("error", "helper: CreateVirtualWheel for pid 1: failed: x", "09:30:00.000"));
    file.append(line("critical", "helper: the pipe exists already", "10:00:00.000"));
    file.append(line("debug", "detail"));
    LogLevelCounter counter;
    counter.setFile(file.path());
    EXPECT_TRUE(counter.poll());
    EXPECT_EQ(counter.count().warnings, 1U);
    EXPECT_EQ(counter.count().errors, 2U);   // error and critical
    EXPECT_TRUE(counter.count().any());
    EXPECT_EQ(counter.count().lastTime, "10:00:00.000");
    EXPECT_EQ(counter.count().lastMessage, "helper: the pipe exists already");
}

TEST(LogLevelCounter, AFileWithoutWarningsCountsNothing) {
    TempFile file;
    file.append(line("info ", "a") + line("trace", "b") + "not a log line at all\n");
    LogLevelCounter counter;
    counter.setFile(file.path());
    EXPECT_FALSE(counter.poll());
    EXPECT_FALSE(counter.count().any());
    EXPECT_EQ(counter.count().lastMessage, "");
}

TEST(LogLevelCounter, OnlyWhatWasAddedIsReadAgain) {
    TempFile file;
    file.append(line("warning", "one"));
    LogLevelCounter counter;
    counter.setFile(file.path());
    ASSERT_TRUE(counter.poll());
    EXPECT_FALSE(counter.poll());   // nothing new
    file.append(line("info ", "fine"));
    EXPECT_FALSE(counter.poll());   // something new, but no warning
    file.append(line("warning", "two"));
    EXPECT_TRUE(counter.poll());
    EXPECT_EQ(counter.count().warnings, 2U);
    EXPECT_EQ(counter.count().lastMessage, "two");
}

TEST(LogLevelCounter, AHalfWrittenLineIsCountedOnceItEnds) {
    TempFile file;
    const std::string whole = line("warning", "split in two");
    file.append(whole.substr(0, 20));
    LogLevelCounter counter;
    counter.setFile(file.path());
    EXPECT_FALSE(counter.poll());
    file.append(whole.substr(20));
    EXPECT_TRUE(counter.poll());
    EXPECT_EQ(counter.count().warnings, 1U);
    EXPECT_EQ(counter.count().lastMessage, "split in two");
}

TEST(LogLevelCounter, ALineThatNeverEndsIsCountedAsItIs) {
    TempFile file;
    file.append("[2026-10-08 10:00:00.000] [error] [helper] [tid 1 ] " + std::string(LiveLog::kMaxLineBytes, 'x'));
    LogLevelCounter counter;
    counter.setFile(file.path());
    EXPECT_TRUE(counter.poll());
    EXPECT_EQ(counter.count().errors, 1U);
}

TEST(LogLevelCounter, AHugeFileIsReadAPartAtATime) {
    TempFile file;
    const std::string filler = line("info ", std::string(1000, 'f'));
    std::string text;
    while (text.size() < LogLevelCounter::kMaxReadBytes) text += filler;
    file.append(text);
    file.append(line("error", "at the end"));
    LogLevelCounter counter;
    counter.setFile(file.path());
    EXPECT_FALSE(counter.poll());   // the first megabyte: no error yet
    EXPECT_TRUE(counter.poll());    // the rest
    EXPECT_EQ(counter.count().errors, 1U);
}

TEST(LogLevelCounter, AFileThatDoesNotExistYetCountsNothingUntilItDoes) {
    TempFile file;
    LogLevelCounter counter;
    counter.setFile(file.path());
    EXPECT_FALSE(counter.poll());
    file.append(line("warning", "now"));
    EXPECT_TRUE(counter.poll());
}

TEST(LogLevelCounter, AFileThatGoesAwayStartsOverAtZero) {
    TempFile file;
    file.append(line("warning", "w"));
    LogLevelCounter counter;
    counter.setFile(file.path());
    ASSERT_TRUE(counter.poll());
    fs::remove(file.path());
    EXPECT_TRUE(counter.poll());    // changed: back to nothing
    EXPECT_FALSE(counter.count().any());
    EXPECT_FALSE(counter.poll());   // still gone: no change
}

TEST(LogLevelCounter, AFileReplacedByAShorterOneIsCountedAgain) {
    TempFile file;
    file.append(line("warning", "a") + line("warning", "b"));
    LogLevelCounter counter;
    counter.setFile(file.path());
    ASSERT_TRUE(counter.poll());
    ASSERT_EQ(counter.count().warnings, 2U);
    file.replace(line("error", "c"));
    EXPECT_TRUE(counter.poll());
    EXPECT_EQ(counter.count().warnings, 0U);
    EXPECT_EQ(counter.count().errors, 1U);
}

TEST(LogLevelCounter, ANewPathStartsOverAndTheSamePathDoesNot) {
    TempFile first;
    TempFile second;
    first.append(line("warning", "first"));
    second.append(line("info ", "second"));
    LogLevelCounter counter;
    counter.setFile(first.path());
    ASSERT_TRUE(counter.poll());
    counter.setFile(first.path());   // the same: kept
    EXPECT_EQ(counter.count().warnings, 1U);
    counter.setFile(second.path());  // tomorrow's file: from zero
    EXPECT_FALSE(counter.count().any());
    EXPECT_EQ(counter.file(), second.path());
    EXPECT_FALSE(counter.poll());
}

TEST(LogLevelCounter, NoFileCountsNothing) {
    LogLevelCounter counter;
    EXPECT_FALSE(counter.poll());
    counter.setFile({});
    EXPECT_FALSE(counter.poll());
}

// A file that cannot be read (no permission) counts nothing yet, and is counted once it can be read.
TEST(LogLevelCounter, AFileThatCannotBeReadIsCountedOnceItCanBe) {
    TempFile file;
    file.append(line("warning", "now"));
    std::filesystem::permissions(file.path(), std::filesystem::perms::none);
    if (std::ifstream(file.path()).good()) {
        std::filesystem::permissions(file.path(), std::filesystem::perms::owner_all);
        GTEST_SKIP() << "the file stays readable here";
    }
    LogLevelCounter counter;
    counter.setFile(file.path());
    EXPECT_FALSE(counter.poll());
    std::filesystem::permissions(file.path(), std::filesystem::perms::owner_all);
    EXPECT_TRUE(counter.poll());
    EXPECT_EQ(counter.count().warnings, 1U);
}

}  // namespace
}  // namespace wheelbri
