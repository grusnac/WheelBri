// SPDX-License-Identifier: GPL-3.0-or-later
// The command-line service (service/src/main.cpp) as a user runs it: the built wheelbri-service started as a process,
// with HOME and the XDG folders in a folder of the test's own (its settings, logs and lock there), no display, and no
// game controller (this machine may have none, and the test must not touch one it has: every run names a device that
// is not there). What it prints, its exit code and its log are checked: the options, the config and its problems, one
// session at a time, --unhide and --prepare-hide, and the wait for a wheel that does not come.

#if defined(__linux__) && defined(WHEELBRI_SERVICE_EXE)

#include "Platform.hpp"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace wheelbri::service {
namespace {

namespace fs = std::filesystem;

class ServiceMainTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("wheelbri-servicemain-" + std::to_string(::getuid()) + "-" + std::to_string(::getpid()) + "-" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        fs::remove_all(dir_);
        fs::create_directories(dir_ / "runtime");
        fs::create_directories(dir_ / "work");
    }
    void TearDown() override {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            ::waitpid(pid_, nullptr, 0);
        }
        fs::remove_all(dir_);
    }

    static void write(const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }
    [[nodiscard]] static std::string read(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    [[nodiscard]] fs::path settings() const { return dir_ / "config" / "wheelbri"; }
    [[nodiscard]] std::vector<std::string> environment() const {
        return {"HOME=" + dir_.string(), "XDG_CONFIG_HOME=" + (dir_ / "config").string(),
                "XDG_STATE_HOME=" + (dir_ / "state").string(), "XDG_RUNTIME_DIR=" + (dir_ / "runtime").string(),
                "PATH=/usr/bin:/bin", "SPDLOG_LEVEL=info"};   // the info lines too (warn by default)
    }

    // Starts it with `args`; stdout and stderr go to files in the test's folder.
    void start(const std::vector<std::string>& args) {
        std::vector<std::string> argv{WHEELBRI_SERVICE_EXE};
        argv.insert(argv.end(), args.begin(), args.end());
        std::vector<char*> argp;
        for (std::string& arg : argv) argp.push_back(arg.data());
        argp.push_back(nullptr);
        std::vector<std::string> env = environment();
        std::vector<char*> envp;
        for (std::string& entry : env) envp.push_back(entry.data());
        envp.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&actions, 1, (dir_ / "out").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        posix_spawn_file_actions_addopen(&actions, 2, (dir_ / "err").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        posix_spawn_file_actions_addchdir_np(&actions, (dir_ / "work").c_str());
        ASSERT_EQ(posix_spawn(&pid_, argv.front().c_str(), &actions, nullptr, argp.data(), envp.data()), 0);
        posix_spawn_file_actions_destroy(&actions);
    }
    // Its exit code (-1 if it had to be killed after `limit`).
    int wait(const std::chrono::seconds limit = std::chrono::seconds(30)) {
        const auto until = std::chrono::steady_clock::now() + limit;
        int status = 0;
        while (::waitpid(pid_, &status, WNOHANG) == 0) {
            if (std::chrono::steady_clock::now() > until) {
                ::kill(pid_, SIGKILL);
                ::waitpid(pid_, nullptr, 0);
                pid_ = -1;
                return -1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        pid_ = -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    int run(const std::vector<std::string>& args) {
        start(args);
        return wait();
    }
    // Waits (up to `limit`) until this run's log has `text`; true if it did.
    [[nodiscard]] bool logSays(const std::string& text,
                               const std::chrono::seconds limit = std::chrono::seconds(10)) const {
        const auto until = std::chrono::steady_clock::now() + limit;
        while (log().find(text) == std::string::npos) {
            if (std::chrono::steady_clock::now() > until) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return true;
    }
    [[nodiscard]] std::string out() const { return read(dir_ / "out"); }
    [[nodiscard]] std::string err() const { return read(dir_ / "err"); }
    // This run's log file (one per run, the newest in the logs folder).
    [[nodiscard]] std::string log() const {
        fs::path newest;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir_ / "state" / "wheelbri", ec)) {
            const std::string name = entry.path().filename().string();
            if (name.rfind("wheelbri-service-", 0) == 0 && (newest.empty() || entry.path() > newest)) {
                newest = entry.path();
            }
        }
        return newest.empty() ? std::string() : read(newest);
    }
    // A config that names a wheel no machine has, so nothing is ever opened.
    [[nodiscard]] fs::path absentWheelConfig(const std::string& extra = {}) const {
        const fs::path file = dir_ / "absent.ini";
        write(file, "[emulator]\nsource_device = ffff:fffe\n" + extra);
        return file;
    }
    void rig(const std::string& text) const {
        write(settings() / "app.ini", "[app]\nrig = Test rig\n");
        write(settings() / "rigs" / "Test rig.ini", text);
    }

    fs::path dir_;
    pid_t pid_ = -1;
};

// An option it does not know (or one missing its value): the usage, exit 2.
TEST_F(ServiceMainTest, AnUnknownOptionShowsTheUsage) {
    EXPECT_EQ(run({"--bogus"}), 2);
    EXPECT_NE(out().find("usage: wheelbri-service"), std::string::npos);
    EXPECT_EQ(run({"--config"}), 2);
    EXPECT_NE(out().find("usage: wheelbri-service"), std::string::npos);
}

// --old-files takes delete, archive or keep only.
TEST_F(ServiceMainTest, OldFilesTakesThreeWords) {
    EXPECT_EQ(run({"--old-files", "maybe"}), 2);
    // Its first line (a coverage build may add the profiler's own lines when it cannot write its data).
    EXPECT_EQ(err().rfind("--old-files maybe: expected delete, archive or keep\n", 0), 0U) << err();
}

// A config that is not there: exit 2, and the log says which.
TEST_F(ServiceMainTest, AMissingConfigIsSaid) {
    EXPECT_EQ(run({"--config", (dir_ / "nowhere.ini").string()}), 2);
    EXPECT_NE(log().find("cannot open config file '" + (dir_ / "nowhere.ini").string() + "'"), std::string::npos);
}

// A config with problems: each logged, then exit 2 before anything starts.
TEST_F(ServiceMainTest, AConfigWithProblemsDoesNotStart) {
    write(dir_ / "bad.ini", "[emulator]\nsource_device = ffff:fffe\nno_such_key = 1\n[no_such_section]\n");
    EXPECT_EQ(run({"--config", (dir_ / "bad.ini").string(), "--old-files", "keep"}), 2);
    const std::string text = log();
    EXPECT_NE(text.find("problem(s) in the config, fix them and restart"), std::string::npos) << text;
    EXPECT_NE(text.find("no_such_key"), std::string::npos);
}

// --list without a config: the built-in defaults, and what is connected (here perhaps nothing), exit 0.
TEST_F(ServiceMainTest, ListShowsTheControllers) {
    EXPECT_EQ(run({"--list"}), 0);
    const std::string text = log();
    EXPECT_NE(text.find("config: (built-in defaults)"), std::string::npos);
    EXPECT_NE(text.find("reading the wheel through evdev"), std::string::npos);
    if (!fs::exists("/dev/input")) {
        EXPECT_EQ(out(), "no joysticks found\n");
    }
}

// One session per computer: a second one (or --unhide while one runs) stops at once, exit 1, saying why.
TEST_F(ServiceMainTest, OneSessionAtATime) {
    const char* old = std::getenv("XDG_RUNTIME_DIR");
    const std::optional<std::string> saved = old != nullptr ? std::optional<std::string>(old) : std::nullopt;
    setenv("XDG_RUNTIME_DIR", (dir_ / "runtime").c_str(), 1);
    std::string error;
    const auto held = platform::SingleInstance::acquire("wheelbri-service", error);
    if (saved) {
        setenv("XDG_RUNTIME_DIR", saved->c_str(), 1);
    } else {
        unsetenv("XDG_RUNTIME_DIR");
    }
    ASSERT_NE(held, nullptr) << error;
    EXPECT_EQ(run({"--config", absentWheelConfig().string()}), 1);
    EXPECT_NE(log().find("another wheelbri-service is already running"), std::string::npos);
    EXPECT_EQ(run({"--unhide"}), 1);
    EXPECT_NE(log().find("(--unhide: stop that session first)"), std::string::npos);
}

// --unhide with no session running: nothing is hidden here, exit 0.
TEST_F(ServiceMainTest, UnhideEndsAtOnce) {
    EXPECT_EQ(run({"--unhide"}), 0);
}

// --prepare-hide (the installer): no saved rig, a rig without devices, and a rig whose devices cannot be hidden here.
TEST_F(ServiceMainTest, PrepareHideSaysWhatItDid) {
    EXPECT_EQ(run({"--prepare-hide"}), 3);
    EXPECT_EQ(out(), "no saved rig: the first Start sets up the hiding\n");
    rig("");
    EXPECT_EQ(run({"--prepare-hide"}), 3);
    EXPECT_EQ(out(), "the saved rig lists no device to hide\n");
    rig("[device_1]\nname = Base\nid = ffff:fffe\naxis_0 = Wheel\n");
    const int code = run({"--prepare-hide"});
    EXPECT_TRUE(code == 1 || code == 3) << code;   // not hideable here, or not plugged in
    EXPECT_FALSE(out().empty());
}

// --prepare-hide leaves out what is never hidden (a virtual wheel's id) and names a device without a name by its
// id; a rig named in app.ini whose file is gone is no rig.
TEST_F(ServiceMainTest, PrepareHideLeavesOutWhatIsNeverHidden) {
    rig("[device_1]\nname = Virtual\nid = 046d:c262\naxis_0 = Wheel\n[device_2]\nid = ffff:fffe\naxis_0 = Pedal\n");
    const int code = run({"--prepare-hide"});
    EXPECT_TRUE(code == 1 || code == 3) << code;
    EXPECT_NE(log().find("is in the rig but is never hidden"), std::string::npos) << log();
    fs::remove(settings() / "rigs" / "Test rig.ini");
    EXPECT_EQ(run({"--prepare-hide"}), 3);
    EXPECT_EQ(out(), "the saved rig lists no device to hide\n");
}

// The old option names (--virtual-g920, --no-virtual-g920) and --ui are taken; a config path that is not UTF-8 is
// opened as it is (here: not there).
TEST_F(ServiceMainTest, OldOptionNamesAndOddPathsAreTaken) {
    write(dir_ / "default.ini", "[emulator]\n");
    EXPECT_EQ(run({"--virtual-g920", "--no-virtual-g920", "--ui", "--config", (dir_ / "default.ini").string()}), 2);
    EXPECT_NE(log().find("no wheel to read"), std::string::npos);
    EXPECT_EQ(run({"--config", (dir_ / "\xff.ini").string()}), 2);
    EXPECT_NE(log().find("cannot open config file"), std::string::npos);
}

// No rig and no source_device: nothing to read, exit 2; a stop file left from before is removed at the start.
TEST_F(ServiceMainTest, NoWheelToReadEndsTheSession) {
    write(dir_ / "stop", "old");
    write(dir_ / "default.ini", "[emulator]\n");
    EXPECT_EQ(run({"--config", (dir_ / "default.ini").string(), "--stop-file", (dir_ / "stop").string(), "--print",
                   "--no-hide", "--virtual-wheel"}),
              2);
    EXPECT_FALSE(fs::exists(dir_ / "stop"));
    const std::string text = log();
    EXPECT_NE(text.find("stops when " + (dir_ / "stop").string() + " appears"), std::string::npos);
    EXPECT_NE(text.find("no wheel to read: there is no rig with a wheel yet"), std::string::npos);
}

// A mapping that uses a control twice still runs (both inputs press together); the log says which. The stop file is
// written only once the service waits for the wheel: the config's warnings come before it removes an old stop file,
// so one written on the warning alone can be taken for a leftover (a slow run, e.g. under sanitizers) and the service
// then waits the whole 15 s.
TEST_F(ServiceMainTest, AControlUsedTwiceIsWarnedAbout) {
    const fs::path config =
        absentWheelConfig("[buttons]\ntarget_0 = 3\ntarget_1 = 3\n");
    start({"--config", config.string(), "--stop-file", (dir_ / "stop").string()});
    EXPECT_TRUE(logSays("a control used twice")) << log();
    EXPECT_TRUE(logSays("is not connected yet: waiting up to 15 s")) << log();
    write(dir_ / "stop", "");
    EXPECT_EQ(wait(), 0);
}

// The wheel is not there: it waits (up to 15 s); the stop file ends the wait, exit 0. The rig names the wheel when
// the config does not.
TEST_F(ServiceMainTest, TheStopFileEndsTheWaitForTheWheel) {
    rig("[device_1]\nname = Base\nid = ffff:fffe\nkind = wheel\naxis_0 = Wheel\n");
    write(dir_ / "default.ini", "[emulator]\n");
    start({"--config", (dir_ / "default.ini").string(), "--stop-file", (dir_ / "stop").string(), "--ffb-test"});
    EXPECT_TRUE(logSays("is not connected yet: waiting up to 15 s")) << log();
    write(dir_ / "stop", "");
    EXPECT_EQ(wait(), 0);
    const std::string text = log();
    EXPECT_NE(text.find("the wheel ('ffff:fffe') is not connected yet: waiting up to 15 s"), std::string::npos) << text;
    EXPECT_NE(text.find("stopped while waiting for the wheel"), std::string::npos);
}

// The wheel never comes: after the wait it says so, lists what is connected, exit 1.
TEST_F(ServiceMainTest, AWheelThatNeverComesEndsTheSession) {
    EXPECT_EQ(run({"--config", absentWheelConfig().string()}), 1);
    EXPECT_NE(err().find("no joystick matching 'ffff:fffe'. Connected devices:"), std::string::npos);
    EXPECT_NE(log().find("the wheel did not appear within 15 s"), std::string::npos);
}

// The app without a display: its settings are read (the config picked last time is gone: the default is used), the
// window cannot open, exit 1.
TEST_F(ServiceMainTest, TheAppWithoutADisplayEnds) {
    write(settings() / "app.ini", "[app]\nconfig = " + (dir_ / "gone.ini").string() + "\n");
    EXPECT_EQ(run({"--app", "--no-virtual-wheel", "--telemetry-tab", "--ffb-tab"}), 1);
    const std::string text = log();
    EXPECT_NE(text.find("the config picked last time is gone (" + (dir_ / "gone.ini").string() + ")"),
              std::string::npos)
        << text;
    EXPECT_NE(text.find("cannot open the window"), std::string::npos);
}

// The app with no --config: the config picked last time, when it is still there.
TEST_F(ServiceMainTest, TheAppTakesTheConfigPickedLastTime) {
    const fs::path config = absentWheelConfig();
    write(settings() / "app.ini", "[app]\nconfig = " + config.string() + "\n");
    EXPECT_EQ(run({"--app"}), 1);
    const std::string text = log();
    EXPECT_NE(text.find("config: " + config.string()), std::string::npos) << text;
    EXPECT_EQ(text.find("picked last time is gone"), std::string::npos);
}

// The app with --config: that one, and the picked one is not looked at.
TEST_F(ServiceMainTest, TheAppTakesItsConfigFromTheCommandLine) {
    const fs::path config = absentWheelConfig();
    EXPECT_EQ(run({"--app", "--config", config.string()}), 1);
    const std::string text = log();
    EXPECT_NE(text.find("config: " + config.string()), std::string::npos) << text;
    EXPECT_EQ(text.find("picked last time is gone"), std::string::npos);
}

}  // namespace
}  // namespace wheelbri::service

#endif
