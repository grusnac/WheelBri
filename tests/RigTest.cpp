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

TEST(Rig, DeadzonesWriteAndReadBack) {
    Rig rig = sampleRig();
    RigControl& steering = rig.devices[0].axes[0];
    steering.deadzone = 0.01F;
    RigControl& gas = rig.devices[1].axes[1];
    gas.deadzone = 0.03F;
    gas.full = 0.9F;
    const std::string text = writeRig(rig, "Mine");
    EXPECT_NE(text.find("axis_0 = Wheel | steering | deadzone 1%\r\n"), std::string::npos) << text;
    EXPECT_NE(text.find("axis_1 = Gas | throttle | invert | deadzone 3% | full 90%\r\n"), std::string::npos) << text;
    const RigResult back = load(text);
    EXPECT_TRUE(back.errors.empty()) << back.errors.front();
    ASSERT_EQ(back.rig.devices.size(), 2U);
    EXPECT_NEAR(*back.rig.devices[0].axes[0].deadzone, 0.01F, 1e-6F);
    EXPECT_FALSE(back.rig.devices[0].axes[0].full.has_value());
    EXPECT_NEAR(*back.rig.devices[1].axes[1].deadzone, 0.03F, 1e-6F);
    EXPECT_NEAR(*back.rig.devices[1].axes[1].full, 0.9F, 1e-6F);
}

TEST(Rig, DeadzonesInAnyOrderAndRefusedWhereTheyDoNotBelong) {
    const auto control = [](const std::string& line) {
        const RigResult result = load("[device_1]\nid = 30b7:1001\naxis_2 = " + line + "\n");
        return std::make_pair(result.rig.devices.empty() ? RigControl{} : result.rig.devices[0].axes[2],
                              result.errors.empty() ? std::string() : result.errors.front());
    };
    auto [brake, ok] = control("Brake | full 95% | brake | Deadzone 2.5%");   // any order, any case
    EXPECT_TRUE(ok.empty()) << ok;
    EXPECT_EQ(brake.role, ControlRole::Brake);
    EXPECT_NEAR(*brake.deadzone, 0.025F, 1e-6F);
    EXPECT_NEAR(*brake.full, 0.95F, 1e-6F);

    for (const char* bad : {"Hand | handbrake | deadzone 5%", "Wheel | steering | full 90%",
                            "Wheel | steering | deadzone 60%", "Gas | throttle | deadzone 50% | full 40%",
                            "Gas | throttle | deadzone lots", "Gas | deadzone 5%"}) {
        const auto [read, error] = control(bad);
        EXPECT_FALSE(error.empty()) << bad;
        EXPECT_FALSE(read.deadzone.has_value()) << bad;   // dropped, the rest kept
        EXPECT_FALSE(read.full.has_value()) << bad;
    }
}

namespace {

// The wheel (346e:0005) with steering on axis 0, and pedals (30b7:1001, device 1) with throttle 1, brake 2, clutch 3.
MapperConfig pedalMapping() {
    MapperConfig mapper;
    mapper.accessories[0] = {0x30B7, 0x1001};
    mapper.steering.sourceAxis = 0;
    mapper.throttle = {1, 1, false, 0.02F, 0.98F, 1.0F};
    mapper.brake = {2, 1, false, 0.02F, 0.95F, 1.0F};
    mapper.clutch = {3, 1, false, 0.02F, 0.98F, 1.0F};
    return mapper;
}

}  // namespace

TEST(Rig, DeadzonesOfTheRigLaidOverTheMapping) {
    Rig rig = sampleRig();   // the wheel's axis 0 = steering; the pedals' axis 1 = Gas
    rig.devices[0].axes[0].deadzone = 0.01F;
    rig.devices[1].axes[1].deadzone = 0.04F;
    rig.devices[1].axes[1].full = 0.9F;
    rig.devices[1].axes[2] = {"Brake", ControlRole::Brake, false, std::nullopt, 0.85F};   // only its full point
    MapperConfig mapper = pedalMapping();
    const std::vector<std::string> notes = applyRigDeadzones(mapper, rig, "346e:0005");
    EXPECT_NEAR(mapper.steering.deadzone, 0.01F, 1e-6F);
    EXPECT_NEAR(mapper.throttle.deadzoneLow, 0.04F, 1e-6F);
    EXPECT_NEAR(mapper.throttle.saturationHigh, 0.9F, 1e-6F);
    EXPECT_NEAR(mapper.brake.deadzoneLow, 0.02F, 1e-6F);   // the config's
    EXPECT_NEAR(mapper.brake.saturationHigh, 0.85F, 1e-6F);
    EXPECT_NEAR(mapper.clutch.deadzoneLow, 0.02F, 1e-6F);   // not in the rig: untouched
    EXPECT_NEAR(mapper.clutch.saturationHigh, 0.98F, 1e-6F);
    ASSERT_EQ(notes.size(), 3U);
    EXPECT_EQ(notes[0], "steering <- 'Wheel' (axis 0 of My Racing Wheel): dead zone 1% (the rig's)");
    EXPECT_EQ(notes[1], "throttle <- 'Gas' (axis 1 of Pedals): dead zone 4%, full 90% (the rig's)");
    EXPECT_EQ(notes[2], "brake <- 'Brake' (axis 2 of Pedals): dead zone 2%, full 85% (the rig's; the other the config's)");
}

TEST(Rig, DeadzonesFindTheWheelByNameOrAsTheRigsWheel) {
    Rig rig = sampleRig();
    rig.devices[0].axes[0].deadzone = 0.05F;
    for (const char* match : {"", "Racing Wheel", "346e"}) {
        MapperConfig mapper = pedalMapping();
        EXPECT_EQ(applyRigDeadzones(mapper, rig, match).size(), 1U) << match;
        EXPECT_NEAR(mapper.steering.deadzone, 0.05F, 1e-6F) << match;
    }
    MapperConfig other = pedalMapping();
    EXPECT_TRUE(applyRigDeadzones(other, rig, "044f:b696").empty());   // the config's wheel is not in the rig
    EXPECT_EQ(other.steering.deadzone, 0.0F);
}

TEST(Rig, DeadzonesThatDoNotFitTheConfigAreNotUsed) {
    Rig rig = sampleRig();
    rig.devices[1].axes[1].full = 0.01F;   // below the config's dead zone (2%)
    MapperConfig mapper = pedalMapping();
    const std::vector<std::string> notes = applyRigDeadzones(mapper, rig, "346e:0005");
    ASSERT_EQ(notes.size(), 1U);
    EXPECT_NE(notes[0].find("not used"), std::string::npos) << notes[0];
    EXPECT_NEAR(mapper.throttle.saturationHigh, 0.98F, 1e-6F);
}

TEST(Rig, TheG920sButtonsAreRolesWithItsNames) {
    EXPECT_EQ(roleLabel(ControlRole::ButtonY), "Y");
    EXPECT_EQ(roleLabel(ControlRole::Rsb), "RSB");
    EXPECT_EQ(roleLabel(ControlRole::Xbox), "Xbox");
    EXPECT_EQ(roleLabel(ControlRole::DpadUp), "D-pad up");
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
