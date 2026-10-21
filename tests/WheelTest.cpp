// Finding the wheel (service/src/Wheel.cpp): openWheel and wheelConnected over a fake backend, whatever the system.

#include "Wheel.hpp"

#include "LogLinesCapture.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace wheelbri::service {
namespace {

class FakeWheel : public Wheel {
public:
    FakeWheel(WheelInfo info, const int axes) : info_(std::move(info)), axes_(axes) {}
    [[nodiscard]] const WheelInfo& info() const override { return info_; }
    [[nodiscard]] int axes() const override { return axes_; }
    [[nodiscard]] int buttons() const override { return 12; }
    [[nodiscard]] int hats() const override { return 1; }
    bool poll(RawInputState&) override { return true; }
    [[nodiscard]] std::unique_ptr<IFfbSink> openForceFeedback(bool) override { return nullptr; }

private:
    WheelInfo info_;
    int axes_;
};

// Lists `wheels`; opens each except the paths in `refused`; every wheel has `axes` axes.
class FakeBackend : public WheelBackend {
public:
    std::vector<WheelInfo> wheels;
    std::set<std::string> refused;
    int axes = 4;
    int opened = 0;

    [[nodiscard]] std::vector<WheelInfo> list() override { return wheels; }
    [[nodiscard]] std::unique_ptr<Wheel> open(const WheelInfo& wheel) override {
        if (refused.count(wheel.path) > 0) return nullptr;
        ++opened;
        return std::make_unique<FakeWheel>(wheel, axes);
    }
    [[nodiscard]] std::string describe() const override { return "fake"; }
};

WheelInfo moza(const std::string& path) { return {"MOZA R3 Base", 0x346e, 0x0005, path}; }
WheelInfo pad() { return {"Xbox Controller", 0x045e, 0x0b12, "/dev/input/event9"}; }

// The first device whose name or vid:pid holds the match is opened; the others are only listed (at debug).
TEST(OpenWheel, TheFirstMatchingWheelIsOpened) {
    FakeBackend backend;
    backend.wheels = {pad(), moza("/dev/input/event3")};
    testing_support::LogLinesCapture capture("service.input");
    const auto wheel = openWheel(backend, "346e:0005");
    ASSERT_NE(wheel, nullptr);
    EXPECT_EQ(wheel->info().path, "/dev/input/event3");
    EXPECT_EQ(backend.opened, 1);
    EXPECT_TRUE(capture.has("candidate 'Xbox Controller' (045e:0b12) skipped"));
    EXPECT_TRUE(capture.has("candidate 'MOZA R3 Base' (346e:0005) matches"));
    EXPECT_TRUE(capture.has("opened 'MOZA R3 Base' (346e:0005) through fake: axes=4 buttons=12 hats=1"));
    EXPECT_FALSE(capture.has("only the first"));
}

// A matching device that cannot be opened (another program holds it) is passed over for the next one that matches.
TEST(OpenWheel, AWheelThatCannotBeOpenedIsPassedOver) {
    FakeBackend backend;
    backend.wheels = {moza("/dev/input/event3"), moza("/dev/input/event4")};
    backend.refused = {"/dev/input/event3"};
    const auto wheel = openWheel(backend, "MOZA");
    ASSERT_NE(wheel, nullptr);
    EXPECT_EQ(wheel->info().path, "/dev/input/event4");
}

// More axes than the service reads: opened, with a warning that only the first ones are read.
TEST(OpenWheel, TooManyAxesAreWarnedAbout) {
    FakeBackend backend;
    backend.wheels = {moza("/dev/input/event3")};
    backend.axes = static_cast<int>(RawInputState::kMaxAxes) + 1;
    testing_support::LogLinesCapture capture("service.input");
    ASSERT_NE(openWheel(backend, "MOZA"), nullptr);
    EXPECT_TRUE(capture.has("only the first " + std::to_string(RawInputState::kMaxAxes) + " are read"));
}

// Nothing matches: an error naming the match and how many are connected; one that matches but cannot be opened says
// that instead.
TEST(OpenWheel, NoWheelSaysWhy) {
    FakeBackend backend;
    backend.wheels = {pad(), moza("/dev/input/event3")};
    {
        testing_support::LogLinesCapture capture("service.input");
        EXPECT_EQ(openWheel(backend, "Fanatec"), nullptr);
        EXPECT_TRUE(capture.has("no game controller name or vid:pid contains 'Fanatec' (2 connected)"));
    }
    backend.refused = {"/dev/input/event3"};
    {
        testing_support::LogLinesCapture capture("service.input");
        EXPECT_EQ(openWheel(backend, "MOZA"), nullptr);
        EXPECT_TRUE(capture.has("'MOZA' is connected but cannot be opened (another program may hold it)"));
    }
    EXPECT_EQ(backend.opened, 0);
}

// Waiting for a lost wheel retries quietly: no candidate lines, and the failure itself at most every 30 s.
TEST(OpenWheel, QuietRetriesLogLittle) {
    FakeBackend backend;
    backend.wheels = {pad()};
    testing_support::LogLinesCapture capture("service.input");
    EXPECT_EQ(openWheel(backend, "MOZA", true), nullptr);
    EXPECT_EQ(openWheel(backend, "MOZA", true), nullptr);
    const std::string text = capture.text();
    EXPECT_EQ(text.find("candidate"), std::string::npos);
    // At most one of the two failures is said (none when another quiet retry said it less than 30 s ago).
    const auto first = text.find("no game controller name");
    EXPECT_TRUE(first == std::string::npos || text.find("no game controller name", first + 1) == std::string::npos);
}

// wheelConnected only looks: true when one matches, false otherwise, and nothing is opened.
TEST(OpenWheel, WheelConnectedOnlyLooks) {
    FakeBackend backend;
    backend.wheels = {pad(), moza("/dev/input/event3")};
    EXPECT_TRUE(wheelConnected(backend, "346e"));
    EXPECT_FALSE(wheelConnected(backend, "Fanatec"));
    backend.wheels.clear();
    EXPECT_FALSE(wheelConnected(backend, "346e"));
    EXPECT_EQ(backend.opened, 0);
}

}  // namespace
}  // namespace wheelbri::service
