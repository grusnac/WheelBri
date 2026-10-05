#include "wheelbri/StartNeeds.hpp"

#include <gtest/gtest.h>

namespace wheelbri {
namespace {

RigDevice device(const std::string& name, const std::uint16_t vid, const std::uint16_t pid, const bool steering) {
    RigDevice d;
    d.name = name;
    d.id = {vid, pid};
    if (steering) d.axes[0].role = ControlRole::Steering;
    return d;
}

Rig rig() {
    Rig r;
    r.devices.push_back(device("Pedals", 0x30b7, 0x1001, false));
    r.devices.push_back(device("Moza R3", 0x346e, 0x0005, true));
    r.devices.push_back(device("Handbrake", 0x1234, 0x0002, false));
    return r;
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

}  // namespace
}  // namespace wheelbri
