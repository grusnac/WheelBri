#include "wheelbri/LocalDate.hpp"
#include "wheelbri/Retention.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace wheelbri {
namespace {

namespace fs = std::filesystem;

TEST(LocalDate, AddDaysCrossesMonthsAndYears) {
    EXPECT_EQ(addDays("2026-09-30", 1), "2026-10-01");
    EXPECT_EQ(addDays("2026-03-01", -1), "2026-02-28");
    EXPECT_EQ(addDays("2024-03-01", -1), "2024-02-29");
    EXPECT_EQ(addDays("2026-12-31", 1), "2027-01-01");
    EXPECT_EQ(addDays("2026-09-30", -7), "2026-09-23");
    EXPECT_EQ(addDays("not a date", 1), std::nullopt);
}

TEST(LocalDate, StartOfDayIsLocalMidnight) {
    const auto start = startOfDay("2026-09-30");
    ASSERT_TRUE(start.has_value());
    EXPECT_EQ(localDate(*start), "2026-09-30");
    EXPECT_EQ(localDate(*start - std::chrono::seconds(1)), "2026-09-29");
}

TEST(LocalDate, FindsDatesInFileNames) {
    EXPECT_EQ(findDate("telemetry-2026-09-30.csv"), "2026-09-30");
    EXPECT_EQ(findDate("wheelbri-service-2026-09-30.log"), "2026-09-30");
    EXPECT_EQ(findDate("wheelbri-service-2026-09-30-03.log"), "2026-09-30");   // numbered per run
    EXPECT_EQ(findDate("telemetry-2026-09-30-12.log.xz"), "2026-09-30");
    EXPECT_EQ(findDate("telemetry-20260930-142501.csv"), "2026-09-30");  // format 1 names
    EXPECT_EQ(findDate("wheelbri-service.1.log"), std::nullopt);
    EXPECT_EQ(findDate("x-2026-13-01.log"), std::nullopt);
}

TEST(Retention, ParsesActionsAndArchivers) {
    EXPECT_EQ(parseOldFilesAction("archive"), OldFilesAction::Archive);
    EXPECT_EQ(parseOldFilesAction("delete"), OldFilesAction::Delete);
    EXPECT_EQ(parseOldFilesAction("keep"), OldFilesAction::Keep);
    EXPECT_EQ(parseOldFilesAction("remove"), std::nullopt);
    EXPECT_TRUE(isKnownArchiver("7z"));
    EXPECT_TRUE(isKnownArchiver("auto"));
    EXPECT_FALSE(isKnownArchiver("rar"));
}

TEST(Retention, OnlyTouchesItsOwnFiles) {
    EXPECT_TRUE(isManagedFile("wheelbri-service-2026-09-30.log"));
    EXPECT_TRUE(isManagedFile("wheelbri-shim-2026-09-30.log"));
    EXPECT_TRUE(isManagedFile("wheelbri-bridge-2026-09-30.log"));
    EXPECT_TRUE(isManagedFile("wheelbri-logi-2026-10-11-01.log"));   // wheelbri-logi.dll inside a game
    EXPECT_TRUE(isManagedFile("wheelemu-service-2026-09-30.log"));   // the old name: moved in with the folder
    EXPECT_TRUE(isManagedFile("wheelemu-shim-2026-09-30.log"));
    EXPECT_FALSE(isManagedFile("wheelemu.ini"));
    EXPECT_TRUE(isManagedFile("telemetry-2026-09-30.csv"));
    EXPECT_TRUE(isManagedFile("telemetry-2026-09-30.log"));   // since format 3
    EXPECT_TRUE(isManagedFile("ffb-2026-09-30.log"));
    EXPECT_TRUE(isManagedFile("ffb-2026-09-30.csv.xz"));
    EXPECT_TRUE(isManagedFile("wheelbri-logs-2026-09-30.7z"));
    EXPECT_FALSE(isManagedFile("notes.txt"));
    EXPECT_TRUE(isManagedFile("wheelbri-driver-2026-09-30.log"));         // the drivers and installers: same folder
    EXPECT_TRUE(isManagedFile("wheelbri-hide-2026-09-30.log"));
    EXPECT_TRUE(isManagedFile("wheelbri-hide-install-2026-09-30.log"));
    EXPECT_TRUE(isManagedFile("wheelbri-helper-2026-10-06.log"));         // the WheelBri Helper (service)
    EXPECT_TRUE(isManagedFile("wheelbri-install-2026-09-30.log"));
    EXPECT_TRUE(isArchive("wheelbri-logs-2026-09-30.7z"));
    EXPECT_TRUE(isArchive("telemetry-2026-09-30.csv.zst"));
    EXPECT_FALSE(isArchive("telemetry-2026-09-30.csv"));
}

class RetentionFolder : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / "wheelbri-retention-test";
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    void touch(const std::string& name) { std::ofstream(dir_ / name) << "x"; }

