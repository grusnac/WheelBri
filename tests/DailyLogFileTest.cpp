// DailyLogFile: the WheelBri Helper's log before and after it has somewhere to write, across days and failures.

#include "wheelbri/DailyLogFile.hpp"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wheelbri {
namespace {

// What the fake files hold, shared with the test after the log took the target.
struct Disk {
    std::map<std::string, std::vector<std::string>> files;   // date -> lines
    std::vector<std::string> opened;                         // every open, in order
    int closes = 0;
    bool openFails = false;
    int writesBeforeFailure = -1;   // -1: never fails
    bool isOpen = false;
};

class FakeTarget final : public LogTarget {
public:
    explicit FakeTarget(Disk& disk) : disk_(disk) {}

    bool open(const std::string& date) override {
        EXPECT_FALSE(disk_.isOpen) << "opened twice without a close";
        disk_.opened.push_back(date);
        if (disk_.openFails) return false;
        date_ = date;
        disk_.isOpen = true;
        return true;
    }
    bool write(const std::string_view line) override {
        EXPECT_TRUE(disk_.isOpen) << "written while closed";
        if (disk_.writesBeforeFailure == 0) return false;
        if (disk_.writesBeforeFailure > 0) --disk_.writesBeforeFailure;
        disk_.files[date_].emplace_back(line);
        return true;
    }
    void close() override {
        EXPECT_TRUE(disk_.isOpen) << "closed while not open";
        disk_.isOpen = false;
        ++disk_.closes;
    }

private:
    Disk& disk_;
    std::string date_;
};

std::string note(const std::string& message) {
    return "NOTE " + message;
}

constexpr const char* kDay1 = "2026-10-08";
constexpr const char* kDay2 = "2026-10-09";

DailyLogFile::Settings settings(const std::size_t maxPending, const std::uint64_t retryMs) {
    DailyLogFile::Settings s;
    s.maxPending = maxPending;
    s.retryMs = retryMs;
    return s;
}

// --- before anyone it could write for has connected ---

TEST(DailyLogFile, WithoutATargetLinesWait) {
    DailyLogFile log;
    log.write("a", kDay1, 0);
    log.write("b", kDay1, 100'000);   // no target: nothing to try, however long
    EXPECT_FALSE(log.hasTarget());
    EXPECT_FALSE(log.isOpen());
    EXPECT_EQ(log.pending(), 2U);
}

TEST(DailyLogFile, AttachingWritesWhatWaitedInOrderThenNewLines) {
    Disk disk;
    DailyLogFile log({}, note);
    log.write("starting", kDay1, 0);
    log.write("listening", kDay1, 1);
    log.attach(std::make_unique<FakeTarget>(disk), kDay1, 2);
    EXPECT_TRUE(log.isOpen());
    EXPECT_EQ(log.fileDate(), kDay1);
    log.write("connected", kDay1, 3);
    EXPECT_EQ(disk.files[kDay1], (std::vector<std::string>{"starting", "listening", "connected"}));
    EXPECT_EQ(log.pending(), 0U);
}

TEST(DailyLogFile, TheOldestWaitingLinesGoFirstAndTheirCountIsWritten) {
    Disk disk;
    DailyLogFile log(settings(2, 60'000), note);
    log.write("1", kDay1, 0);
    log.write("2", kDay1, 0);
    log.write("3", kDay1, 0);
    log.write("4", kDay1, 0);
    EXPECT_EQ(log.pending(), 2U);
    EXPECT_EQ(log.dropped(), 2U);
    log.attach(std::make_unique<FakeTarget>(disk), kDay1, 0);
    EXPECT_EQ(disk.files[kDay1], (std::vector<std::string>{
                                     "NOTE 2 earlier lines were dropped while this log had no file", "3", "4"}));
    EXPECT_EQ(log.dropped(), 0U);
}

TEST(DailyLogFile, WithoutANoteLineTheDropCountIsOnlyForgotten) {
    Disk disk;
    DailyLogFile log(settings(1, 60'000));
    log.write("1", kDay1, 0);
    log.write("2", kDay1, 0);
    log.attach(std::make_unique<FakeTarget>(disk), kDay1, 0);
    EXPECT_EQ(disk.files[kDay1], std::vector<std::string>{"2"});
    EXPECT_EQ(log.dropped(), 0U);
}

TEST(DailyLogFile, NoRoomAtAllCountsEveryLine) {
    Disk disk;
    DailyLogFile log(settings(0, 60'000), note);
    log.write("1", kDay1, 0);
    log.write("2", kDay1, 0);
    EXPECT_EQ(log.pending(), 0U);
    EXPECT_EQ(log.dropped(), 2U);
    log.attach(std::make_unique<FakeTarget>(disk), kDay1, 0);
    EXPECT_EQ(disk.files[kDay1].front(), "NOTE 2 earlier lines were dropped while this log had no file");
}

// --- attaching again, or to nothing ---

TEST(DailyLogFile, AnotherUserTakesTheLogAndTheFirstFileIsClosed) {
    Disk first;
    Disk second;
    DailyLogFile log;
    log.attach(std::make_unique<FakeTarget>(first), kDay1, 0);
    log.write("for the first", kDay1, 1);
    log.attach(std::make_unique<FakeTarget>(second), kDay1, 2);
    log.write("for the second", kDay1, 3);
    EXPECT_EQ(first.closes, 1);
    EXPECT_FALSE(first.isOpen);
    EXPECT_EQ(first.files[kDay1], std::vector<std::string>{"for the first"});
    EXPECT_EQ(second.files[kDay1], std::vector<std::string>{"for the second"});
}

TEST(DailyLogFile, ANullTargetClosesTheFileAndLinesWaitAgain) {
    Disk disk;
    DailyLogFile log;
    log.attach(std::make_unique<FakeTarget>(disk), kDay1, 0);
    log.attach(nullptr, kDay1, 1);
    EXPECT_EQ(disk.closes, 1);
    EXPECT_FALSE(log.hasTarget());
    log.write("waits", kDay1, 2);
    EXPECT_EQ(log.pending(), 1U);
}

TEST(DailyLogFile, AttachingToAClosedTargetDoesNotCloseItAgain) {
    Disk failing;
    failing.openFails = true;
    Disk good;
    DailyLogFile log;
    log.attach(std::make_unique<FakeTarget>(failing), kDay1, 0);
    log.attach(std::make_unique<FakeTarget>(good), kDay1, 1);
    EXPECT_EQ(failing.closes, 0);   // the fake fails the test on a close while not open
    EXPECT_TRUE(log.isOpen());
}

TEST(DailyLogFile, TheFileIsClosedWhenTheLogEnds) {
    Disk disk;
    {
        DailyLogFile log;
        log.attach(std::make_unique<FakeTarget>(disk), kDay1, 0);
    }
    EXPECT_EQ(disk.closes, 1);
    Disk never;
    never.openFails = true;
    {
        DailyLogFile log;
        log.attach(std::make_unique<FakeTarget>(never), kDay1, 0);
    }
    EXPECT_EQ(never.closes, 0);
}

// --- a new day ---

TEST(DailyLogFile, ANewDayGoesIntoItsOwnFileAtOnce) {
    Disk disk;
    DailyLogFile log;
    log.attach(std::make_unique<FakeTarget>(disk), kDay1, 0);
    log.write("late", kDay1, 10);
    log.write("early", kDay2, 11);   // straight after midnight: no retry wait for a new day
    EXPECT_EQ(disk.opened, (std::vector<std::string>{kDay1, kDay2}));
    EXPECT_EQ(disk.closes, 1);
    EXPECT_EQ(disk.files[kDay1], std::vector<std::string>{"late"});
    EXPECT_EQ(disk.files[kDay2], std::vector<std::string>{"early"});
    EXPECT_EQ(log.fileDate(), kDay2);
}

TEST(DailyLogFile, ANewDayWhoseFileCannotOpenWaitsAndIsTriedAgainLater) {
    Disk disk;
    DailyLogFile log(settings(100, 1'000), note);
    log.attach(std::make_unique<FakeTarget>(disk), kDay1, 0);
    disk.openFails = true;
    log.write("a", kDay2, 10);      // the new day's file fails
    log.write("b", kDay2, 500);     // too soon to try again
    EXPECT_EQ(disk.opened.size(), 2U);
    EXPECT_EQ(log.pending(), 2U);
    disk.openFails = false;
    log.write("c", kDay2, 1'010);   // a second later: tried, and everything written in order
    EXPECT_EQ(disk.opened.size(), 3U);
    EXPECT_EQ(disk.files[kDay2], (std::vector<std::string>{"a", "b", "c"}));
}

// --- a file that cannot be opened, or stops taking lines ---

TEST(DailyLogFile, AFileThatCannotBeOpenedIsTriedAgainAfterTheRetryTime) {
    Disk disk;
    disk.openFails = true;
    DailyLogFile log(settings(100, 1'000));
    log.attach(std::make_unique<FakeTarget>(disk), kDay1, 0);
    EXPECT_FALSE(log.isOpen());
    log.write("a", kDay1, 999);      // one before
    EXPECT_EQ(disk.opened.size(), 1U);
    disk.openFails = false;
    log.write("b", kDay1, 1'000);    // exactly then
    EXPECT_EQ(disk.opened.size(), 2U);
    EXPECT_EQ(disk.files[kDay1], (std::vector<std::string>{"a", "b"}));
}

TEST(DailyLogFile, AClockThatWentBackTriesAgainRatherThanNeverAgain) {
    Disk disk;
    disk.openFails = true;
    DailyLogFile log(settings(100, 1'000));
    log.attach(std::make_unique<FakeTarget>(disk), kDay1, 50'000);
    disk.openFails = false;
    log.write("a", kDay1, 10);
    EXPECT_TRUE(log.isOpen());
}

TEST(DailyLogFile, AFailedWriteClosesTheFileKeepsTheLineAndRetriesLater) {
    Disk disk;
    DailyLogFile log(settings(100, 1'000));
    log.attach(std::make_unique<FakeTarget>(disk), kDay1, 0);
    disk.writesBeforeFailure = 1;
    log.write("a", kDay1, 10);       // written
    log.write("b", kDay1, 20);       // fails: closed, kept
    EXPECT_FALSE(log.isOpen());
    EXPECT_EQ(disk.closes, 1);
    EXPECT_EQ(log.pending(), 1U);
    disk.writesBeforeFailure = -1;
    log.write("c", kDay1, 500);      // too soon: kept too
    EXPECT_EQ(log.pending(), 2U);
    log.write("d", kDay1, 1'020);
    EXPECT_EQ(disk.files[kDay1], (std::vector<std::string>{"a", "b", "c", "d"}));
    EXPECT_EQ(log.pending(), 0U);
}

TEST(DailyLogFile, AFailureWhileWritingWhatWaitedKeepsTheRestInOrder) {
    Disk disk;
    DailyLogFile log(settings(100, 1'000));
    log.write("1", kDay1, 0);
    log.write("2", kDay1, 0);
    log.write("3", kDay1, 0);
    disk.writesBeforeFailure = 1;
    log.attach(std::make_unique<FakeTarget>(disk), kDay1, 0);   // "1" written, then the file fails
    EXPECT_FALSE(log.isOpen());
    EXPECT_EQ(log.pending(), 2U);
    disk.writesBeforeFailure = -1;
    log.write("4", kDay1, 2'000);
    EXPECT_EQ(disk.files[kDay1], (std::vector<std::string>{"1", "2", "3", "4"}));
}

TEST(DailyLogFile, AFailureWritingTheDropCountKeepsTheCount) {
    Disk disk;
    DailyLogFile log(settings(1, 1'000), note);
    log.write("1", kDay1, 0);
    log.write("2", kDay1, 0);
    disk.writesBeforeFailure = 0;
    log.attach(std::make_unique<FakeTarget>(disk), kDay1, 0);
    EXPECT_EQ(log.dropped(), 1U);
    EXPECT_EQ(log.pending(), 1U);
    disk.writesBeforeFailure = -1;
    log.write("3", kDay1, 1'000);
    EXPECT_EQ(disk.files[kDay1], (std::vector<std::string>{
                                     "NOTE 1 earlier lines were dropped while this log had no file", "2", "3"}));
}

TEST(DailyLogFile, AnOpenFileTakesLinesWithoutTryingToOpenAgain) {
    Disk disk;
    DailyLogFile log(settings(100, 0));   // a retry time of 0 must not reopen an open file
    log.attach(std::make_unique<FakeTarget>(disk), kDay1, 0);
    for (int i = 0; i < 5; ++i) log.write("x", kDay1, static_cast<std::uint64_t>(i) * 100'000);
    EXPECT_EQ(disk.opened.size(), 1U);
    EXPECT_EQ(disk.files[kDay1].size(), 5U);
}

}  // namespace
}  // namespace wheelbri
