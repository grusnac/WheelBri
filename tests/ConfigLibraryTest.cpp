#include "wheelbri/ConfigLibrary.hpp"

#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/IniDocument.hpp"

#include <gtest/gtest.h>

#include <algorithm>

#include <fstream>
#include <sstream>

using namespace wheelbri;

namespace {

const std::string kG920 = "; the rig as a G920\r\n[target]\r\nprofile = g920\r\n";
const std::string kOther = "[target]\nprofile = g920\n; another\n";

class ConfigLibraryFiles : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("wheelbri-configs-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(builtIn());
        write(builtIn() / "g920-virtual-device.ini", kG920);
        write(builtIn() / "a-other.ini", kOther);
        write(builtIn() / "wheelbri-shim.ini", "[shim]\n");
        write(builtIn() / "notes.txt", "not a config");
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    [[nodiscard]] std::filesystem::path builtIn() const { return dir_ / "config"; }
    [[nodiscard]] std::filesystem::path user() const { return dir_ / "user"; }
    [[nodiscard]] std::vector<ConfigEntry> list() const { return listConfigs(builtIn(), user()); }

    static void write(const std::filesystem::path& path, const std::string& text) {
        std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
    }
    static std::string read(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();
        return text.str();
    }

    std::filesystem::path dir_;
};

}  // namespace

TEST(ConfigLibrary, ChecksNames) {
    const std::vector<ConfigEntry> existing = {{"g920-virtual-device", "x/g920-virtual-device.ini", true}};
    EXPECT_EQ(configNameProblem("My rig (rally)", existing), "");
    EXPECT_NE(configNameProblem("", existing), "");
    EXPECT_NE(configNameProblem(std::string(kMaxConfigName + 1, 'a'), existing), "");
    EXPECT_NE(configNameProblem("a/b", existing), "");
    EXPECT_NE(configNameProblem("a;b", existing), "");
    EXPECT_NE(configNameProblem(" lead", existing), "");
    EXPECT_NE(configNameProblem("trail.", existing), "");
    EXPECT_NE(configNameProblem("G920-Virtual-Device", existing), "");   // the same name as Windows sees it
}

TEST(ConfigLibrary, NewTextNamesItsTemplateAndStillParses) {
    const std::string text = newConfigText("Rally", "g920-virtual-device.ini", "\xEF\xBB\xBF" + kG920);
    EXPECT_EQ(configTemplateOf(text), "g920-virtual-device.ini");
    EXPECT_EQ(text.find("\xEF\xBB\xBF"), std::string::npos);
    EXPECT_NE(text.find("\r\n; made from: g920-virtual-device.ini\r\n"), std::string::npos);   // the template's line ends
    const auto parsed = IniDocument::parse(text);
    EXPECT_TRUE(parsed.errors.empty());
    EXPECT_EQ(parsed.document.get("target", "profile"), "g920");
}

TEST(ConfigLibrary, TemplateLineOnlyCountsAtTheTop) {
    EXPECT_EQ(configTemplateOf("; made from: a.ini\n[x]\n"), "a.ini");
    EXPECT_EQ(configTemplateOf("\n  ; other\r\n; made from: b.ini  \r\n"), "b.ini");
    EXPECT_EQ(configTemplateOf("[x]\n; made from: a.ini\n"), "");
    EXPECT_EQ(configTemplateOf("; hand made\n[x]\n"), "");
}

TEST_F(ConfigLibraryFiles, ListsBuiltInThenOwnAndLeavesTheShimOut) {
    EXPECT_TRUE(listConfigs(builtIn(), user()).size() == 2);   // no user folder yet
    std::filesystem::create_directories(user());
    write(user() / "b.ini", kOther);
    write(user() / "A.ini", kOther);
    const auto configs = list();
    ASSERT_EQ(configs.size(), 4U);
    EXPECT_EQ(configs[0].name, "a-other");
    EXPECT_TRUE(configs[0].builtIn);
    EXPECT_EQ(configs[1].name, "g920-virtual-device");
    EXPECT_EQ(configs[2].name, "A");
    EXPECT_FALSE(configs[2].builtIn);
    EXPECT_EQ(configs[3].name, "b");
}

TEST_F(ConfigLibraryFiles, ResetFindsTheRenamedBuiltIn) {
    // A copy made before the built-ins were renamed (moza-r3-as-g920.ini, later rig-as-g920.ini).
    std::filesystem::create_directories(user());
    write(user() / "Old.ini", newConfigText("Old", "moza-r3-as-g920.ini", kG920));
    const auto configs = list();
    const auto found = std::find_if(configs.begin(), configs.end(), [](const ConfigEntry& c) { return c.name == "Old"; });
    ASSERT_NE(found, configs.end());
    const ConfigEntry old = *found;
    ASSERT_TRUE(resetTemplateOf(old, configs).has_value());
    EXPECT_EQ(resetTemplateOf(old, configs)->name, "g920-virtual-device");
}

