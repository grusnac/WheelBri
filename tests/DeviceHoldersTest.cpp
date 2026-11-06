// SPDX-License-Identifier: GPL-3.0-or-later
// Which programs have the wheel open, as the hide filter's messages name them (DeviceHolders.hpp).

#include "wheelbri/DeviceHolders.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace wheelbri::holders {
namespace {

TEST(DeviceHolders, MatchesTheDeviceObjectIgnoringCase) {
    const std::vector<std::string> wheel{R"(\Device\000000e3)", R"(\Device\USBPDO-9)"};
    EXPECT_TRUE(namesDevice(R"(\Device\000000e3)", wheel));
    EXPECT_TRUE(namesDevice(R"(\DEVICE\000000E3)", wheel));
    EXPECT_TRUE(namesDevice(R"(\Device\USBPDO-9)", wheel));
    EXPECT_TRUE(namesDevice(R"(\Device\000000e3\KeyboardClass)", wheel));   // a path after the device's name
}

TEST(DeviceHolders, DoesNotMatchAnotherDeviceWithTheSameStart) {
    const std::vector<std::string> wheel{R"(\Device\000000e3)"};
    EXPECT_FALSE(namesDevice(R"(\Device\000000e30)", wheel));   // \Device\000000e30 is another device
    EXPECT_FALSE(namesDevice(R"(\Device\000000e)", wheel));
    EXPECT_FALSE(namesDevice(R"(\Device\HarddiskVolume3\Users\x.txt)", wheel));
    EXPECT_FALSE(namesDevice("", wheel));
    EXPECT_FALSE(namesDevice(R"(\Device\000000e3)", {}));
    EXPECT_FALSE(namesDevice(R"(\Device\000000e3)", {""}));
}

TEST(DeviceHolders, NamesEachProcessOnceInPidOrder) {
    EXPECT_EQ(describe({{14196, "firefox.exe"}, {7356, "XboxService.exe"}, {14196, "firefox.exe"}}),
              "XboxService.exe (pid 7356), firefox.exe (pid 14196)");
    EXPECT_EQ(describe({{4242, ""}}), "pid 4242");
    EXPECT_EQ(describe({}), "");
}

}  // namespace
}  // namespace wheelbri::holders
