// The service's Windows platform code (service/src/PlatformWin32.cpp), as PlatformPosixTest covers the Linux one:
// clock and sleep, the program's own file and folders, who owns a UDP port, the running programs, process images, the
// single-instance lock and child processes in a job. Windows only (MinGW: run under Wine).

#include "Platform.hpp"

#include <gtest/gtest.h>

#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

namespace wheelbri::service {
namespace {

using namespace std::chrono_literals;

// Run as a child by the tests below (WHEELBRI_TEST_CHILD set): sleeps until killed, or exits with the code asked.
TEST(PlatformWin32Child, ActsAsAskedWhenAChild) {
    const char* const asked = std::getenv("WHEELBRI_TEST_CHILD");
    if (asked == nullptr) return;   // only as a child of the tests below: a pass otherwise, not a skip in every run
    if (std::string(asked) == "sleep") {
        std::this_thread::sleep_for(60s);
    }
    std::exit(std::atoi(asked));
}

// The test program as a child running only the test above, which does `what`.
std::unique_ptr<platform::ChildProcess> startChild(const char* what, std::string& error) {
    SetEnvironmentVariableA("WHEELBRI_TEST_CHILD", what);
    auto child = platform::ChildProcess::start(
        {platform::executablePath().string(), "--gtest_filter=PlatformWin32Child.ActsAsAskedWhenAChild"}, error);
    SetEnvironmentVariableA("WHEELBRI_TEST_CHILD", nullptr);
    return child;
}

std::optional<int> waitFor(platform::ChildProcess& child, const std::chrono::seconds limit = 20s) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until) {
        if (const auto code = child.poll()) return code;
        std::this_thread::sleep_for(10ms);
    }
    return std::nullopt;
}

// The clock only goes forward; a sleep lasts at least as long as asked; 0 only yields.
TEST(PlatformWin32, ClockAndSleep) {
    platform::init();
    const std::uint64_t before = platform::nowNs();
    platform::sleepNs(0);
    platform::sleepNs(2'000'000);
    const std::uint64_t after = platform::nowNs();
    EXPECT_GE(after - before, 2'000'000U);
    EXPECT_GE(platform::nowMs(), before / 1'000'000);
}

// The program's own file, the system folder, a Windows build and the user's Documents folder.
TEST(PlatformWin32, ProgramAndFolders) {
    const std::filesystem::path self = platform::executablePath();
    EXPECT_EQ(self.filename(), "wheelbri_tests.exe");
    EXPECT_TRUE(std::filesystem::exists(platform::systemFolder() / "kernel32.dll"));
    EXPECT_GT(platform::windowsBuild(), 0U);
    EXPECT_FALSE(platform::documentsFolder().empty());
    EXPECT_TRUE(platform::canPickFile());
    EXPECT_NE(platform::name(), nullptr);
}

// The thread's priority and name: set, or the reason it was not.
TEST(PlatformWin32, ThreadPriorityAndName) {
    std::thread worker([] {
        platform::nameThisThread("wheelbri test");
        std::string error;
        if (!platform::raiseThreadPriority(error)) {
            EXPECT_FALSE(error.empty());
        }
    });
    worker.join();
    (void)platform::isElevated();   // either answer; it must not fail
}

// The owner of a UDP port: this process for one it bound, nobody (0) for a free one; or the system cannot say.
TEST(PlatformWin32, UdpPortOwner) {
    WSADATA data{};
    ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &data), 0);
    const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_NE(s, INVALID_SOCKET);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    int length = sizeof(address);
    ASSERT_EQ(getsockname(s, reinterpret_cast<sockaddr*>(&address), &length), 0);
    const auto port = ntohs(address.sin_port);
    if (const auto owner = platform::udpPortOwner(port)) {
        EXPECT_TRUE(*owner == GetCurrentProcessId() || *owner == 0U);   // Wine may not know the owner
    }
    closesocket(s);
    if (const auto owner = platform::udpPortOwner(port)) {
        EXPECT_EQ(*owner, 0U);
    }
    WSACleanup();
}

// This program is among the running ones, and its image is named by its id; an id no process has is not, with why.
TEST(PlatformWin32, RunningProgramsAndImages) {
    const auto programs = platform::runningPrograms();
    ASSERT_TRUE(programs);
    const DWORD self = GetCurrentProcessId();
    EXPECT_TRUE(std::any_of(programs->begin(), programs->end(), [self](const platform::RunningProgram& program) {
        return program.pid == self && program.name == "wheelbri_tests.exe";
    }));
    EXPECT_EQ(platform::processImage(self).filename(), "wheelbri_tests.exe");
    EXPECT_TRUE(platform::lastProcessImageError().empty());
    EXPECT_TRUE(platform::processImage(0xFFFFFFF0U).empty());
    EXPECT_FALSE(platform::lastProcessImageError().empty());
}

// One holder of a lock name at a time (another thread, as a second session would be); free again once released.
TEST(PlatformWin32, OnlyOneInstanceHoldsTheLock) {
    const std::string name = "wheelbri-test-lock-" + std::to_string(GetCurrentProcessId());
    std::string error;
    auto first = platform::SingleInstance::acquire(name, error);
    ASSERT_NE(first, nullptr) << error;
    std::thread other([&] {
        std::string otherError;
        EXPECT_EQ(platform::SingleInstance::acquire(name, otherError), nullptr);
        EXPECT_NE(otherError.find("already running"), std::string::npos);
    });
    other.join();
    first.reset();
    std::thread again([&] {
        std::string againError;
        EXPECT_NE(platform::SingleInstance::acquire(name, againError), nullptr) << againError;
    });
    again.join();
}

// A child's exit code; a program that cannot start (or none) is an error; a running child is killed with its job, also
// when the object goes; a released or kept child is left alone.
TEST(PlatformWin32, ChildProcesses) {
    std::string error;
    EXPECT_EQ(platform::ChildProcess::start({}, error), nullptr);
    EXPECT_EQ(error, "no program");
    EXPECT_EQ(platform::ChildProcess::start({"C:\\no such folder\\no.exe", "a b", "q\"uote", "back\\\\"}, error),
              nullptr);
    EXPECT_NE(error.find("CreateProcess failed"), std::string::npos);

    auto exits = startChild("3", error);
    ASSERT_NE(exits, nullptr) << error;
    EXPECT_EQ(waitFor(*exits), 3);
    EXPECT_EQ(exits->poll(), 3);   // asked again: the same
    exits->kill();                 // over already: nothing

    auto sleeps = startChild("sleep", error);
    ASSERT_NE(sleeps, nullptr) << error;
    EXPECT_FALSE(sleeps->poll());
    sleeps->kill();
    EXPECT_FALSE(sleeps->poll() == std::nullopt);

    {
        auto dropped = startChild("sleep", error);   // the object goes: the child with it
        ASSERT_NE(dropped, nullptr) << error;
    }
    auto released = startChild("0", error);
    ASSERT_NE(released, nullptr) << error;
    released->release();
    auto kept = startChild("0", error);
    ASSERT_NE(kept, nullptr) << error;
    kept->keepRunningAfterExit();
    std::this_thread::sleep_for(200ms);
}

// The console's stop handler can be installed (a session sets it once).
TEST(PlatformWin32, ConsoleStopHandler) {
    platform::installConsoleStopHandler([] {}, [] { return false; });
    SUCCEED();
}

// No folder watch on Windows (device notices are used instead).
TEST(PlatformWin32, NoFolderWatch) {
    EXPECT_EQ(platform::FolderWatch::create(std::filesystem::temp_directory_path(), [] {}), nullptr);
}

}  // namespace
}  // namespace wheelbri::service
