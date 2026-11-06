// SPDX-License-Identifier: GPL-3.0-or-later
// The Mapping tab's top row (service/src/ConfigPicker.cpp): the target and the way, which config Start uses, what is
// remembered in app.ini, and the clicks on its buttons. Drawn on a window that only records.

#include "ConfigPicker.hpp"

#include "wheelbri/ConfigLibrary.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
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

// Where the row's buttons are when it is drawn at (4, 18): "Target" (64), G25 G27 G920 G923 (52 each, edges shared),
// 16, "Mode" (48), Virtual Shim (72 each); a point inside each.
constexpr float kRowY = 22;
constexpr float kG25X = 88;
constexpr float kG27X = 139;
constexpr float kG920X = 190;
constexpr float kG923X = 241;
constexpr float kVirtualX = 357;
constexpr float kShimX = 428;
// For the G25 and G27, a second row 18 lower: "Order" (64), Logitech driver and Native (128 each).
constexpr float kOrderRowY = kRowY + 18;
constexpr float kDriverOrderX = 130;
constexpr float kNativeOrderX = 260;

class ConfigPickerFiles : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("wheelbri-picker-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(builtIn());
        std::filesystem::create_directories(user());
        for (const char* name : {"g920.ini", "g25.ini", "g27.ini", "g923.ini"}) {
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
    // The file's first line, the file closed again (Windows cannot delete a file that is open).
    static std::string firstLine(const std::filesystem::path& path) {
        std::ifstream in(path);
        std::string line;
        std::getline(in, line);
        return line;
    }

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
    EXPECT_STREQ(ConfigPicker::targetName(Target::G27), "G27");
    EXPECT_STREQ(ConfigPicker::profileName(Target::G27), "g27");
    EXPECT_STREQ(ConfigPicker::targetName(Target::G923), "G923");
    EXPECT_STREQ(ConfigPicker::profileName(Target::G923), "g923");
    EXPECT_EQ(ConfigPicker::builtInName(Target::G923), "g923.ini");
    EXPECT_STREQ(ConfigPicker::wayName(Way::VirtualDevice), "Virtual");
    EXPECT_STREQ(ConfigPicker::wayName(Way::Shim), "Shim");
    EXPECT_EQ(ConfigPicker::builtInName(Target::G920), "g920.ini");   // one for both modes
    EXPECT_EQ(ConfigPicker::builtInName(Target::G25), "g25.ini");
    EXPECT_EQ(ConfigPicker::builtInName(Target::G27), "g27.ini");
    EXPECT_EQ(ConfigPicker::olderOwnNames(Target::G25)[0], "my-g25-virtual-device.ini");
    EXPECT_EQ(ConfigPicker::olderOwnNames(Target::G25)[1], "my-g25-in-game-shim.ini");
}

TEST(ConfigPickerNames, ParsesOnlyWhatTheAppWrites) {
    using Target = ConfigPicker::Target;
    using Way = ConfigPicker::Way;
    EXPECT_EQ(ConfigPicker::parseTarget("g920"), Target::G920);
    EXPECT_EQ(ConfigPicker::parseTarget("g25"), Target::G25);
    EXPECT_EQ(ConfigPicker::parseTarget("g27"), Target::G27);
    EXPECT_EQ(ConfigPicker::parseTarget("g923"), Target::G923);
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
    EXPECT_EQ(picker.name(), "G920 Mode Virtual");
    EXPECT_STREQ(picker.profileName(), "g920");
    EXPECT_TRUE(picker.builtIn());
    EXPECT_EQ(picker.selected(), (builtIn() / "g920.ini").string());
    EXPECT_EQ(picker.ownPath(), user() / "my-g920.ini");
    EXPECT_EQ(setting("config"), picker.selected());   // the command line service and the next start read it
}

TEST_F(ConfigPickerFiles, ReadsTheSavedTargetAndWay) {
    ASSERT_TRUE(writeAppSetting(state(), "target", "g25"));
    ASSERT_TRUE(writeAppSetting(state(), "way", "shim"));
    const ConfigPicker picker(window_, settings());
    EXPECT_EQ(picker.target(), ConfigPicker::Target::G25);
    EXPECT_EQ(picker.way(), ConfigPicker::Way::Shim);
    EXPECT_FALSE(picker.virtualDevice());
    EXPECT_EQ(picker.name(), "G25 Mode Shim");
    EXPECT_EQ(picker.selected(), (builtIn() / "g25.ini").string());
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
    write(user() / "my-g25.ini", "[emulator]\ntarget_profile = g25\n");
    ConfigPicker picker(window_, settings());
    EXPECT_FALSE(picker.builtIn());
    EXPECT_EQ(picker.selected(), (user() / "my-g25.ini").string());
    const std::uint64_t before = picker.changes();
    std::string error;
    ASSERT_TRUE(picker.dropOwnEdits(error)) << error;
    EXPECT_TRUE(picker.builtIn());
    EXPECT_FALSE(std::filesystem::exists(user() / "my-g25.ini"));
    EXPECT_EQ(picker.selected(), (builtIn() / "g25.ini").string());
    EXPECT_GT(picker.changes(), before);
    EXPECT_TRUE(picker.dropOwnEdits(error));   // nothing to drop: still fine
}

TEST_F(ConfigPickerFiles, DefaultSaysWhenTheOwnMappingCannotGo) {
    std::filesystem::create_directories(user() / "my-g920.ini" / "inside");   // a folder: not deletable
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

// Every Target and Mode is a button: one click picks it, the chosen one is filled (not clickable), and it is
// remembered; each says what it is in its hover text.
TEST_F(ConfigPickerFiles, TheButtonsSwitchTargetAndWayAndRememberThem) {
    ConfigPicker picker(window_, settings());
    std::vector<std::string> tips;
    const float end = picker.draw(4, 18, true, [&tips](float, float, float, float, std::string text) {
        tips.push_back(std::move(text));
    });
    EXPECT_FLOAT_EQ(end, 484);   // after the last button and its gap
    for (const char* name : {"G25", "G27", "G920", "G923", "Virtual", "Shim"}) EXPECT_TRUE(window_.shows(name)) << name;
    ASSERT_EQ(tips.size(), 6U);
    EXPECT_NE(tips[0].find("G25: for older games"), std::string::npos);
    EXPECT_NE(tips[4].find("every game sees"), std::string::npos);
    const std::uint64_t before = picker.changes();
    EXPECT_FALSE(picker.onClick(kG920X, kRowY));   // the chosen one: nothing to do

    EXPECT_TRUE(picker.onClick(kG923X, kRowY));
    EXPECT_EQ(picker.target(), ConfigPicker::Target::G923);
    EXPECT_EQ(setting("target"), "g923");
    EXPECT_EQ(picker.selected(), (builtIn() / "g923.ini").string());
    EXPECT_GT(picker.changes(), before);
    EXPECT_FALSE(picker.onClick(kG25X, kRowY));   // the row is stale until it is drawn again

    window_.begin({});
    picker.draw(4, 18, true);
    EXPECT_TRUE(picker.onClick(kShimX, kRowY));
    EXPECT_EQ(picker.way(), ConfigPicker::Way::Shim);
    EXPECT_EQ(setting("way"), "shim");
    EXPECT_EQ(picker.selected(), (builtIn() / "g923.ini").string());

    picker.draw(4, 18, true);
    EXPECT_TRUE(picker.onClick(kG25X, kRowY));
    EXPECT_EQ(picker.target(), ConfigPicker::Target::G25);
    EXPECT_EQ(setting("target"), "g25");
    EXPECT_EQ(picker.selected(), (builtIn() / "g25.ini").string());
    picker.draw(4, 18, true);
    EXPECT_TRUE(picker.onClick(kG27X, kRowY));
    EXPECT_EQ(picker.target(), ConfigPicker::Target::G27);
    EXPECT_EQ(picker.selected(), (builtIn() / "g27.ini").string());
    picker.draw(4, 18, true);
    EXPECT_TRUE(picker.onClick(kG920X, kRowY));
    EXPECT_EQ(picker.target(), ConfigPicker::Target::G920);
    EXPECT_EQ(picker.selected(), (builtIn() / "g920.ini").string());
    picker.draw(4, 18, true);
    EXPECT_TRUE(picker.onClick(kVirtualX, kRowY));
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
    EXPECT_TRUE(picker.onClick(kG923X, kRowY));
    EXPECT_EQ(picker.target(), ConfigPicker::Target::G923);   // chosen, only not remembered
    EXPECT_EQ(picker.selected(), (builtIn() / "g923.ini").string());
    picker.draw(4, 18, true);
    EXPECT_TRUE(picker.onClick(kShimX, kRowY));
    EXPECT_EQ(picker.way(), ConfigPicker::Way::Shim);
    EXPECT_TRUE(std::filesystem::is_directory(state() / "inside"));
}

TEST_F(ConfigPickerFiles, WhileASessionRunsTheButtonsAreOff) {
    ConfigPicker picker(window_, settings());
    picker.draw(4, 18, false);
    EXPECT_FALSE(picker.onClick(kG923X, kRowY));
    EXPECT_FALSE(picker.onClick(kShimX, kRowY));
    EXPECT_EQ(picker.target(), ConfigPicker::Target::G920);
    EXPECT_EQ(picker.way(), ConfigPicker::Way::VirtualDevice);
}

// From Start until Stop (the app locks it): the buttons of a draw from before Start do nothing, a draw while locked has
// none even when asked for them, and choosing does nothing either. After Stop they work again.
TEST_F(ConfigPickerFiles, LockedFromStartToStopNothingChanges) {
    ConfigPicker picker(window_, settings());
    picker.draw(4, 18, true);   // drawn before Start: buttons on
    picker.setLocked(true);
    EXPECT_TRUE(picker.locked());
    EXPECT_FALSE(picker.onClick(kG923X, kRowY));
    picker.draw(4, 18, true);
    EXPECT_FALSE(picker.onClick(kG923X, kRowY));
    EXPECT_FALSE(picker.onClick(kShimX, kRowY));
    EXPECT_EQ(picker.target(), ConfigPicker::Target::G920);
    EXPECT_EQ(picker.way(), ConfigPicker::Way::VirtualDevice);
    EXPECT_TRUE(setting("target").empty());

    picker.setLocked(false);   // Stop
    picker.draw(4, 18, true);
    EXPECT_TRUE(picker.onClick(kG923X, kRowY));
    EXPECT_EQ(picker.target(), ConfigPicker::Target::G923);
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

TEST(ConfigPickerNames, WindowsByItsBuild) {
    EXPECT_EQ(ConfigPicker::windowsName(0), "");
    EXPECT_EQ(ConfigPicker::windowsName(7601), "Windows 7");
    EXPECT_EQ(ConfigPicker::windowsName(9200), "Windows 8");
    EXPECT_EQ(ConfigPicker::windowsName(9600), "Windows 8.1");
    EXPECT_EQ(ConfigPicker::windowsName(10240), "Windows 10");
    EXPECT_EQ(ConfigPicker::windowsName(19045), "Windows 10");
    EXPECT_EQ(ConfigPicker::windowsName(22000), "Windows 11");
    EXPECT_EQ(ConfigPicker::windowsName(26100), "Windows 11");
}

TEST_F(ConfigPickerFiles, WithoutTheVirtualDeviceItIsShimOnly) {
    ConfigPicker::Settings shimOnly = settings();
    shimOnly.virtualAvailable = false;
    ConfigPicker picker(window_, shimOnly);   // nothing saved: Shim, not the virtual G920
    EXPECT_EQ(picker.way(), ConfigPicker::Way::Shim);
    EXPECT_EQ(picker.selected(), (builtIn() / "g920.ini").string());

    // No Virtual button: Shim alone, locked, its hover text says why; the target still changes.
    std::vector<std::string> tips;
    picker.draw(4, 18, true,
                [&tips](float, float, float, float, std::string text) { tips.push_back(std::move(text)); });
    EXPECT_TRUE(window_.shows("Shim"));
    EXPECT_FALSE(window_.shows("Virtual"));
    ASSERT_EQ(tips.size(), 5U);
    EXPECT_EQ(tips[4], "On this Windows, only Mode Shim is available: Mode Virtual's driver needs Windows 11.");
    EXPECT_FALSE(picker.onClick(kVirtualX, kRowY));
    EXPECT_FALSE(picker.onClick(kShimX, kRowY));
    EXPECT_EQ(picker.way(), ConfigPicker::Way::Shim);
    EXPECT_TRUE(picker.onClick(kG923X, kRowY));
    EXPECT_EQ(picker.target(), ConfigPicker::Target::G923);
    EXPECT_EQ(picker.selected(), (builtIn() / "g923.ini").string());
    EXPECT_TRUE(setting("way").empty());   // nothing written for the way
}

TEST_F(ConfigPickerFiles, TheShimOnlyHoverTextNamesThisWindows) {
    ConfigPicker::Settings onWindows7 = settings();
    onWindows7.virtualAvailable = false;
    onWindows7.windowsName = ConfigPicker::windowsName(7601);
    ConfigPicker picker(window_, onWindows7);
    std::vector<std::string> tips;
    picker.draw(4, 18, false,
                [&tips](float, float, float, float, std::string text) { tips.push_back(std::move(text)); });
    ASSERT_EQ(tips.size(), 5U);
    EXPECT_EQ(tips[4], "On Windows 7, only Mode Shim is available: Mode Virtual's driver needs Windows 11. "
                       "Fixed while WheelBri runs.");
}

TEST_F(ConfigPickerFiles, ASavedVirtualIsShimWhereItCannotRunAndStaysSaved) {
    ASSERT_TRUE(writeAppSetting(state(), "way", "virtual"));
    ConfigPicker::Settings shimOnly = settings();
    shimOnly.virtualAvailable = false;
    const ConfigPicker picker(window_, shimOnly);
    EXPECT_EQ(picker.way(), ConfigPicker::Way::Shim);
    EXPECT_EQ(picker.name(), "G920 Mode Shim");
    EXPECT_EQ(setting("way"), "virtual");   // for the same app.ini on Windows 11 later
    const ConfigPicker onWindows11(window_, settings());
    EXPECT_EQ(onWindows11.way(), ConfigPicker::Way::VirtualDevice);
}

TEST_F(ConfigPickerFiles, AClickBesideTheButtonsIsNotTheRows) {
    ConfigPicker picker(window_, settings());
    picker.draw(4, 18, true);
    EXPECT_FALSE(picker.onClick(30, kRowY));    // on the "Target" label
    EXPECT_FALSE(picker.onClick(kG923X, 60));
    EXPECT_FALSE(picker.onClick(500, kRowY));
    EXPECT_EQ(picker.target(), ConfigPicker::Target::G920);
}

}  // namespace
// One mapping for both modes: switching Mode keeps the same file, the user's own included.
TEST_F(ConfigPickerFiles, BothModesUseTheSameMapping) {
    write(user() / "my-g920.ini", "[emulator]\n");
    ConfigPicker picker(window_, settings());
    const std::string virtualOne = picker.selected();
    EXPECT_EQ(virtualOne, (user() / "my-g920.ini").string());
    picker.draw(4, 18, true);
    EXPECT_TRUE(picker.onClick(kShimX, kRowY));
    EXPECT_EQ(picker.way(), ConfigPicker::Way::Shim);
    EXPECT_EQ(picker.selected(), virtualOne);
}

// Before 2026-10-15 each mode had its own mapping: the newer becomes the one for both, once; the older files stay.
TEST_F(ConfigPickerFiles, TheNewerOlderMappingBecomesTheOneForBothModes) {
    write(user() / "my-g920-virtual-device.ini", "virtual\n");
    write(user() / "my-g920-in-game-shim.ini", "shim\n");
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(user() / "my-g920-virtual-device.ini", now - std::chrono::hours(2));
    std::filesystem::last_write_time(user() / "my-g920-in-game-shim.ini", now - std::chrono::hours(1));
    ConfigPicker picker(window_, settings());
    EXPECT_FALSE(picker.builtIn());
    EXPECT_EQ(picker.selected(), (user() / "my-g920.ini").string());
    EXPECT_EQ(firstLine(user() / "my-g920.ini"), "shim");   // the newer
    EXPECT_TRUE(std::filesystem::exists(user() / "my-g920-virtual-device.ini"));   // never deleted
    EXPECT_TRUE(std::filesystem::exists(user() / "my-g920-in-game-shim.ini"));

    // Default drops my-g920.ini, and the older files do not bring it back.
    std::string error;
    ASSERT_TRUE(picker.dropOwnEdits(error)) << error;
    EXPECT_TRUE(picker.builtIn());
    const ConfigPicker again(window_, settings());
    EXPECT_TRUE(again.builtIn());
    EXPECT_FALSE(std::filesystem::exists(user() / "my-g920.ini"));
}

// An own mapping for both modes already there: the older files are not read.
TEST_F(ConfigPickerFiles, AnOwnMappingForBothModesIsKept) {
    write(user() / "my-g25.ini", "mine\n");
    write(user() / "my-g25-in-game-shim.ini", "older\n");
    ASSERT_TRUE(writeAppSetting(state(), "target", "g25"));
    const ConfigPicker picker(window_, settings());
    EXPECT_EQ(firstLine(user() / "my-g25.ini"), "mine");
}


// Two older per-mode mappings: the newer one becomes the one for both modes, whichever of the two it is.
TEST_F(ConfigPickerFiles, TheNewerOfTwoOlderMappingsInEitherOrder) {
    for (const bool virtualNewer : {true, false}) {
        std::filesystem::remove_all(user());
        std::filesystem::create_directories(user());
        std::filesystem::remove(state());
        const auto names = ConfigPicker::olderOwnNames(ConfigPicker::Target::G920);
        write(user() / names[0], "; virtual\n");
        write(user() / names[1], "; shim\n");
        const auto now = std::filesystem::file_time_type::clock::now();
        std::filesystem::last_write_time(user() / names[0], now - std::chrono::hours(virtualNewer ? 1 : 2));
        std::filesystem::last_write_time(user() / names[1], now - std::chrono::hours(virtualNewer ? 2 : 1));
        ConfigPicker picker(window_, settings());
        EXPECT_EQ(firstLine(picker.ownPath()), virtualNewer ? "; virtual" : "; shim");
    }
}

#if !defined(_WIN32)
// The older mapping cannot be copied (a dangling link where my-g920.ini goes): said in the log, tried again next time.
TEST_F(ConfigPickerFiles, AnOlderMappingThatCannotBeCopied) {
    const auto names = ConfigPicker::olderOwnNames(ConfigPicker::Target::G920);
    write(user() / names[0], "; virtual\n");
    std::filesystem::create_symlink(dir_ / "nowhere" / "file", user() / "my-g920.ini");
    ConfigPicker picker(window_, settings());
    EXPECT_TRUE(picker.builtIn());
    EXPECT_EQ(setting("older_mapping_g920"), "");
}
#endif

// While locked, choosing a target from elsewhere (Back to the G920) does nothing; nor does a Mode button. The buttons
// are greyed then, the chosen ones still marked, and their hover text says they are fixed.
TEST_F(ConfigPickerFiles, LockedNothingChoosesATarget) {
    ConfigPicker picker(window_, settings());
    picker.useTarget(ConfigPicker::Target::G25);
    picker.draw(4, 18, true);
    picker.setLocked(true);
    picker.useTarget(ConfigPicker::Target::G920);
    EXPECT_EQ(picker.target(), ConfigPicker::Target::G25);
    EXPECT_FALSE(picker.onClick(kShimX, kRowY));
    EXPECT_EQ(picker.way(), ConfigPicker::Way::VirtualDevice);
    std::vector<std::string> tips;
    picker.draw(4, 18, true,
                [&tips](float, float, float, float, std::string text) { tips.push_back(std::move(text)); });
    ASSERT_FALSE(tips.empty());
    EXPECT_NE(tips[0].find("Fixed while WheelBri runs."), std::string::npos);
}

TEST(ConfigPickerNames, ButtonOrders) {
    using Target = ConfigPicker::Target;
    EXPECT_TRUE(ConfigPicker::hasButtonOrders(Target::G25));
    EXPECT_TRUE(ConfigPicker::hasButtonOrders(Target::G27));
    EXPECT_FALSE(ConfigPicker::hasButtonOrders(Target::G920));
    EXPECT_FALSE(ConfigPicker::hasButtonOrders(Target::G923));
    EXPECT_EQ(ConfigPicker::ownName(Target::G25, ButtonOrder::Driver), "my-g25.ini");
    EXPECT_EQ(ConfigPicker::ownName(Target::G25, ButtonOrder::Native), "my-g25-native.ini");
    EXPECT_EQ(ConfigPicker::ownName(Target::G27, ButtonOrder::Native), "my-g27-native.ini");
    EXPECT_EQ(ConfigPicker::renumberedKey(Target::G27), "renumbered_g27");
}

// Each order of the G25 has its own mapping: Native's own file is my-g25-native.ini, WheelBri's default serves both;
// the order is kept per target in app.ini and shown on the row only for the G25 and G27.
TEST_F(ConfigPickerFiles, EachButtonOrderHasItsOwnMapping) {
    ConfigPicker picker(window_, settings());
    picker.draw(4, 18, true);
    EXPECT_FALSE(window_.shows("Order"));   // the G920: one order
    EXPECT_EQ(picker.rowsHeight(), 16);
    picker.useButtonOrder(ButtonOrder::Native);
    EXPECT_EQ(picker.buttonOrder(), ButtonOrder::Driver);

    picker.useTarget(ConfigPicker::Target::G25);
    picker.draw(4, 18, true);
    EXPECT_TRUE(window_.shows("Order"));
    EXPECT_TRUE(window_.shows("Logitech"));
    EXPECT_EQ(picker.rowsHeight(), 34);   // two rows: the first's right end stays free for the shim's game
    EXPECT_EQ(picker.buttonOrder(), ButtonOrder::Driver);
    EXPECT_EQ(setting("order_g25"), "driver");
    EXPECT_TRUE(picker.onClick(kNativeOrderX, kOrderRowY));
    EXPECT_EQ(picker.buttonOrder(), ButtonOrder::Native);
    EXPECT_EQ(setting("order_g25"), "native");
    EXPECT_EQ(picker.name(), "G25 Native Mode Virtual");
    EXPECT_EQ(picker.ownPath(), user() / "my-g25-native.ini");
    EXPECT_TRUE(picker.builtIn());   // nothing of the user's: the default, Start sets the order
    EXPECT_EQ(picker.selected(), (builtIn() / "g25.ini").string());
    EXPECT_TRUE(setting("renumbered_g25").empty());   // nothing copied, nothing to check

    write(user() / "my-g25-native.ini", "[buttons]\norder = native\n");
    picker.refresh();
    EXPECT_FALSE(picker.builtIn());
    EXPECT_EQ(picker.selected(), (user() / "my-g25-native.ini").string());

    picker.useTarget(ConfigPicker::Target::G27);   // its own order
    EXPECT_EQ(picker.buttonOrder(), ButtonOrder::Driver);
    picker.useTarget(ConfigPicker::Target::G25);
    EXPECT_EQ(picker.buttonOrder(), ButtonOrder::Native);
    picker.draw(4, 18, true);
    EXPECT_TRUE(picker.onClick(kDriverOrderX, kOrderRowY));
    EXPECT_EQ(picker.ownPath(), user() / "my-g25.ini");
    EXPECT_TRUE(picker.builtIn());

    picker.setLocked(true);   // from Start to Stop: fixed
    picker.useButtonOrder(ButtonOrder::Native);
    EXPECT_EQ(picker.buttonOrder(), ButtonOrder::Driver);
}

// The first Native with the user's own Logitech driver mapping: a copy of it, order = native, its numbers to check;
// a later Native keeps its own file. Default drops Native's alone and its marks.
TEST_F(ConfigPickerFiles, TheFirstNativeCopiesTheDriverMapping) {
    ASSERT_TRUE(writeAppSetting(state(), "target", "g25"));
    write(user() / "my-g25.ini", "; mine\n[buttons]\ntarget_0 = 3\n");
    ConfigPicker picker(window_, settings());
    picker.useButtonOrder(ButtonOrder::Native);
    const std::filesystem::path native = user() / "my-g25-native.ini";
    ASSERT_TRUE(std::filesystem::exists(native));
    std::ifstream in(native, std::ios::binary);
    const std::string copied{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    in.close();
    EXPECT_NE(copied.find("; mine"), std::string::npos) << copied;
    EXPECT_NE(copied.find("target_0 = 3"), std::string::npos) << copied;
    EXPECT_NE(copied.find("order = native"), std::string::npos) << copied;
    EXPECT_EQ(setting("renumbered_g25"), "pending");
    EXPECT_FALSE(picker.builtIn());
    EXPECT_EQ(picker.selected(), native.string());
    EXPECT_EQ(firstLine(user() / "my-g25.ini"), "; mine");   // the driver one as it was

    // Again: Native's own stays as it is.
    write(native, "[buttons]\norder = native\ntarget_0 = 9\n");
    ASSERT_TRUE(writeAppSetting(state(), "renumbered_g25", ""));
    picker.useButtonOrder(ButtonOrder::Driver);
    picker.useButtonOrder(ButtonOrder::Native);
    EXPECT_EQ(firstLine(native), "[buttons]");
    EXPECT_TRUE(setting("renumbered_g25").empty());

    ASSERT_TRUE(writeAppSetting(state(), "renumbered_g25", "0"));
    std::string error;
    ASSERT_TRUE(picker.dropOwnEdits(error)) << error;
    EXPECT_FALSE(std::filesystem::exists(native));
    EXPECT_TRUE(std::filesystem::exists(user() / "my-g25.ini"));
    EXPECT_TRUE(setting("renumbered_g25").empty());
    EXPECT_TRUE(picker.builtIn());
}

// A my-g25.ini from before there were two (order = native in it) becomes the Native mapping once: copied, Native
// picked, its own line back to driver. One without the line stays Logitech driver.
TEST_F(ConfigPickerFiles, AnOlderNativeMappingBecomesTheNativeOne) {
    ASSERT_TRUE(writeAppSetting(state(), "target", "g25"));
    write(user() / "my-g25.ini", "[buttons]\ntarget_0 = 3\norder = native\n");
    write(user() / "my-g27.ini", "[buttons]\ntarget_0 = 3\n");
    ConfigPicker picker(window_, settings());
    EXPECT_EQ(picker.buttonOrder(), ButtonOrder::Native);
    EXPECT_EQ(setting("order_g25"), "native");
    EXPECT_EQ(picker.selected(), (user() / "my-g25-native.ini").string());
    EXPECT_TRUE(setting("renumbered_g25").empty());   // made for Native: its numbers are right
    std::ifstream in(user() / "my-g25.ini", std::ios::binary);
    const std::string driver{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    in.close();
    EXPECT_NE(driver.find("order = driver"), std::string::npos) << driver;
    EXPECT_NE(driver.find("target_0 = 3"), std::string::npos) << driver;

    picker.useTarget(ConfigPicker::Target::G27);
    EXPECT_EQ(picker.buttonOrder(), ButtonOrder::Driver);
    EXPECT_EQ(setting("order_g27"), "driver");
    EXPECT_FALSE(std::filesystem::exists(user() / "my-g27-native.ini"));
}

// The Order row says what each choice means: the label and both buttons have hover text.
TEST_F(ConfigPickerFiles, TheOrderRowExplainsItself) {
    ASSERT_TRUE(writeAppSetting(state(), "target", "g25"));
    ConfigPicker picker(window_, settings());
    std::vector<std::string> tips;
    picker.draw(4, 18, true,
                [&tips](float, float, float, float, std::string text) { tips.push_back(std::move(text)); });
    const auto has = [&tips](const std::string& start) {
        return std::any_of(tips.begin(), tips.end(), [&start](const std::string& t) { return t.rfind(start, 0) == 0; });
    };
    EXPECT_TRUE(has("Which numbers the game gets the G25's buttons by."));
    EXPECT_TRUE(has("Numbered as with Logitech's driver, from 1."));
    EXPECT_TRUE(has("Numbered as the wheel reports them, from 0."));
}

// The G923's TrueForce row: shown for the G923 only; without Logitech's TrueForce SDK On is greyed, says why, and
// cannot be chosen.
TEST_F(ConfigPickerFiles, TrueForceNeedsLogitechsSdk) {
    ConfigPicker picker(window_, settings());   // no SDK
    picker.draw(4, 18, true);
    EXPECT_FALSE(window_.shows("TrueForce"));   // the G920
    picker.useTarget(ConfigPicker::Target::G923);
    EXPECT_EQ(picker.rowsHeight(), 34);
    std::vector<std::string> tips;
    window_.begin({});
    picker.draw(4, 18, true,
                [&tips](float, float, float, float, std::string text) { tips.push_back(std::move(text)); });
    EXPECT_TRUE(window_.shows("TrueForce"));
    EXPECT_TRUE(window_.shows("needs Logitech G HUB"));
    EXPECT_TRUE(std::any_of(tips.begin(), tips.end(),
                            [](const std::string& t) { return t.rfind("On needs Logitech G HUB installed", 0) == 0; }));
    EXPECT_FALSE(picker.onClick(kNativeOrderX, kOrderRowY));   // On: greyed
    picker.useTrueForce(true);
    EXPECT_FALSE(picker.trueForce());
    EXPECT_FALSE(picker.trueForceAtStart());
    EXPECT_EQ(setting("trueforce_g923"), "off");
}

// With the SDK: On is kept in app.ini and Start runs with it, in Mode Virtual and for the G923 only.
TEST_F(ConfigPickerFiles, TrueForceOnIsKeptAndUsedAtStart) {
    ConfigPicker::Settings withSdk = settings();
    withSdk.trueForceSdk = [] { return std::string("C:/Logi/trueforce/1_3_12/trueforce_sdk_x64.dll"); };
    ConfigPicker picker(window_, withSdk);
    picker.useTarget(ConfigPicker::Target::G923);
    window_.begin({});
    picker.draw(4, 18, true);
    EXPECT_FALSE(window_.shows("needs Logitech G HUB"));
    EXPECT_TRUE(picker.onClick(kNativeOrderX, kOrderRowY));   // On
    EXPECT_TRUE(picker.trueForce());
    EXPECT_TRUE(picker.trueForceAtStart());
    EXPECT_EQ(setting("trueforce_g923"), "on");

    picker.useTarget(ConfigPicker::Target::G920);   // another target: none at Start, the G923's kept
    EXPECT_FALSE(picker.trueForceAtStart());
    picker.useTarget(ConfigPicker::Target::G923);
    EXPECT_TRUE(picker.trueForceAtStart());
    picker.draw(4, 18, true);
    EXPECT_TRUE(picker.onClick(kShimX, kRowY));   // Mode Shim: no virtual G923, no TrueForce
    EXPECT_FALSE(picker.trueForceAtStart());
    picker.draw(4, 18, true);
    EXPECT_FALSE(picker.onClick(kDriverOrderX, kOrderRowY));   // greyed in Mode Shim
    EXPECT_TRUE(picker.onClick(kVirtualX, kRowY));
    EXPECT_TRUE(picker.trueForceAtStart());

    picker.setLocked(true);   // from Start to Stop: fixed
    picker.useTrueForce(false);
    EXPECT_TRUE(picker.trueForce());
    picker.setLocked(false);
    picker.draw(4, 18, true);
    EXPECT_TRUE(picker.onClick(kDriverOrderX, kOrderRowY));   // Off
    EXPECT_FALSE(picker.trueForceAtStart());
    EXPECT_EQ(setting("trueforce_g923"), "off");

    ConfigPicker again(window_, withSdk);   // remembered
    EXPECT_FALSE(again.trueForce());
}

// G HUB removed after On was picked: Start runs with Off, the choice is kept for when it is back.
TEST_F(ConfigPickerFiles, TrueForceWithoutTheSdkAnyMoreIsOffAtStart) {
    std::string sdk = "C:/Logi/trueforce/1_3_12/trueforce_sdk_x64.dll";
    ConfigPicker::Settings changing = settings();
    changing.trueForceSdk = [&sdk] { return sdk; };
    ASSERT_TRUE(writeAppSetting(state(), "target", "g923"));
    ConfigPicker picker(window_, changing);
    picker.useTrueForce(true);
    ASSERT_TRUE(picker.trueForceAtStart());
    sdk.clear();
    picker.refresh();
    EXPECT_FALSE(picker.trueForceInstalled());
    EXPECT_TRUE(picker.trueForce());
    EXPECT_FALSE(picker.trueForceAtStart());
    sdk = "C:/Logi/trueforce/1_3_13/trueforce_sdk_x64.dll";
    picker.refresh();
    EXPECT_TRUE(picker.trueForceAtStart());
}

// Before the switch TrueForce was set by hand in the G923's file: the first time, the switch starts from it (the
// user's own mapping, else WheelBri's default; also its older name). Afterwards app.ini alone counts.
TEST_F(ConfigPickerFiles, TrueForceStartsFromTheG923sFile) {
    write(user() / "my-g923.ini", "[emulator]\ntrueforce = true\n");
    {
        ConfigPicker picker(window_, settings());
        EXPECT_TRUE(picker.trueForce());
        EXPECT_EQ(setting("trueforce_g923"), "on");
    }
    write(user() / "my-g923.ini", "[emulator]\ntrueforce = false\n");
    {
        ConfigPicker picker(window_, settings());
        EXPECT_TRUE(picker.trueForce());   // app.ini's
    }
    std::filesystem::remove(user() / "my-g923.ini");
    ASSERT_TRUE(writeAppSetting(state(), "trueforce_g923", ""));
    write(builtIn() / "g923.ini", "[emulator]\ntrueforce_probe = yes\n");
    {
        ConfigPicker picker(window_, settings());
        EXPECT_TRUE(picker.trueForce());
    }
    ASSERT_TRUE(writeAppSetting(state(), "trueforce_g923", ""));
    write(builtIn() / "g923.ini", "[emulator]\n");
    ConfigPicker picker(window_, settings());
    EXPECT_FALSE(picker.trueForce());
    EXPECT_EQ(setting("trueforce_g923"), "off");
}

}  // namespace wheelbri::service
