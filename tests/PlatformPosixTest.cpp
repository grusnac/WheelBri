// The service's Linux platform code (PlatformPosix.cpp) with real files and processes: the single-instance lock,
// child processes (Housekeeping's compressors) and the clock.

#include "Platform.hpp"

#include "wheelbri/KnownGames.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace wheelbri::service::platform {
namespace {

// XDG_RUNTIME_DIR pointed at a temporary folder for one test, then put back.
class RuntimeFolder {
public:
    explicit RuntimeFolder(const std::string& value) {
        const char* old = std::getenv("XDG_RUNTIME_DIR");
        if (old != nullptr) old_ = old;
        hadOld_ = old != nullptr;
        setenv("XDG_RUNTIME_DIR", value.c_str(), 1);
    }
    ~RuntimeFolder() {
        if (hadOld_) {
            setenv("XDG_RUNTIME_DIR", old_.c_str(), 1);
        } else {
            unsetenv("XDG_RUNTIME_DIR");
        }
    }
    RuntimeFolder(const RuntimeFolder&) = delete;
    RuntimeFolder& operator=(const RuntimeFolder&) = delete;

private:
    std::string old_;
    bool hadOld_ = false;
};

std::filesystem::path tempFolder(const std::string& name) {
    const auto folder = std::filesystem::temp_directory_path() /
                        ("wheelbri-platform-" + name + "-" + std::to_string(::getpid()));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    return folder;
}

// The child's exit code, waiting up to `seconds`.
std::optional<int> waitFor(ChildProcess& child, const int seconds = 5) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < until) {
        if (auto code = child.poll()) return code;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return std::nullopt;
}

// --- SingleInstance ---

TEST(PlatformPosix, OnlyOneInstanceHoldsTheLock) {
    const auto folder = tempFolder("lock");
    RuntimeFolder runtime(folder.string());
    std::string error;
    auto first = SingleInstance::acquire("wheelbri-test", error);
    ASSERT_NE(first, nullptr) << error;
    EXPECT_TRUE(std::filesystem::exists(folder / ("wheelbri-test-" + std::to_string(::getuid()) + ".lock")));

    auto second = SingleInstance::acquire("wheelbri-test", error);
    EXPECT_EQ(second, nullptr);
    EXPECT_NE(error.find("already running"), std::string::npos) << error;

    std::string other;
    EXPECT_NE(SingleInstance::acquire("wheelbri-other", other), nullptr) << other;   // another name: its own lock

    first.reset();   // released: the next one gets it
    EXPECT_NE(SingleInstance::acquire("wheelbri-test", error), nullptr) << error;
    std::filesystem::remove_all(folder);
}

TEST(PlatformPosix, ALockThatCannotBeCreatedSaysWhere) {
    RuntimeFolder runtime("/nonexistent-wheelbri-folder");
    std::string error;
    EXPECT_EQ(SingleInstance::acquire("wheelbri-test", error), nullptr);
    EXPECT_NE(error.find("cannot create the single-instance lock /nonexistent-wheelbri-folder/"), std::string::npos)
        << error;
}

TEST(PlatformPosix, AnEmptyRuntimeFolderFallsBackToTmp) {
    RuntimeFolder runtime("");
    std::string error;
    const std::string name = "wheelbri-test-" + std::to_string(::getpid());
    auto lock = SingleInstance::acquire(name, error);
    ASSERT_NE(lock, nullptr) << error;
    const auto file = std::filesystem::path("/tmp") / (name + "-" + std::to_string(::getuid()) + ".lock");
    EXPECT_TRUE(std::filesystem::exists(file));
    lock.reset();
    std::filesystem::remove(file);
}

// --- ChildProcess ---

TEST(PlatformPosix, AChildsExitCodeIsReported) {
    std::string error;
    auto child = ChildProcess::start({"/bin/sh", "-c", "exit 3"}, error);
    ASSERT_NE(child, nullptr) << error;
    EXPECT_EQ(waitFor(*child), 3);
    EXPECT_EQ(child->poll(), 3);   // asked again: the same answer
}

TEST(PlatformPosix, AChildEndedByASignalReports128PlusIt) {
    std::string error;
    auto child = ChildProcess::start({"/bin/sh", "-c", "kill -TERM $$"}, error);
    ASSERT_NE(child, nullptr) << error;
    EXPECT_EQ(waitFor(*child), 128 + 15);
}

