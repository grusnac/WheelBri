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
        EXPECT_TRUE(roleFits(role, false));
        EXPECT_EQ(role == ControlRole::Handbrake, roleFits(role, true));   // the handbrake: axis or button
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
    const RigControl& gas = old.rig.devices[0].axes[1];
    EXPECT_EQ(gas.label, "Gas");
    EXPECT_EQ(gas.role, ControlRole::Throttle);
    EXPECT_TRUE(gas.invert);
    EXPECT_EQ(old.rig.devices[0].axes[2].role, ControlRole::Brake);   // in any order, any case
    EXPECT_EQ(old.rig.devices[0].axes[3].role, ControlRole::Clutch);
    const std::string text = writeRig(old.rig, "Mine");
    EXPECT_EQ(text.find("deadzone"), std::string::npos) << text;
    EXPECT_EQ(text.find("full "), std::string::npos) << text;
    EXPECT_NE(text.find("axis_1 = Gas | throttle | invert\r\n"), std::string::npos) << text;
    // On a button they were never allowed: still not.
    EXPECT_FALSE(load("[device_1]\nid = 30b7:1001\nbutton_1 = A | deadzone 3%\n").errors.empty());
}

TEST(Rig, TheG920sButtonsAreRolesWithItsNames) {
    EXPECT_EQ(roleLabel(ControlRole::ButtonY), "Y");
    EXPECT_EQ(roleLabel(ControlRole::Rsb), "RSB");
    EXPECT_EQ(roleLabel(ControlRole::Xbox), "Xbox");
    EXPECT_EQ(roleLabel(ControlRole::DpadUp), "D-pad Up");
    EXPECT_EQ(roleLabel(ControlRole::DpadLeft), "D-pad Left");
    EXPECT_EQ(roleLabel(ControlRole::LeftPaddle), "L Paddle");
    EXPECT_EQ(roleLabel(ControlRole::RightPaddle), "R Paddle");
    EXPECT_EQ(g920ButtonFor(ControlRole::ShiftUp), "R Paddle");
    // A name made by picking the role under the old role names reads as the new one; a name of the user's own stays.
    const IniDocument oldNames = IniDocument::parse(
        "[device_1]\nname = Wheel\nid = 346e:0005\nkind = wheel\nbutton_12 = Left paddle | left paddle\n"
        "button_13 = Right Paddle | right paddle\nbutton_14 = Left paddle | button\nbutton_15 = Shift | right paddle\n"
        "button_4 = D-pad up | d-pad up\nbutton_5 = Hat right | d-pad right\n")
                                     .document;
    const RigResult read = loadRig(oldNames);
    ASSERT_EQ(read.rig.devices.size(), 1U);
    EXPECT_EQ(read.rig.devices[0].buttons[12].label, "L Paddle");
    EXPECT_EQ(read.rig.devices[0].buttons[13].label, "R Paddle");
    EXPECT_EQ(read.rig.devices[0].buttons[14].label, "Left paddle");
    EXPECT_EQ(read.rig.devices[0].buttons[15].label, "Shift");
    EXPECT_EQ(read.rig.devices[0].buttons[4].label, "D-pad Up");
    EXPECT_EQ(read.rig.devices[0].buttons[5].label, "Hat right");
    EXPECT_EQ(roleLabel(ControlRole::Steering), "Steering");
    EXPECT_EQ(roleLabel(ControlRole::None), "");
    for (const ControlRole role : kButtonRoles) {
        EXPECT_LE(roleLabel(role).size(), DeviceLabels::kMaxLabel) << roleName(role);   // fits the name field
    }
    for (const ControlRole role : kAxisRoles) EXPECT_LE(roleLabel(role).size(), DeviceLabels::kMaxLabel);
    // Every named G920 button has a role that stands for it.
    const DeviceProfile g920(g920Spec());
    for (std::size_t i = 0; i < 32; ++i) {
        const std::string name(g920.buttonName(i));
        if (name.empty()) continue;
        bool found = false;
        for (const ControlRole role : kButtonRoles) found = found || g920ButtonFor(role) == name;
        EXPECT_TRUE(found) << name;
    }
    // Written and read back by name.
    Rig rig = sampleRig();
    rig.devices[0].buttons[3] = {"Y", ControlRole::ButtonY, false};
    rig.devices[0].buttons[9] = {"RSB", ControlRole::Rsb, false};
    const std::string text = writeRig(rig, "Mine");
    EXPECT_NE(text.find("button_3 = Y | y\r\n"), std::string::npos) << text;
    const RigResult back = load(text);
    EXPECT_TRUE(back.errors.empty());
    EXPECT_EQ(back.rig, rig);
}