    fs::path dir_;
};

TEST_F(RetentionFolder, GroupsOldFilesByDay) {
    for (const char* name : {"wheelbri-service-2026-09-20.log", "telemetry-2026-09-20.csv", "ffb-2026-09-20.csv",
                             "wheelbri-shim-2026-09-21.log", "telemetry-2026-09-23.csv", "telemetry-2026-09-30.csv",
                             "wheelbri-logs-2026-09-10.7z", "notes-2026-09-01.txt"}) {
        touch(name);
    }
    const auto old = filesBefore(dir_, "2026-09-23", false);
    ASSERT_EQ(old.size(), 2U);
    EXPECT_EQ(old.at("2026-09-20").size(), 3U);
    EXPECT_EQ(old.at("2026-09-21").size(), 1U);

    const auto withArchives = filesBefore(dir_, "2026-09-23", true);
    ASSERT_EQ(withArchives.size(), 3U);
    EXPECT_EQ(withArchives.at("2026-09-10").front().filename(), "wheelbri-logs-2026-09-10.7z");
}

TEST_F(RetentionFolder, UndatedFilesUseTheirLastWriteDay) {
    touch("wheelbri-service.log");
    EXPECT_TRUE(filesBefore(dir_, localDate(WallClock::now()), true).empty());  // written today
    fs::last_write_time(dir_ / "wheelbri-service.log",
                        fs::file_time_type::clock::now() - std::chrono::hours(24 * 30));
    EXPECT_EQ(filesBefore(dir_, localDate(WallClock::now()), true).size(), 1U);
}

// Dates that are not dates: the wrong length or separators, month or day out of range, numbers that run on. None is
// a date, so a file named so falls back to its last-write day instead of a made-up one.
TEST(LocalDate, RejectsWhatIsNotACalendarDate) {
    for (const char* bad : {"2026-00-10", "2026-13-10", "2026-09-00", "2026-09-32", "2026/09/30", "2026-09/30",
                            "2026-9-30", "2026-09-301", "", "abcd-ef-gh"}) {
        EXPECT_EQ(addDays(bad, 1), std::nullopt) << bad;
        EXPECT_EQ(startOfDay(bad), std::nullopt) << bad;
    }
    EXPECT_EQ(findDate("x-12026-09-30.log"), std::nullopt);    // inside a longer number
    EXPECT_EQ(findDate("x-202609301.log"), std::nullopt);      // nine digits: not a format 1 date
    EXPECT_EQ(findDate("x-20261301.log"), std::nullopt);       // month 13
    EXPECT_EQ(findDate("x-2026-09-3"), std::nullopt);          // cut short at the end
    EXPECT_EQ(findDate("x-2026"), std::nullopt);
    EXPECT_EQ(findDate("x-2026-0a-30-20260930.log"), "2026-09-30");   // a later good one is found
}

// Upper case (Windows names), every component's prefix and the old wheelemu names are WheelBri's; names that only
// look alike are not.
TEST(Retention, KnowsEveryComponentAndNothingElse) {
    for (const char* ours : {"WheelBri-Service-2026-09-30-01.LOG", "wheelbri-uhid-2026-09-30-01.log",
                             "wheelbri-sh-2026-09-30-01.log", "wheelbri-sh.log", "wheelbri-logs-2026-09-30.zip",
                             "wheelemu-driver-2026-09-30.log", "TELEMETRY-2026-09-30-01.log"}) {
        EXPECT_TRUE(isManagedFile(ours)) << ours;
    }
    for (const char* theirs : {"wheelbri-shortcut-2026-09-30.lnk", "wheelbri.ini", "app.ini", "my-g920.ini",
                               "wheelbriservice-2026-09-30.log", "xwheelbri-service-2026-09-30.log",
                               "telemetry.log", "ffb.log", "perf-2026-09-30-142501.log", "wheelbri-", ""}) {
        EXPECT_FALSE(isManagedFile(theirs)) << theirs;
    }
    for (const char* archive : {"x.7z", "x.ZIP", "x.log.xz", "x.log.zst", "x.log.gz"}) {
        EXPECT_TRUE(isArchive(archive)) << archive;
    }
    for (const char* plain : {"x.log", "x.gzip", "xz", ""}) {
        EXPECT_FALSE(isArchive(plain)) << plain;
    }
    EXPECT_STREQ(toString(OldFilesAction::Keep), "keep");
    EXPECT_STREQ(toString(OldFilesAction::Delete), "delete");
    EXPECT_STREQ(toString(OldFilesAction::Archive), "archive");
}

