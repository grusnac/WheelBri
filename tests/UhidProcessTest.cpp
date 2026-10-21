// The app's wheelbri-uhid on Linux (service/src/UhidProcess.cpp). A fake wheelbri-uhid (a shell script, named so the
// process list sees it under that name) is put next to the test program, where UhidProcess looks first: one that
// stops at once, one that runs until its stop file appears, one that ignores it, one that ends by itself; and one
// already running before Start, which is used and left alone.

#if defined(__linux__)

#include "UhidProcess.hpp"

#include "Platform.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace wheelbri::service {
namespace {

namespace fs = std::filesystem;

class UhidProcessTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("wheelbri-uhidprocess-" + std::to_string(::getuid()) + "-" + std::to_string(::getpid()) + "-" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        fake_ = platform::executablePath().parent_path() / "wheelbri-uhid";
        ASSERT_FALSE(fs::exists(fake_)) << "a wheelbri-uhid next to the tests is in the way";
        ASSERT_FALSE(uhidRunning()) << "a wheelbri-uhid runs on this computer: these tests need it stopped";
    }
    void TearDown() override {
        fs::remove(fake_);
        fs::remove_all(dir_);
    }
    // The fake: `body` runs with the stop file in $2 (wheelbri-uhid --stop-file F --parent P --target T).
    void fake(const std::string& body) const {
        {
            std::ofstream out(fake_, std::ios::binary);
            out << "#!/bin/sh\nPATH=/usr/bin:/bin\necho \"$@\" > \"" << (dir_ / "args").string() << "\"\n" << body;
        }
        fs::permissions(fake_, fs::perms::owner_all);
    }
    [[nodiscard]] std::string args() const {
        std::ifstream in(dir_ / "args");
        std::string line;
        std::getline(in, line);
        return line;
    }

    fs::path dir_;
    fs::path fake_;
};

// Not built (nothing next to the program, nor in build/tools/uhid when that is not built either): Start says so.
TEST_F(UhidProcessTest, NotFoundSaysWhere) {
    if (fs::exists(platform::executablePath().parent_path().parent_path() / "tools" / "uhid" / "wheelbri-uhid")) {
        GTEST_SKIP() << "wheelbri-uhid is built in this tree";
    }
    std::string error;
    EXPECT_EQ(UhidProcess::start(dir_, "g920", error), nullptr);
    EXPECT_EQ(error.rfind("wheelbri-uhid not found next to ", 0), 0U) << error;
}

// One that ends at once (no /dev/uhid permission, a port taken): Start fails with its exit code.
TEST_F(UhidProcessTest, OneThatStopsAtOnceIsAnError) {
    fake("exit 4\n");
    std::string error;
    EXPECT_EQ(UhidProcess::start(dir_, "g25", error), nullptr);
    EXPECT_EQ(error, "wheelbri-uhid stopped at once (exit 4): its log says why (scripts/linux/install-udev-rules.sh run "
                     "once?)");
}

// A program that cannot be run: Start fails with the system's reason.
TEST_F(UhidProcessTest, OneThatCannotRunIsAnError) {
    fake("exit 0\n");
    fs::permissions(fake_, fs::perms::owner_read | fs::perms::owner_write);
    std::string error;
    EXPECT_EQ(UhidProcess::start(dir_, "g920", error), nullptr);
    EXPECT_NE(error.find("cannot start"), std::string::npos) << error;
}

// Ours: started with the stop file, this process as its parent and the target; running while it runs; stopped by
// the stop file when the object goes (which is then removed). A leftover stop file is removed first.
TEST_F(UhidProcessTest, OursStartsAndStopsByItsStopFile) {
    fake("while [ ! -f \"$2\" ]; do sleep 0.02; done\nexit 0\n");
    std::ofstream(dir_ / "uhid-app.stop") << "left over\n";
    std::string error;
    auto uhid = UhidProcess::start(dir_, "g923", error);
    ASSERT_NE(uhid, nullptr) << error;
    EXPECT_EQ(uhid->describe(), "wheelbri-uhid");
    EXPECT_TRUE(uhid->running());
    EXPECT_TRUE(uhidRunning());
    EXPECT_EQ(args(), "--stop-file " + (dir_ / "uhid-app.stop").string() + " --parent " + std::to_string(::getpid()) +
                          " --target g923");
    uhid.reset();
    EXPECT_FALSE(fs::exists(dir_ / "uhid-app.stop"));
    EXPECT_FALSE(uhidRunning());
}

// One that ends by itself while the session runs: not running any more; nothing to stop at the end.
TEST_F(UhidProcessTest, OneThatEndsByItselfIsNotRunning) {
    fake("sleep 0.7\nexit 0\n");
    std::string error;
    auto uhid = UhidProcess::start(dir_, "g920", error);
    ASSERT_NE(uhid, nullptr) << error;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (uhid->running() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_FALSE(uhid->running());
    uhid.reset();
    EXPECT_FALSE(fs::exists(dir_ / "uhid-app.stop"));   // never written
}

// One that ignores its stop file is killed after 3 s.
TEST_F(UhidProcessTest, OneThatIgnoresTheStopFileIsKilled) {
    fake("trap '' TERM\nwhile :; do sleep 0.05; done\n");
    std::string error;
    auto uhid = UhidProcess::start(dir_, "g920", error);
    ASSERT_NE(uhid, nullptr) << error;
    const auto before = std::chrono::steady_clock::now();
    uhid.reset();
    EXPECT_GE(std::chrono::steady_clock::now() - before, std::chrono::milliseconds(2500));
    EXPECT_FALSE(uhidRunning());
}

// One already running (wheelbri.sh's): used as it is, left alone at the end, and always "running".
TEST_F(UhidProcessTest, OneAlreadyRunningIsUsedAndLeftAlone) {
    fake("while [ ! -f \"" + (dir_ / "end").string() + "\" ]; do sleep 0.02; done\nexit 0\n");
    std::string error;
    auto theirs = platform::ChildProcess::start({fake_.string()}, error);
    ASSERT_NE(theirs, nullptr) << error;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!uhidRunning() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(uhidRunning());
    auto uhid = UhidProcess::start(dir_, "g920", error);
    ASSERT_NE(uhid, nullptr) << error;
    EXPECT_EQ(uhid->describe(), "wheelbri-uhid (already running)");
    EXPECT_TRUE(uhid->running());
    uhid.reset();
    EXPECT_FALSE(theirs->poll().has_value());   // still running: not ours to stop
    std::ofstream(dir_ / "end") << "\n";
    const auto ended = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!theirs->poll() && std::chrono::steady_clock::now() < ended) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

}  // namespace
}  // namespace wheelbri::service

#endif
