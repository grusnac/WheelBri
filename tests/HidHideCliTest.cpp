#include "wheelbri/HidHideCli.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

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

TEST(HidHideCli, NeverHidesKeyboardsMiceAndSystemControls) {
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

TEST(HidHideCli, TurnsDirectInputPathsIntoInstanceIds) {
    EXPECT_EQ(instanceIdOfHidPath(R"(\\?\hid#vid_346e&pid_0005&mi_02#8&8052f69&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030})"),
              R"(HID\VID_346E&PID_0005&MI_02\8&8052F69&0&0000)");
    EXPECT_EQ(instanceIdOfHidPath(R"(\\?\HID#VID_346E&PID_0004#7&1&0&0000)"), R"(HID\VID_346E&PID_0004\7&1&0&0000)");
    EXPECT_EQ(instanceIdOfHidPath(R"(\\?\usb#vid_346e&pid_0005#x#{a5dcbf10-6530-11d2-901f-00c04fb951ed})"), "");
    EXPECT_EQ(instanceIdOfHidPath(""), "");
}

TEST(HidHideCli, NeverHidesTheVirtualWheel) {
    EXPECT_TRUE(mayHide({0x346E, 0x0004}));
    EXPECT_FALSE(mayHide({0x046D, 0xC262}));
    EXPECT_FALSE(mayHide({0x046D, 0xC299}));   // the virtual G25
    EXPECT_FALSE(mayHide({}));
    EXPECT_EQ(deviceIdText({0x346E, 0x0004}), "346e:0004");
}

namespace {

// One container with the devices given (JSON text), as --dev-gaming prints them.
std::string container(const std::string& devices) {
    return R"([ { "friendlyName" : "X" , "devices" : [ )" + devices + " ] } ]";
}

std::string device(const std::string& path, const std::string& gaming = "true") {
    return R"({ "present" : true , "gamingDevice" : )" + gaming + R"( , "deviceInstancePath" : ")" + path + R"(" })";
}

}  // namespace

// Every JSON escape HidHideCLI may print, in a field we do not read and in the path we do: read, not an error.
TEST(HidHideCli, ReadsEveryStringEscape) {
    const std::string json = R"([ { "friendlyName" : "a\"b\\c\/d\n\t\r\b\f\u0041\u20ac\?" , "devices" : [ )" +
                             device(R"(HID\\VID_346E&PID_0005\\1\u0026)") + " ] } ]";
    const auto found = gamingDevicesOf(json, "346E");
    ASSERT_EQ(found.size(), 1U);
    EXPECT_EQ(found[0], "HID\\VID_346E&PID_0005\\1&");   // \u0026 is '&'
    const std::string euro = container(device(R"(HID\\VID_346E&PID_0005\\\u20ac)"));
    ASSERT_EQ(gamingDevicesOf(euro, "346E").size(), 1U);
    EXPECT_EQ(gamingDevicesOf(euro, "346E")[0], "HID\\VID_346E&PID_0005\\\xE2\x82\xAC");   // three UTF-8 bytes
}

// Output cut off or garbled (HidHideCLI killed, a pipe closed early): nothing is chosen, nothing throws.
TEST(HidHideCli, CutOrGarbledOutputGivesNothing) {
    const std::string good = container(device(R"(HID\\VID_346E&PID_0005\\1)"));
    ASSERT_EQ(gamingDevicesOf(good, "346E").size(), 1U);
    for (std::size_t length = 0; length < good.size(); ++length) {
        EXPECT_TRUE(gamingDevicesOf(good.substr(0, length), "346E").empty()) << good.substr(0, length);
    }
    for (const std::string& bad : {
             good + " ]",                                               // text after the document
             good + "x",
             std::string(R"([ { "devices" "x" } ])"),                   // no colon
             std::string(R"([ { "devices" : [ ] ])"),                   // no closing brace
             std::string(R"([ { devices : [ ] } ])"),                   // a key without quotes
             std::string(R"([ { "a" : 1 } )"),                          // no closing bracket
             std::string(R"([ { "a" : @ } ])"),                         // not a value
             std::string(R"([ "\u12" ])"),                              // a short \u escape
             std::string("[ \"abc\\"),                                  // cut inside an escape
             std::string("[ \"\\u1"),                                    // cut inside a \u escape
         }) {
        EXPECT_TRUE(gamingDevicesOf(bad, "346E").empty()) << bad;
        EXPECT_TRUE(describeDevicesOf(bad, "346E").empty()) << bad;
    }
}

// Nesting deeper than any real output (a broken or hostile program): refused, without running out of stack.
TEST(HidHideCli, VeryDeepNestingIsRefused) {
    const std::string deep = std::string(40, '[') + std::string(40, ']');
    EXPECT_TRUE(gamingDevicesOf(deep, "346E").empty());
    const std::string fine = "[" + std::string(30, '[') + std::string(30, ']') + "]";
    EXPECT_TRUE(gamingDevicesOf(fine, "346E").empty());   // read, but no devices in it
    EXPECT_TRUE(gamingDevicesOf(std::string(100000, '['), "346E").empty());
}

// Shapes other than HidHideCLI's: a root object, containers without a device list, paths that are not strings,
// gamingDevice missing or not a bool, numbers of every form, empty objects and arrays. Only real gaming devices count.
TEST(HidHideCli, OnlyWellFormedGamingDevicesCount) {
    EXPECT_TRUE(gamingDevicesOf(R"({ "devices" : [ ] })", "346E").empty());
    const std::string mixed =
        R"([ { } , [ ] , { "devices" : { } } , { "devices" : [ { } , )"
        R"({ "deviceInstancePath" : 3 , "gamingDevice" : true } , )"
        R"({ "deviceInstancePath" : "HID\\VID_346E&PID_0001\\a" } , )"
        R"({ "deviceInstancePath" : "HID\\VID_346E&PID_0002\\a" , "gamingDevice" : "true" } , )"
        R"({ "deviceInstancePath" : "HID\\VID_346E&PID_0003\\a" , "gamingDevice" : null , "n" : -1.5e+3 } , )"
        R"({ "deviceInstancePath" : "HID\\VID_346E&PID_0004\\a" , "gamingDevice" : true , "n" : 2E-1 } , )"
        R"({ "deviceInstancePath" : "hid\\vid_346e&pid_0004\\A" , "gamingDevice" : true } ] } ])";
    const auto found = gamingDevicesOf(mixed, "346E");
    ASSERT_EQ(found.size(), 1U);   // the same path in another case is the same device
    EXPECT_EQ(found[0], "HID\\VID_346E&PID_0004\\a");
    const auto described = describeDevicesOf(mixed, "346e");
    ASSERT_EQ(described.size(), 5U);
    EXPECT_EQ(described[0], "HID\\VID_346E&PID_0001\\a (gamingDevice ?, present ?)");   // missing: '?'
    EXPECT_EQ(described[1], "HID\\VID_346E&PID_0002\\a (gamingDevice true, present ?)");   // a string, as written
    EXPECT_EQ(described[2], "HID\\VID_346E&PID_0003\\a (gamingDevice , present ?)");      // null: nothing
    EXPECT_TRUE(describeDevicesOf(mixed, std::string(200, 'A')).empty());   // a needle longer than any path
}

// Instance ids and paths in the forms Windows uses: mixed-case hex, a path without the \\?\ prefix or GUID, and
// ids that only look like HID ones.
TEST(HidHideCli, ReadsIdsAndPathsInEveryCase) {
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

// HidHideCLI's list output: lines without two quotes are skipped; a last line without a line end still counts.
TEST(HidHideCli, QuotedValuesSkipLinesWithoutAPair) {
    EXPECT_EQ(quotedValues("\"a\"\n\none \" quote\n\"\"\n--x \"b\""), (std::vector<std::string>{"a", "", "b"}));
    EXPECT_TRUE(quotedValues("").empty());
}

// A document that is not a list (a number ending the text, an object): nothing found; ids with characters that are
// not hex digits on either side of the range are refused; each game controller id counts on its own.
TEST(HidHideCli, MoreShapesThatAreRefusedOrTaken) {
    EXPECT_TRUE(gamingDevicesOf("5", "346E").empty());
    EXPECT_TRUE(gamingDevicesOf("{ }", "346E").empty());
    for (const char* id : {R"(HID\VID_34!E&PID_0004\X)", R"(HID\VID_34gE&PID_0004\X)", R"(HID\VID_34GE&PID_0004\X)",
                           R"(HID\VID_346E&PID_00~4\X)", R"(HID\VID_346E-PID_0004\X)"}) {
        EXPECT_FALSE(hidDeviceIdOf(id).has_value()) << id;
    }
    EXPECT_TRUE(isGameController({"HID_DEVICE_UP:0001_U:0004"}));
    EXPECT_TRUE(isGameController({"HID_DEVICE_UP:0001_U:0005"}));
    EXPECT_TRUE(isGameController({"HID_DEVICE_UP:0001_U:0008"}));
    EXPECT_FALSE(isGameController({"HID_DEVICE_UP:0002"}));   // shorter than the simulation prefix
}

}  // namespace wheelbri::hidhide
