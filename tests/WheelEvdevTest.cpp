// The evdev backend (service/src/WheelEvdev.cpp) where no input device can be made (no /dev/uinput): what it does
// with nodes that are not input devices. A regular file of input_event records stands in for a device's stream (the
// ioctls fail on it, so it has no axes or buttons), a folder for a node that fails to read, a FIFO for one with
// nothing to read yet. What needs a real device (capabilities, effects) is covered by EvdevLayout and WheelOutput.

#if defined(__linux__)

#include "Wheel.hpp"

#include "LogLinesCapture.hpp"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <linux/input.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace wheelbri::service {
namespace {

namespace fs = std::filesystem;

class WheelEvdevTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("wheelbri-evdev-" + std::to_string(::getuid()) + "-" + std::to_string(::getpid()) + "-" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        backend_ = startWheelBackend();
        ASSERT_NE(backend_, nullptr);
    }
    void TearDown() override { fs::remove_all(dir_); }

    // `count` key events (each followed by nothing else), and a SYN_DROPPED when asked.
    void writeEvents(const fs::path& file, const int count, const bool dropped) const {
        std::vector<input_event> events;
        for (int i = 0; i < count; ++i) {
            input_event event{};
            event.type = EV_KEY;
            event.code = BTN_JOYSTICK;
            event.value = i % 2;
            events.push_back(event);
        }
        if (dropped) {
            input_event event{};
            event.type = EV_SYN;
            event.code = SYN_DROPPED;
            events.push_back(event);
        }
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(events.data()),
                  static_cast<std::streamsize>(events.size() * sizeof(input_event)));
    }
    [[nodiscard]] WheelInfo info(const fs::path& path) const { return {"Test Wheel", 0x346e, 0x0005, path.string()}; }

    fs::path dir_;
    std::unique_ptr<WheelBackend> backend_;
};

// The backend's name, and the listing of a machine without /dev/input (or with nothing a game controller): empty.
TEST_F(WheelEvdevTest, TheBackendListsWhatThereIs) {
    EXPECT_EQ(backend_->describe(), "evdev");
    if (!fs::exists("/dev/input")) {
        EXPECT_TRUE(backend_->list().empty());
    }
}

// A node that cannot be opened: no wheel, and the log says which and why.
TEST_F(WheelEvdevTest, ANodeThatCannotBeOpenedIsNoWheel) {
    testing_support::LogLinesCapture capture("service.input");
    EXPECT_EQ(backend_->open(info(dir_ / "event99")), nullptr);
    EXPECT_TRUE(capture.has("cannot open 'Test Wheel' (" + (dir_ / "event99").string() + ")"));
}

// A device with nothing to say (the stream is empty): every read gives the resting state; it describes itself by its
// info and has no axes, buttons or hats it could tell (the ioctls fail); its descriptor is what the loop waits on.
TEST_F(WheelEvdevTest, AQuietDeviceReadsItsRestingState) {
    std::ofstream(dir_ / "event0").flush();
    const auto wheel = backend_->open(info(dir_ / "event0"));
    ASSERT_NE(wheel, nullptr);
    EXPECT_EQ(wheel->info().name, "Test Wheel");
    EXPECT_EQ(wheel->name(), "Test Wheel");
    EXPECT_EQ(wheel->axes(), 0);
    EXPECT_EQ(wheel->buttons(), 0);
    EXPECT_EQ(wheel->hats(), 0);
    EXPECT_GE(wheel->waitSource(), 0);
    RawInputState state;
    EXPECT_TRUE(wheel->poll(state));
    EXPECT_TRUE(wheel->poll(state));
}

// More events than one read takes (64): read on until the stream is drained; a short last read ends it.
TEST_F(WheelEvdevTest, ALongStreamIsReadToTheEnd) {
    writeEvents(dir_ / "event1", 64 * 2 + 3, false);
    const auto wheel = backend_->open(info(dir_ / "event1"));
    ASSERT_NE(wheel, nullptr);
    RawInputState state;
    EXPECT_TRUE(wheel->poll(state));
}

// The kernel dropped events (SYN_DROPPED): the state is read again from the device, with a warning (at most every
// 10 s: none when an earlier drop in this process said it less than 10 s ago).
TEST_F(WheelEvdevTest, DroppedEventsMakeItReadTheStateAgain) {
    writeEvents(dir_ / "event2", 3, true);
    const auto wheel = backend_->open(info(dir_ / "event2"));
    ASSERT_NE(wheel, nullptr);
    testing_support::LogLinesCapture capture("service.input");
    RawInputState state;
    EXPECT_TRUE(wheel->poll(state));
    const std::string text = capture.text();
    const std::string line = "'Test Wheel': the kernel dropped input events; reading the state again";
    const auto first = text.find(line);
    EXPECT_TRUE(first == std::string::npos || text.find(line, first + 1) == std::string::npos);
}

// A node whose read fails (here a folder): disconnected, said once, and poll is false.
TEST_F(WheelEvdevTest, AReadThatFailsIsADisconnectedWheel) {
    fs::create_directories(dir_ / "event3");
    const auto wheel = backend_->open(info(dir_ / "event3"));
    ASSERT_NE(wheel, nullptr);
    testing_support::LogLinesCapture capture("service.input");
    RawInputState state;
    EXPECT_FALSE(wheel->poll(state));
    EXPECT_TRUE(capture.has("'Test Wheel' was disconnected:"));
}

// Nothing to read yet (a device that has a writer but no new events): not an error, the state as it was.
TEST_F(WheelEvdevTest, NothingNewIsNotAnError) {
    const fs::path fifo = dir_ / "event4";
    ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
    const int writer = ::open(fifo.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);   // keeps the FIFO open for writing
    ASSERT_GE(writer, 0);
    const auto wheel = backend_->open(info(fifo));
    ASSERT_NE(wheel, nullptr);
    RawInputState state;
    EXPECT_TRUE(wheel->poll(state));
    ::close(writer);
}

// Force feedback on something that is not a force feedback device: none, and why (no EV_FF); on a node that cannot
// be opened for writing: none, and the error.
TEST_F(WheelEvdevTest, ForceFeedbackNeedsAForceFeedbackDevice) {
    std::ofstream(dir_ / "event5").flush();
    const auto wheel = backend_->open(info(dir_ / "event5"));
    ASSERT_NE(wheel, nullptr);
    {
        testing_support::LogLinesCapture capture("service.ffb");
        EXPECT_EQ(wheel->openForceFeedback(false), nullptr);
        EXPECT_TRUE(capture.has("'Test Wheel' has no force feedback (no EV_FF)"));
    }
    fs::remove(dir_ / "event5");
    {
        testing_support::LogLinesCapture capture("service.ffb");
        EXPECT_EQ(wheel->openForceFeedback(true), nullptr);
        EXPECT_TRUE(capture.has("cannot open force feedback on 'Test Wheel'"));
    }
}

}  // namespace
}  // namespace wheelbri::service

#endif
