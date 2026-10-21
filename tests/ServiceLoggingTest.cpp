// The service's folders, the rename from the old project name and the startup banner (service/src/ServiceLogging.cpp),
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

// The old name's folder alone: renamed as a whole to the new name (logs too), said in the banner once.
TEST_F(ServiceFolders, AnOldFolderAloneIsRenamed) {
    const ScopedEnv config("XDG_CONFIG_HOME", (dir_ / "config").string());
    const ScopedEnv state("XDG_STATE_HOME", (dir_ / "state").string());
    write(dir_ / "config" / "wheelemu" / "rigs" / "Moza.ini", "rig");
    write(dir_ / "state" / "wheelemu" / "wheelemu-service.log", "log");
    moveFoldersFromOldName();
    EXPECT_FALSE(fs::exists(dir_ / "config" / "wheelemu"));
    EXPECT_EQ(read(dir_ / "config" / "wheelbri" / "rigs" / "Moza.ini"), "rig");
    EXPECT_FALSE(fs::exists(dir_ / "state" / "wheelemu"));
    EXPECT_EQ(read(dir_ / "state" / "wheelbri" / "wheelemu-service.log"), "log");

    {
        testing_support::LogLinesCapture capture("service");
        logStartupBanner("");
        EXPECT_TRUE(capture.has("rename: moved " + (dir_ / "config" / "wheelemu").string()));
        EXPECT_TRUE(capture.has("rename: moved " + (dir_ / "state" / "wheelemu").string()));
    }
    testing_support::LogLinesCapture again("service");
    logStartupBanner("");
    EXPECT_FALSE(again.has("rename:"));   // said once
}

// Both folders there: what the new one lacks moves over, what it has stays in the old one (which is then kept), and a
// subfolder on both sides is merged the same way; a file against a folder of the same name stays too.
TEST_F(ServiceFolders, BothFoldersAreMergedAndTheNewOneWins) {
    const ScopedEnv config("XDG_CONFIG_HOME", (dir_ / "config").string());
    const ScopedEnv state("XDG_STATE_HOME", (dir_ / "state").string());
    const fs::path oldRoot = dir_ / "config" / "wheelemu";
    const fs::path newRoot = dir_ / "config" / "wheelbri";
    write(oldRoot / "only-old.ini", "old");
    write(oldRoot / "both.ini", "old");
    write(newRoot / "both.ini", "new");
    write(oldRoot / "rigs" / "A.ini", "old A");
    write(oldRoot / "rigs" / "B.ini", "old B");
    write(newRoot / "rigs" / "B.ini", "new B");
    write(oldRoot / "devices", "a file where the new side has a folder");
    fs::create_directories(newRoot / "devices");

    moveFoldersFromOldName();

    EXPECT_EQ(read(newRoot / "only-old.ini"), "old");
    EXPECT_EQ(read(newRoot / "both.ini"), "new");
    EXPECT_EQ(read(oldRoot / "both.ini"), "old");
    EXPECT_EQ(read(newRoot / "rigs" / "A.ini"), "old A");
    EXPECT_EQ(read(newRoot / "rigs" / "B.ini"), "new B");
    EXPECT_EQ(read(oldRoot / "rigs" / "B.ini"), "old B");
    EXPECT_TRUE(fs::is_regular_file(oldRoot / "devices"));
    EXPECT_TRUE(fs::is_directory(newRoot / "devices"));

    testing_support::LogLinesCapture capture("service");
    logStartupBanner("");
    EXPECT_TRUE(capture.has("moved 1 entries from " + (oldRoot / "rigs").string()));
    EXPECT_TRUE(capture.has("moved 1 entries from " + oldRoot.string() + " to " + newRoot.string() +
                            "; 2 stay there"));
}

// Everything moved out of the old folder: the old folder goes, and nothing is said to stay.
TEST_F(ServiceFolders, AnEmptiedOldFolderIsRemoved) {
    const ScopedEnv config("XDG_CONFIG_HOME", (dir_ / "config").string());
    const ScopedEnv state("XDG_STATE_HOME", (dir_ / "state").string());
    write(dir_ / "config" / "wheelemu" / "a.ini", "a");
    fs::create_directories(dir_ / "config" / "wheelbri");
    moveFoldersFromOldName();
    EXPECT_FALSE(fs::exists(dir_ / "config" / "wheelemu"));
    EXPECT_EQ(read(dir_ / "config" / "wheelbri" / "a.ini"), "a");
    testing_support::LogLinesCapture capture("service");
    logStartupBanner("");
    EXPECT_TRUE(capture.has("moved 1 entries from"));
    EXPECT_FALSE(capture.has("stay there"));
}

