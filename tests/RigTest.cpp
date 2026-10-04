#include "wheelbri/Rig.hpp"

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
    wheel.axes[0] = {"Wheel", ControlRole::Steering, false};
    wheel.buttons[13] = {"R paddle", ControlRole::ShiftUp, false};
    wheel.buttons[4] = {"", ControlRole::DpadUp, false};
    RigDevice pedals;
    pedals.name = "Pedals";
    pedals.id = {0x30B7, 0x1001};
    pedals.kind = DeviceKind::Pedals;
    pedals.axes[1] = {"Gas", ControlRole::Throttle, true};
    rig.devices = {wheel, pedals};
    return rig;
}

RigResult load(const std::string& text) {
    const auto parsed = IniDocument::parse(text);
    EXPECT_TRUE(parsed.errors.empty());
    return loadRig(parsed.document);
}

}  // namespace

TEST(Rig, RolesHaveNamesAndKinds) {
    for (const ControlRole role : kAxisRoles) {
        EXPECT_TRUE(isAxisRole(role));
        EXPECT_EQ(parseRole(roleName(role)), role);
    }
    for (const ControlRole role : kButtonRoles) {
        EXPECT_FALSE(isAxisRole(role));
        EXPECT_EQ(parseRole(roleName(role)), role);
    }
    EXPECT_EQ(parseRole(" D-Pad Up "), ControlRole::DpadUp);
    EXPECT_EQ(parseRole("Left Paddle"), ControlRole::LeftPaddle);
    EXPECT_EQ(roleName(ControlRole::RightPaddle), std::string("right paddle"));
    EXPECT_FALSE(parseRole("joystick").has_value());
}

TEST(Rig, WritesAndReadsBack) {
    const Rig rig = sampleRig();
    const std::string text = writeRig(rig, "Mine");
    EXPECT_NE(text.find("axis_1 = Gas | throttle | invert\r\n"), std::string::npos);
    EXPECT_NE(text.find("button_4 =  | d-pad up\r\n"), std::string::npos);
    const RigResult back = load(text);
    EXPECT_TRUE(back.errors.empty()) << back.errors.front();
    EXPECT_EQ(back.rig, rig);
    ASSERT_NE(back.rig.wheel(), nullptr);
    EXPECT_EQ(back.rig.wheel()->name, "My Racing Wheel");
    ASSERT_NE(back.rig.find({0x30B7, 0x1001}), nullptr);
}

TEST(Rig, ReportsBadLinesAndKeepsTheRest) {
    const RigResult result = load(
        "[device_1]\nid = 346e:0005\naxis_0 = Wheel | brakes\nbutton_99 = x\nbutton_1 = A | throttle\ncolour = red\n"
        "[device_2]\nname = no id\n[other]\n");
    EXPECT_EQ(result.errors.size(), 6U);
    ASSERT_EQ(result.rig.devices.size(), 1U);
    EXPECT_EQ(result.rig.devices[0].axes[0].label, "Wheel");
    EXPECT_EQ(result.rig.devices[0].axes[0].role, ControlRole::None);   // an axis cannot be a button role
    EXPECT_EQ(result.rig.devices[0].buttons[1].role, ControlRole::None);
}

TEST(Rig, ConvertsToAndFromDeviceNames) {
    const RigDevice device = sampleRig().devices[0];
    const DeviceLabels labels = labelsOf(device);
    EXPECT_EQ(labels.buttons[13], "R paddle");
    EXPECT_EQ(labels.vendorId, 0x346E);
    const RigDevice back = rigDeviceFrom(labels);
    EXPECT_EQ(back.buttons[13].label, "R paddle");
    EXPECT_EQ(back.buttons[13].role, ControlRole::None);
}

TEST(Rig, FilesListSaveRenameAndNames) {
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
    EXPECT_FALSE(renameRig(dir, "b rig", "a RIG").empty());   // taken (case ignored)
    EXPECT_EQ(renameRig(dir, "b rig", "Home"), "");
    EXPECT_EQ(listRigs(dir), (std::vector<std::string>{"A rig", "Home"}));
    EXPECT_NE(rigNameProblem("", {}), "");
    EXPECT_NE(rigNameProblem("home", {"Home"}).find("rig"), std::string::npos);
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
