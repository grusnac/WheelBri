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
        write(builtIn() / "g920.ini", kG920);
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
    const std::vector<ConfigEntry> existing = {{"g920", "x/g920.ini", true}};
    EXPECT_EQ(configNameProblem("My rig (rally)", existing), "");
    EXPECT_NE(configNameProblem("", existing), "");
    EXPECT_NE(configNameProblem(std::string(kMaxConfigName + 1, 'a'), existing), "");
    EXPECT_NE(configNameProblem("a/b", existing), "");
    EXPECT_NE(configNameProblem("a;b", existing), "");
    EXPECT_NE(configNameProblem(" lead", existing), "");
    EXPECT_NE(configNameProblem("trail.", existing), "");
    EXPECT_NE(configNameProblem("G920", existing), "");   // the same name as Windows sees it
}

TEST(ConfigLibrary, NewTextNamesItsTemplateAndStillParses) {
    const std::string text = newConfigText("Rally", "g920.ini", "\xEF\xBB\xBF" + kG920);
    EXPECT_EQ(configTemplateOf(text), "g920.ini");
    EXPECT_EQ(text.find("\xEF\xBB\xBF"), std::string::npos);
    EXPECT_NE(text.find("\r\n; made from: g920.ini\r\n"), std::string::npos);   // the template's line ends
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
    EXPECT_EQ(configs[1].name, "g920");
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
    EXPECT_EQ(resetTemplateOf(old, configs)->name, "g920");
    // And one made when each mode had its own built-in (until 2026-10-15).
    for (const char* older : {"g920-virtual-device.ini", "g920-in-game-shim.ini"}) {
        write(user() / "Older.ini", newConfigText("Older", older, kG920));
        const auto again = list();
        const auto olderEntry =
            std::find_if(again.begin(), again.end(), [](const ConfigEntry& c) { return c.name == "Older"; });
        ASSERT_NE(olderEntry, again.end());
        ASSERT_TRUE(resetTemplateOf(*olderEntry, again).has_value()) << older;
        EXPECT_EQ(resetTemplateOf(*olderEntry, again)->name, "g920") << older;
    }
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
    EXPECT_EQ(resetTemplateOf(rally, withRally)->name, "g920");
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
    EXPECT_EQ(configTemplateOf(read(made.path)), "g920.ini");
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

// Names that would be a problem on Windows or in the list: spaces or dots at either end, Windows' device names with
// an extension, characters outside the allowed set.
TEST(ConfigLibrary, ChecksTheEndsAndDeviceNamesMore) {
    for (const char* name : {"trail ", ".lead", "con.ini.bak", "COM7", "lpt0.x", "a\xC3\xA9", "tab\there", "a+b"}) {
        EXPECT_FALSE(configNameProblem(name, {}).empty()) << name;
    }
    for (const char* name : {"com-", "lptx", "comb", "a.b", "(x)_y-z", "com"}) {
        EXPECT_TRUE(configNameProblem(name, {}).empty()) << name;
    }
    EXPECT_EQ(configNameProblem(std::string(kMaxConfigName, 'a'), {}), "");   // exactly the longest allowed
}

// A template saved with LF line ends: the new config's own lines use LF too.
TEST(ConfigLibrary, NewTextUsesTheTemplatesLfLineEnds) {
    const std::string text = newConfigText("Rally", "a.ini", kOther);
    EXPECT_EQ(text.find('\r'), std::string::npos);
    EXPECT_NE(text.find("; made from: a.ini\n\n" + kOther), std::string::npos);
}

// The "made from" line as a user's editor may leave it: after a BOM, with a tab at the end, after a '#' comment,
// at the very end of a file with no line end; nothing for an empty or blank file.
TEST(ConfigLibrary, FindsTheTemplateLineHoweverTheFileWasSaved) {
    EXPECT_EQ(configTemplateOf("\xEF\xBB\xBF; made from: a.ini\r\n"), "a.ini");
    EXPECT_EQ(configTemplateOf("# notes\n; made from: b.ini\t \n"), "b.ini");
    EXPECT_EQ(configTemplateOf("; first\n; made from: c.ini"), "c.ini");
    EXPECT_EQ(configTemplateOf("; made from: \n"), "");
    EXPECT_EQ(configTemplateOf(""), "");
    EXPECT_EQ(configTemplateOf("\n\r\n  \t\n"), "");
    EXPECT_EQ(configTemplateOf("key = 1\n; made from: a.ini\n"), "");   // after a key: not the header any more
}

// A user's folder may hold anything: folders and other files named like configs, upper-case extensions, a file of
// their own called wheelbri-shim (only the built-in folder's is the shim's).
TEST_F(ConfigLibraryFiles, ListsOnlyConfigFiles) {
    std::filesystem::create_directories(user() / "folder.ini");
    write(user() / "Upper.INI", kOther);
    write(user() / "wheelbri-shim.ini", kOther);
    write(user() / "readme.md", "x");
    write(builtIn() / "WheelBri-Shim-old.ini", "[shim]\n");   // the shim's, whatever the case
    write(user() / "Caf\xC3\xA9.ini", kOther);                  // a name typed before names were checked
    const auto configs = list();
    std::vector<std::string> names;
    for (const ConfigEntry& config : configs) names.push_back(config.name + (config.builtIn ? "*" : ""));
    EXPECT_EQ(names, (std::vector<std::string>{"a-other*", "g920*", "Caf\xC3\xA9", "Upper", "wheelbri-shim"}));
}

// Copies made from each older built-in name all reset to today's g920.ini; a user's own g920.ini is never the
// template, and a template named in the file but not installed is nothing.
TEST_F(ConfigLibraryFiles, ResetTemplateRecognisesEveryOlderName) {
    std::filesystem::create_directories(user());
    write(user() / "g27.ini", kOther);   // the user's own, never a template
    for (const char* older : {"moza-r3-as-g29.ini", "rig-as-g920.ini", "RIG-AS-G29.INI", "g920.ini"}) {
        write(user() / "Old.ini", newConfigText("Old", older, kG920));
        const auto configs = list();
        const auto old =
            std::find_if(configs.begin(), configs.end(), [](const ConfigEntry& c) { return c.name == "Old"; });
        ASSERT_NE(old, configs.end());
        const auto from = resetTemplateOf(*old, configs);
        ASSERT_TRUE(from.has_value()) << older;
        EXPECT_TRUE(from->builtIn) << older;
        EXPECT_EQ(from->path, builtIn() / "g920.ini") << older;
    }
    write(user() / "Old.ini", newConfigText("Old", "g27.ini", kG920));
    const auto configs = list();
    const ConfigEntry old{"Old", user() / "Old.ini", false};
    EXPECT_FALSE(resetTemplateOf(old, configs).has_value());
    // Built in, or gone since the list was made: nothing to reset to.
    EXPECT_FALSE(resetTemplateOf(configs[1], configs).has_value());
    EXPECT_FALSE(resetTemplateOf({"Gone", user() / "Gone.ini", false}, configs).has_value());
}

// New: the built-in deleted after the list was made, a file of that name made meanwhile (by hand, or another copy
// of the app), and a user folder that cannot be made (a file is in its place). Each fails with a reason, and
// nothing is overwritten.
TEST_F(ConfigLibraryFiles, NewFailsCleanly) {
    const auto configs = list();
    const ConfigEntry gone{"gone", builtIn() / "gone.ini", true};
    ConfigResult result = createConfig(user(), "Rally", gone, configs);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.rfind("cannot read ", 0), 0U) << result.error;

    std::filesystem::create_directories(user());
    write(user() / "Rally.ini", "mine");
    result = createConfig(user(), "Rally", configs[1], configs);   // `configs` was listed before it was made
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("already exists"), std::string::npos) << result.error;
    EXPECT_EQ(read(user() / "Rally.ini"), "mine");
    result = createConfigWithText(user(), "Rally", configs[1], "[x]\n", configs);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("already exists"), std::string::npos) << result.error;
    EXPECT_EQ(read(user() / "Rally.ini"), "mine");

    const std::filesystem::path blocked = dir_ / "blocked";
    write(blocked, "a file where the folder should be");
    result = createConfig(blocked, "Rally", configs[1], configs);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.rfind("cannot write ", 0), 0U) << result.error;
    result = createConfigWithText(blocked, "Rally", configs[1], "[x]\n", configs);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.rfind("cannot write ", 0), 0U) << result.error;
    EXPECT_EQ(read(blocked), "a file where the folder should be");

    // A bad name is refused before anything is read or written.
    result = createConfigWithText(user(), "con", configs[1], "[x]\n", configs);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(std::filesystem::exists(user() / "con.ini"));
}

