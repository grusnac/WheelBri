// SPDX-License-Identifier: GPL-3.0-or-later
// The service's Windows device code that needs neither a device nor Administrator (Windows only): the hide filter's
// checks and answers for devices that are not there, and the virtual wheel's start without its driver. What touches a
// real device or HKLM is not run here.

#include "HideFilter.hpp"
#include "VirtualWheelDevice.hpp"

#include "wheelbri/HidWheelDevice.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace wheelbri::service {
namespace {

constexpr const char* kNoSuchInterface = R"(HID\VID_FFFF&PID_FFFE&MI_00\9&DEADBEEF&0&0000)";

// A rig device that is not plugged in: NotConnected, with no interfaces, and it says so.
TEST(WinHideFilter, ADeviceNotPluggedInIsNotConnected) {
    const std::vector<HideFilter::Check> checks = HideFilter::check({{DeviceId{0xFFFF, 0xFFFE}, "Nowhere"}}, true);
    ASSERT_EQ(checks.size(), 1U);
    EXPECT_EQ(checks[0].state, HideFilter::State::NotConnected);
    EXPECT_TRUE(checks[0].interfaces.empty());
    EXPECT_EQ(checks[0].target.name, "Nowhere");
    EXPECT_STRNE(HideFilter::stateName(checks[0].state), "");
}

// Every state has a name for the window and the log.
TEST(WinHideFilter, EveryStateHasAName) {
    for (const auto state : {HideFilter::State::Ready, HideFilter::State::NotInstalled, HideFilter::State::NeedsRestart,
                             HideFilter::State::NotConnected}) {
        EXPECT_STRNE(HideFilter::stateName(state), "");
    }
}

// An interface that is not there: the helper refuses to touch it, it has no filter of ours, and nothing is changed.
TEST(WinHideFilter, AnInterfaceThatIsNotThereIsRefusedAndLeftAlone) {
    const std::string problem = HideFilter::interfaceProblem(kNoSuchInterface);
    EXPECT_NE(problem.find("not connected"), std::string::npos) << problem;
    EXPECT_FALSE(HideFilter::hasOurDriver(kNoSuchInterface));
    bool changed = true;
    const std::string error = HideFilter::setFilterListed(kNoSuchInterface, true, changed);
    EXPECT_FALSE(changed);
    EXPECT_NE(error.find("not connected"), std::string::npos) << error;
}

// No interfaces: nothing to wait for or restart, and the wait gives up at once when cancelled.
TEST(WinHideFilter, WaitingForNothingIsDoneAndACancelEndsAWait) {
    std::string why;
    EXPECT_TRUE(HideFilter::waitUntilStarted({}, 100, {}, why, 0));
    EXPECT_FALSE(HideFilter::waitUntilStarted({kNoSuchInterface}, 10'000, [] { return true; }, why, 0));
}

// The virtual wheel without its driver (no test machine has it): no device is made, and the error says why.
TEST(WinVirtualWheel, WithoutItsDriverNoWheelIsMade) {
    const VirtualWheelDevice::DriverState state = VirtualWheelDevice::driverState();
    if (state == VirtualWheelDevice::DriverState::Installed) GTEST_SKIP() << "the driver is installed here";
    std::string error;
    const auto device = VirtualWheelDevice::create(*virtualWheel("g920"), error, [] { return true; });
    EXPECT_EQ(device, nullptr);
    EXPECT_FALSE(error.empty());
}

}  // namespace
}  // namespace wheelbri::service
