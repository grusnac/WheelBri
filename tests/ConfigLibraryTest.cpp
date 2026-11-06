// SPDX-License-Identifier: GPL-3.0-or-later
// The app's state file (ConfigLibrary.hpp): [app] settings read raw and written back without losing the rest.

#include "wheelbri/ConfigLibrary.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace wheelbri;

namespace {

class ConfigLibraryFiles : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("wheelbri-configs-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

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