// Reset: the config deleted since the list was made, its built-in deleted, or the file held where it cannot be
// replaced. Each fails with a reason and the config is left as it was.
TEST_F(ConfigLibraryFiles, ResetFailsCleanly) {
    const auto configs = list();
    ASSERT_TRUE(createConfig(user(), "Rally", configs[1], configs).ok);
    const auto withRally = list();
    const ConfigEntry rally = withRally.back();
    const std::string fresh = read(rally.path);

    // Its built-in is gone from disk (but still in the list).
    std::filesystem::rename(builtIn() / "g920.ini", dir_ / "g920.ini.away");
    ConfigResult result = resetConfig(rally, withRally);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.rfind("cannot read ", 0), 0U) << result.error;
    std::filesystem::rename(dir_ / "g920.ini.away", builtIn() / "g920.ini");

    // The temporary file's name is taken by a folder with something in it: the write fails, the config stays.
    write(rally.path, fresh + "; my change\n");
    std::filesystem::create_directories(user() / "Rally.ini.tmp" / "busy");
    result = resetConfig(rally, withRally);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.rfind("cannot write ", 0), 0U) << result.error;
    EXPECT_EQ(read(rally.path), fresh + "; my change\n");
    std::filesystem::remove_all(user() / "Rally.ini.tmp");

    // The config itself deleted since the list was made.
    std::filesystem::remove(rally.path);
    result = resetConfig(rally, withRally);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.rfind("cannot read ", 0), 0U) << result.error;
    EXPECT_FALSE(std::filesystem::exists(rally.path));
}