TEST(PlatformPosix, ARunningChildIsKilledAndWaitedFor) {
    std::string error;
    auto child = ChildProcess::start({"/bin/sleep", "30"}, error);
    ASSERT_NE(child, nullptr) << error;
    EXPECT_FALSE(child->poll());   // still running
    child->kill();
    EXPECT_EQ(child->poll(), 128 + 9);
    child->kill();   // again: nothing to do
    EXPECT_EQ(child->poll(), 128 + 9);
}

TEST(PlatformPosix, AProgramThatCannotStartIsAnError) {
    std::string error;
    EXPECT_EQ(ChildProcess::start({}, error), nullptr);
    EXPECT_EQ(error, "no program");
    error.clear();
    EXPECT_EQ(ChildProcess::start({"/nonexistent/wheelbri-compressor", "-x"}, error), nullptr);
    EXPECT_NE(error.find("cannot start /nonexistent/wheelbri-compressor"), std::string::npos) << error;
}

TEST(PlatformPosix, TheChildGetsNoInputAndItsOutputGoesNowhere) {
    std::string error;
    // Reads stdin (the null device: at once empty) and writes to stdout; ends 0 when stdin was empty.
    auto child = ChildProcess::start({"/bin/sh", "-c", "read line; test -z \"$line\" && echo out && echo err >&2"},
                                     error);
    ASSERT_NE(child, nullptr) << error;
    EXPECT_EQ(waitFor(*child), 0);
}

TEST(PlatformPosix, DestroyingTheObjectKillsARunningChild) {
    const auto folder = tempFolder("child");
    const auto marker = folder / "finished";
    std::string error;
    {
        auto child = ChildProcess::start({"/bin/sh", "-c", "sleep 0.3; touch '" + marker.string() + "'"}, error);
        ASSERT_NE(child, nullptr) << error;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    EXPECT_FALSE(std::filesystem::exists(marker));   // killed before it got there
    std::filesystem::remove_all(folder);
}

TEST(PlatformPosix, AChildKeptRunningOutlivesTheObject) {
    const auto folder = tempFolder("kept");
    const auto marker = folder / "finished";
    std::string error;
    {
        auto child = ChildProcess::start({"/bin/sh", "-c", "sleep 0.2; touch '" + marker.string() + "'"}, error);
        ASSERT_NE(child, nullptr) << error;
        child->keepRunningAfterExit();
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!std::filesystem::exists(marker) && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(std::filesystem::exists(marker));
    std::filesystem::remove_all(folder);
}

// --- the clock and the rest ---

TEST(PlatformPosix, SleepingWaitsAtLeastAsLongAsAsked) {
    const std::uint64_t before = nowNs();
    sleepNs(2 * kNsPerMs);
    EXPECT_GE(nowNs() - before, 2 * kNsPerMs);
    sleepNs(0);   // a yield, never a hang
    EXPECT_GE(nowNs(), before);
}

TEST(PlatformPosix, TheProgramKnowsItsOwnFile) {
    const auto self = executablePath();
    ASSERT_FALSE(self.empty());
    EXPECT_TRUE(std::filesystem::exists(self));
    EXPECT_EQ(self.filename(), "wheelbri_tests");
    EXPECT_STREQ(name(), "Linux");
    EXPECT_EQ(systemFolder(), std::filesystem::path("/usr/bin"));
    EXPECT_TRUE(isElevated());   // nothing to elevate on Linux
}

TEST(PlatformPosix, RaisingThePriorityEitherWorksOrSaysWhy) {
    std::string error;
    std::thread([&error] {   // its own thread: the test runner's keeps its priority
        if (!raiseThreadPriority(error)) {
            EXPECT_FALSE(error.empty());
        }
    }).join();
}

// KnownGames reads the running programs: this test program is among them, by the first word of its command line.
TEST(PlatformPosix, RunningProgramsListsThisOne) {
    const std::vector<std::string> programs = runningPrograms();
    ASSERT_FALSE(programs.empty());
    bool found = false;
    for (const std::string& program : programs) {
        found = found || programName(program) == "wheelbri_tests";
    }
    EXPECT_TRUE(found);
}

}  // namespace
}  // namespace wheelbri::service::platform
