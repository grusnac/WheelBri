#include "wheelbri/ShimGames.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <string>

namespace wheelbri {
namespace {

class ShimGamesFiles : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("wheelbri-shim-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(game());
        std::filesystem::create_directories(dir_ / "shim");
        write(game() / "Game.exe", "MZ");
        write(sourceDll(), "our shim v1");
        write(sourceSettings(), "[shim]\n");
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    static void write(const std::filesystem::path& path, const std::string& text) {
        std::ofstream(path, std::ios::binary) << text;
    }
    static std::string read(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    [[nodiscard]] std::filesystem::path game() const { return dir_ / "Need for Speed"; }
    [[nodiscard]] std::filesystem::path sourceDll() const { return dir_ / "shim" / kShimDll; }
    [[nodiscard]] std::filesystem::path sourceSettings() const { return dir_ / "shim" / kShimSettings; }

    std::filesystem::path dir_;
};

TEST_F(ShimGamesFiles, ListRoundTripsAndSelects) {
    ShimGameList list;
    EXPECT_EQ(addShimGame(list, game()), 0U);
    EXPECT_EQ(addShimGame(list, dir_ / "Other"), 1U);
    EXPECT_EQ(addShimGame(list, game()), 0U);   // the same folder again: found, selected
    EXPECT_EQ(list.games[0].name, "Need for Speed");
    EXPECT_EQ(list.selected, game());
    ASSERT_TRUE(writeShimGames(dir_ / "state" / "shim-games.txt", list));
    const ShimGameList back = readShimGames(dir_ / "state" / "shim-games.txt");
    EXPECT_EQ(back.games, list.games);
    EXPECT_EQ(back.selected, game());
    EXPECT_TRUE(readShimGames(dir_ / "none.txt").games.empty());
}

TEST_F(ShimGamesFiles, AGameFolderNeedsAnExe) {
    EXPECT_EQ(gameFolderProblem(game()), "");
    EXPECT_NE(gameFolderProblem(dir_ / "shim"), "");
    EXPECT_NE(gameFolderProblem(dir_ / "missing"), "");
}

TEST_F(ShimGamesFiles, InstallThenRemove) {
    EXPECT_EQ(shimState(game(), sourceDll()), ShimState::Missing);
    const ShimResult put = installShim(game(), sourceDll(), sourceSettings());
    ASSERT_TRUE(put.ok) << put.error;
    EXPECT_EQ(put.changed.size(), 2U);
    EXPECT_EQ(shimState(game(), sourceDll()), ShimState::Installed);

    // A newer build of the shim: outdated, and installing again replaces the DLL but keeps the user's settings.
    write(game() / kShimSettings, "[shim]\nemulate = true\n");
    write(sourceDll(), "our shim v2");
    EXPECT_EQ(shimState(game(), sourceDll()), ShimState::Outdated);
    ASSERT_TRUE(installShim(game(), sourceDll(), sourceSettings()).ok);
    EXPECT_EQ(read(game() / kShimDll), "our shim v2");
    EXPECT_EQ(read(game() / kShimSettings), "[shim]\nemulate = true\n");

    const ShimResult removed = removeShim(game(), sourceDll());
    ASSERT_TRUE(removed.ok) << removed.error;
    EXPECT_EQ(shimState(game(), sourceDll()), ShimState::Missing);
    EXPECT_FALSE(std::filesystem::exists(game() / kShimSettings));
    EXPECT_TRUE(std::filesystem::exists(game() / "Game.exe"));
}

TEST_F(ShimGamesFiles, TheTargetIsSetInTheShimsSettings) {
    write(game() / kShimSettings, "; mine\n[shim]\nsource_vid = 346e   ; my rig\ntarget_profile = g920\n");
    const ShimResult g25 = setShimTarget(game(), "g25");
    ASSERT_TRUE(g25.ok) << g25.error;
    EXPECT_EQ(g25.changed.size(), 1U);
    EXPECT_EQ(read(game() / kShimSettings), "; mine\n[shim]\nsource_vid = 346e   ; my rig\ntarget_profile = g25\n");
    const ShimResult again = setShimTarget(game(), "g25");   // already so: nothing written
    ASSERT_TRUE(again.ok);
    EXPECT_TRUE(again.changed.empty());
}

TEST_F(ShimGamesFiles, TheTargetMakesTheSettingsWhenMissing) {
    ASSERT_TRUE(setShimTarget(game(), "g920").ok);
    EXPECT_EQ(read(game() / kShimSettings), "[shim]\ntarget_profile = g920\n");
}

TEST_F(ShimGamesFiles, TheTargetMustBeAWheelWheelBriEmulates) {
    write(game() / kShimSettings, "[shim]\ntarget_profile = g920\n");
    for (const char* bad : {"", "g29", "G25", "g25 "}) {
        const ShimResult result = setShimTarget(game(), bad);
        EXPECT_FALSE(result.ok) << bad;
        EXPECT_NE(result.error.find("g920, g25"), std::string::npos) << result.error;
        EXPECT_TRUE(result.changed.empty());
    }
    EXPECT_EQ(read(game() / kShimSettings), "[shim]\ntarget_profile = g920\n");   // untouched
}

TEST_F(ShimGamesFiles, TheTargetNeverMakesTheGameFolder) {
    const auto missing = dir_ / "not installed";
    const ShimResult result = setShimTarget(missing, "g25");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("is not there"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(missing));
    EXPECT_FALSE(setShimTarget({}, "g25").ok);                       // no folder at all
    EXPECT_FALSE(setShimTarget(game() / "Game.exe", "g25").ok);      // a file, not a folder
    EXPECT_EQ(read(game() / "Game.exe"), "MZ");
}

TEST_F(ShimGamesFiles, AFolderInTheSettingsPlaceIsLeftAlone) {
    std::filesystem::create_directories(game() / kShimSettings / "inside");
    const ShimResult result = setShimTarget(game(), "g25");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("is a folder"), std::string::npos);
    EXPECT_TRUE(std::filesystem::is_directory(game() / kShimSettings / "inside"));
}

TEST_F(ShimGamesFiles, TheTargetKeepsLineEndsBomAndComments) {
    write(game() / kShimSettings, "\xEF\xBB\xBF[shim]\r\nemulate = true ; mine\r\ntarget_profile = g920\r\n");
    ASSERT_TRUE(setShimTarget(game(), "g25").ok);
    EXPECT_EQ(read(game() / kShimSettings), "\xEF\xBB\xBF[shim]\r\nemulate = true ; mine\r\ntarget_profile = g25\r\n");
}

TEST_F(ShimGamesFiles, TheTargetIsReadLikeTheShimReadsIt) {
    // Already so, with a comment after it: nothing written.
    write(game() / kShimSettings, "[shim]\ntarget_profile = g25   ; the app sets it\n");
    const ShimResult same = setShimTarget(game(), "g25");
    ASSERT_TRUE(same.ok);
    EXPECT_TRUE(same.changed.empty());
    EXPECT_EQ(read(game() / kShimSettings), "[shim]\ntarget_profile = g25   ; the app sets it\n");
    // Written twice: the shim reads the last one, so that is the one set and the earlier one goes.
    write(game() / kShimSettings, "[shim]\ntarget_profile = g25\ntarget_profile = g920\n");
    const ShimResult twice = setShimTarget(game(), "g25");
    ASSERT_TRUE(twice.ok);
    EXPECT_EQ(twice.changed.size(), 1U);
    EXPECT_EQ(read(game() / kShimSettings), "[shim]\ntarget_profile = g25\n");
    // In another section it is not the shim's: [shim] gets its own.
    write(game() / kShimSettings, "[other]\ntarget_profile = g25\n");
    ASSERT_TRUE(setShimTarget(game(), "g25").ok);
    EXPECT_EQ(read(game() / kShimSettings), "[other]\ntarget_profile = g25\n\n[shim]\ntarget_profile = g25\n");
}

TEST_F(ShimGamesFiles, TheTargetSaysWhenTheFileCannotBeWritten) {
    write(game() / kShimSettings, "[shim]\ntarget_profile = g920\n");
    // Its temporary file cannot be made (a folder holds that name): the old file stays as it was.
    std::filesystem::create_directories(game() / (std::string(kShimSettings) + ".tmp") / "busy");
    const ShimResult result = setShimTarget(game(), "g25");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("cannot write"), std::string::npos);
    EXPECT_TRUE(result.changed.empty());
    EXPECT_EQ(read(game() / kShimSettings), "[shim]\ntarget_profile = g920\n");
}

TEST_F(ShimGamesFiles, TheTargetSaysWhenTheFileCannotBeRead) {
    write(game() / kShimSettings, "[shim]\ntarget_profile = g920\n");
    std::filesystem::permissions(game() / kShimSettings, std::filesystem::perms::none);
    if (std::ifstream(game() / kShimSettings).good()) {   // Administrator / root, or Windows: still readable
        std::filesystem::permissions(game() / kShimSettings, std::filesystem::perms::owner_all);
        GTEST_SKIP() << "the file stays readable here";
    }
    const ShimResult result = setShimTarget(game(), "g25");
    std::filesystem::permissions(game() / kShimSettings, std::filesystem::perms::owner_all);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("cannot read"), std::string::npos);
    EXPECT_EQ(read(game() / kShimSettings), "[shim]\ntarget_profile = g920\n");
}

