#include "wheelbri/DeviceLabels.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

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

// A whole file as the older Wheel setup tab wrote it: every part read back.
TEST(DeviceLabels, ReadsAWholeFile) {
    const auto loaded = load("; WheelBri device\n[device]\nname = My pedals\nid = 044f:b696\nkind = pedals\n"
                             "[axis_labels]\naxis_2 = Gas\n[button_labels]\nbutton_13 = R paddle\n");
    EXPECT_TRUE(loaded.errors.empty());
    DeviceLabels labels;
    labels.name = "My pedals";
    labels.vendorId = 0x044F;
    labels.productId = 0xB696;
    labels.kind = DeviceKind::Pedals;
    labels.axes[2] = "Gas";
    labels.buttons[13] = "R paddle";
    EXPECT_EQ(loaded.labels, labels);
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

// A hand-edited file with a typo in a key or a section name: reported, not silently dropped.
TEST(DeviceLabels, ReportsUnknownKeysAndSections) {
    const auto loaded = load("stray = 1\n[device]\nname = Pedals\nnmae = typo\n[devcie]\nid = 046d:c262\n"
                             "[axis_labels]\naxis_9 = x\n");
    ASSERT_EQ(loaded.errors.size(), 4U);
    EXPECT_EQ(loaded.errors[0], "[axis_labels] unknown key 'axis_9'");   // kMaxAxes is 8
    EXPECT_EQ(loaded.errors[1], "keys found before the first [section]");
    EXPECT_EQ(loaded.errors[2], "unknown section [devcie]");
    EXPECT_EQ(loaded.errors[3], "[device] unknown key 'nmae'");
    EXPECT_EQ(loaded.labels.name, "Pedals");   // what could be read is kept
    EXPECT_EQ(loaded.labels.vendorId, 0);      // the id was in the wrong section
}

// An axis name typed too long by hand is cut (at a character, not inside a UTF-8 sequence) and reported.
TEST(DeviceLabels, CutsTooLongAxisNames) {
    const auto loaded = load("[axis_labels]\naxis_1 = Embrayage gauche\naxis_2 = \xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
                             "\xC3\xA9\xC3\xA9\xC3\xA9\n");
    EXPECT_EQ(loaded.errors.size(), 2U);
    EXPECT_EQ(loaded.labels.axes[1], "Embrayage ga");
    EXPECT_EQ(loaded.labels.axes[2], "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9");   // 12 bytes, 6 characters
}

// An empty file and empty values: no names, no errors, the kind stays other.
TEST(DeviceLabels, EmptyFileAndEmptyValues) {
    EXPECT_TRUE(load("").errors.empty());
    const auto loaded = load("[device]\r\nname =\r\n[button_labels]\r\nbutton_0 =\r\n");
    EXPECT_TRUE(loaded.errors.empty());
    EXPECT_EQ(loaded.labels, DeviceLabels{});
}

namespace {

class DeviceLabelsFolder : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string test = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        dir_ = std::filesystem::temp_directory_path() / ("wheelbri-DeviceLabels-" + test);
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    void write(const std::filesystem::path& name, const std::string& text) const {
        std::ofstream(dir_ / name, std::ios::binary | std::ios::trunc) << text;
    }

    std::filesystem::path dir_;
};

// A device file as the older Wheel setup tab wrote it.
std::string named(const std::string& name, const std::uint16_t vendor, const std::uint16_t product) {
    return "[device]\nname = " + name + "\nid = " + deviceId(vendor, product) + "\n";
}

}  // namespace

// The Mapping tab's sources: only .ini files with an id, sorted by name (any case), then by id.
TEST_F(DeviceLabelsFolder, ListsDevicesWithAnIdSortedByName) {
    write("346e-0005.ini", named("wheel", 0x346E, 0x0005));
    write("044f-b696.ini", named("Pedals", 0x044F, 0xB696));
    write("1234-0002.ini", named("pedals", 0x1234, 0x0002));
    write("1234-0001.ini", named("Pedals", 0x1234, 0x0001));
    write("noid.ini", "[device]\nname = no id\n");                    // no id: not offered
    write("zero.ini", "[device]\nname = zero\nid = 0000:0000\n");     // the empty id: not offered
    write("half.ini", "[device]\nname = half\nid = 0000:0001\n");     // vendor 0 alone is still an id
    write("notes.txt", named("txt", 0x1111, 0x2222));   // not an .ini
    std::filesystem::create_directories(dir_ / "folder.ini");                  // a folder, not a file
    write(std::filesystem::path(u8"pédales-ü.ini"), named("Ume", 0x0EB7, 0x0001));
    const std::vector<DeviceLabels> found = listDeviceLabels(dir_);
    ASSERT_EQ(found.size(), 6U);
    EXPECT_EQ(found[0].name, "half");
    EXPECT_EQ(found[1].vendorId, 0x044F);   // "Pedals" 044f:b696
    EXPECT_EQ(found[2].productId, 0x0001);  // "Pedals" 1234:0001
    EXPECT_EQ(found[3].productId, 0x0002);  // "pedals" 1234:0002: the same name in another case, sorted by id
    EXPECT_EQ(found[4].name, "Ume");        // a non-ASCII file name is read too
    EXPECT_EQ(found[5].name, "wheel");
}

// No devices folder yet (first start), or a file where the folder should be: nothing offered, nothing thrown.
TEST_F(DeviceLabelsFolder, AMissingFolderListsNothing) {
    EXPECT_TRUE(listDeviceLabels(dir_ / "missing").empty());
    write("plain", "x");
    EXPECT_TRUE(listDeviceLabels(dir_ / "plain").empty());
    EXPECT_TRUE(listDeviceLabels({}).empty());
}

// A device file that cannot be read (no permission) is left out; the others are still offered.
TEST_F(DeviceLabelsFolder, AnUnreadableFileIsLeftOut) {
    write("346e-0005.ini", named("wheel", 0x346E, 0x0005));
    write("044f-b696.ini", named("Pedals", 0x044F, 0xB696));
    std::filesystem::permissions(dir_ / "044f-b696.ini", std::filesystem::perms::none);
    const bool readable = std::ifstream(dir_ / "044f-b696.ini").good();
    const std::vector<DeviceLabels> found = listDeviceLabels(dir_);
    std::filesystem::permissions(dir_ / "044f-b696.ini", std::filesystem::perms::owner_all);
    if (readable) GTEST_SKIP() << "the file stays readable here";
    ASSERT_EQ(found.size(), 1U);
    EXPECT_EQ(found[0].name, "wheel");
}

// A kind outside the enum (a corrupt value cast in) is named "other", never undefined.
TEST(DeviceLabels, AnUnknownKindIsOther) {
    EXPECT_STREQ(deviceKindName(static_cast<DeviceKind>(99)), "other");
}
