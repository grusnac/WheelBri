#include "wheelbri/DeviceLabels.hpp"

#include <gtest/gtest.h>

using namespace wheelbri;

namespace {

DeviceLabelsResult load(const std::string& text) {
    const auto parsed = IniDocument::parse(text);
    EXPECT_TRUE(parsed.errors.empty());
    return loadDeviceLabels(parsed.document);
}

}  // namespace

TEST(DeviceLabels, NamesFilesAndIds) {
    EXPECT_EQ(deviceLabelsFileName(0x346E, 0x0005), "346e-0005.ini");
    EXPECT_EQ(deviceId(0x346E, 0x0005), "346e:0005");
}

TEST(DeviceLabels, ParsesKinds) {
    for (const DeviceKind kind : kDeviceKinds) EXPECT_EQ(parseDeviceKind(deviceKindName(kind)), kind);
    EXPECT_EQ(parseDeviceKind("Button Box"), DeviceKind::ButtonBox);
    EXPECT_FALSE(parseDeviceKind("joystick").has_value());
}

TEST(DeviceLabels, WritesAndReadsBack) {
    DeviceLabels labels;
    labels.name = "My pedals";
    labels.vendorId = 0x044F;
    labels.productId = 0xB696;
    labels.kind = DeviceKind::Pedals;
    labels.axes[2] = "Gas";
    labels.buttons[13] = "R paddle";
    const std::string text = writeDeviceLabels("", labels);
    const auto loaded = load(text);
    EXPECT_TRUE(loaded.errors.empty());
    EXPECT_EQ(loaded.labels, labels);
    // Saving the same labels again changes nothing.
    EXPECT_EQ(writeDeviceLabels(text, labels), text);
}

TEST(DeviceLabels, KeepsCommentsAndDropsClearedLabels) {
    DeviceLabels labels;
    labels.buttons[3] = "Y";
    std::string text = writeDeviceLabels("; my notes\n", labels);
    labels.buttons[3].clear();
    text = writeDeviceLabels(text, labels);
    EXPECT_NE(text.find("; my notes"), std::string::npos);
    EXPECT_EQ(text.find("button_3"), std::string::npos);
}

TEST(DeviceLabels, ReportsBadValues) {
    const auto loaded = load("[device]\nid = nope\nkind = joystick\n[button_labels]\nbutton_64 = x\nbutton_1 = Much too long a name\n");
    EXPECT_EQ(loaded.errors.size(), 4U);
    EXPECT_EQ(loaded.labels.buttons[1], "Much too lon");
    EXPECT_EQ(loaded.labels.kind, DeviceKind::Other);
}

TEST(DeviceLabels, AppliesToTheMapping) {
    DeviceLabels labels;
    labels.axes[0] = "Wheel";
    labels.buttons[35] = "Start";
    MapperConfig mapper;
    mapper.sourceButtonLabels[1] = "old";
    applyDeviceLabels(labels, mapper);
    EXPECT_EQ(mapper.sourceAxisLabels[0], "Wheel");
    EXPECT_EQ(mapper.sourceButtonLabels[35], "Start");
    EXPECT_TRUE(mapper.sourceButtonLabels[1].empty());   // the device file is the whole truth
}

TEST(DeviceLabels, IdsAreReadStrictly) {
    EXPECT_EQ(load("[device]\nid = 046d:c262\n").labels.productId, 0xC262);
    EXPECT_EQ(load("[device]\nid = -1:0005\n").errors.size(), 1U);
    EXPECT_EQ(load("[device]\nid = 0x46:c262\n").errors.size(), 1U);
    EXPECT_EQ(load("[device]\nid = 046d:c262x\n").errors.size(), 1U);
}

TEST(DeviceLabels, ANameWithACommentCharacterSurvivesSaving) {
    DeviceLabels labels;
    labels.name = "Shifter #1";
    labels.vendorId = 0x046D;
    labels.productId = 0xC262;
    EXPECT_EQ(load(writeDeviceLabels("", labels)).labels.name, "Shifter  1");
}
