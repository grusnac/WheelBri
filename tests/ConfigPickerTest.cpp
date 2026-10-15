// The Mapping tab's top row (service/src/ConfigPicker.cpp): the target and the way, which config Start uses, what is
// remembered in app.ini, and the clicks on its arrows. Drawn on a window that only records.

#include "ConfigPicker.hpp"

#include "wheelbri/ConfigLibrary.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace wheelbri::service {
namespace {

class RecordingWindow final : public ui::UiWindow {
public:
    std::vector<std::string> texts;

    ui::WindowInput pump() override { return {}; }
    [[nodiscard]] bool visible() const override { return true; }
    void begin(ui::Colour) override { texts.clear(); }
    void fill(float, float, float, float, ui::Colour) override {}
    void outline(float, float, float, float, ui::Colour) override {}
    void text(float, float, const std::string_view value, ui::Colour) override { texts.emplace_back(value); }
    void present() override {}
    bool openFolder(const std::filesystem::path&, std::string&) override { return false; }
    bool pickFolder(const std::string&, std::filesystem::path&, std::string&) override { return false; }
    [[nodiscard]] std::string describe() const override { return "recording"; }

    [[nodiscard]] bool shows(const std::string& text) const {
        return std::find(texts.begin(), texts.end(), text) != texts.end();
    }
};

// Where the row's arrows are when it is drawn at (4, 18): "Target" (64) < value (72) >, 16, "Run as" (56) < (80) >.
constexpr float kRowY = 22;
constexpr float kTargetLeft = 80;
constexpr float kTargetRight = 184;
constexpr float kWayLeft = 284;
constexpr float kWayRight = 396;

class ConfigPickerFiles : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("wheelbri-picker-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(builtIn());
        std::filesystem::create_directories(user());
        for (const char* name :
             {"g920-virtual-device.ini", "g920-in-game-shim.ini", "g25-virtual-device.ini", "g25-in-game-shim.ini"}) {
            write(builtIn() / name, "[emulator]\n");
        }
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    static void write(const std::filesystem::path& path, const std::string& text) {
        std::ofstream(path, std::ios::binary) << text;
    }
    [[nodiscard]] std::filesystem::path builtIn() const { return dir_ / "config"; }
    [[nodiscard]] std::filesystem::path user() const { return dir_ / "configs"; }
    [[nodiscard]] std::filesystem::path state() const { return dir_ / "app.ini"; }
    [[nodiscard]] ConfigPicker::Settings settings() const { return {builtIn(), user(), state()}; }
    [[nodiscard]] std::string setting(const std::string& key) const { return readAppSetting(state(), key); }

    RecordingWindow window_;
    std::filesystem::path dir_;
};

TEST(ConfigPickerNames, TargetsAndWays) {
    using Target = ConfigPicker::Target;
    using Way = ConfigPicker::Way;
    EXPECT_STREQ(ConfigPicker::targetName(Target::G920), "G920");
    EXPECT_STREQ(ConfigPicker::targetName(Target::G25), "G25");
    EXPECT_STREQ(ConfigPicker::profileName(Target::G920), "g920");
    EXPECT_STREQ(ConfigPicker::profileName(Target::G25), "g25");
    EXPECT_STREQ(ConfigPicker::wayName(Way::VirtualDevice), "Virtual");
    EXPECT_STREQ(ConfigPicker::wayName(Way::Shim), "Shim");
    EXPECT_EQ(ConfigPicker::builtInName(Target::G920, Way::VirtualDevice), "g920-virtual-device.ini");
    EXPECT_EQ(ConfigPicker::builtInName(Target::G920, Way::Shim), "g920-in-game-shim.ini");
    EXPECT_EQ(ConfigPicker::builtInName(Target::G25, Way::VirtualDevice), "g25-virtual-device.ini");
    EXPECT_EQ(ConfigPicker::builtInName(Target::G25, Way::Shim), "g25-in-game-shim.ini");
}

TEST(ConfigPickerNames, ParsesOnlyWhatTheAppWrites) {
    using Target = ConfigPicker::Target;
    using Way = ConfigPicker::Way;
    EXPECT_EQ(ConfigPicker::parseTarget("g920"), Target::G920);
    EXPECT_EQ(ConfigPicker::parseTarget("g25"), Target::G25);
    for (const char* other : {"", "G25", "g29", "g25 ", "g920;"}) {
        EXPECT_FALSE(ConfigPicker::parseTarget(other)) << other;
    }
    EXPECT_EQ(ConfigPicker::parseWay("virtual"), Way::VirtualDevice);
    EXPECT_EQ(ConfigPicker::parseWay("shim"), Way::Shim);
    for (const char* other : {"", "Shim", "uhid", "shim "}) EXPECT_FALSE(ConfigPicker::parseWay(other)) << other;
}

TEST_F(ConfigPickerFiles, WithNothingSavedItIsTheVirtualG920) {
    const ConfigPicker picker(window_, settings());
    EXPECT_EQ(picker.target(), ConfigPicker::Target::G920);
    EXPECT_EQ(picker.way(), ConfigPicker::Way::VirtualDevice);
    EXPECT_TRUE(picker.virtualDevice());
    EXPECT_EQ(picker.name(), "G920 virtual device");
    EXPECT_STREQ(picker.profileName(), "g920");
    EXPECT_TRUE(picker.builtIn());
    EXPECT_EQ(picker.selected(), (builtIn() / "g920-virtual-device.ini").string());
    EXPECT_EQ(picker.ownPath(), user() / "my-g920-virtual-device.ini");
    EXPECT_EQ(setting("config"), picker.selected());   // the command line service and the next start read it
}

TEST_F(ConfigPickerFiles, ReadsTheSavedTargetAndWay) {
    ASSERT_TRUE(writeAppSetting(state(), "target", "g25"));
    ASSERT_TRUE(writeAppSetting(state(), "way", "shim"));
    const ConfigPicker picker(window_, settings());
    EXPECT_EQ(picker.target(), ConfigPicker::Target::G25);
    EXPECT_EQ(picker.way(), ConfigPicker::Way::Shim);
    EXPECT_FALSE(picker.virtualDevice());
    EXPECT_EQ(picker.name(), "G25 in-game shim");
    EXPECT_EQ(picker.selected(), (builtIn() / "g25-in-game-shim.ini").string());
}

TEST_F(ConfigPickerFiles, UnknownSavedValuesAreTheVirtualG920) {
    ASSERT_TRUE(writeAppSetting(state(), "target", "g29"));
    ASSERT_TRUE(writeAppSetting(state(), "way", "bridge"));
    const ConfigPicker picker(window_, settings());
    EXPECT_EQ(picker.target(), ConfigPicker::Target::G920);
    EXPECT_EQ(picker.way(), ConfigPicker::Way::VirtualDevice);
    EXPECT_EQ(setting("target"), "g29");   // only read: left as it was until the user picks
}

TEST_F(ConfigPickerFiles, TheUsersOwnMappingWinsAndDefaultDropsIt) {
    ASSERT_TRUE(writeAppSetting(state(), "target", "g25"));
    write(user() / "my-g25-virtual-device.ini", "[emulator]\ntarget_profile = g25\n");
    ConfigPicker picker(window_, settings());
    EXPECT_FALSE(picker.builtIn());
    EXPECT_EQ(picker.selected(), (user() / "my-g25-virtual-device.ini").string());
    const std::uint64_t before = picker.changes();
    std::string error;
    ASSERT_TRUE(picker.dropOwnEdits(error)) << error;
    EXPECT_TRUE(picker.builtIn());
    EXPECT_FALSE(std::filesystem::exists(user() / "my-g25-virtual-device.ini"));
    EXPECT_EQ(picker.selected(), (builtIn() / "g25-virtual-device.ini").string());
    EXPECT_GT(picker.changes(), before);
    EXPECT_TRUE(picker.dropOwnEdits(error));   // nothing to drop: still fine
}

TEST_F(ConfigPickerFiles, DefaultSaysWhenTheOwnMappingCannotGo) {
    std::filesystem::create_directories(user() / "my-g920-virtual-device.ini" / "inside");   // a folder: not deletable
    ConfigPicker picker(window_, settings());
    std::string error;
    EXPECT_FALSE(picker.dropOwnEdits(error));
    EXPECT_NE(error.find("cannot delete"), std::string::npos);
}

TEST_F(ConfigPickerFiles, WithoutConfigsNothingIsSelected) {
    std::filesystem::remove_all(builtIn());
    const ConfigPicker picker(window_, settings());
    EXPECT_TRUE(picker.selected().empty());
    EXPECT_TRUE(picker.builtIn());
    EXPECT_TRUE(setting("config").empty());   // an empty selection is not written
}

TEST_F(ConfigPickerFiles, TheArrowsSwitchTargetAndWayAndRememberThem) {
    ConfigPicker picker(window_, settings());
    const float end = picker.draw(4, 18, true);
    EXPECT_FLOAT_EQ(end, 412);   // after the last arrow and its gap
    EXPECT_TRUE(window_.shows("G920"));
    EXPECT_TRUE(window_.shows("Virtual"));
    const std::uint64_t before = picker.changes();

    EXPECT_TRUE(picker.onClick(kTargetRight, kRowY));
    EXPECT_EQ(picker.target(), ConfigPicker::Target::G25);
    EXPECT_EQ(setting("target"), "g25");
    EXPECT_EQ(picker.selected(), (builtIn() / "g25-virtual-device.ini").string());
    EXPECT_GT(picker.changes(), before);
    EXPECT_FALSE(picker.onClick(kTargetRight, kRowY));   // the row is stale until it is drawn again

    window_.begin({});
    picker.draw(4, 18, true);
    EXPECT_TRUE(window_.shows("G25"));
    EXPECT_TRUE(picker.onClick(kWayLeft, kRowY));
    EXPECT_EQ(picker.way(), ConfigPicker::Way::Shim);
    EXPECT_EQ(setting("way"), "shim");
    EXPECT_EQ(picker.selected(), (builtIn() / "g25-in-game-shim.ini").string());

    picker.draw(4, 18, true);
    EXPECT_TRUE(picker.onClick(kTargetLeft, kRowY));   // either arrow goes to the other target
    EXPECT_EQ(picker.target(), ConfigPicker::Target::G920);
    picker.draw(4, 18, true);
    EXPECT_TRUE(picker.onClick(kWayRight, kRowY));
    EXPECT_EQ(picker.way(), ConfigPicker::Way::VirtualDevice);
    EXPECT_EQ(setting("way"), "virtual");

    // Remembered: a new picker (the next start) comes back the same.
    ASSERT_TRUE(writeAppSetting(state(), "target", "g25"));
    const ConfigPicker again(window_, settings());
    EXPECT_EQ(again.target(), ConfigPicker::Target::G25);
}

TEST_F(ConfigPickerFiles, AStateFileThatCannotBeWrittenKeepsTheChoiceForNow) {
    std::filesystem::create_directories(state() / "inside");   // a folder where app.ini goes: never written
    ConfigPicker picker(window_, settings());
    picker.draw(4, 18, true);
    EXPECT_TRUE(picker.onClick(kTargetRight, kRowY));
    EXPECT_EQ(picker.target(), ConfigPicker::Target::G25);   // chosen, only not remembered
    EXPECT_EQ(picker.selected(), (builtIn() / "g25-virtual-device.ini").string());
    picker.draw(4, 18, true);
    EXPECT_TRUE(picker.onClick(kWayRight, kRowY));
    EXPECT_EQ(picker.way(), ConfigPicker::Way::Shim);
    EXPECT_TRUE(std::filesystem::is_directory(state() / "inside"));
}

TEST_F(ConfigPickerFiles, WhileASessionRunsTheArrowsAreOff) {
    ConfigPicker picker(window_, settings());
    picker.draw(4, 18, false);
    EXPECT_FALSE(picker.onClick(kTargetRight, kRowY));
    EXPECT_FALSE(picker.onClick(kWayRight, kRowY));
    EXPECT_EQ(picker.target(), ConfigPicker::Target::G920);
    EXPECT_EQ(picker.way(), ConfigPicker::Way::VirtualDevice);
}

TEST(ConfigPickerNames, TheVirtualDeviceNeedsWindows11) {
    EXPECT_FALSE(ConfigPicker::virtualDeviceSupported(7601));    // Windows 7 SP1
    EXPECT_FALSE(ConfigPicker::virtualDeviceSupported(9600));    // Windows 8.1
    EXPECT_FALSE(ConfigPicker::virtualDeviceSupported(19045));   // Windows 10 22H2
    EXPECT_FALSE(ConfigPicker::virtualDeviceSupported(21999));
    EXPECT_TRUE(ConfigPicker::virtualDeviceSupported(22000));    // Windows 11
    EXPECT_TRUE(ConfigPicker::virtualDeviceSupported(26100));
    EXPECT_TRUE(ConfigPicker::virtualDeviceSupported(0));        // not Windows (Linux: uhid), or unknown
}

TEST_F(ConfigPickerFiles, WithoutTheVirtualDeviceItIsShimOnly) {
    ConfigPicker::Settings shimOnly = settings();
    shimOnly.virtualAvailable = false;
    ConfigPicker picker(window_, shimOnly);   // nothing saved: Shim, not the virtual G920
    EXPECT_EQ(picker.way(), ConfigPicker::Way::Shim);
    EXPECT_EQ(picker.selected(), (builtIn() / "g920-in-game-shim.ini").string());

    // Both Run as arrows are off; the target still changes.
    picker.draw(4, 18, true);
    EXPECT_TRUE(window_.shows("Shim"));
    EXPECT_FALSE(picker.onClick(kWayLeft, kRowY));
    EXPECT_FALSE(picker.onClick(kWayRight, kRowY));
    EXPECT_EQ(picker.way(), ConfigPicker::Way::Shim);
    EXPECT_TRUE(picker.onClick(kTargetRight, kRowY));
    EXPECT_EQ(picker.target(), ConfigPicker::Target::G25);
    EXPECT_EQ(picker.selected(), (builtIn() / "g25-in-game-shim.ini").string());
    EXPECT_TRUE(setting("way").empty());   // nothing written for the way
}

TEST_F(ConfigPickerFiles, ASavedVirtualIsShimWhereItCannotRunAndStaysSaved) {
    ASSERT_TRUE(writeAppSetting(state(), "way", "virtual"));
    ConfigPicker::Settings shimOnly = settings();
    shimOnly.virtualAvailable = false;
    const ConfigPicker picker(window_, shimOnly);
    EXPECT_EQ(picker.way(), ConfigPicker::Way::Shim);
    EXPECT_EQ(picker.name(), "G920 in-game shim");
    EXPECT_EQ(setting("way"), "virtual");   // for the same app.ini on Windows 11 later
    const ConfigPicker onWindows11(window_, settings());
    EXPECT_EQ(onWindows11.way(), ConfigPicker::Way::VirtualDevice);
}

TEST_F(ConfigPickerFiles, AClickBesideTheArrowsIsNotTheRows) {
    ConfigPicker picker(window_, settings());
    picker.draw(4, 18, true);
    EXPECT_FALSE(picker.onClick(130, kRowY));   // on the value, between the arrows
    EXPECT_FALSE(picker.onClick(kTargetRight, 60));
    EXPECT_FALSE(picker.onClick(500, kRowY));
    EXPECT_EQ(picker.target(), ConfigPicker::Target::G920);
}

}  // namespace
}  // namespace wheelbri::service
