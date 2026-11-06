// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/Rig.hpp"
#include "wheelbri/DeviceProfile.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace wheelbri;

namespace {

Rig sampleRig() {
    Rig rig;
    RigDevice wheel;
    wheel.name = "My Racing Wheel";
    wheel.id = {0x346E, 0x0005};
    wheel.kind = DeviceKind::Wheel;
    wheel.axes[0] = {"Wheel", ""};
    wheel.buttons[13] = {"R paddle", "R Paddle"};
    wheel.buttons[4] = {"D-pad Up", ""};
    RigDevice pedals;
    pedals.name = "Pedals";
    pedals.id = {0x30B7, 0x1001};
    pedals.kind = DeviceKind::Pedals;
    pedals.axes[1] = {"Gas", ""};
    rig.devices = {wheel, pedals};
    return rig;
}

RigResult load(const std::string& text) {
    const auto parsed = IniDocument::parse(text);
    EXPECT_TRUE(parsed.errors.empty());
    return loadRig(parsed.document);
}

}  // namespace

// The popup's one-click names fit the name field.
TEST(Rig, OfferedNamesFitTheNameField) {
    for (const std::string_view name : kAxisNames) EXPECT_LE(name.size(), DeviceLabels::kMaxLabel) << name;
    for (const std::string_view name : kButtonNames) EXPECT_LE(name.size(), DeviceLabels::kMaxLabel) << name;
}

TEST(Rig, WritesAndReadsBack) {
    const Rig rig = sampleRig();
    const std::string text = writeRig(rig, "Mine");
    EXPECT_NE(text.find("axis_1 = Gas\r\n"), std::string::npos);
    EXPECT_NE(text.find("button_4 = D-pad Up\r\n"), std::string::npos);
    const RigResult back = load(text);
    EXPECT_TRUE(back.errors.empty()) << back.errors.front();
    EXPECT_EQ(back.rig, rig);
    ASSERT_NE(back.rig.wheel(), nullptr);
    EXPECT_EQ(back.rig.wheel()->name, "My Racing Wheel");
    ASSERT_NE(back.rig.find({0x30B7, 0x1001}), nullptr);
}

TEST(Rig, ReportsBadLinesAndKeepsTheRest) {
    const RigResult result = load(
        "[device_1]\nid = 346e:0005\naxis_0 = Wheel | brakes\nbutton_99 = x\nbutton_1 = A | fast\ncolour = red\n"
        "[device_2]\nname = no id\n[other]\n");
    EXPECT_EQ(result.errors.size(), 6U);
    ASSERT_EQ(result.rig.devices.size(), 1U);
    EXPECT_EQ(result.rig.devices[0].axes[0], (RigControl{"Wheel", ""}));   // the name kept
    EXPECT_EQ(result.rig.devices[0].buttons[1], (RigControl{"A", "A"}));   // named as a choice: that choice
}

TEST(Rig, ConvertsToAndFromDeviceNames) {
    const RigDevice device = sampleRig().devices[0];
    const DeviceLabels labels = labelsOf(device);
    EXPECT_EQ(labels.buttons[13], "R paddle");
    EXPECT_EQ(labels.vendorId, 0x346E);
    const RigDevice back = rigDeviceFrom(labels);
    EXPECT_EQ(back.buttons[13].label, "R paddle");
}

TEST(Rig, FilesListAndSave) {
    const auto dir = std::filesystem::temp_directory_path() / "wheelbri-rigs-test";
    std::filesystem::remove_all(dir);
    EXPECT_TRUE(listRigs(dir).empty());
    ASSERT_TRUE(saveRig(dir, "b rig", sampleRig()));
    ASSERT_TRUE(saveRig(dir, "A rig", Rig{}));
    EXPECT_EQ(listRigs(dir), (std::vector<std::string>{"A rig", "b rig"}));
    std::vector<std::string> errors;
    const auto read = readRig(dir, "b rig", errors);
    ASSERT_TRUE(read.has_value());
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(*read, sampleRig());
    std::filesystem::remove_all(dir);
}

TEST(Rig, ADeviceNameWithACommentCharacterSurvivesSaving) {
    Rig rig;
    RigDevice box;
    box.name = "Button Box #2";   // as Windows may name it
    box.id = {0x1234, 0x5678};
    rig.devices = {box};
    const RigResult back = load(writeRig(rig, "Mine"));
    ASSERT_EQ(back.rig.devices.size(), 1U);
    EXPECT_EQ(back.rig.devices[0].name, "Button Box  2");   // not cut at '#'
    EXPECT_EQ(back.rig.devices[0].id, (DeviceId{0x1234, 0x5678}));
}

