// SPDX-License-Identifier: GPL-3.0-or-later
// The rig's HID ids for the hide filter (HideIds.hpp).

#include "wheelbri/HideIds.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace wheelbri::hideids {
namespace {

TEST(HideIds, ReadsTheUsbIdsOfAHidDevice) {
    EXPECT_EQ(hidDeviceIdOf(R"(HID\VID_346E&PID_0004&MI_02\A&397610E2&0&0000)"), (DeviceId{0x346E, 0x0004}));
    EXPECT_EQ(hidDeviceIdOf(R"(hid\vid_30b7&pid_1001\7&1)"), (DeviceId{0x30B7, 0x1001}));
    EXPECT_FALSE(hidDeviceIdOf(R"(USB\VID_346E&PID_0004\X)").has_value());
    EXPECT_FALSE(hidDeviceIdOf(R"(HID\VID_34&PID_0004)").has_value());
    EXPECT_FALSE(hidDeviceIdOf(R"(HID\VID_ZZZZ&PID_0004\X)").has_value());
    EXPECT_FALSE(hidDeviceIdOf("").has_value());
}

TEST(HideIds, TellsGameControllersFromOtherCollections) {
    EXPECT_TRUE(isGameController({"HID_DEVICE_SYSTEM_GAME", "HID_DEVICE_UP:0001_U:0004", "HID_DEVICE"}));
    EXPECT_TRUE(isGameController({"HID_DEVICE_UP:0001_U:0008", "HID_DEVICE"}));   // multi-axis: many wheels
    EXPECT_TRUE(isGameController({"hid_device_up:0001_u:0005"}));
    EXPECT_TRUE(isGameController({"HID_DEVICE_UP:0002_U:0002", "HID_DEVICE"}));   // simulation controls
    EXPECT_FALSE(isGameController({"HID_DEVICE_UP:FF00_U:0001", "HID_DEVICE"}));  // a vendor tool's collection
    EXPECT_FALSE(isGameController({"HID_DEVICE_SYSTEM_KEYBOARD", "HID_DEVICE_UP:0001_U:0006", "HID_DEVICE"}));
    EXPECT_FALSE(isGameController({}));
}

TEST(HideIds, NeverHidesKeyboardsMiceAndSystemControls) {
    EXPECT_EQ(neverHiddenUsage({"HID_DEVICE_UP:0001_U:0004", "HID_DEVICE"}), "");   // joystick
    EXPECT_EQ(neverHiddenUsage({"HID_DEVICE_UP:0001_U:0008", "HID_DEVICE"}), "");   // multi-axis (many wheels)
    EXPECT_EQ(neverHiddenUsage({}), "");                                            // none given: DirectInput decides
    EXPECT_NE(neverHiddenUsage({"HID_DEVICE_SYSTEM_KEYBOARD", "HID_DEVICE_UP:0001_U:0006", "HID_DEVICE"}), "");
    EXPECT_NE(neverHiddenUsage({"hid_device_system_mouse"}), "");
    EXPECT_NE(neverHiddenUsage({"HID_DEVICE_UP:0001_U:0080"}), "");                 // power / sleep
    EXPECT_NE(neverHiddenUsage({"HID_DEVICE_UP:000C_U:0001"}), "");                 // media keys
    EXPECT_NE(neverHiddenUsage({"HID_DEVICE_UP:000D_U:0004"}), "");                 // touch screen
    EXPECT_NE(neverHiddenUsage({"HID_DEVICE_UP:0001_U:0002"}).find("mouse"), std::string::npos);
}

TEST(HideIds, TurnsDirectInputPathsIntoInstanceIds) {
    EXPECT_EQ(instanceIdOfHidPath(R"(\\?\hid#vid_346e&pid_0005&mi_02#8&8052f69&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030})"),
              R"(HID\VID_346E&PID_0005&MI_02\8&8052F69&0&0000)");
    EXPECT_EQ(instanceIdOfHidPath(R"(\\?\HID#VID_346E&PID_0004#7&1&0&0000)"), R"(HID\VID_346E&PID_0004\7&1&0&0000)");
    EXPECT_EQ(instanceIdOfHidPath(R"(\\?\usb#vid_346e&pid_0005#x#{a5dcbf10-6530-11d2-901f-00c04fb951ed})"), "");
    EXPECT_EQ(instanceIdOfHidPath(""), "");
}

TEST(HideIds, NeverHidesTheVirtualWheel) {
    EXPECT_TRUE(mayHide({0x346E, 0x0004}));
    EXPECT_FALSE(mayHide({0x046D, 0xC262}));
    EXPECT_FALSE(mayHide({0x046D, 0xC299}));   // the virtual G25
    EXPECT_FALSE(mayHide({}));
    EXPECT_EQ(deviceIdText({0x346E, 0x0004}), "346e:0004");
}

// Instance ids and paths in the forms Windows uses: mixed-case hex, a path without the \\?\ prefix or GUID, and
// ids that only look like HID ones.
TEST(HideIds, ReadsIdsAndPathsInEveryCase) {
    EXPECT_EQ(hidDeviceIdOf(R"(HID\VID_aBcD&PID_00fF\x)"), (DeviceId{0xABCD, 0x00FF}));
    EXPECT_FALSE(hidDeviceIdOf(R"(HID\VID_346E&PIX_0004\X)").has_value());
    EXPECT_FALSE(hidDeviceIdOf(R"(HID\VID_346E&PID_00G4\X)").has_value());
    EXPECT_FALSE(hidDeviceIdOf(R"(HID\VID_346E&PID_000)").has_value());   // cut short
    EXPECT_EQ(instanceIdOfHidPath(R"(hid#vid_346e&pid_0005#7&1)"), R"(HID\VID_346E&PID_0005\7&1)");
    EXPECT_EQ(instanceIdOfHidPath(R"(\\?\hi)"), "");
    EXPECT_EQ(instanceIdOfHidPath(R"(\\?)"), "");
    EXPECT_FALSE(isGameController({"HID_DEVICE_UP:0002"}));   // shorter than the simulation prefix
    EXPECT_TRUE(isGameController({"HID_DEVICE", "HID_DEVICE_UP:0001_U:0005"}));
    EXPECT_EQ(neverHiddenUsage({"HID_DEVICE_UP:000C"}), "");   // too short for the prefix
    EXPECT_TRUE(containsIgnoreCase("abc", ""));
    EXPECT_FALSE(containsIgnoreCase("ab", "abc"));
}

// Ids with characters that are not hex digits on either side of the range are refused; each game controller id
// counts on its own.
TEST(HideIds, MoreIdsThatAreRefusedOrTaken) {
    for (const char* id : {R"(HID\VID_34!E&PID_0004\X)", R"(HID\VID_34gE&PID_0004\X)", R"(HID\VID_34GE&PID_0004\X)",
                           R"(HID\VID_346E&PID_00~4\X)", R"(HID\VID_346E-PID_0004\X)"}) {
        EXPECT_FALSE(hidDeviceIdOf(id).has_value()) << id;
    }
    EXPECT_TRUE(isGameController({"HID_DEVICE_UP:0001_U:0004"}));
    EXPECT_TRUE(isGameController({"HID_DEVICE_UP:0001_U:0005"}));
    EXPECT_TRUE(isGameController({"HID_DEVICE_UP:0001_U:0008"}));
    EXPECT_FALSE(isGameController({"HID_DEVICE_UP:0002"}));   // shorter than the simulation prefix
}

}  // namespace
}  // namespace wheelbri::hideids
