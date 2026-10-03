#include "wheelbri/HidHideCli.hpp"

#include <gtest/gtest.h>

namespace wheelbri::hidhide {
namespace {

// As HidHideCLI --dev-gaming prints it (from a real run, 2026-09-30; shortened).
constexpr std::string_view kGaming = R"( [ { "friendlyName" : "R3 Racing Wheel and Pedals" , "devices" : [
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
    const auto moza = gamingDevicesOf(kGaming, "346E");
    ASSERT_EQ(moza.size(), 1U);
    EXPECT_EQ(moza[0], R"(HID\VID_346E&PID_0005&MI_02\8&8052F69&0&0000)");
    EXPECT_EQ(gamingDevicesOf(kGaming, "346e").size(), 1U);   // any case
    EXPECT_EQ(gamingDevicesOf(kGaming, "046D").size(), 1U);
    EXPECT_TRUE(gamingDevicesOf(kGaming, "1234").empty());
}

TEST(HidHideCli, DevicesAreDescribedForTheLog) {
    const auto described = describeDevicesOf(kGaming, "346E");
    ASSERT_EQ(described.size(), 2U);
    EXPECT_NE(described[1].find("gamingDevice false, present true"), std::string::npos);
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
}  // namespace wheelbri::hidhide
