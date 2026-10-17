#include "wheelbri/StartNeeds.hpp"

#include <gtest/gtest.h>

namespace wheelbri {
namespace {

RigDevice device(const std::string& name, const std::uint16_t vid, const std::uint16_t pid, const bool wheel) {
    RigDevice d;
    d.name = name;
    d.id = {vid, pid};
    if (wheel) d.kind = DeviceKind::Wheel;
    return d;
}

Rig rig() {
    Rig r;
    r.devices.push_back(device("Pedals", 0x30b7, 0x1001, false));
    r.devices.push_back(device("Moza R3", 0x346e, 0x0005, true));
    r.devices.push_back(device("Handbrake", 0x1234, 0x0002, false));
    return r;
}

EmulatorConfig withTarget(const std::string& target) {
    EmulatorConfig config;
    config.targetProfile = target;
    return config;
}

const PresentDevice kR3{"R3 Racing Wheel and Pedals", 0x346e, 0x0005};
const PresentDevice kPedals{"Pedals", 0x30b7, 0x1001};

TEST(StartNeeds, NoRigNoStart) {
    const StartNeeds needs = startNeeds(EmulatorConfig{}, nullptr);
    EXPECT_TRUE(needs.devices.empty());
    EXPECT_FALSE(needs.problem.empty());
    const Rig empty;
    EXPECT_FALSE(startNeeds(EmulatorConfig{}, &empty).problem.empty());   // a rig with no device
}

TEST(StartNeeds, TheRigsWheelWhenTheConfigNamesNone) {
    const Rig r = rig();
    const StartNeeds needs = startNeeds(EmulatorConfig{}, &r);
    ASSERT_EQ(needs.devices.size(), 1U);   // only the wheel: the mapping reads no accessory
    EXPECT_EQ(needs.devices[0].label, "Moza R3");
    EXPECT_TRUE(missingDevices(needs, {kR3}).empty());
    EXPECT_EQ(missingDevices(needs, {kPedals}), std::vector<std::string>{"Moza R3"});
    EXPECT_EQ(missingDevices(needs, {}), std::vector<std::string>{"Moza R3"});
}

TEST(StartNeeds, OnlyTheAccessoriesTheMappingUses) {
    const Rig r = rig();
    EmulatorConfig config;
    config.mapper.accessories[0] = {0x30b7, 0x1001};   // the pedals; the handbrake is in the rig but not mapped
    const StartNeeds needs = startNeeds(config, &r);
    ASSERT_EQ(needs.devices.size(), 2U);
    EXPECT_EQ(needs.devices[1].label, "Pedals");
    EXPECT_TRUE(missingDevices(needs, {kR3, kPedals}).empty());
    EXPECT_EQ(missingDevices(needs, {kR3}), std::vector<std::string>{"Pedals"});
    EXPECT_EQ(missingDevices(needs, {}), (std::vector<std::string>{"Moza R3", "Pedals"}));
}

TEST(StartNeeds, SourceDeviceFromTheConfig) {
    const Rig r = rig();
    EmulatorConfig config;
    config.sourceDeviceMatch = "346e";
    const StartNeeds needs = startNeeds(config, &r);
    ASSERT_EQ(needs.devices.size(), 1U);
    EXPECT_EQ(needs.devices[0].label, "Moza R3");   // the rig's name for it
    EXPECT_TRUE(missingDevices(needs, {kR3}).empty());
    // Without a rig it still works, named by the config's text.
    const StartNeeds bare = startNeeds(config, nullptr);
    EXPECT_TRUE(bare.problem.empty());
    EXPECT_EQ(missingDevices(bare, {}), std::vector<std::string>{"346e"});
}

TEST(StartNeeds, AnAccessoryThatIsTheWheelIsNotNeededTwice) {
    const Rig r = rig();
    EmulatorConfig config;
    config.mapper.accessories[0] = {0x346e, 0x0005};
    EXPECT_EQ(startNeeds(config, &r).devices.size(), 1U);
}

TEST(StartNeeds, AnUnnamedAccessoryShowsItsId) {
    const Rig r = rig();
    EmulatorConfig config;
    config.mapper.accessories[0] = {0x0eb7, 0x0e04};
    const StartNeeds needs = startNeeds(config, &r);
    ASSERT_EQ(needs.devices.size(), 2U);
    EXPECT_EQ(needs.devices[1].label, "0eb7:0e04");
}

// A config with nothing mapped: no axis, button or D-pad direction.
EmulatorConfig unmapped() {
    EmulatorConfig config;
    config.mapper.steering.sourceAxis = kUnmapped;
    config.mapper.buttonMap.fill(kUnmapped);
    return config;
}

TEST(StartNeeds, NoRigSaysNoRig) {
    EXPECT_EQ(startNeeds(EmulatorConfig{}, nullptr).problem, kNoRig);
}

TEST(StartNeeds, ARigWithNothingMappedSaysNoMapping) {
    const Rig r = rig();
    const StartNeeds needs = startNeeds(unmapped(), &r);
    EXPECT_EQ(needs.problem, kNoMapping);
    EXPECT_TRUE(needs.devices.empty());
    EXPECT_TRUE(startNeeds(EmulatorConfig{}, &r).problem.empty());   // the default mapping: steering from the wheel
}

TEST(StartNeeds, OneControlFromTheRigIsAMapping) {
    const Rig r = rig();
    EmulatorConfig config = unmapped();
    config.mapper.buttonMap[3] = 7;   // one button of the wheel
    EXPECT_TRUE(startNeeds(config, &r).problem.empty());
    config = unmapped();
    config.mapper.hatButtons.up = 4;   // one D-pad direction
    EXPECT_TRUE(startNeeds(config, &r).problem.empty());
    config = unmapped();
    config.mapper.accessories[0] = {0x30b7, 0x1001};   // the throttle from the rig's pedals
    config.mapper.throttle.sourceAxis = 1;
    config.mapper.throttle.sourceDevice = 1;
    EXPECT_TRUE(startNeeds(config, &r).problem.empty());
}

TEST(StartNeeds, ControlsFromDevicesOutsideTheRigAreNoMapping) {
    const Rig r = rig();
    EmulatorConfig config = unmapped();
    config.mapper.accessories[0] = {0x0eb7, 0x0e04};   // not in the rig
    config.mapper.brake.sourceAxis = 2;
    config.mapper.brake.sourceDevice = 1;
    EXPECT_EQ(startNeeds(config, &r).problem, kNoMapping);
    config = EmulatorConfig{};
    config.sourceDeviceMatch = "0eb7";   // a wheel the rig does not have, everything mapped from it
    EXPECT_EQ(startNeeds(config, &r).problem, kNoMapping);
}

TEST(StartNeeds, TheMappingMustBeForThePickedTarget) {
    EXPECT_EQ(targetProblem(withTarget("g920"), "g920"), "");
    EXPECT_EQ(targetProblem(withTarget("g25"), "g25"), "");
    EXPECT_EQ(targetProblem(withTarget("g920"), "g25"),
              "THE MAPPING IS FOR THE G920: Default, or target_profile = g25 in it");
    EXPECT_EQ(targetProblem(withTarget("g25"), "g920"),
              "THE MAPPING IS FOR THE G25: Default, or target_profile = g920 in it");
    EXPECT_EQ(targetProblem(withTarget("g29"), "g25"),
              "UNKNOWN TARGET 'g29' in the mapping: Default, or target_profile = g25");
    EXPECT_EQ(targetProblem(withTarget(""), "g920"),
              "UNKNOWN TARGET '' in the mapping: Default, or target_profile = g920");
    EXPECT_EQ(targetProblem(EmulatorConfig{}, "g920"), "");   // a config without the line: the G920
}

// Each kind of control on its own counts: the brake, the clutch, a button's secondary, each D-pad direction.
TEST(StartNeeds, EveryKindOfControlCountsOnItsOwn) {
    const Rig r = rig();
    EmulatorConfig config = unmapped();
    config.mapper.brake.sourceAxis = 5;
    EXPECT_TRUE(mappingUsesRig(config.mapper, r, true));
    config = unmapped();
    config.mapper.clutch.sourceAxis = 6;
    EXPECT_TRUE(mappingUsesRig(config.mapper, r, true));
    config = unmapped();
    config.mapper.buttonSecondary[4][1] = ButtonSource{0, 3, true, false, 0.5F};
    EXPECT_TRUE(mappingUsesRig(config.mapper, r, true));
    EXPECT_FALSE(mappingUsesRig(config.mapper, r, false));   // the wheel is not one of the rig's
    for (int direction = 1; direction < 4; ++direction) {
        config = unmapped();
        HatButtons& hat = config.mapper.hatButtons;
        (direction == 1 ? hat.right : direction == 2 ? hat.down : hat.left) = 9;
        EXPECT_TRUE(mappingUsesRig(config.mapper, r, true)) << direction;
    }
    EXPECT_FALSE(mappingUsesRig(unmapped().mapper, r, true));
}

// Device numbers that name no accessory (negative, past the slots, an empty slot): not from the rig.
TEST(StartNeeds, DeviceNumbersWithoutAnAccessoryAreNotTheRigs) {
    const Rig r = rig();
    for (const int device : {-1, 2, 4, 99}) {
        EmulatorConfig config = unmapped();
        config.mapper.accessories[0] = {0x30b7, 0x1001};
        config.mapper.throttle.sourceAxis = 1;
        config.mapper.throttle.sourceDevice = device;
        EXPECT_FALSE(mappingUsesRig(config.mapper, r, true)) << device;
    }
}

// A rig whose devices have no id (a hand-made Rig): no wheel to start with.
TEST(StartNeeds, ARigWheelWithoutAnIdIsNoWheel) {
    Rig r;
    r.devices.push_back(RigDevice{});
    EXPECT_EQ(startNeeds(EmulatorConfig{}, &r).problem, "NO WHEEL IN THE RIG (Rig tab)");
    const Rig empty;
    EXPECT_EQ(startNeeds(EmulatorConfig{}, &empty).problem, "NO WHEEL IN THE RIG (Rig tab)");
}

// Devices with no name yet are shown by their id; a source_device the rig has under no name keeps the config's
// text; a rig device without an id is never the config's wheel.
TEST(StartNeeds, UnnamedDevicesShowTheirIds) {
    Rig r;
    r.devices.push_back(RigDevice{});                         // no id: skipped when matching
    r.devices.push_back(device("", 0x346e, 0x0005, true));
    r.devices.push_back(device("", 0x30b7, 0x1001, false));
    EmulatorConfig config;
    config.mapper.accessories[0] = {0x30b7, 0x1001};
    config.mapper.throttle.sourceAxis = 1;
    config.mapper.throttle.sourceDevice = 1;
    StartNeeds needs = startNeeds(config, &r);
    ASSERT_TRUE(needs.problem.empty()) << needs.problem;
    ASSERT_EQ(needs.devices.size(), 2U);
    EXPECT_EQ(needs.devices[0].label, "346e:0005");
    EXPECT_EQ(needs.devices[1].label, "30b7:1001");
    config.sourceDeviceMatch = "346e";
    needs = startNeeds(config, &r);
    ASSERT_TRUE(needs.problem.empty()) << needs.problem;
    EXPECT_EQ(needs.devices[0].label, "346e");   // the rig has no name for it
    EXPECT_EQ(needs.devices[0].id, (DeviceId{0x346e, 0x0005}));
}

// No rig, a wheel named by the config: its accessories are needed by id (no names to show), and the wheel is found
// by its name when connected.
TEST(StartNeeds, WithoutARigAccessoriesAreNeededById) {
    EmulatorConfig config;
    config.sourceDeviceMatch = "R3";
    config.mapper.accessories[0] = {0x30b7, 0x1001};
    config.mapper.accessories[1] = {0x346e, 0x0005};   // the wheel's id: no rig to tell, so needed as an accessory
    const StartNeeds needs = startNeeds(config, nullptr);
    ASSERT_TRUE(needs.problem.empty()) << needs.problem;
    ASSERT_EQ(needs.devices.size(), 3U);
    EXPECT_EQ(needs.devices[1].label, "30b7:1001");
    EXPECT_EQ(needs.devices[2].label, "346e:0005");
    EXPECT_EQ(missingDevices(needs, {kR3}), (std::vector<std::string>{"30b7:1001"}));
    EXPECT_EQ(missingDevices(needs, {kPedals}), (std::vector<std::string>{"R3", "346e:0005"}));
}

// A device of the same maker but another product is not the one needed.
TEST(StartNeeds, AnotherProductOfTheSameMakerIsMissing) {
    StartNeeds needs;
    needs.devices.push_back(NeededDevice{"Pedals", "", {0x30b7, 0x1001}});
    EXPECT_EQ(missingDevices(needs, {PresentDevice{"Other", 0x30b7, 0x1002}}), (std::vector<std::string>{"Pedals"}));
    EXPECT_TRUE(missingDevices(needs, {PresentDevice{"Pedals", 0x30b7, 0x1001}}).empty());
}

}  // namespace
}  // namespace wheelbri