// app.ini as a user may have edited it: CRLF, indented lines, comments, a section header in another case or
// missing its ']', lines without '=', other sections with the same keys. Only [app] counts.
TEST_F(ConfigLibraryFiles, ReadsAppSettingsFromAHandEditedFile) {
    const auto state = dir_ / "app.ini";
    write(state, "config = before any section\r\n[other]\r\nrig = Not this\r\n  ; [app]\r\n\t[ App ]\r\n"
                 "no equals sign\r\n\t RIG\t=\t Desk \t\r\n# rig = commented\r\n[ignored\r\nconfig = x");
    EXPECT_EQ(readAppSetting(state, "rig"), "Desk");
    EXPECT_EQ(readAppSetting(state, "config"), "");   // "[ignored" ends [app]: it is no section of ours
    write(state, "[app]\nrig =\n");
    EXPECT_EQ(readAppSetting(state, "rig"), "");
    write(state, "");
    EXPECT_EQ(readAppSetting(state, "rig"), "");
}

// Write then read gives what was written: also over a value that only differs after a '#' or ';' (which the
// editor would take for a comment), and over a hand-written key or section in another case.
TEST_F(ConfigLibraryFiles, AppSettingsReadBackWhatWasWritten) {
    const auto state = dir_ / "app.ini";
    ASSERT_TRUE(writeAppSetting(state, "rig", "Desk #2"));
    ASSERT_TRUE(writeAppSetting(state, "rig", "Desk"));
    EXPECT_EQ(readAppSetting(state, "rig"), "Desk");
    ASSERT_TRUE(writeAppSetting(state, "rig", "Desk;old"));
    ASSERT_TRUE(writeAppSetting(state, "rig", "Desk"));
    EXPECT_EQ(readAppSetting(state, "rig"), "Desk");

    write(state, "[App]\r\nConfig = old.ini\r\nway = shim\r\n");
    ASSERT_TRUE(writeConfigSelection(state, "new.ini"));
    EXPECT_EQ(readConfigSelection(state), "new.ini");
    EXPECT_EQ(readAppSetting(state, "way"), "shim");   // the rest of the file is kept
    EXPECT_NE(read(state).find("\r\n"), std::string::npos);   // and its line ends

    // A key written twice by hand: the last counts, as everywhere else; saving leaves one.
    write(state, "[app]\nrig = A\nrig = B\n");
    EXPECT_EQ(readAppSetting(state, "rig"), "B");
    ASSERT_TRUE(writeAppSetting(state, "rig", "C"));
    EXPECT_EQ(readAppSetting(state, "rig"), "C");
    EXPECT_EQ(read(state).find("rig = A"), std::string::npos);
}