// app.ini names configs by full path: paths into the old folder (either slash) point into the new one afterwards;
// a file with no such path is left as it was, and so is a missing one.
TEST_F(ServiceFolders, AppIniPathsToTheOldFolderAreRewritten) {
    const ScopedEnv config("XDG_CONFIG_HOME", (dir_ / "config").string());
    const ScopedEnv state("XDG_STATE_HOME", (dir_ / "state").string());
    moveFoldersFromOldName();   // no app.ini: nothing to do
    EXPECT_FALSE(fs::exists(appStateFile()));

    write(appStateFile(), "[app]\nconfig = /home/me/.config/wheelemu/configs/a.ini\nother = C:\\Users\\me\\wheelemu\\b.ini\n"
                          "keep = /x/wheelemu-old/c.ini\n");
    moveFoldersFromOldName();
    EXPECT_EQ(read(appStateFile()), "[app]\nconfig = /home/me/.config/wheelbri/configs/a.ini\nother = "
                                    "C:\\Users\\me\\wheelbri\\b.ini\nkeep = /x/wheelemu-old/c.ini\n");
    testing_support::LogLinesCapture capture("service");
    logStartupBanner("");
    EXPECT_TRUE(capture.has("app.ini: paths to the old folder now point to the new one"));

    const std::string clean = "[app]\nconfig = /x/wheelbri/a.ini\n";
    write(appStateFile(), clean);
    moveFoldersFromOldName();
    EXPECT_EQ(read(appStateFile()), clean);
}

// A folder in the old one where the new one has a file of that name: it stays in the old folder, as a file against
// a folder does.
TEST_F(ServiceFolders, AFolderAgainstAFileStaysBehind) {
    const ScopedEnv config("XDG_CONFIG_HOME", (dir_ / "config").string());
    const ScopedEnv state("XDG_STATE_HOME", (dir_ / "state").string());
    write(dir_ / "config" / "wheelemu" / "rigs" / "A.ini", "old A");
    write(dir_ / "config" / "wheelbri" / "rigs", "a file where the old side has a folder");
    moveFoldersFromOldName();
    EXPECT_EQ(read(dir_ / "config" / "wheelemu" / "rigs" / "A.ini"), "old A");
    EXPECT_TRUE(fs::is_regular_file(dir_ / "config" / "wheelbri" / "rigs"));
}

// app.ini that cannot be written again (its temporary file's name is taken by a folder with something in it): left as it was, and the
// banner says nothing about it.
TEST_F(ServiceFolders, AnAppIniThatCannotBeWrittenIsLeftAsItWas) {
    const ScopedEnv config("XDG_CONFIG_HOME", (dir_ / "config").string());
    const ScopedEnv state("XDG_STATE_HOME", (dir_ / "state").string());
    const std::string text = "[app]\nconfig = /x/wheelemu/a.ini\n";
    write(appStateFile(), text);
    write(fs::path(appStateFile().string() + ".tmp") / "x", "a folder that is not empty");
    moveFoldersFromOldName();
    EXPECT_EQ(read(appStateFile()), text);
    testing_support::LogLinesCapture capture("service");
    logStartupBanner("");
    EXPECT_FALSE(capture.has("app.ini:"));
}

// Only the folder's own name is the old one: a "wheelbri" earlier in the path (a user called wheelbri, say) is not
// renamed, so the old folder looked for is next to the new one, not in some other user's tree.
TEST_F(ServiceFolders, OnlyTheLastWheelbriInThePathIsTheOldName) {
    const ScopedEnv config("XDG_CONFIG_HOME", (dir_ / "wheelbri" / "config").string());
    const ScopedEnv state("XDG_STATE_HOME", (dir_ / "wheelbri" / "state").string());
    write(dir_ / "wheelbri" / "config" / "wheelemu" / "app.ini", "[app]\n");
    write(dir_ / "wheelemu" / "config" / "wheelemu" / "someone-elses.ini", "theirs");
    moveFoldersFromOldName();
    EXPECT_EQ(read(dir_ / "wheelbri" / "config" / "wheelbri" / "app.ini"), "[app]\n");
    EXPECT_EQ(read(dir_ / "wheelemu" / "config" / "wheelemu" / "someone-elses.ini"), "theirs");
    EXPECT_FALSE(fs::exists(dir_ / "wheelbri" / "config" / "wheelbri" / "someone-elses.ini"));
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
