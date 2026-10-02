#include "wheelemu/LocalDate.hpp"
#include "wheelemu/Retention.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <system_error>

namespace wheelemu {
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
    EXPECT_EQ(findDate("wheelemu-service-2026-09-30.log"), "2026-09-30");
    EXPECT_EQ(findDate("telemetry-20260930-142501.csv"), "2026-09-30");  // format 1 names
    EXPECT_EQ(findDate("wheelemu-service.1.log"), std::nullopt);
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
    EXPECT_TRUE(isManagedFile("wheelemu-service-2026-09-30.log"));
    EXPECT_TRUE(isManagedFile("wheelemu-shim-2026-09-30.log"));
    EXPECT_TRUE(isManagedFile("wheelemu-bridge-2026-09-30.log"));
    EXPECT_TRUE(isManagedFile("telemetry-2026-09-30.csv"));
    EXPECT_TRUE(isManagedFile("ffb-2026-09-30.csv.xz"));
    EXPECT_TRUE(isManagedFile("wheelemu-logs-2026-09-30.7z"));
    EXPECT_FALSE(isManagedFile("notes.txt"));
    EXPECT_TRUE(isArchive("wheelemu-logs-2026-09-30.7z"));
    EXPECT_TRUE(isArchive("telemetry-2026-09-30.csv.zst"));
    EXPECT_FALSE(isArchive("telemetry-2026-09-30.csv"));
}

class RetentionFolder : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / "wheelemu-retention-test";
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
    for (const char* name : {"wheelemu-service-2026-09-20.log", "telemetry-2026-09-20.csv", "ffb-2026-09-20.csv",
                             "wheelemu-shim-2026-09-21.log", "telemetry-2026-09-23.csv", "telemetry-2026-09-30.csv",
                             "wheelemu-logs-2026-09-10.7z", "notes-2026-09-01.txt"}) {
        touch(name);
    }
    const auto old = filesBefore(dir_, "2026-09-23", false);
    ASSERT_EQ(old.size(), 2U);
    EXPECT_EQ(old.at("2026-09-20").size(), 3U);
    EXPECT_EQ(old.at("2026-09-21").size(), 1U);

    const auto withArchives = filesBefore(dir_, "2026-09-23", true);
    ASSERT_EQ(withArchives.size(), 3U);
    EXPECT_EQ(withArchives.at("2026-09-10").front().filename(), "wheelemu-logs-2026-09-10.7z");
}

TEST_F(RetentionFolder, UndatedFilesUseTheirLastWriteDay) {
    touch("wheelemu-service.log");
    EXPECT_TRUE(filesBefore(dir_, localDate(WallClock::now()), true).empty());  // written today
    fs::last_write_time(dir_ / "wheelemu-service.log",
                        fs::file_time_type::clock::now() - std::chrono::hours(24 * 30));
    EXPECT_EQ(filesBefore(dir_, localDate(WallClock::now()), true).size(), 1U);
}

}  // namespace
}  // namespace wheelemu
