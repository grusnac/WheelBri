// AppLogLevel: the Logs tab's Level button for the whole app (warn -> info -> debug -> trace -> warn), remembered in
// the app's state file, applied to this process and handed to the listeners (the helper, the drivers).

#include "AppLogLevel.hpp"

#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/Logging.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace wheelbri::service {
namespace {

class AppLogLevelTest : public ::testing::Test {
protected:
    void SetUp() override {
        before_ = logging::currentLevel();
        dir_ = std::filesystem::temp_directory_path() /
               ("wheelbri-apploglevel-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        stateFile_ = dir_ / "app.ini";
    }
    void TearDown() override {
        spdlog::set_level(before_);
        std::filesystem::remove_all(dir_);
    }
    void store(const std::string& text) { std::ofstream(stateFile_) << text; }

    spdlog::level::level_enum before_ = spdlog::level::info;
    std::filesystem::path dir_;
    std::filesystem::path stateFile_;
};

TEST_F(AppLogLevelTest, NothingStoredIsWarnAndThisProcessLogsAtWarn) {
    logging::setLevel("trace");
    AppLogLevel level;
    EXPECT_FALSE(level.started());
    level.start(stateFile_);
    EXPECT_TRUE(level.started());
    EXPECT_EQ(level.current(), LogLevel::Warn);
    EXPECT_EQ(logging::currentLevel(), spdlog::level::warn);
    EXPECT_EQ(level.changes(), 0U);
    EXPECT_FALSE(std::filesystem::exists(stateFile_));   // starting writes nothing
}

TEST_F(AppLogLevelTest, TheStoredLevelIsTakenAndAQuietOneIsWarn) {
    store("[app]\nlog_level = debug\n");
    AppLogLevel debug;
    debug.start(stateFile_);
    EXPECT_EQ(debug.current(), LogLevel::Debug);
    EXPECT_EQ(logging::currentLevel(), spdlog::level::debug);
    store("[app]\nlog_level = off\n");   // edited by hand: warnings and errors stay
    AppLogLevel off;
    off.start(stateFile_);
    EXPECT_EQ(off.current(), LogLevel::Warn);
    store("[app]\nlog_level = chatty\n");
    AppLogLevel unknown;
    unknown.start(stateFile_);
    EXPECT_EQ(unknown.current(), LogLevel::Warn);
}

TEST_F(AppLogLevelTest, EachClickTakesTheNextLevelRemembersItAndTellsTheListeners) {
    store("; WheelBri app state\n[app]\nrig = my rig\n");
    AppLogLevel level;
    level.start(stateFile_);
    std::vector<LogLevel> heard;
    level.addListener([&](const LogLevel l) { heard.push_back(l); });
    EXPECT_EQ(level.next(), LogLevel::Info);
    EXPECT_EQ(logging::currentLevel(), spdlog::level::info);
    EXPECT_EQ(readAppSetting(stateFile_, "log_level"), "info");
    EXPECT_EQ(readAppSetting(stateFile_, "rig"), "my rig");   // the other settings stay
    EXPECT_EQ(level.next(), LogLevel::Debug);
    EXPECT_EQ(level.next(), LogLevel::Trace);
    EXPECT_EQ(logging::currentLevel(), spdlog::level::trace);
    EXPECT_EQ(level.next(), LogLevel::Warn);
    EXPECT_EQ(logging::currentLevel(), spdlog::level::warn);
    EXPECT_EQ(readAppSetting(stateFile_, "log_level"), "warn");
    EXPECT_EQ(heard, (std::vector<LogLevel>{LogLevel::Info, LogLevel::Debug, LogLevel::Trace, LogLevel::Warn}));
    EXPECT_EQ(level.changes(), 4U);
    // The next start of the app begins where it was left.
    level.set(LogLevel::Debug);
    AppLogLevel again;
    again.start(stateFile_);
    EXPECT_EQ(again.current(), LogLevel::Debug);
}

TEST_F(AppLogLevelTest, TheSameLevelAgainChangesNothingAndAQuietOneIsWarn) {
    AppLogLevel level;
    level.start(stateFile_);
    int calls = 0;
    level.addListener([&](LogLevel) { ++calls; });
    level.set(LogLevel::Warn);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(level.changes(), 0U);
    level.set(LogLevel::Debug);
    level.set(LogLevel::Off);   // never quieter than warn
    EXPECT_EQ(level.current(), LogLevel::Warn);
    EXPECT_EQ(logging::currentLevel(), spdlog::level::warn);
    EXPECT_EQ(calls, 2);
}

TEST_F(AppLogLevelTest, AStateFileThatCannotBeWrittenStillChangesTheLevel) {
    std::filesystem::create_directories(stateFile_);   // a folder where the file should be
    AppLogLevel level;
    level.start(stateFile_);
    EXPECT_EQ(level.next(), LogLevel::Info);
    EXPECT_EQ(level.current(), LogLevel::Info);
    EXPECT_EQ(logging::currentLevel(), spdlog::level::info);
}

TEST_F(AppLogLevelTest, WithoutTheAppItIsThisProcesssOwnLevel) {
    AppLogLevel level;   // the command-line service: never started
    logging::setLevel("debug");
    EXPECT_EQ(level.current(), LogLevel::Debug);
    logging::setLevel("error");   // its config may say error: the others still get warn
    EXPECT_EQ(level.current(), LogLevel::Warn);
    logging::setLevel("off");
    EXPECT_EQ(level.current(), LogLevel::Warn);
}

// Set before start (no state file): the level is applied even when it is the default one, and nothing is written.
TEST_F(AppLogLevelTest, ASetBeforeStartAppliesEvenTheDefaultAndWritesNothing) {
    AppLogLevel level;
    logging::setLevel("debug");
    level.set(kDefaultAppLogLevel);
    EXPECT_TRUE(level.started());
    EXPECT_EQ(level.changes(), 1U);
    EXPECT_EQ(level.current(), kDefaultAppLogLevel);
    EXPECT_EQ(logging::currentLevel(), spdlog::level::warn);
    EXPECT_FALSE(std::filesystem::exists(stateFile_));
}

TEST_F(AppLogLevelTest, TheProcessHasOne) {
    EXPECT_EQ(&AppLogLevel::shared(), &AppLogLevel::shared());
}

}  // namespace
}  // namespace wheelbri::service
