// The privileged steps away from Windows (service/src/PrivilegedSteps.cpp, Privileged.cpp, HideFilter.cpp,
// VirtualWheelDevice.cpp): there is no driver, filter or virtual wheel, so every step says so and nothing is changed.
// What each step does on Windows is the helper's and the installers' (HelperPipeTest, the scripts).

#ifndef _WIN32

#include "HideFilter.hpp"
#include "Privileged.hpp"
#include "PrivilegedSteps.hpp"
#include "VirtualWheelDevice.hpp"

#include "wheelbri/HidWheelDevice.hpp"

#include <gtest/gtest.h>

#include <string>

namespace wheelbri::service {
namespace {

const PrivilegedSteps::Cancelled kNever = [] { return false; };

// Here the process counts as Administrator: the steps run in it (DirectSteps), and each says it is Windows-only.
TEST(PrivilegedStepsElsewhere, EveryStepSaysItIsForWindows) {
    EXPECT_TRUE(PrivilegedSteps::available());
    PrivilegedSteps::Problem problem = PrivilegedSteps::Problem::HelperFailed;
    std::string text = "before";
    const auto steps = PrivilegedSteps::open(problem, text);
    ASSERT_NE(steps, nullptr);
    EXPECT_EQ(problem, PrivilegedSteps::Problem::None);
    EXPECT_STREQ(steps->how(), "as Administrator");

    std::string message;
    EXPECT_FALSE(steps->installWheelDriver(kNever, message));
    EXPECT_EQ(message, "the virtual wheel's driver is for Windows only");

    message.clear();
    EXPECT_EQ(steps->putHideFilter({}, {}, kNever, message), privileged::HideSetup::NothingToDo);
    EXPECT_EQ(message, "");
    EXPECT_EQ(steps->putHideFilter({"346e:0005"}, {"HID\\A", "HID\\B"}, kNever, message),
              privileged::HideSetup::Failed);
    EXPECT_NE(message.find("the hide filter is for Windows only (HID\\A"), std::string::npos) << message;

    std::string instanceId = "none";
    std::string error;
    EXPECT_FALSE(steps->createVirtualWheel(*virtualWheel("g920"), false, kNever, instanceId, error));
    EXPECT_EQ(error, "the virtual wheel exists on Windows only");
    EXPECT_EQ(instanceId, "none");
    steps->removeVirtualWheel();   // nothing there: nothing to remove

    message.clear();
    EXPECT_FALSE(steps->hideRig(message));
    EXPECT_EQ(message, "cannot write HKLM\\SOFTWARE\\wheelbri\\hide");
    steps->showRig();
    steps->shareLogFolder();
    steps->setDriverLogLevel(LogLevel::Debug);   // no driver to tell: nothing to warn about
}

// The privileged:: functions themselves away from Windows.
TEST(PrivilegedStepsElsewhere, ThePrivilegedFunctionsDoNothing) {
    EXPECT_TRUE(privileged::findDriverScript("install-driver.ps1", "driver", 3).empty());
    bool started = true;
    std::string error;
    EXPECT_FALSE(privileged::runDriverScript("x.ps1", {"-Quiet"}, "x", kNever, started, error));
    EXPECT_FALSE(started);
    EXPECT_EQ(error, "driver scripts run on Windows only");
    EXPECT_FALSE(privileged::writeAllowedPid(1234, 5678));
    EXPECT_EQ(privileged::allowedPid(), 0U);
    std::string message;
    EXPECT_TRUE(privileged::writeLogLevel(LogLevel::Info, message));
    EXPECT_FALSE(privileged::allowOnly(nullptr, message));
    EXPECT_EQ(message, "the hide filter is for Windows only");
}

// The hide filter away from Windows: nothing is hidden, every device is reported not connected, and the calls that
// would change something do nothing.
TEST(PrivilegedStepsElsewhere, TheHideFilterHidesNothing) {
    std::string why;
    EXPECT_FALSE(HideFilter::waitUntilStarted({"HID\\A"}, 1000, kNever, why, 0));
    EXPECT_EQ(why, "not on Windows");
    const auto checks = HideFilter::check({{DeviceId{0x346e, 0x0005}, "MOZA R3"}, {DeviceId{0x044f, 0xb66e}, "Pedals"}},
                                          true);
    ASSERT_EQ(checks.size(), 2U);
    EXPECT_EQ(checks[0].state, HideFilter::State::NotConnected);
    EXPECT_EQ(checks[1].target.name, "Pedals");
    EXPECT_EQ(checks[1].detail, "not on Windows");
    EXPECT_STREQ(HideFilter::stateName(HideFilter::State::Ready), "hidden while WheelBri runs");
    EXPECT_STREQ(HideFilter::stateName(HideFilter::State::NotInstalled), "not hidden");
    EXPECT_EQ(HideFilter::interfaceProblem("HID\\A"), "HID\\A: the hide filter is for Windows only");
    HideFilter filter;
    EXPECT_FALSE(filter.hide());
    filter.restore();
    HideFilter::clear();
    HideFilter::clearStale();
}

// The virtual wheel's driver state and DirectInput entries away from Windows: unknown, and nothing written.
TEST(PrivilegedStepsElsewhere, TheVirtualWheelIsUnknownHere) {
    EXPECT_EQ(VirtualWheelDevice::driverState(), VirtualWheelDevice::DriverState::Unknown);
    const DirectInputUserEntries entries(*virtualWheel("g923"), false);
    std::string error;
    EXPECT_EQ(VirtualWheelDevice::create(*virtualWheel("g25"), error, kNever), nullptr);
    EXPECT_EQ(error, "the virtual wheel exists on Windows only");
}

}  // namespace
}  // namespace wheelbri::service

#endif
