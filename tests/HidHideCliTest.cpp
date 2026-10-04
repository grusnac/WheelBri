#include "wheelbri/HidHideCli.hpp"

#include <gtest/gtest.h>

namespace wheelbri::hidhide {
namespace {

// As HidHideCLI --dev-gaming prints it (from a real run, 2026-09-30; shortened).
constexpr std::string_view kGaming = R"( [ { "friendlyName" : "My Racing Wheel" , "devices" : [
{ "present" : true ,
"gamingDevice" : true ,
"symbolicLink" : "\\?\\HID#VID_346E&PID_0005&MI_02#8&8052F69&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}" ,
"vendor" : "Gudsen" ,
"usage" : "Joystick" ,
"deviceInstancePath" : "HID\\VID_346E&PID_0005&MI_02\\8&8052F69&0&0000" ,
"baseContainerDeviceCount" : 3 } ,
{ "present" : true ,
"gamingDevice" : false ,
"usage" : "Vendor" ,
"deviceInstancePath" : "HID\\VID_346E&PID_0005&MI_00\\8&1&0&0000" ,
"baseContainerDeviceCount" : 3 } ] } ,
{ "friendlyName" : "Logitech G920 Driving Force Racing Wheel USB" , "devices" : [
{ "present" : true , "gamingDevice" : true ,
"deviceInstancePath" : "HID\\VID_046D&PID_C262\\1&2&3" , "baseContainerDeviceCount" : 1 } ] } ]
)";

TEST(HidHideCli, OnlyTheWheelsGameControllerInterfaceIsChosen) {
    const auto wheel = gamingDevicesOf(kGaming, "346E");
    ASSERT_EQ(wheel.size(), 1U);
    EXPECT_EQ(wheel[0], R"(HID\VID_346E&PID_0005&MI_02\8&8052F69&0&0000)");
    EXPECT_EQ(gamingDevicesOf(kGaming, "346e").size(), 1U);   // any case
    EXPECT_EQ(gamingDevicesOf(kGaming, "046D").size(), 1U);
    EXPECT_TRUE(gamingDevicesOf(kGaming, "1234").empty());
}

TEST(HidHideCli, DevicesAreDescribedForTheLog) {
    const auto described = describeDevicesOf(kGaming, "346E");
    ASSERT_EQ(described.size(), 2U);
    EXPECT_NE(described[1].find("gamingDevice false, present true"), std::string::npos);
}

TEST(HidHideCli, UnicodeEscapes) {
    std::string json(kGaming);
    const std::string name = "\"My Racing Wheel\"";
    json.replace(json.find(name), name.size(), "\"My Racing Wh\\u00e9el\"");
    EXPECT_EQ(gamingDevicesOf(json, "346E").size(), 1U);   // a proper escape: read as usual
    std::string broken(kGaming);
    broken.replace(broken.find(name), name.size(), "\"My \\uZZZZ Wheel\"");
    EXPECT_TRUE(gamingDevicesOf(broken, "346E").empty());   // not four hex digits: broken JSON, no exception
    std::string signedHex(kGaming);
    signedHex.replace(signedHex.find(name), name.size(), "\"My \\u+0aB Wheel\"");
    EXPECT_TRUE(gamingDevicesOf(signedHex, "346E").empty());
}

TEST(HidHideCli, BrokenJsonGivesNothing) {
    EXPECT_TRUE(gamingDevicesOf("[ { \"devices\" : [ { ", "346E").empty());
    EXPECT_TRUE(gamingDevicesOf("", "346E").empty());
}

TEST(HidHideCli, ListsAndSpellings) {
    const auto listed = quotedValues("--dev-hide \"HID\\VID_346E&PID_0005&MI_02\\8&8052F69&0&0000\"\r\n--dev-hide \"HID\\X\"\r\n");
    ASSERT_EQ(listed.size(), 2U);
    EXPECT_EQ(listed[0], R"(HID\VID_346E&PID_0005&MI_02\8&8052F69&0&0000)");
    const auto stored = storedSpellings({R"(HID\VID_346E&PID_0005&MI_02\8&8052f69&0&0000)", R"(HID\X)"},
                                        {R"(HID\VID_346E&PID_0005&MI_02\8&8052F69&0&0000)"});
    ASSERT_EQ(stored.size(), 1U);
    EXPECT_EQ(stored[0], R"(HID\VID_346E&PID_0005&MI_02\8&8052f69&0&0000)");   // HidHide's own spelling
}

}  // namespace
TEST(HidHideCli, ReadsTheUsbIdsOfAHidDevice) {
    EXPECT_EQ(hidDeviceIdOf(R"(HID\VID_346E&PID_0004&MI_02\A&397610E2&0&0000)"), (DeviceId{0x346E, 0x0004}));
    EXPECT_EQ(hidDeviceIdOf(R"(hid\vid_30b7&pid_1001\7&1)"), (DeviceId{0x30B7, 0x1001}));
    EXPECT_FALSE(hidDeviceIdOf(R"(USB\VID_346E&PID_0004\X)").has_value());
    EXPECT_FALSE(hidDeviceIdOf(R"(HID\VID_34&PID_0004)").has_value());
    EXPECT_FALSE(hidDeviceIdOf(R"(HID\VID_ZZZZ&PID_0004\X)").has_value());
    EXPECT_FALSE(hidDeviceIdOf("").has_value());
}

TEST(HidHideCli, TellsGameControllersFromOtherCollections) {
    EXPECT_TRUE(isGameController({"HID_DEVICE_SYSTEM_GAME", "HID_DEVICE_UP:0001_U:0004", "HID_DEVICE"}));
    EXPECT_TRUE(isGameController({"HID_DEVICE_UP:0001_U:0008", "HID_DEVICE"}));   // multi-axis: many wheels
    EXPECT_TRUE(isGameController({"hid_device_up:0001_u:0005"}));
    EXPECT_TRUE(isGameController({"HID_DEVICE_UP:0002_U:0002", "HID_DEVICE"}));   // simulation controls
    EXPECT_FALSE(isGameController({"HID_DEVICE_UP:FF00_U:0001", "HID_DEVICE"}));  // a vendor tool's collection
    EXPECT_FALSE(isGameController({"HID_DEVICE_SYSTEM_KEYBOARD", "HID_DEVICE_UP:0001_U:0006", "HID_DEVICE"}));
    EXPECT_FALSE(isGameController({}));
}

TEST(HidHideCli, TurnsDirectInputPathsIntoInstanceIds) {
    EXPECT_EQ(instanceIdOfHidPath(R"(\\?\hid#vid_346e&pid_0005&mi_02#8&8052f69&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030})"),
              R"(HID\VID_346E&PID_0005&MI_02\8&8052F69&0&0000)");
    EXPECT_EQ(instanceIdOfHidPath(R"(\\?\HID#VID_346E&PID_0004#7&1&0&0000)"), R"(HID\VID_346E&PID_0004\7&1&0&0000)");
    EXPECT_EQ(instanceIdOfHidPath(R"(\\?\usb#vid_346e&pid_0005#x#{a5dcbf10-6530-11d2-901f-00c04fb951ed})"), "");
    EXPECT_EQ(instanceIdOfHidPath(""), "");
}

TEST(HidHideCli, NeverHidesTheVirtualG920) {
    EXPECT_TRUE(mayHide({0x346E, 0x0004}));
    EXPECT_FALSE(mayHide({0x046D, 0xC262}));
    EXPECT_FALSE(mayHide({}));
    EXPECT_EQ(deviceIdText({0x346E, 0x0004}), "346e:0004");
}

}  // namespace wheelbri::hidhide
