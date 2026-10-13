#include "wheelbri/Rig.hpp"
#include "wheelbri/DeviceProfile.hpp"

#include <gtest/gtest.h>

#include <fstream>

using namespace wheelbri;

namespace {

Rig sampleRig() {
    Rig rig;
    RigDevice wheel;
    wheel.name = "My Racing Wheel";
    wheel.id = {0x346E, 0x0005};
    wheel.kind = DeviceKind::Wheel;
    wheel.axes[0] = {"Wheel", false};
    wheel.buttons[13] = {"R paddle", false};
    wheel.buttons[4] = {"D-pad Up", false};
    RigDevice pedals;
    pedals.name = "Pedals";
    pedals.id = {0x30B7, 0x1001};
    pedals.kind = DeviceKind::Pedals;
    pedals.axes[1] = {"Gas", true};
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
    EXPECT_NE(text.find("axis_1 = Gas | invert\r\n"), std::string::npos);
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
    EXPECT_EQ(result.rig.devices[0].axes[0], (RigControl{"Wheel", false}));   // the name kept
    EXPECT_EQ(result.rig.devices[0].buttons[1], (RigControl{"A", false}));
}

TEST(Rig, ConvertsToAndFromDeviceNames) {
    const RigDevice device = sampleRig().devices[0];
    const DeviceLabels labels = labelsOf(device);
    EXPECT_EQ(labels.buttons[13], "R paddle");
    EXPECT_EQ(labels.vendorId, 0x346E);
    const RigDevice back = rigDeviceFrom(labels);
    EXPECT_EQ(back.buttons[13], (RigControl{"R paddle", false}));
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
    EXPECT_EQ(old.rig.devices[0].axes[1], (RigControl{"Gas", true}));
    EXPECT_EQ(old.rig.devices[0].axes[2], (RigControl{"Brake", false}));   // in any order, any case
    EXPECT_EQ(old.rig.devices[0].axes[3], (RigControl{"Clutch", false}));
    const std::string text = writeRig(old.rig, "Mine");
    EXPECT_EQ(text.find("deadzone"), std::string::npos) << text;
    EXPECT_EQ(text.find("full "), std::string::npos) << text;
    EXPECT_NE(text.find("axis_1 = Gas | invert\r\n"), std::string::npos) << text;
    // On a button they were never allowed: still not.
    EXPECT_FALSE(load("[device_1]\nid = 30b7:1001\nbutton_1 = A | deadzone 3%\n").errors.empty());
}

// Roles are gone (since 2026-10-13: WheelBri does not guess what a control is for). An older rig's are read and
// ignored: a control keeps its name (a role-only one gets the role's), a steering axis makes its device the wheel, and
// saving it again leaves the roles out.
TEST(Rig, OlderRolesAreIgnoredAndNotWrittenAgain) {
    const RigResult old = load(
        "[device_1]\nname = Moza R3\nid = 346e:0005\nkind = other\naxis_0 = Wheel | steering\n"
        "button_3 = Y | y\nbutton_4 = | d-pad up\nbutton_12 = Left paddle | left paddle\nbutton_0 = Hand | handbrake\n"
        "[device_2]\nname = Pedals\nid = 30b7:1001\nkind = pedals\naxis_1 = Gas | throttle | invert\n");
    EXPECT_TRUE(old.errors.empty()) << old.errors.front();
    ASSERT_EQ(old.rig.devices.size(), 2U);
    const RigDevice& r3 = old.rig.devices[0];
    EXPECT_EQ(r3.axes[0], (RigControl{"Wheel", false}));
    EXPECT_EQ(r3.buttons[3], (RigControl{"Y", false}));
    EXPECT_EQ(r3.buttons[4], (RigControl{"D-pad Up", false}));        // a role only: the role's name
    EXPECT_EQ(r3.buttons[12], (RigControl{"Left paddle", false}));    // the user's name as it was
    EXPECT_EQ(r3.buttons[0], (RigControl{"Hand", false}));
    EXPECT_EQ(r3.kind, DeviceKind::Wheel);                            // it had the steering axis
    EXPECT_EQ(old.rig.devices[1].axes[1], (RigControl{"Gas", true}));
    ASSERT_NE(old.rig.wheel(), nullptr);
    EXPECT_EQ(old.rig.wheel()->name, "Moza R3");
    const std::string text = writeRig(old.rig, "Mine");
    for (const char* role : {"steering", "| y", "d-pad up\r", "left paddle\r", "throttle", "handbrake"}) {
        EXPECT_EQ(text.find(role), std::string::npos) << role << "\n" << text;
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
