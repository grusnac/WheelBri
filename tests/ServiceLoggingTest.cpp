// SPDX-License-Identifier: GPL-3.0-or-later
// The service's folders and the startup banner (service/src/ServiceLogging.cpp),
// on Linux: every folder follows XDG_CONFIG_HOME / XDG_STATE_HOME (or HOME), set per test to a temporary folder.

#include "ServiceLogging.hpp"

#include "LogLinesCapture.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

namespace wheelbri::service {
namespace {

namespace fs = std::filesystem;

// An environment variable set (or removed) for one test, then put back.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::optional<std::string>& value) : name_(name) {
        if (const char* old = std::getenv(name)) old_ = old;
        if (value) {
            setenv(name, value->c_str(), 1);
        } else {
            unsetenv(name);
        }
    }
    ~ScopedEnv() {
        if (old_) {
            setenv(name_, old_->c_str(), 1);
        } else {
            unsetenv(name_);
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* name_;
    std::optional<std::string> old_;
};

class ServiceFolders : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("wheelbri-servicelogging-" + std::to_string(::getuid()) + "-" + std::to_string(::getpid()) + "-" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        // Any notes an earlier test left behind are logged (and dropped) here, before this test's own.
        logStartupBanner("");
    }
    void TearDown() override { fs::remove_all(dir_); }

    static void write(const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }
    [[nodiscard]] static std::string read(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    fs::path dir_;
};

// XDG_CONFIG_HOME holds the settings (and everything named after them), XDG_STATE_HOME the logs.
TEST_F(ServiceFolders, TheXdgFoldersHoldEverything) {
    const ScopedEnv config("XDG_CONFIG_HOME", (dir_ / "config").string());
    const ScopedEnv state("XDG_STATE_HOME", (dir_ / "state").string());
    const fs::path settings = dir_ / "config" / "wheelbri";
    EXPECT_EQ(userSettingsFolder(), settings);
    EXPECT_EQ(deviceLabelsFolder(), settings / "devices");
    EXPECT_EQ(userConfigFolder(), settings / "configs");
    EXPECT_EQ(rigsFolder(), settings / "rigs");
    EXPECT_EQ(appStateFile(), settings / "app.ini");
    EXPECT_EQ(userGamesFile(), settings / "games.ini");
    EXPECT_EQ(shimGamesFile(), settings / "shim-games.txt");
    EXPECT_EQ(fs::path(defaultLogFile()), dir_ / "state" / "wheelbri" / "wheelbri-service.log");
}

// Without the XDG variables: ~/.config/wheelbri and ~/.local/state/wheelbri.
TEST_F(ServiceFolders, WithoutXdgTheyAreUnderHome) {
    const ScopedEnv home("HOME", dir_.string());
    const ScopedEnv config("XDG_CONFIG_HOME", std::nullopt);
    const ScopedEnv state("XDG_STATE_HOME", std::nullopt);
    EXPECT_EQ(userSettingsFolder(), dir_ / ".config" / "wheelbri");
    EXPECT_EQ(fs::path(defaultLogFile()), dir_ / ".local" / "state" / "wheelbri" / "wheelbri-service.log");
}

// A config may only send logs into WheelBri's own folders: the folder itself or below it, after links and ".." are
// resolved; a sibling whose name starts the same, a parent, a link out and an empty path are outside.
TEST_F(ServiceFolders, OnlyPathsInsideTheUsersFoldersAreInside) {
    const ScopedEnv config("XDG_CONFIG_HOME", (dir_ / "config").string());
    const ScopedEnv state("XDG_STATE_HOME", (dir_ / "state").string());
    fs::create_directories(dir_ / "config" / "wheelbri");
    fs::create_directories(dir_ / "state" / "wheelbri");
    fs::create_directories(dir_ / "elsewhere");

    EXPECT_TRUE(isInsideUserFolders(dir_ / "config" / "wheelbri"));
    EXPECT_TRUE(isInsideUserFolders(dir_ / "config" / "wheelbri" / "logs" / "x.log"));   // need not exist yet
    EXPECT_TRUE(isInsideUserFolders(dir_ / "state" / "wheelbri" / "telemetry.csv"));
    EXPECT_FALSE(isInsideUserFolders(dir_ / "config" / "wheelbri-other" / "x.log"));
    EXPECT_FALSE(isInsideUserFolders(dir_ / "config"));                                  // the parent
    EXPECT_FALSE(isInsideUserFolders(dir_ / "config" / "wheelbri" / ".." / ".." / "elsewhere" / "x.log"));
    EXPECT_FALSE(isInsideUserFolders(dir_ / "elsewhere" / "x.log"));
    fs::create_directory_symlink(dir_ / "elsewhere", dir_ / "config" / "wheelbri" / "out");
    EXPECT_FALSE(isInsideUserFolders(dir_ / "config" / "wheelbri" / "out" / "x.log"));
    EXPECT_FALSE(isInsideUserFolders(fs::path()));
}

// Windows-only checks are nothing elsewhere: no folder problem, no link removed, the log folder left alone.
TEST_F(ServiceFolders, TheWindowsChecksDoNothingHere) {
    const ScopedEnv config("XDG_CONFIG_HOME", (dir_ / "config").string());
    EXPECT_EQ(userFoldersProblem(), "");
    write(dir_ / "logs" / "wheelbri-service.log", "x");
    fs::create_symlink(dir_ / "logs" / "wheelbri-service.log", dir_ / "logs" / "telemetry-link.csv");
    EXPECT_TRUE(removeLinkedLogFiles(dir_ / "logs").empty());
    EXPECT_TRUE(fs::is_symlink(dir_ / "logs" / "telemetry-link.csv"));
    shareLogFolder(dir_ / "logs");
    EXPECT_TRUE(fs::exists(dir_ / "logs" / "wheelbri-service.log"));
}

// Outside the window build a startup problem goes to stderr, prefixed with the program's name.
TEST_F(ServiceFolders, AStartupProblemGoesToStderr) {
    ::testing::internal::CaptureStderr();
    reportStartupProblem("no wheel", true);
    reportStartupProblem("no config", false);
    EXPECT_EQ(::testing::internal::GetCapturedStderr(), "wheelbri: no wheel\nwheelbri: no config\n");
}

// The banner: version and platform, the config (or that the built-in defaults run), and SPDLOG_LEVEL when it is set.
TEST_F(ServiceFolders, TheBannerSaysWhatRuns) {
    {
        const ScopedEnv levels("SPDLOG_LEVEL", std::nullopt);
        testing_support::LogLinesCapture capture("service");
        logStartupBanner("");
        EXPECT_TRUE(capture.has("wheelbri-service "));
        EXPECT_TRUE(capture.has("config: (built-in defaults)"));
        EXPECT_FALSE(capture.has("SPDLOG_LEVEL"));
    }
    {
        const ScopedEnv levels("SPDLOG_LEVEL", std::string("debug"));
        testing_support::LogLinesCapture capture("service");
        logStartupBanner("/x/my.ini");
        EXPECT_TRUE(capture.has("config: /x/my.ini"));
        EXPECT_TRUE(capture.has("log levels overridden by SPDLOG_LEVEL='debug'"));
    }
}

}  // namespace
}  // namespace wheelbri::service