TEST_F(ConfigLibraryFiles, NewCopiesTheBuiltInAndResetCopiesItAgain) {
    const auto configs = list();
    const ConfigResult made = createConfig(user(), "Rally", configs[1], configs);
    ASSERT_TRUE(made.ok) << made.error;
    EXPECT_EQ(made.path, user() / "Rally.ini");
    const std::string fresh = read(made.path);
    EXPECT_NE(fresh.find(kG920), std::string::npos);

    // The same name again is refused, and the file is left alone.
    write(made.path, fresh + "[axis_labels]\r\naxis_0 = Mine\r\n");
    const auto withRally = list();
    EXPECT_FALSE(createConfig(user(), "rally", withRally[1], withRally).ok);

    const ConfigEntry rally = withRally.back();
    ASSERT_EQ(rally.name, "Rally");
    ASSERT_TRUE(resetTemplateOf(rally, withRally).has_value());
    EXPECT_EQ(resetTemplateOf(rally, withRally)->name, "g920-virtual-device");
    const ConfigResult reset = resetConfig(rally, withRally);
    ASSERT_TRUE(reset.ok) << reset.error;
    EXPECT_EQ(read(made.path), fresh);   // the change is gone
}

TEST_F(ConfigLibraryFiles, ResetRefusesBuiltInAndHandMadeConfigs) {
    std::filesystem::create_directories(user());
    write(user() / "hand.ini", kOther);
    write(user() / "orphan.ini", "; made from: gone.ini\n" + kOther);
    const auto configs = list();
    EXPECT_FALSE(resetConfig(configs[0], configs).ok);   // built in
    EXPECT_EQ(read(builtIn() / "a-other.ini"), kOther);
    for (const ConfigEntry& config : configs) {
        if (config.builtIn) continue;
        EXPECT_FALSE(resetTemplateOf(config, configs).has_value()) << config.name;
        const ConfigResult result = resetConfig(config, configs);
        EXPECT_FALSE(result.ok) << config.name;
        EXPECT_FALSE(result.error.empty());
    }
    EXPECT_EQ(read(user() / "hand.ini"), kOther);
}

TEST_F(ConfigLibraryFiles, SaveAsWritesTheEditedTextAndResetGoesBackToTheBuiltIn) {
    const auto configs = list();
    const std::string edited = kG920 + "[buttons]\r\ntarget_6 = 1209:2000/0\r\n";
    const ConfigResult made = createConfigWithText(user(), "Mine", configs[1], edited, configs);
    ASSERT_TRUE(made.ok) << made.error;
    EXPECT_NE(read(made.path).find("target_6 = 1209:2000/0"), std::string::npos);
    EXPECT_EQ(configTemplateOf(read(made.path)), "g920-virtual-device.ini");
    const auto again = list();
    EXPECT_FALSE(createConfigWithText(user(), "mine", configs[1], edited, again).ok);
    ASSERT_TRUE(resetConfig(again.back(), again).ok);
    EXPECT_EQ(read(made.path).find("target_6"), std::string::npos);
}

TEST_F(ConfigLibraryFiles, RemembersThePickedConfig) {
    const std::filesystem::path state = dir_ / "state" / "app.ini";
    EXPECT_EQ(readConfigSelection(state), "");
    ASSERT_TRUE(writeConfigSelection(state, "C:\\Program Files\\WheelBri\\config\\rig-as-a.ini"));
    EXPECT_EQ(readConfigSelection(state), "C:\\Program Files\\WheelBri\\config\\rig-as-a.ini");
    ASSERT_TRUE(writeConfigSelection(state, "/home/me/.config/wheelbri/configs/Rally.ini"));
    EXPECT_EQ(readConfigSelection(state), "/home/me/.config/wheelbri/configs/Rally.ini");
}

TEST_F(ConfigLibraryFiles, AppSettingsKeepCommentCharactersInValues) {
    const auto state = dir_ / "app.ini";
    ASSERT_TRUE(writeAppSetting(state, "config", "C:\\Games #1\\my;config.ini"));
    ASSERT_TRUE(writeAppSetting(state, "rig", "Desk"));
    EXPECT_EQ(readAppSetting(state, "config"), "C:\\Games #1\\my;config.ini");
    EXPECT_EQ(readAppSetting(state, "rig"), "Desk");
    EXPECT_EQ(readAppSetting(state, "missing"), "");
}

TEST_F(ConfigLibraryFiles, AppSettingsAreNotWrittenOverAFileThatCannotBeRead) {
    const auto state = dir_ / "app.ini";
    std::filesystem::create_directories(state);   // there, but not a file that can be read
    EXPECT_FALSE(writeAppSetting(state, "rig", "Desk"));
    EXPECT_TRUE(std::filesystem::is_directory(state));
}

TEST(ConfigLibrary, WindowsDeviceNamesAreRefused) {
    for (const char* name : {"con", "NUL", "aux.backup", "com1", "LPT9", "prn"}) {
        EXPECT_FALSE(configNameProblem(name, {}).empty()) << name;
    }
    for (const char* name : {"console", "com10", "my con", "nulled"}) {
        EXPECT_TRUE(configNameProblem(name, {}).empty()) << name;
    }
}
