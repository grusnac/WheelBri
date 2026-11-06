// SPDX-License-Identifier: GPL-3.0-or-later
// wheelbri-uhid as a program (tools/uhid/main.cpp), run as the app runs it, with a plain file standing in for
// /dev/uhid: what it writes to the "kernel" (the device made, its input reports, the device removed) lands in the
// file, and events appended to the file are what it reads back. Every way it stops: the stop file, the program that
// started it going away, and each thing it cannot do (no device, a device it may not open, the kernel refusing the
// wheel, the ports in use, a bad option).

#include "wheelbri/FfbPacket.hpp"
#include "wheelbri/LogLevelPacket.hpp"
#include "wheelbri/StatePacket.hpp"
#include "wheelbri/UdpLoopback.hpp"

#include "UhidTestLock.hpp"

#include <gtest/gtest.h>

#include <linux/uhid.h>
#include <sys/stat.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace wheelbri {
namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

// Starts the program with `args`; its pid.
pid_t start(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    std::string exe = WHEELBRI_UHID_EXE;
    argv.push_back(exe.data());
    std::vector<std::string> copies = args;
    for (std::string& arg : copies) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    EXPECT_EQ(::posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ), 0);
    return pid;
}

// Its exit code, or nullopt (and it is killed) when it did not stop within `limit`.
std::optional<int> waitFor(const pid_t pid, const std::chrono::milliseconds limit) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until) {
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == pid) {
            return WIFEXITED(status) ? std::optional<int>(WEXITSTATUS(status)) : std::optional<int>(-1);
        }
        std::this_thread::sleep_for(10ms);
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    return std::nullopt;
}

int run(const std::vector<std::string>& args) {
    return waitFor(start(args), 10s).value_or(-2);
}

// A port the system hands out now (free a moment later, as the program binds it).
std::uint16_t freePort() {
    const auto probe = UdpLoopbackReceiver::create(0);
    return probe != nullptr ? probe->localPort() : 0;
}

std::vector<uhid_event> eventsIn(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::vector<uhid_event> events;
    uhid_event event{};
    while (in.read(reinterpret_cast<char*>(&event), sizeof(event))) events.push_back(event);
    return events;
}

void append(const fs::path& file, const uhid_event& event) {
    std::ofstream(file, std::ios::binary | std::ios::app).write(reinterpret_cast<const char*>(&event), sizeof(event));
}

uhid_event output(const std::vector<std::uint8_t>& bytes) {
    uhid_event e{};
    e.type = UHID_OUTPUT;
    std::memcpy(e.u.output.data, bytes.data(), bytes.size());
    e.u.output.size = static_cast<std::uint16_t>(bytes.size());
    e.u.output.rtype = UHID_OUTPUT_REPORT;
    return e;
}

bool waitUntil(const std::function<bool()>& done, const std::chrono::milliseconds limit = 5s) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until) {
        if (done()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return done();
}

class UhidMain : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("wheelbri-uhidmain-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        ::setenv("XDG_STATE_HOME", dir_.c_str(), 1);   // its log next to the test's files, not the user's
    }
    void TearDown() override { fs::remove_all(dir_); }

    UhidTestLock lock_;   // first member: held from before SetUp until after TearDown
    fs::path dir_;
};

// The wheel's life as the service sees it: the wheel is made; states from the service become input reports; the
// app's log level is taken (louder, then quieter); states stopping turn the wheel neutral; the stop file (one left
// over from an earlier run removed at the start) ends it, the wheel removed.
TEST_F(UhidMain, MakesTheWheelFeedsItAndRemovesIt) {
    const fs::path device = dir_ / "uhid";
    std::ofstream(device).close();
    const fs::path stop = dir_ / "stop";
    std::ofstream(stop) << "left over from an earlier run";
    const std::uint16_t statePort = freePort();
    const pid_t pid = start({"--device", device.string(), "--stop-file", stop.string(), "--state-port",
                             std::to_string(statePort), "--ffb-port", std::to_string(freePort()), "--parent",
                             std::to_string(::getpid()), "--target", "g920"});
    ASSERT_TRUE(waitUntil([&] { return !eventsIn(device).empty(); })) << "no device made";
    EXPECT_EQ(eventsIn(device).front().type, UHID_CREATE2);
    EXPECT_FALSE(fs::exists(stop));

    const auto publisher = UdpLoopbackPublisher::create(statePort);
    ASSERT_NE(publisher, nullptr);
    StatePacket state;
    state.report.steering = 40000;
    for (int i = 0; i < 5; ++i) {
        ++state.sequence;
        publisher->publish(state);
        publisher->publishLogLevel(i < 2 ? LogLevel::Debug : LogLevel::Warn);
        std::this_thread::sleep_for(30ms);
    }
    std::this_thread::sleep_for(1700ms);   // no states for longer than kStaleMs: neutral

    std::ofstream(stop).close();
    EXPECT_EQ(waitFor(pid, 5s), 0);
    const std::vector<uhid_event> events = eventsIn(device);
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back().type, UHID_DESTROY);
    bool steered = false;
    for (const uhid_event& e : events) {
        if (e.type != UHID_INPUT2) continue;
        const int steering = e.u.input2.data[1] | (e.u.input2.data[2] << 8);
        steered = steered || steering != 0x8000;
    }
    EXPECT_TRUE(steered) << "the service's steering never reached an input report";
}

