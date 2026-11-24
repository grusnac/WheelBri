// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/GameFiles.hpp"

#include "wheelbri/FileText.hpp"
#include "wheelbri/IniDocument.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace wheelbri {
namespace {

const std::string kOriginal = "<ActionMap deviceType=\"G25\" >\r\n"
                              "\t<Action actionName = \"Replay Pause\" >\r\n"
                              "\t\t<Axis axisName = \"win_key_f1\" deviceName = \"Keyboard\" />\r\n"
                              "\t</Action>\r\n"
                              "</ActionMap>\r\n";
const std::string kAdded = "<ActionMap deviceType=\"G25\" >\r\n"
                           "\t<Action actionName = \"Replay Pause\" >\r\n"
                           "\t\t<Axis axisName = \"win_key_f1\" deviceName = \"Keyboard\" />\r\n"
                           "\t\t<Axis axisName = \"win_con_di_button17\" />\r\n"
                           "\t</Action>\r\n"
                           "</ActionMap>\r\n";

FileValueMap gridLike() {
    const auto ini = IniDocument::parse("[game_files]\nfile = ActionMap/actionMapPreset1.xml\nnote = n\n"
                                        "insert.1 = Action[actionName=Replay Pause] | end | "
                                        "<Axis axisName = \"win_con_di_button17\" />\n"
                                        "insert.1.note = A pauses the replay\n");
    return parseFileValueMap(ini.document.sections().at("game_files")).map;
}

class GameFiles : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string test = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        folder_ = std::filesystem::temp_directory_path() / ("wheelbri-gamefiles-" + test);
        std::filesystem::remove_all(folder_);
        std::filesystem::create_directories(folder_ / "ActionMap");
    }
    void TearDown() override { std::filesystem::remove_all(folder_); }

    [[nodiscard]] std::filesystem::path file() const { return folder_ / "ActionMap" / "actionMapPreset1.xml"; }
    [[nodiscard]] std::filesystem::path backup() const {
        return folder_ / "ActionMap" / "actionMapPreset1.xml.before-wheelbri";
    }

    std::filesystem::path folder_;
    FileValueMap map_ = gridLike();
};

TEST_F(GameFiles, AddKeepsTheOriginalOnceRemoveTakesTheLinesOut) {
    ASSERT_TRUE(writeFileReplacing(file(), kOriginal));
    EXPECT_EQ(readGameFilesState(folder_, map_).status, GameFilesState::Status::Read);
    EXPECT_TRUE(readGameFilesState(folder_, map_).lines.none());

    GameFilesResult added = addGameFiles(folder_, map_);
    ASSERT_EQ(added.outcome, GameFilesResult::Outcome::Written) << added.why;
    EXPECT_EQ(added.changed, 1U);
    EXPECT_EQ(readFileText(file()).value_or(""), kAdded);
    EXPECT_EQ(readFileText(backup()).value_or(""), kOriginal);
    EXPECT_TRUE(readGameFilesState(folder_, map_).lines.all());
    EXPECT_EQ(addGameFiles(folder_, map_).outcome, GameFilesResult::Outcome::AsAsked);

    const GameFilesResult removed = removeGameFiles(folder_, map_);
    ASSERT_EQ(removed.outcome, GameFilesResult::Outcome::Written) << removed.why;
    EXPECT_EQ(readFileText(file()).value_or(""), kOriginal);
    EXPECT_EQ(removeGameFiles(folder_, map_).outcome, GameFilesResult::Outcome::AsAsked);

    // The backup is never written over: it stays the file as it first was.
    ASSERT_TRUE(writeFileReplacing(file(), kOriginal + "\r\n"));
    ASSERT_EQ(addGameFiles(folder_, map_).outcome, GameFilesResult::Outcome::Written);
    EXPECT_EQ(readFileText(backup()).value_or(""), kOriginal);
}

TEST_F(GameFiles, NoFolderOrNoFileWritesNothing) {
    EXPECT_EQ(readGameFilesState({}, map_).status, GameFilesState::Status::NoFolder);
    EXPECT_EQ(addGameFiles({}, map_).outcome, GameFilesResult::Outcome::NoFile);
    EXPECT_EQ(readGameFilesState(folder_, map_).status, GameFilesState::Status::NoFile);
    const GameFilesResult result = addGameFiles(folder_, map_);
    EXPECT_EQ(result.outcome, GameFilesResult::Outcome::NoFile);
    EXPECT_EQ(result.why, "no ActionMap/actionMapPreset1.xml in the game's folder");
    EXPECT_FALSE(std::filesystem::exists(file()));
    EXPECT_FALSE(std::filesystem::exists(backup()));
}

TEST_F(GameFiles, AFileNotAsExpectedIsRefusedAndLeftAlone) {
    const std::string other = "<ActionMap>\r\n\t<Action actionName = \"Boost\" >\r\n\t</Action>\r\n</ActionMap>\r\n";
    ASSERT_TRUE(writeFileReplacing(file(), other));
    const GameFilesState state = readGameFilesState(folder_, map_);
    EXPECT_EQ(state.status, GameFilesState::Status::Unreadable);
    EXPECT_NE(state.why.find("no Action[actionName=Replay Pause] in the file"), std::string::npos) << state.why;
    const GameFilesResult result = addGameFiles(folder_, map_);
    EXPECT_EQ(result.outcome, GameFilesResult::Outcome::Refused);
    EXPECT_EQ(readFileText(file()).value_or(""), other);
    EXPECT_FALSE(std::filesystem::exists(backup()));
}

TEST_F(GameFiles, LinksAreNotFollowed) {
    std::error_code ec;
    const std::filesystem::path elsewhere = folder_ / "elsewhere.xml";
    ASSERT_TRUE(writeFileReplacing(elsewhere, kOriginal));
    std::filesystem::create_symlink(elsewhere, file(), ec);
    if (ec) GTEST_SKIP() << "links cannot be made here: " << ec.message();
    EXPECT_EQ(addGameFiles(folder_, map_).outcome, GameFilesResult::Outcome::Refused);
    EXPECT_EQ(readGameFilesState(folder_, map_).status, GameFilesState::Status::Unreadable);
    EXPECT_EQ(readFileText(elsewhere).value_or(""), kOriginal);

    // The backup's name is a link: nothing written there, nor into the file.
    std::filesystem::remove(file());
    ASSERT_TRUE(writeFileReplacing(file(), kOriginal));
    const std::filesystem::path target = folder_ / "target";
    std::filesystem::create_symlink(target, backup(), ec);
    ASSERT_FALSE(ec);
    EXPECT_EQ(addGameFiles(folder_, map_).outcome, GameFilesResult::Outcome::Refused);
    EXPECT_FALSE(std::filesystem::exists(target));
    EXPECT_EQ(readFileText(file()).value_or(""), kOriginal);
}

}  // namespace
}  // namespace wheelbri
