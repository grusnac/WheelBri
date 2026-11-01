// The service's Linux platform code (PlatformPosix.cpp) with real files and processes: the single-instance lock,
// child processes (Housekeeping's compressors) and the clock.

#include "Platform.hpp"

#include "wheelbri/GameProfiles.hpp"
#include "wheelbri/UdpLoopback.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <array>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>

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

// An empty XDG_RUNTIME_DIR, or none at all (a session without one, e.g. over ssh): the lock goes to /tmp.
TEST(PlatformPosix, NoRuntimeFolderFallsBackToTmp) {
    for (const bool unset : {false, true}) {
        RuntimeFolder runtime("");
        if (unset) unsetenv("XDG_RUNTIME_DIR");   // put back by `runtime`
        std::string error;
        const std::string name = "wheelbri-test-" + std::to_string(::getpid()) + (unset ? "-unset" : "-empty");
        auto lock = SingleInstance::acquire(name, error);
        ASSERT_NE(lock, nullptr) << error;
        const auto file = std::filesystem::path("/tmp") / (name + "-" + std::to_string(::getuid()) + ".lock");
        EXPECT_TRUE(std::filesystem::exists(file)) << unset;
        lock.reset();
        std::filesystem::remove(file);
    }
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

// GameDetector reads the running programs: this test program is among them, by the first word of its command line.
TEST(PlatformPosix, RunningProgramsListsThisOne) {
    const auto programs = runningPrograms();
#if defined(__linux__)
    ASSERT_TRUE(programs.has_value());
    ASSERT_FALSE(programs->empty());
    // This process, by its pid: other test processes of the same program may run beside it (ctest -j).
    bool found = false;
    for (const RunningProgram& program : *programs) {
        if (program.pid == static_cast<std::uint32_t>(::getpid())) {
            found = true;
            EXPECT_EQ(programName(program.name), "wheelbri_tests");
        }
        EXPECT_FALSE(program.name.empty());
    }
    EXPECT_TRUE(found);
#else
    EXPECT_FALSE(programs.has_value());   // cannot list here: game detection says so, the pedals never move
#endif
    EXPECT_TRUE(documentsFolder().empty());
}

#if defined(__linux__)
// A program started with posix_spawn under the command line `argv` (argv[0] need not be the program's file).
// With `input` >= 0 that descriptor is its standard input.
pid_t spawnAs(const char* const file, std::vector<std::string> argv, const int input = -1) {
    std::vector<char*> args;
    for (std::string& arg : argv) args.push_back(arg.data());
    args.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (input >= 0) posix_spawn_file_actions_adddup2(&actions, input, 0);
    pid_t pid = -1;
    char* const noEnvironment[] = {nullptr};
    const int result = posix_spawn(&pid, file, &actions, nullptr, args.data(), noEnvironment);
    posix_spawn_file_actions_destroy(&actions);
    return result == 0 ? pid : -1;
}

// The state letter in /proc/<pid>/stat ('Z' for a zombie), or 0.
char processState(const pid_t pid) {
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    std::getline(stat, line);
    const std::size_t paren = line.rfind(')');
    return paren != std::string::npos && paren + 2 < line.size() ? line[paren + 2] : '\0';
}

// A game started through Wine by hand ("wine64 C:\...\NFS16.exe"): the program is the word after the loader, so the
// game's profile is found; a loader with nothing after it is listed as itself.
TEST(PlatformPosix, AGameStartedThroughWineIsNamedByItsExe) {
    const auto folder = tempFolder("wine");
    const auto game = folder / "NFS16.exe";
    ASSERT_EQ(::mkfifo(game.c_str(), 0600), 0);   // cat blocks opening it: a process that stays until killed
    const pid_t wine = spawnAs("/bin/cat", {"wine64", game.string()});
    std::array<int, 2> feed{-1, -1};   // the bare loader reads it: it waits until the test closes it
    ASSERT_EQ(::pipe2(feed.data(), O_CLOEXEC), 0);
    const pid_t bare = spawnAs("/bin/cat", {"wine-preloader"}, feed[0]);
    ASSERT_GT(wine, 0);
    ASSERT_GT(bare, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto programs = runningPrograms();
    ::kill(wine, SIGKILL);
    ::kill(bare, SIGKILL);
    ::waitpid(wine, nullptr, 0);
    ::waitpid(bare, nullptr, 0);
    ::close(feed[0]);
    ::close(feed[1]);
    std::filesystem::remove_all(folder);
    ASSERT_TRUE(programs.has_value());
    std::string wineName;
    std::string bareName;
    for (const RunningProgram& program : *programs) {
        if (program.pid == static_cast<std::uint32_t>(wine)) wineName = program.name;
        if (program.pid == static_cast<std::uint32_t>(bare)) bareName = program.name;
    }
    EXPECT_EQ(wineName, game.string());
    EXPECT_EQ(programName(wineName), "nfs16.exe");
    EXPECT_EQ(bareName, "wine-preloader");
}

// Every name Wine's loader goes by ("wine", "wine64", "wine-preloader", "wine64-preloader") hands the name on to the
// program after it; a command line longer than what is read (8 KiB) is cut, never read on.
TEST(PlatformPosix, EveryWineLoaderNameAndALongCommandLine) {
    const auto folder = tempFolder("loaders");
    std::array<int, 2> feed{-1, -1};   // the long one's `cat` reads it: it waits until the test closes it
    ASSERT_EQ(::pipe2(feed.data(), O_CLOEXEC), 0);
    const auto gameA = folder / "a.exe";   // FIFOs: cat blocks opening them, so each process stays until killed
    const auto gameB = folder / "b.exe";
    ASSERT_EQ(::mkfifo(gameA.c_str(), 0600), 0);
    ASSERT_EQ(::mkfifo(gameB.c_str(), 0600), 0);
    const pid_t wine = spawnAs("/bin/cat", {"/usr/bin/wine", gameA.string()});
    const pid_t preloader = spawnAs("/bin/cat", {"wine64-preloader", gameB.string()});
    const std::string longName(9000, 'x');
    const pid_t longOne = spawnAs("/bin/cat", {longName}, feed[0]);
    ASSERT_GT(wine, 0);
    ASSERT_GT(preloader, 0);
    ASSERT_GT(longOne, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto programs = runningPrograms();
    for (const pid_t pid : {wine, preloader, longOne}) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, nullptr, 0);
    }
    ::close(feed[0]);
    ::close(feed[1]);
    std::filesystem::remove_all(folder);
    ASSERT_TRUE(programs.has_value());
    std::string wineName;
    std::string preloaderName;
    std::string longOneName;
    for (const RunningProgram& program : *programs) {
        if (program.pid == static_cast<std::uint32_t>(wine)) wineName = program.name;
        if (program.pid == static_cast<std::uint32_t>(preloader)) preloaderName = program.name;
        if (program.pid == static_cast<std::uint32_t>(longOne)) longOneName = program.name;
    }
    EXPECT_EQ(wineName, gameA.string());
    EXPECT_EQ(preloaderName, gameB.string());
    EXPECT_EQ(longOneName, std::string(8192, 'x'));
}

// A process that has ended but is not reaped yet (a zombie) has no command line: it is not listed.
TEST(PlatformPosix, AZombieIsNotARunningProgram) {
    const pid_t child = spawnAs("/bin/true", {"/bin/true"});
    ASSERT_GT(child, 0);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (processState(child) != 'Z' && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(processState(child), 'Z');
    const auto programs = runningPrograms();
    ::waitpid(child, nullptr, 0);
    ASSERT_TRUE(programs.has_value());
    for (const RunningProgram& program : *programs) EXPECT_NE(program.pid, static_cast<std::uint32_t>(child));
}

// A thread name longer than the kernel keeps (15 characters) is cut, not refused; no name at all is ignored.
TEST(PlatformPosix, ThreadNamesAreCutToWhatTheKernelKeeps) {
    std::string named;
    std::thread([&named] {
        nameThisThread(nullptr);   // nothing happens
        nameThisThread("wheelbri a rather long role");
        std::ifstream comm("/proc/self/task/" + std::to_string(::gettid()) + "/comm");
        std::getline(comm, named);
    }).join();
    EXPECT_EQ(named, "wheelbri a rath");
}
#endif

// A child let go (release): no exit code is known any more and destroying the object leaves it running.
TEST(PlatformPosix, AReleasedChildIsLeftAlone) {
    const auto folder = tempFolder("released");
    const auto marker = folder / "finished";
    std::string error;
    {
        auto child = ChildProcess::start({"/bin/sh", "-c", "sleep 0.2; touch '" + marker.string() + "'"}, error);
        ASSERT_NE(child, nullptr) << error;
        child->release();
        EXPECT_EQ(child->poll(), -1);
        child->kill();   // not ours any more: nothing happens
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!std::filesystem::exists(marker) && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(std::filesystem::exists(marker));
    std::filesystem::remove_all(folder);
}

#if defined(__linux__)
// The force feedback sender check (lookupFfbSender): a socket this process sends from is this user's; a port nobody
// holds is nobody's.
TEST(PlatformPosix, AUdpPortIsOursOnlyWhileOurSocketHoldsIt) {
    auto listener = UdpLoopbackListener::create(0);
    ASSERT_NE(listener, nullptr);
    const auto sender = UdpLoopbackSender::create(listener->localPort());
    ASSERT_NE(sender, nullptr);
    const std::array<std::uint8_t, 1> byte{1};
    ASSERT_TRUE(sender->send(byte));
    std::array<std::uint8_t, 4> buffer{};
    std::uint16_t from = 0;
    std::optional<std::size_t> size;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!size && std::chrono::steady_clock::now() < deadline) {
        size = listener->receive(buffer, &from);
        if (!size) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(size.has_value());
    EXPECT_EQ(udpPortIsOurUser(from), std::optional<bool>(true));
    EXPECT_EQ(udpPortIsOurUser(listener->localPort()), std::optional<bool>(true));
    const std::uint16_t port = listener->localPort();
    // Closed: nobody's any more (the system may hand the port to someone else, never to the closed socket).
    listener.reset();
    EXPECT_EQ(udpPortIsOurUser(port), std::optional<bool>(false));
}
#endif

}  // namespace
}  // namespace wheelbri::service::platform