TEST_F(ShimGamesFiles, AnotherModsDllIsLeftAlone) {
    write(game() / kShimDll, "someone else's proxy");
    EXPECT_EQ(shimState(game(), sourceDll()), ShimState::Foreign);
    EXPECT_FALSE(installShim(game(), sourceDll(), sourceSettings()).ok);
    EXPECT_FALSE(removeShim(game(), sourceDll()).ok);
    EXPECT_EQ(read(game() / kShimDll), "someone else's proxy");
}

TEST_F(ShimGamesFiles, LinesThatAreNotUtf8AreSkipped) {
    const auto file = dir_ / "shim-games.txt";
    write(file, "game\tBad\t" + std::string("C:\\Spiele\\\xFC") + "\ngame\tGood\t" + pathToUtf8(game()) + "\n");
    const ShimGameList list = readShimGames(file);
    ASSERT_EQ(list.games.size(), 1U);
    EXPECT_EQ(list.games[0].folder, game());
}

TEST_F(ShimGamesFiles, GamesCanBeTakenOffTheList) {
    ShimGameList list;
    addShimGame(list, dir_ / "A");
    addShimGame(list, dir_ / "B");
    addShimGame(list, dir_ / "C");
    list.selected = dir_ / "B";
    EXPECT_TRUE(removeShimGame(list, 0));        // not the selected one: the selection stays
    EXPECT_EQ(list.selected, dir_ / "B");
    EXPECT_TRUE(removeShimGame(list, 0));        // the selected one: the first game left is picked
    ASSERT_EQ(list.games.size(), 1U);
    EXPECT_EQ(list.selected, dir_ / "C");
    EXPECT_FALSE(removeShimGame(list, 5));
    EXPECT_TRUE(removeShimGame(list, 0));
    EXPECT_TRUE(list.games.empty());
    EXPECT_TRUE(list.selected.empty());
    ASSERT_TRUE(writeShimGames(dir_ / "list.txt", list));
    EXPECT_TRUE(readShimGames(dir_ / "list.txt").games.empty());
}

}  // namespace
}  // namespace wheelbri