// Dead zones are the devices' own software's (since 2026-10-13): an older rig's are read and ignored, never an error,
// and not written again.
TEST(Rig, OldDeadzonesAreIgnoredAndNotWrittenAgain) {
    const RigResult old = load("[device_1]\nid = 30b7:1001\nname = Pedals\n"
                               "axis_1 = Gas | throttle | invert | deadzone 3% | full 90%\n"
                               "axis_2 = Brake | full 95% | brake | Deadzone 2.5%\n"
                               "axis_3 = Clutch | clutch | deadzone lots\n");
    EXPECT_TRUE(old.errors.empty()) << old.errors.front();
    ASSERT_EQ(old.rig.devices.size(), 1U);
    EXPECT_EQ(old.rig.devices[0].axes[1], (RigControl{"Gas", "Throttle"}));
    EXPECT_EQ(old.rig.devices[0].axes[2], (RigControl{"Brake", "Brake"}));   // in any order, any case
    EXPECT_EQ(old.rig.devices[0].axes[3], (RigControl{"Clutch", "Clutch"}));
    const std::string text = writeRig(old.rig, "Mine");
    EXPECT_EQ(text.find("deadzone"), std::string::npos) << text;
    EXPECT_EQ(text.find("full "), std::string::npos) << text;
    EXPECT_NE(text.find("axis_1 = Gas | throttle\r\n"), std::string::npos) << text;   // its invert left out
    // On a button they were never allowed: still not.
    EXPECT_FALSE(load("[device_1]\nid = 30b7:1001\nbutton_1 = A | deadzone 3%\n").errors.empty());
}

// An older rig's roles are what its controls are now ("It is"), by today's names: kept, and written again.
TEST(Rig, OlderRolesAreTheChoicesTheyNameNow) {
    const RigResult old = load(
        "[device_1]\nname = Moza R3\nid = 346e:0005\nkind = other\naxis_0 = Wheel | steering\n"
        "button_3 = Y | y\nbutton_4 = | d-pad up\nbutton_12 = Left paddle | left paddle\nbutton_0 = Hand | handbrake\n"
        "[device_2]\nname = Pedals\nid = 30b7:1001\nkind = pedals\naxis_1 = Gas | throttle | invert\n");
    EXPECT_TRUE(old.errors.empty()) << old.errors.front();
    ASSERT_EQ(old.rig.devices.size(), 2U);
    const RigDevice& r3 = old.rig.devices[0];
    EXPECT_EQ(r3.axes[0], (RigControl{"Wheel", "Steering"}));
    EXPECT_EQ(r3.buttons[3], (RigControl{"Y", "Y"}));
    EXPECT_EQ(r3.buttons[4], (RigControl{"", "D Up"}));                  // no name: shown as D Up
    EXPECT_EQ(r3.buttons[4].displayName(), "D Up");
    EXPECT_EQ(r3.buttons[12], (RigControl{"Left paddle", "L Paddle"}));  // the user's name as it was
    EXPECT_EQ(r3.buttons[0], (RigControl{"Hand", "Handbrake"}));
    EXPECT_EQ(r3.kind, DeviceKind::Wheel);                                      // it had the steering axis
    EXPECT_EQ(old.rig.devices[1].axes[1], (RigControl{"Gas", "Throttle"}));
    ASSERT_NE(old.rig.wheel(), nullptr);
    EXPECT_EQ(old.rig.wheel()->name, "Moza R3");
    const std::string text = writeRig(old.rig, "Mine");
    for (const char* line : {"axis_0 = Wheel | steering\r", "button_4 = | d up\r",
                             "button_12 = Left paddle | l paddle\r", "axis_1 = Gas | throttle\r"}) {
        EXPECT_NE(text.find(line), std::string::npos) << line << "\n" << text;
    }
    EXPECT_NE(text.find("kind = wheel\r\n"), std::string::npos) << text;
    EXPECT_EQ(load(text).rig, old.rig);
}