// The game's side, through a pipe standing in for /dev/uhid (what is written there is read back, as the kernel's
// queue would hand it over): the game opens the wheel and plays a constant force, which goes on to the service.
TEST_F(UhidMain, TheGamesForceGoesToTheService) {
    const fs::path device = dir_ / "uhid";
    ASSERT_EQ(::mkfifo(device.c_str(), 0600), 0);
    const fs::path stop = dir_ / "stop";
    const auto ffb = UdpLoopbackListener::create(0);
    ASSERT_NE(ffb, nullptr);
    const pid_t pid = start({"--device", device.string(), "--stop-file", stop.string(), "--state-port",
                             std::to_string(freePort()), "--ffb-port", std::to_string(ffb->localPort())});
    std::this_thread::sleep_for(300ms);   // it opened the pipe and made the wheel
    uhid_event open{};
    open.type = UHID_OPEN;
    for (const uhid_event& event : {open, output({0x15, 0x01, 0xA0, 0x0F}),                     // constant, block 1
                                    output({0x11, 0x01, 0x01, 0xDC, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF,
                                            0xFF, 0x04, 0xFF, 0x1F, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF}),   // set effect
                                    output({0x1A, 0x01, 0x01, 0x01})}) {                         // start
        append(device, event);
    }
    std::array<std::uint8_t, 64> buffer{};
    bool played = false;
    EXPECT_TRUE(waitUntil([&] {
        while (const auto size = ffb->receive(buffer)) {
            if (const auto packet = decodeFfbPacket(std::span<const std::uint8_t>(buffer.data(), *size))) {
                played = played || (packet->kind == FfbPacket::Kind::SlotState && packet->playing);
            }
        }
        return played;
    })) << "the game's force never reached the service";
    std::ofstream(stop).close();
    EXPECT_EQ(waitFor(pid, 5s), 0);
}

// Started by the app (--parent): when that program is gone (here: it is not the one that started it), it stops.
TEST_F(UhidMain, StopsWhenTheProgramThatStartedItIsGone) {
    const fs::path device = dir_ / "uhid";
    std::ofstream(device).close();
    const int notOurParent = ::getpid() + 100000;
    EXPECT_EQ(run({"--device", device.string(), "--state-port", std::to_string(freePort()), "--ffb-port",
                   std::to_string(freePort()), "--parent", std::to_string(notOurParent)}),
              0);
    EXPECT_EQ(eventsIn(device).back().type, UHID_DESTROY);
}

// What it cannot do ends it with 1 (and says why in its log); help is no error, a bad option is.
TEST_F(UhidMain, EveryWayItCannotStart) {
    EXPECT_EQ(run({"--help"}), 0);
    EXPECT_EQ(run({"--no-such-option"}), 2);
    EXPECT_EQ(run({"--device", (dir_ / "missing").string()}), 1);   // no uhid module
    EXPECT_EQ(run({"--device", "/dev/full", "--state-port", std::to_string(freePort()), "--ffb-port",
                   std::to_string(freePort())}),
              1);   // the kernel refuses the wheel: every write fails

    const fs::path device = dir_ / "uhid";
    std::ofstream(device).close();
    const auto taken = UdpLoopbackReceiver::create(0);   // another wheelbri-uhid has the state port
    ASSERT_NE(taken, nullptr);
    EXPECT_EQ(run({"--device", device.string(), "--state-port", std::to_string(taken->localPort()), "--ffb-port",
                   std::to_string(freePort())}),
              1);

    fs::permissions(device, fs::perms::none);
    if (!std::ifstream(device).good()) {   // not when Administrator / root
        EXPECT_EQ(run({"--device", device.string()}), 1);   // uhid without the udev rule
    }
    fs::permissions(device, fs::perms::owner_all);
}

}  // namespace
}  // namespace wheelbri