TEST(Rig, SameRoleElsewhere) {
    Rig rig = sampleRig();   // wheel: button 13 = shift up 'R paddle', button 4 = d-pad up; pedals: axis 1 throttle
    const DeviceId wheel{0x346E, 0x0005};
    const DeviceId pedals{0x30B7, 0x1001};
    EXPECT_EQ(sameRoleElsewhere(rig, wheel, false, 20, ControlRole::DpadUp), "button 4 of My Racing Wheel");
    EXPECT_EQ(sameRoleElsewhere(rig, wheel, false, 4, ControlRole::DpadUp), "");   // itself
    // Right paddle and Shift up are the same G920 button.
    EXPECT_EQ(sameRoleElsewhere(rig, pedals, false, 0, ControlRole::RightPaddle),
              "button 13 ('R paddle') of My Racing Wheel");
    EXPECT_EQ(sameRoleElsewhere(rig, wheel, true, 3, ControlRole::Throttle), "axis 1 ('Gas') of Pedals");
    EXPECT_EQ(sameRoleElsewhere(rig, wheel, false, 20, ControlRole::Button), "");   // plain buttons repeat
    EXPECT_EQ(sameRoleElsewhere(rig, wheel, false, 20, ControlRole::ButtonY), "");
}

TEST(Rig, OnlyOneSidedAxesDriveButtons) {
    for (const ControlRole role : {ControlRole::Throttle, ControlRole::Brake, ControlRole::Clutch, ControlRole::Handbrake}) {
        EXPECT_TRUE(axisDrivesButtons(role)) << roleName(role);
    }
    for (const ControlRole role : {ControlRole::Steering, ControlRole::OtherAxis, ControlRole::None, ControlRole::Button}) {
        EXPECT_FALSE(axisDrivesButtons(role)) << roleName(role);
    }
    // A handbrake switched to report a button keeps its role.
    const RigResult back = load("[device_1]\nid = 346e:1000\nbutton_0 = Hand | handbrake\naxis_0 = Lever | handbrake\n");
    EXPECT_TRUE(back.errors.empty()) << back.errors.front();
    EXPECT_EQ(back.rig.devices[0].buttons[0].role, ControlRole::Handbrake);
    EXPECT_EQ(back.rig.devices[0].axes[0].role, ControlRole::Handbrake);
}

TEST(Rig, AnAxisPressingAButtonShouldBeOneSided) {
    Rig rig = sampleRig();   // the pedals' axis 1 = Gas (throttle); the wheel's axis 0 = steering
    MapperConfig mapper;
    mapper.accessories[0] = {0x30B7, 0x1001};
    mapper.buttonSecondary[2][0] = ButtonSource{1, 1, true, false, 0.5F};   // the gas pedal: fine
    EXPECT_TRUE(buttonAxisProblems(mapper, rig, "346e:0005").empty());
    mapper.buttonSecondary[3][0] = ButtonSource{0, 0, true, false, 0.5F};   // the steering: said
    const std::vector<std::string> problems = buttonAxisProblems(mapper, rig, "346e:0005");
    ASSERT_EQ(problems.size(), 1U);
    EXPECT_NE(problems[0].find("target_3_or: axis 0 of My Racing Wheel is the steering"), std::string::npos)
        << problems[0];
}