// The wheel (when the mapping names none): the first device of kind wheel, else the first device.
TEST(Rig, TheWheelIsTheFirstOfKindWheelElseTheFirst) {
    Rig rig = sampleRig();
    std::swap(rig.devices[0], rig.devices[1]);   // pedals first
    ASSERT_NE(rig.wheel(), nullptr);
    EXPECT_EQ(rig.wheel()->name, "My Racing Wheel");
    rig.devices[1].kind = DeviceKind::Other;
    EXPECT_EQ(rig.wheel()->name, "Pedals");
    EXPECT_EQ(Rig{}.wheel(), nullptr);
}

// What a control is ("It is") is kept whatever its name: named, renamed, or with no name (shown as the choice).
TEST(Rig, TheChoiceIsKeptWithOrWithoutAName) {
    Rig rig = sampleRig();
    rig.devices[0].buttons[7] = {"Horn", "Button"};
    rig.devices[0].buttons[8] = {"", "A"};
    EXPECT_TRUE(rig.devices[0].buttons[8].defined());
    EXPECT_EQ(rig.devices[0].buttons[7].displayName(), "Horn");
    EXPECT_EQ(rig.devices[0].buttons[8].displayName(), "A");
    const std::string text = writeRig(rig, "Mine");
    EXPECT_NE(text.find("button_7 = Horn | button\r\n"), std::string::npos) << text;
    EXPECT_NE(text.find("button_8 = | a\r\n"), std::string::npos) << text;
    const RigResult back = load(text);
    EXPECT_TRUE(back.errors.empty()) << back.errors.front();
    EXPECT_EQ(back.rig, rig);
    // The Mapping tab's names: an unnamed control shows what it is.
    EXPECT_EQ(labelsOf(back.rig.devices[0]).buttons[8], "A");
    EXPECT_EQ(labelsOf(back.rig.devices[0]).buttons[7], "Horn");
    // Anything else after the name is a mistake.
    EXPECT_FALSE(load("[device_1]\nid = 346e:0005\naxis_1 = Gas | horn\n").errors.empty());
    EXPECT_FALSE(load("[device_1]\nid = 346e:0005\nbutton_1 = Gas | horn\n").errors.empty());
}

// Hand-edited values the Rig tab would never write: each reported, the rest of the device still read.
TEST(Rig, ReportsBadIdsKindsAndControlNumbers) {
    const RigResult result = load("[device_1]\nid = 346e:0005\nkind = joystick\naxis_x = a\naxis_ = b\naxis_0007 = c\n"
                                  "axis_8 = d\nbutton_64 = e\naxis_007 = Gas\n"
                                  "[device_2]\nid = nope\nname = Pedals\n");
    ASSERT_EQ(result.errors.size(), 8U);
    EXPECT_EQ(result.errors[0], "[device_1] axis_ = 'b': no such axis (0..7)");
    EXPECT_EQ(result.errors[1], "[device_1] axis_0007 = 'c': no such axis (0..7)");   // four digits: not read
    EXPECT_EQ(result.errors[5], "[device_1] kind = 'joystick': expected wheel, pedals, shifter, handbrake, button box "
                                "or other");
    EXPECT_EQ(result.errors[6], "[device_2] id = 'nope': expected vvvv:pppp (hex)");
    EXPECT_EQ(result.errors[7], "[device_2] has no id: left out");
    ASSERT_EQ(result.rig.devices.size(), 1U);
    EXPECT_EQ(result.rig.devices[0].kind, DeviceKind::Other);
    EXPECT_EQ(result.rig.devices[0].axes[7].label, "Gas");   // leading zeros are fine
}

// A control name typed too long by hand: cut to what the name field holds, and reported.
TEST(Rig, CutsTooLongControlNames) {
    const RigResult result = load("[device_1]\nid = 346e:0005\nbutton_2 = A much too long name | invert\n");
    ASSERT_EQ(result.errors.size(), 1U);
    EXPECT_NE(result.errors[0].find("at most 12 characters"), std::string::npos);
    EXPECT_EQ(result.rig.devices[0].buttons[2], (RigControl{"A much too l", ""}));
}