// A file whose day cannot be read (gone meanwhile) is today's: never removed by mistake.
TEST(Retention, AnUnknownDayIsToday) {
    EXPECT_EQ(fileDay("/nonexistent-wheelbri-folder/wheelbri-service.log"), localDate(WallClock::now()));
    EXPECT_EQ(fileDay("/nonexistent-wheelbri-folder/ffb-2026-09-01-02.log"), "2026-09-01");
}

// A folder that is not there gives nothing; folders named like logs and other programs' files with dates are never
// listed; a name that is not ASCII is read and kept.
TEST_F(RetentionFolder, NeverListsWhatIsNotWheelBrisOwnFile) {
    EXPECT_TRUE(filesBefore(dir_ / "missing", "2026-09-30", true).empty());
    fs::create_directories(dir_ / "wheelbri-service-2026-09-01.log");      // a folder, not a log
    touch("notes-2026-09-01.txt");
    touch("game-2026-09-01.log");
    touch("wheelbri-shortcut-2026-09-01.lnk");
    touch("telemetry-\xC3\xBC-2026-09-01-01.log");                          // a letter that is not ASCII
    touch("WHEELBRI-SHIM-2026-09-01-01.LOG");
    touch("ffb-2026-09-01-01.log");
    const auto old = filesBefore(dir_, "2026-09-30", true);
    ASSERT_EQ(old.size(), 1U);
    const auto& files = old.at("2026-09-01");
    std::vector<std::string> names;
    for (const auto& file : files) names.push_back(file.filename().string());
    EXPECT_EQ(names, (std::vector<std::string>{"WHEELBRI-SHIM-2026-09-01-01.LOG", "ffb-2026-09-01-01.log",
                                               "telemetry-\xC3\xBC-2026-09-01-01.log"}));
}

// Every component's log is recognised under both names the project has had, and only with the dash that
// ends the component ("wheelbri-shimmy" is still the shim's; "wheelbri-shx" is no one's).
TEST(Retention, EveryComponentUnderBothNames) {
    for (const char* project : {"wheelbri", "wheelemu", "WheelBri"}) {
        for (const char* component : {"-service-", "-shim-", "-bridge-", "-logs-", "-driver-", "-hide-", "-install-",
                                      "-uhid-", "-helper-", "-logi-", "-sh.", "-sh-"}) {
            const std::string name = std::string(project) + component + "2026-09-30.log";
            EXPECT_TRUE(isManagedFile(name)) << name;
        }
        EXPECT_FALSE(isManagedFile(std::string(project) + "-shx-2026-09-30.log"));
        EXPECT_FALSE(isManagedFile(std::string(project) + "-unknown-2026-09-30.log"));
    }
    EXPECT_TRUE(isManagedFile("ffb-2026-09-30-01.log"));
}

// A value outside the enum (a corrupt setting cast in) still has a name, never a crash.
TEST(Retention, AnUnknownActionHasAName) {
    EXPECT_STREQ(toString(static_cast<OldFilesAction>(99)), "?");
    EXPECT_EQ(parseOldFilesAction(""), std::nullopt);
    EXPECT_EQ(parseOldFilesAction("Keep"), std::nullopt);   // the INI's words are lower case
}

// A date at the very end of a name, a dash in the wrong place or a letter in the day: found only when it is a date.
TEST(LocalDate, DatesAtTheEdgesOfAName) {
    EXPECT_EQ(findDate("20261001"), "2026-10-01");              // eight digits that end the text
    EXPECT_EQ(findDate("log-2026-10x01.log"), std::nullopt);    // not a dash before the day
    EXPECT_EQ(findDate("log-2026-10-x1.log"), std::nullopt);    // not a digit in the day
    EXPECT_EQ(findDate("log-2026-10-0"), std::nullopt);          // cut short
}

}  // namespace
}  // namespace wheelbri
