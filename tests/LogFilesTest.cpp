// LogFileNames / LogFiles: one numbered log file per run of a program, the newest found again.

#include "wheelbri/LogFiles.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace wheelbri::logfiles {
namespace {

namespace fs = std::filesystem;

// --- the names ---

TEST(LogFileNames, NumbersHaveAtLeastTwoDigits) {
    EXPECT_EQ(numberedName("wheelbri-service", "2026-10-08", 3), "wheelbri-service-2026-10-08-03.log");
    EXPECT_EQ(numberedName("wheelbri-service", "2026-10-08", 12), "wheelbri-service-2026-10-08-12.log");
    EXPECT_EQ(numberedName("telemetry", "2026-10-08", 100, ".log"), "telemetry-2026-10-08-100.log");
    EXPECT_EQ(numberedName("x", "2026-10-08", -5), "x-2026-10-08-00.log");
    EXPECT_EQ(numberedName("x", "2026-10-08", 1, ".csv"), "x-2026-10-08-01.csv");
}

TEST(LogFileNames, DatesAreTenCharactersOfDigitsAndDashes) {
    EXPECT_TRUE(isDateText("2026-10-08"));
    EXPECT_FALSE(isDateText("2026-10-8"));
    EXPECT_FALSE(isDateText("2026/10/08"));
    EXPECT_FALSE(isDateText("2026-1a-08"));
    EXPECT_FALSE(isDateText("20261008XX"));
    EXPECT_FALSE(isDateText(""));
}

TEST(LogFileNames, ReadsNumberedOldAndArchivedNames) {
    auto parts = partsOf("wheelbri-service-2026-10-08-03.log", "wheelbri-service");
    ASSERT_TRUE(parts.has_value());
    EXPECT_EQ(parts->date, "2026-10-08");
    EXPECT_EQ(parts->index, 3);
    parts = partsOf("wheelbri-service-2026-10-08.log", "wheelbri-service");   // the old daily file
    ASSERT_TRUE(parts.has_value());
    EXPECT_EQ(parts->index, 0);
    parts = partsOf("telemetry-2026-10-08-12.log.xz", "telemetry");          // archived
    ASSERT_TRUE(parts.has_value());
    EXPECT_EQ(parts->index, 12);
    parts = partsOf("ffb-2026-10-08-100.log", "ffb");
    ASSERT_TRUE(parts.has_value());
    EXPECT_EQ(parts->index, 100);
}

TEST(LogFileNames, OtherNamesAreNotTheStemsLogs) {
    EXPECT_FALSE(partsOf("wheelbri-driver-install-2026-10-08.log", "wheelbri-driver").has_value());
    EXPECT_FALSE(partsOf("wheelbri-shim-2026-10-08-01.log", "wheelbri-service").has_value());
    EXPECT_FALSE(partsOf("wheelbri-service.log", "wheelbri-service").has_value());
    EXPECT_FALSE(partsOf("wheelbri-service-2026-10-08", "wheelbri-service").has_value());      // no extension
    EXPECT_FALSE(partsOf("wheelbri-service-2026-10-08-.log", "wheelbri-service").has_value());  // no number
    EXPECT_FALSE(partsOf("wheelbri-service-2026-10-08-3a.log", "wheelbri-service").has_value());
    EXPECT_FALSE(partsOf("wheelbri-service-2026-10-08-03", "wheelbri-service").has_value());   // ends at the number
    EXPECT_FALSE(partsOf("wheelbri-service-2026-10-08_03.log", "wheelbri-service").has_value());
    EXPECT_FALSE(partsOf("wheelbri-service-2026-10-08-999999.log", "wheelbri-service").has_value());   // too large
    EXPECT_FALSE(partsOf("wheelbri-serviceX2026-10-08-01.log", "wheelbri-service").has_value());
    EXPECT_FALSE(partsOf("wheelbri-service-x026-10-08-01.log", "wheelbri-service").has_value());
    EXPECT_FALSE(partsOf("", "wheelbri-service").has_value());
}

TEST(LogFileNames, TheNextNumberIsOneMoreThanTheHighestOfThatDay) {
    const std::vector<std::string> names = {
        "wheelbri-service-2026-10-08-01.log", "wheelbri-service-2026-10-08-04.log",   // a gap: not filled
        "wheelbri-service-2026-10-07-09.log",                                         // another day
        "wheelbri-shim-2026-10-08-11.log",                                            // another stem
        "wheelbri-service-2026-10-08-06.log.xz",                                      // archived counts
        "notes.txt",
    };
    EXPECT_EQ(nextIndex(names, "wheelbri-service", "2026-10-08"), 7);
    EXPECT_EQ(nextIndex(names, "wheelbri-service", "2026-10-09"), 1);
    EXPECT_EQ(nextIndex({}, "wheelbri-service", "2026-10-08"), 1);
    EXPECT_EQ(nextIndex({"wheelbri-service-2026-10-08.log"}, "wheelbri-service", "2026-10-08"), 1);   // old: 0
    EXPECT_EQ(nextIndex({"x-2026-10-08-99999.log"}, "x", "2026-10-08"), kMaxIndex);   // never past the limit
    EXPECT_TRUE(hasIndex(names, "wheelbri-service", "2026-10-08", 4));
    EXPECT_FALSE(hasIndex(names, "wheelbri-service", "2026-10-08", 2));
    EXPECT_FALSE(hasIndex(names, "wheelbri-service", "2026-10-07", 4));
}

TEST(LogFileNames, TheNewestIsTheLatestDayThenTheHighestNumber) {
    EXPECT_EQ(newestOf({"a-2026-10-07-09.log", "a-2026-10-08-02.log", "a-2026-10-08-10.log", "a-2026-10-08-03.log"}, "a"),
              "a-2026-10-08-10.log");   // 10 after 03, not as text
    EXPECT_EQ(newestOf({"a-2026-10-08.log", "a-2026-10-08-01.log"}, "a"), "a-2026-10-08-01.log");
    EXPECT_EQ(newestOf({"a-2026-10-09.log", "a-2026-10-08-07.log"}, "a"), "a-2026-10-09.log");   // a later old file
    EXPECT_EQ(newestOf({"a-2026-10-08-02.log.xz", "a-2026-10-08-02.log"}, "a"), "a-2026-10-08-02.log");
    EXPECT_EQ(newestOf({"a-2026-10-08-02.log", "a-2026-10-08-02.log.xz"}, "a"), "a-2026-10-08-02.log");
    EXPECT_EQ(newestOf({"a-2026-10-08-02.log.xz", "a-2026-10-08-02.log.gz"}, "a"), "a-2026-10-08-02.log.xz");
    EXPECT_FALSE(newestOf({"b-2026-10-08-02.log", "readme.txt"}, "a").has_value());
    EXPECT_FALSE(newestOf({}, "a").has_value());
}

// --- the files ---

class LogFilesFolder : public ::testing::Test {
protected:
    void SetUp() override {
        folder_ = fs::temp_directory_path() /
                  ("wheelbri-logfiles-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::error_code ec;
        fs::remove_all(folder_, ec);
        fs::create_directories(folder_);
        forgetRun();
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(folder_, ec);
        forgetRun();
    }
    void touch(const std::string& name) const { std::ofstream(folder_ / name) << "x"; }
    fs::path run(const std::string& stem, const std::string& date = "2026-10-08") const {
        std::string error;
        const fs::path path = runFile(folder_, stem, date, ".log", error);
        EXPECT_FALSE(path.empty()) << error;
        return path;
    }
    fs::path folder_;
};

TEST_F(LogFilesFolder, TheFirstRunOfTheDayIsOneAndItsFileIsMade) {
    const fs::path path = run("wheelbri-service");
    EXPECT_EQ(path.filename(), "wheelbri-service-2026-10-08-01.log");
    EXPECT_TRUE(fs::exists(path));
    EXPECT_EQ(fs::file_size(path), 0U);
}

TEST_F(LogFilesFolder, TheSameRunGetsTheSameFileAgain) {
    const fs::path first = run("wheelbri-service");
    std::ofstream(first, std::ios::app | std::ios::binary) << "a session\n";
    EXPECT_EQ(run("wheelbri-service"), first);   // a second session of this run appends to it
    EXPECT_EQ(fs::file_size(first), 10U);        // not made again (emptied)
}

TEST_F(LogFilesFolder, ANewRunTakesTheNextNumberAfterWhatIsThere) {
    touch("wheelbri-service-2026-10-08-01.log");
    touch("wheelbri-service-2026-10-08-02.log");
    EXPECT_EQ(run("wheelbri-service").filename(), "wheelbri-service-2026-10-08-03.log");
    forgetRun();   // the program starts again
    EXPECT_EQ(run("wheelbri-service").filename(), "wheelbri-service-2026-10-08-04.log");
}

TEST_F(LogFilesFolder, EveryFileOfOneRunCarriesTheRunsNumber) {
    touch("wheelbri-service-2026-10-08-01.log");
    touch("wheelbri-service-2026-10-08-02.log");
    EXPECT_EQ(run("wheelbri-service").filename(), "wheelbri-service-2026-10-08-03.log");
    // Telemetry of this run has no file yet: it takes 03 too, not 01.
    EXPECT_EQ(run("telemetry").filename(), "telemetry-2026-10-08-03.log");
    EXPECT_EQ(run("ffb").filename(), "ffb-2026-10-08-03.log");
}

TEST_F(LogFilesFolder, ARunNumberAlreadyTakenForAStemTakesThatStemsNext) {
    touch("telemetry-2026-10-08-01.log");   // an older run's telemetry, with a number this run now has
    EXPECT_EQ(run("wheelbri-service").filename(), "wheelbri-service-2026-10-08-01.log");
    EXPECT_EQ(run("telemetry").filename(), "telemetry-2026-10-08-02.log");
}

TEST_F(LogFilesFolder, ANewDayStartsAtOneAgainInTheSameRun) {
    touch("wheelbri-service-2026-10-08-05.log");
    EXPECT_EQ(run("wheelbri-service").filename(), "wheelbri-service-2026-10-08-06.log");
    EXPECT_EQ(run("wheelbri-service", "2026-10-09").filename(), "wheelbri-service-2026-10-09-01.log");
    EXPECT_EQ(run("telemetry", "2026-10-09").filename(), "telemetry-2026-10-09-01.log");
}

TEST_F(LogFilesFolder, ANumberTakenAMomentAgoIsSkipped) {
    // Two programs starting at once both see 03 free; the one that creates it second takes 04.
    touch("x-2026-10-08-03.log");
    std::string error;
    int made = 0;
    const fs::path path = createNumbered(folder_, "x", "2026-10-08", ".log", 3, error, &made);
    EXPECT_EQ(path.filename(), "x-2026-10-08-04.log");
    EXPECT_EQ(made, 4);
    EXPECT_EQ(createNumbered(folder_, "x", "2026-10-08", ".log", 0, error).filename(), "x-2026-10-08-01.log");
}

TEST_F(LogFilesFolder, OfManyCreatingOneNameExactlyOneGetsIt) {
    std::atomic<int> made{0};
    std::vector<std::thread> threads;
    std::vector<fs::path> got(8);
    for (std::size_t t = 0; t < got.size(); ++t) {
        threads.emplace_back([&, t] {
            std::string error;
            got[t] = createNumbered(folder_, "race", "2026-10-08", ".log", 1, error);
            if (got[t].filename() == "race-2026-10-08-01.log") ++made;
        });
    }
    for (std::thread& thread : threads) thread.join();
    EXPECT_EQ(made.load(), 1);
    EXPECT_EQ(std::set<fs::path>(got.begin(), got.end()).size(), got.size());   // all different
}

TEST_F(LogFilesFolder, AFileWithoutAnExtensionWorksToo) {
    std::string error;
    const fs::path path = runFile(folder_, "plain", "2026-10-08", "", error);
    EXPECT_EQ(path.filename(), "plain-2026-10-08-01");
    EXPECT_EQ(runFile(folder_, "plain", "2026-10-08", "", error), path);
}

TEST_F(LogFilesFolder, AMissingFolderIsMade) {
    std::string error;
    const fs::path path = runFile(folder_ / "logs" / "deeper", "x", "2026-10-08", ".log", error);
    ASSERT_FALSE(path.empty()) << error;
    EXPECT_EQ(path.filename(), "x-2026-10-08-01.log");
    EXPECT_TRUE(fs::exists(path));
}

TEST_F(LogFilesFolder, AFolderThatCannotBeMadeGivesNoFileAndSaysWhy) {
    touch("blocker");   // a file where the folder should be
    std::string error;
    EXPECT_TRUE(runFile(folder_ / "blocker", "x", "2026-10-08", ".log", error).empty());
    EXPECT_NE(error, "");
}

TEST_F(LogFilesFolder, NoFreeNumberLeftGivesNoFile) {
    touch("x-2026-10-08-99999.log");
    std::string error;
    // The next number is the limit itself, which is taken: nothing is left.
    EXPECT_TRUE(runFile(folder_, "x", "2026-10-08", ".log", error).empty());
    EXPECT_NE(error.find("no free number"), std::string::npos) << error;
}

TEST_F(LogFilesFolder, TheNewestFileIsFoundAgain) {
    EXPECT_TRUE(newestFile(folder_, "wheelbri-shim").empty());
    touch("wheelbri-shim-2026-10-07-04.log");
    touch("wheelbri-shim-2026-10-08-02.log");
    touch("wheelbri-shim-2026-10-08-10.log");
    touch("wheelbri-service-2026-10-09-01.log");
    EXPECT_EQ(newestFile(folder_, "wheelbri-shim"), folder_ / "wheelbri-shim-2026-10-08-10.log");
    EXPECT_TRUE(newestFile(folder_ / "missing", "wheelbri-shim").empty());
}

TEST_F(LogFilesFolder, NamesInListsTheFolderAndNothingForAMissingOne) {
    touch("a.log");
    touch("b.log");
    const std::vector<std::string> names = namesIn(folder_);
    EXPECT_EQ(std::set<std::string>(names.begin(), names.end()), (std::set<std::string>{"a.log", "b.log"}));
    EXPECT_TRUE(namesIn(folder_ / "missing").empty());
    // No folder at all: the current one (a log named without a folder goes there).
    const std::vector<std::string> here = namesIn({});
    const std::vector<std::string> dot = namesIn(".");
    EXPECT_EQ(std::set<std::string>(here.begin(), here.end()), std::set<std::string>(dot.begin(), dot.end()));
}

}  // namespace
}  // namespace wheelbri::logfiles