// An older role a button cannot have now ("| steering") is dropped without an error, and an empty part ("a | | b")
// is not a mistake.
TEST(Rig, OlderRolesThatNoLongerFitAndEmptyPartsAreDropped) {
    const RigResult result = load("[device_1]\nid = 346e:0005\nbutton_1 = Wheel | steering\naxis_2 = Gas | | invert\n");
    EXPECT_TRUE(result.errors.empty()) << result.errors.front();
    EXPECT_EQ(result.rig.devices[0].buttons[1], (RigControl{"Wheel", ""}));
    EXPECT_EQ(result.rig.devices[0].axes[2], (RigControl{"Gas", ""}));
    EXPECT_EQ(result.rig.devices[0].kind, DeviceKind::Other);   // only a Steering *axis* makes it the wheel
}

// The same device twice (a copied section), and more devices than a rig holds: left out, each said once.
TEST(Rig, DuplicateAndExtraDevicesAreLeftOut) {
    std::string text;
    for (int i = 1; i <= 6; ++i) {
        const int product = i == 2 ? 1 : i;   // device_2 repeats device_1's id
        text += "[device_" + std::to_string(i) + "]\nid = 1234:000" + std::to_string(product) + "\n";
    }
    const RigResult result = load(text);
    ASSERT_EQ(result.errors.size(), 2U);
    EXPECT_EQ(result.errors[0], "[device_2] 1234:0001 is in the rig twice: left out");
    EXPECT_EQ(result.errors[1], "[device_6]: at most 4 devices in a rig: left out");
    ASSERT_EQ(result.rig.devices.size(), Rig::kMaxDevices);
    EXPECT_EQ(result.rig.devices.back().id, (DeviceId{0x1234, 0x0005}));
}

// An older rig's "| invert" (before 2026-10-19: the Mapping tab's Invert is the one that counts) is read without an
// error, ignored, and not written again; an axis with nothing else is then nothing.
TEST(Rig, AnOlderInvertIsIgnored) {
    const RigResult old = load("[device_1]\nid = 30b7:1001\naxis_1 = Gas | invert\naxis_3 = | invert\n");
    EXPECT_TRUE(old.errors.empty()) << old.errors.front();
    EXPECT_EQ(old.rig.devices[0].axes[1], (RigControl{"Gas", ""}));
    EXPECT_FALSE(old.rig.devices[0].axes[3].defined());
    const std::string text = writeRig(old.rig, "Mine");
    EXPECT_EQ(text.find("| invert"), std::string::npos) << text;
    EXPECT_EQ(text.find("axis_3"), std::string::npos) << text;
}

// The editable lookup finds a device by id, and can change it.
TEST(Rig, FindsADeviceById) {
    Rig rig = sampleRig();
    RigDevice* pedals = rig.find({0x30B7, 0x1001});
    ASSERT_NE(pedals, nullptr);
    pedals->name = "Renamed";
    EXPECT_EQ(rig.devices[1].name, "Renamed");
    EXPECT_EQ(rig.find({0x1111, 0x2222}), nullptr);
}

// The rigs folder: only .ini files (any case of the extension), not folders or other files; a missing rig is
// nothing, not an empty rig; a non-ASCII name works.
TEST(Rig, ListsOnlyRigFilesAndReadsNothingForAMissingOne) {
    const auto dir = std::filesystem::temp_directory_path() / "wheelbri-Rig-ListsOnlyRigFiles";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "folder.ini");
    std::ofstream(dir / "notes.txt") << "x";
    std::ofstream(dir / "Upper.INI") << "[device_1]\nid = 346e:0005\n";
    const std::string unicode = "Rig \xC3\xA9t\xC3\xA9";   // "Rig été"
    ASSERT_TRUE(saveRig(dir, unicode, sampleRig()));
    EXPECT_EQ(listRigs(dir), (std::vector<std::string>{unicode, "Upper"}));
    std::vector<std::string> errors{"stale"};
    EXPECT_FALSE(readRig(dir, "gone", errors).has_value());
    const auto read = readRig(dir, unicode, errors);
    ASSERT_TRUE(read.has_value());
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(*read, sampleRig());
    // A rig file with a broken line: the parser's errors come first, then the rig's.
    std::ofstream(dir / "broken.ini", std::ios::trunc) << "[device_1\n[device_1]\nid = 346e:0005\ncolour = red\n";
    ASSERT_TRUE(readRig(dir, "broken", errors).has_value());
    ASSERT_EQ(errors.size(), 2U);
    EXPECT_EQ(errors[0], "line 1: malformed section header");
    EXPECT_EQ(errors[1], "[device_1] colour = 'red': unknown key");
    std::filesystem::remove_all(dir);
}
