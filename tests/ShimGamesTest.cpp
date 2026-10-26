#include "wheelbri/ShimGames.hpp"

#include <gtest/gtest.h>

#include <cctype>
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

// A game's own pedal axes (the Games tab) go into its wheelbri-shim.ini with game_axes = wheelbri, and come out
// again when none are set; axes the user wrote (no game_axes) are never taken out.
TEST_F(ShimGamesFiles, TheGamesAxesAreWrittenAndTakenOut) {
    write(game() / kShimSettings, "[shim]\ntarget_profile = g920\n");
    JoystickLayout axes;
    axes.throttle = ObjectGuid::Slider;
    axes.brake = ObjectGuid::YAxis;
    axes.clutch = ObjectGuid::RzAxis;
    const ShimResult set = setShimGameAxes(game(), axes);
    ASSERT_TRUE(set.ok) << set.error;
    EXPECT_EQ(set.changed.size(), 1U);
    EXPECT_EQ(read(game() / kShimSettings), "[shim]\ntarget_profile = g920\nthrottle_axis = slider\nbrake_axis = y\n"
                                            "clutch_axis = rz\ngame_axes = wheelbri\n");
    EXPECT_TRUE(setShimGameAxes(game(), axes).changed.empty());   // already so
    const ShimResult out = setShimGameAxes(game(), std::nullopt);
    ASSERT_TRUE(out.ok) << out.error;
    EXPECT_EQ(read(game() / kShimSettings), "[shim]\ntarget_profile = g920\n");

    const std::string mine = "[shim]\nthrottle_axis = z   ; my own\n";
    write(game() / kShimSettings, mine);
    const ShimResult untouched = setShimGameAxes(game(), std::nullopt);
    ASSERT_TRUE(untouched.ok);
    EXPECT_TRUE(untouched.changed.empty());
    EXPECT_EQ(read(game() / kShimSettings), mine);

    axes.brake = ObjectGuid::Slider;   // two pedals on one axis: the shim would refuse it
    EXPECT_FALSE(setShimGameAxes(game(), axes).ok);
    EXPECT_EQ(read(game() / kShimSettings), mine);
}

TEST_F(ShimGamesFiles, TheGameInAFolderIsFoundByItsProgram) {
    GameProfile other;
    other.id = "other";
    other.program = "other.exe";
    GameProfile mine;
    mine.id = "mine";
    mine.program = "game.exe";   // the fixture's Game.exe, matched without case
    const std::vector<GameProfile> games{other, mine};
    const GameProfile* const found = gameInFolder(game(), games);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->id, "mine");
    EXPECT_EQ(gameInFolder(game(), std::span<const GameProfile>(games.data(), 1)), nullptr);
    EXPECT_EQ(gameInFolder(dir_ / "not there", games), nullptr);
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
        EXPECT_NE(result.error.find("g25, g27, g920, g923"), std::string::npos) << result.error;
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

// Another mod's dinput8.dll is chained: renamed aside, the shim in its place hands DirectInput on to it; removing the
// shim gives the mod its place back.
TEST_F(ShimGamesFiles, AnotherModsDllIsChained) {
    write(game() / kShimDll, "someone else's proxy");
    EXPECT_EQ(shimState(game(), sourceDll()), ShimState::Foreign);
    EXPECT_FALSE(removeShim(game(), sourceDll()).ok);   // not ours to delete
    EXPECT_FALSE(shimChained(game()));

    const ShimResult put = installShim(game(), sourceDll(), sourceSettings());
    ASSERT_TRUE(put.ok) << put.error;
    EXPECT_EQ(read(game() / kShimDll), "our shim v1");
    EXPECT_EQ(read(game() / kShimChainDll), "someone else's proxy");
    EXPECT_NE(read(game() / kShimSettings).find("chain = dinput8-chain.dll"), std::string::npos);
    EXPECT_TRUE(shimChained(game()));
    EXPECT_EQ(shimState(game(), sourceDll()), ShimState::Installed);

    const ShimResult removed = removeShim(game(), sourceDll());
    ASSERT_TRUE(removed.ok) << removed.error;
    EXPECT_EQ(read(game() / kShimDll), "someone else's proxy");
    EXPECT_FALSE(std::filesystem::exists(game() / kShimChainDll));
    EXPECT_FALSE(std::filesystem::exists(game() / kShimSettings));
}

// The mod updated over our shim: its new DLL is chained, the one before kept as .old.
TEST_F(ShimGamesFiles, AModUpdatedOverTheShimIsChainedAgain) {
    write(game() / kShimDll, "mod v1");
    ASSERT_TRUE(installShim(game(), sourceDll(), sourceSettings()).ok);
    write(game() / kShimDll, "mod v2");   // no mark, and the settings chain: the mod's, not an older shim of ours
    EXPECT_EQ(shimState(game(), sourceDll()), ShimState::Foreign);

    const ShimResult put = installShim(game(), sourceDll(), sourceSettings());
    ASSERT_TRUE(put.ok) << put.error;
    EXPECT_EQ(read(game() / kShimDll), "our shim v1");
    EXPECT_EQ(read(game() / kShimChainDll), "mod v2");
    EXPECT_EQ(read(game() / (std::string(kShimChainDll) + ".old")), "mod v1");
}

// Settings without the chain line next to a chained shim (written by hand, or a step that failed) get it back.
TEST_F(ShimGamesFiles, AChainedShimGetsItsChainLine) {
    write(game() / kShimDll, "our shim v1");
    write(game() / kShimChainDll, "someone else's proxy");
    write(game() / kShimSettings, "[shim]\ntarget_profile = g25\n");
    ASSERT_TRUE(installShim(game(), sourceDll(), sourceSettings()).ok);
    EXPECT_EQ(read(game() / kShimSettings), "[shim]\ntarget_profile = g25\nchain = dinput8-chain.dll\n");

    write(game() / kShimSettings, "[shim]\nchain = mine.dll\n");   // one set by hand is kept
    ASSERT_TRUE(installShim(game(), sourceDll(), sourceSettings()).ok);
    EXPECT_EQ(read(game() / kShimSettings), "[shim]\nchain = mine.dll\n");
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

// A minimal PE file: "MZ", the PE header's offset at 0x3C, "PE\0\0" and the machine; `pad` bytes after it.
std::string peFile(const unsigned machine, const std::size_t pad = 0) {
    std::string bytes(64, '\0');
    bytes[0] = 'M';
    bytes[1] = 'Z';
    bytes[0x3C] = 64;
    bytes += std::string("PE\0\0", 4);
    bytes += static_cast<char>(machine & 0xFFU);
    bytes += static_cast<char>((machine >> 8U) & 0xFFU);
    return bytes + std::string(pad, '\0');
}

TEST_F(ShimGamesFiles, ReadsWhichProcessorAProgramIsFor) {
    write(game() / "a.exe", peFile(0x014C));
    write(game() / "b.exe", peFile(0x8664));
    write(game() / "c.exe", peFile(0xAA64));   // ARM64
    EXPECT_EQ(peArchitecture(game() / "a.exe"), Architecture::X86);
    EXPECT_EQ(peArchitecture(game() / "b.exe"), Architecture::X64);
    EXPECT_EQ(peArchitecture(game() / "c.exe"), Architecture::Unknown);
    EXPECT_EQ(peArchitecture(game() / "Game.exe"), Architecture::Unknown);   // "MZ" only
    EXPECT_EQ(peArchitecture(game() / "none.exe"), Architecture::Unknown);
}

TEST_F(ShimGamesFiles, AGameIsWhatItsLargestExeIs) {
    EXPECT_EQ(gameArchitecture(game()), Architecture::Unknown);   // Game.exe is not a real program
    write(game() / "launcher.exe", peFile(0x8664));
    write(game() / "NFSC.exe", peFile(0x014C, 1000));
    EXPECT_EQ(gameArchitecture(game()), Architecture::X86);

    const ShimSources sources{dir_ / "shim" / kShimDll, dir_ / "shim" / "x86" / kShimDll};
    EXPECT_EQ(shimSourceFor(game(), sources), sources.x86);
    write(game() / "NFSC.exe", peFile(0x8664, 1000));
    EXPECT_EQ(shimSourceFor(game(), sources), sources.x64);
    std::filesystem::remove(game() / "NFSC.exe");
    std::filesystem::remove(game() / "launcher.exe");
    EXPECT_EQ(shimSourceFor(game(), sources), sources.x64);   // cannot tell: 64-bit, as before
}

// A mod's DLL built for the other processor could not run in the game: left as it is, nothing written.
TEST_F(ShimGamesFiles, AModForTheOtherProcessorIsLeftAlone) {
    write(game() / "NFSC.exe", peFile(0x014C, 100));   // a 32-bit game
    write(game() / kShimDll, peFile(0x8664));           // a 64-bit mod
    const ShimResult put = installShim(game(), sourceDll(), sourceSettings());
    EXPECT_FALSE(put.ok);
    EXPECT_TRUE(put.leftAlone);
    EXPECT_NE(put.error.find("other processor"), std::string::npos) << put.error;
    EXPECT_EQ(read(game() / kShimDll), peFile(0x8664));
    EXPECT_FALSE(std::filesystem::exists(game() / kShimChainDll));
    EXPECT_FALSE(std::filesystem::exists(game() / kShimSettings));
}

TEST_F(ShimGamesFiles, NextToTheAsiLoaderTheShimIsDinput8Hooked) {
    const std::string loader = "MZ ... IsUltimateASILoader ... DirectInput8Create";
    write(game() / kShimDll, loader);
    EXPECT_TRUE(isAsiLoader(game() / kShimDll));
    EXPECT_FALSE(isAsiLoader(sourceDll()));
    EXPECT_EQ(shimDllIn(game()), game() / kShimDllHooked);
    EXPECT_EQ(shimState(game(), sourceDll()), ShimState::Missing);   // not "another dinput8.dll"

    const ShimResult put = installShim(game(), sourceDll(), sourceSettings());
    ASSERT_TRUE(put.ok) << put.error;
    EXPECT_EQ(read(game() / kShimDllHooked), "our shim v1");
    EXPECT_EQ(read(game() / kShimDll), loader);   // the loader is never touched
    EXPECT_EQ(shimState(game(), sourceDll()), ShimState::Installed);

    write(sourceDll(), "our shim v2");
    EXPECT_EQ(shimState(game(), sourceDll()), ShimState::Outdated);
    ASSERT_TRUE(installShim(game(), sourceDll(), sourceSettings()).ok);
    EXPECT_EQ(read(game() / kShimDllHooked), "our shim v2");

    const ShimResult removed = removeShim(game(), sourceDll());
    ASSERT_TRUE(removed.ok) << removed.error;
    EXPECT_FALSE(std::filesystem::exists(game() / kShimDllHooked));
    EXPECT_FALSE(std::filesystem::exists(game() / kShimSettings));
    EXPECT_EQ(read(game() / kShimDll), loader);
}

// Next to the ASI Loader, another mod's dinput8Hooked.dll is chained the same way; the loader is never touched.
TEST_F(ShimGamesFiles, AnotherDinput8HookedIsChained) {
    write(game() / kShimDll, "IsUltimateASILoader");
    write(game() / kShimDllHooked, "someone else's");
    EXPECT_EQ(shimState(game(), sourceDll()), ShimState::Foreign);
    ASSERT_TRUE(installShim(game(), sourceDll(), sourceSettings()).ok);
    EXPECT_EQ(read(game() / kShimDllHooked), "our shim v1");
    EXPECT_EQ(read(game() / kShimChainDll), "someone else's");
    EXPECT_EQ(read(game() / kShimDll), "IsUltimateASILoader");

    ASSERT_TRUE(removeShim(game(), sourceDll()).ok);
    EXPECT_EQ(read(game() / kShimDllHooked), "someone else's");
    EXPECT_EQ(read(game() / kShimDll), "IsUltimateASILoader");
    EXPECT_FALSE(std::filesystem::exists(game() / kShimChainDll));
}

TEST_F(ShimGamesFiles, OurMarkSaysTheDllIsOurs) {
    write(game() / kShimDll, "MZ ... IsWheelBriShim ... DirectInput8Create");   // an older build of ours, no settings
    EXPECT_TRUE(isWheelBriShim(game() / kShimDll));
    EXPECT_FALSE(isWheelBriShim(game() / "Game.exe"));
    EXPECT_FALSE(isWheelBriShim(game() / "missing.dll"));
    EXPECT_EQ(shimState(game(), sourceDll()), ShimState::Outdated);
    EXPECT_EQ(shimState(game(), dir_ / "missing.dll"), ShimState::Installed);

    const ShimResult put = installShim(game(), sourceDll(), sourceSettings());
    ASSERT_TRUE(put.ok) << put.error;
    EXPECT_EQ(read(game() / kShimDll), "our shim v1");
}

TEST_F(ShimGamesFiles, WithoutOurMarkOrSettingsTheDllIsAnotherMods) {
    write(game() / kShimDll, "MZ ... DirectInput8Create");
    EXPECT_FALSE(isWheelBriShim(game() / kShimDll));
    EXPECT_EQ(shimState(game(), sourceDll()), ShimState::Foreign);
}

TEST(ShimChain, TheChainIsABareDllNameInTheGameFolder) {
    EXPECT_TRUE(validChainName(kShimChainDll));
    EXPECT_TRUE(validChainName("ReShade 64.DLL"));
    EXPECT_FALSE(validChainName(""));
    EXPECT_FALSE(validChainName(".dll"));
    EXPECT_FALSE(validChainName("dinput8.dll"));      // the shim's own names
    EXPECT_FALSE(validChainName("DInput8.DLL"));
    EXPECT_FALSE(validChainName("dinput8Hooked.dll"));
    EXPECT_FALSE(validChainName("dinput8.dll."));     // Windows would open dinput8.dll
    EXPECT_FALSE(validChainName("dinput8.dll "));
    EXPECT_FALSE(validChainName("DINPUT~1.DLL"));     // a short name can be any file
    EXPECT_FALSE(validChainName("..\\x.dll"));
    EXPECT_FALSE(validChainName("mods/x.dll"));
    EXPECT_FALSE(validChainName("C:x.dll"));
    EXPECT_FALSE(validChainName("x.exe"));
    EXPECT_FALSE(validChainName("x\tdll.dll"));
    EXPECT_FALSE(validChainName("\xc3\xa9.dll"));
    EXPECT_FALSE(validChainName(std::string(61, 'a') + ".dll"));
}

TEST_F(ShimGamesFiles, OurShimForTheOtherProcessorIsReplaced) {
    // The 64-bit shim put into a 32-bit game before there was a 32-bit one: ours (its settings are there), outdated.
    ASSERT_TRUE(installShim(game(), sourceDll(), sourceSettings()).ok);
    write(dir_ / "x86.dll", "our 32-bit shim");
    EXPECT_EQ(shimState(game(), dir_ / "x86.dll"), ShimState::Outdated);
    ASSERT_TRUE(installShim(game(), dir_ / "x86.dll", sourceSettings()).ok);
    EXPECT_EQ(read(game() / kShimDll), "our 32-bit shim");
}

// A damaged or foreign program file (cut short, not MZ, the PE header past its end or misspelt) is never mistaken
// for a 32- or 64-bit game.
TEST_F(ShimGamesFiles, ACutOrDamagedProgramIsOfNoKnownProcessor) {
    std::string notMz = peFile(0x014C);
    notMz[1] = 'X';
    write(game() / "notmz.exe", notMz);
    write(game() / "short.exe", peFile(0x014C).substr(0, 40));   // shorter than the DOS header
    std::string pastEnd = peFile(0x014C);
    pastEnd[0x3C] = 0x7F;                                         // the PE header would start past the end
    write(game() / "pastend.exe", pastEnd);
    std::string badSignature = peFile(0x014C);
    badSignature[65] = 'X';                                       // "PX\0\0"
    write(game() / "badsig.exe", badSignature);
    std::string notZeroed = peFile(0x014C);
    notZeroed[66] = 'x';                                          // "PEx\0"
    write(game() / "pex.exe", notZeroed);
    std::string notZeroed2 = peFile(0x014C);
    notZeroed2[67] = 'x';                                         // "PE\0x"
    write(game() / "pe0x.exe", notZeroed2);
    std::string notPe = peFile(0x014C);
    notPe[64] = 'Q';                                              // "QE\0\0"
    write(game() / "qe.exe", notPe);
    for (const char* name : {"notmz.exe", "short.exe", "pastend.exe", "badsig.exe", "pex.exe", "pe0x.exe", "qe.exe"}) {
        EXPECT_EQ(peArchitecture(game() / name), Architecture::Unknown) << name;
    }
    EXPECT_EQ(gameArchitecture(game()), Architecture::Unknown);
}

// Only regular .exe files count, in any case, and the largest decides even when a smaller one is listed after it.
TEST_F(ShimGamesFiles, TheLargestRealExeDecidesInAnyCase) {
    std::filesystem::create_directories(game() / "Tools.exe");      // a folder named like a program: not one
    write(game() / "GAME32.EXE", peFile(0x014C, 4000));             // upper case: still a program
    write(game() / "small1.exe", peFile(0x8664));
    write(game() / "small2.exe", peFile(0x8664, 10));
    write(game() / "readme.txt", peFile(0x8664, 9000));             // not a program, however large
    EXPECT_EQ(gameArchitecture(game()), Architecture::X86);
    EXPECT_EQ(gameArchitecture(dir_ / "no such folder"), Architecture::Unknown);
}

// A dinput8.dll over 64 MB is not read whole to look for the ASI Loader's export: it is not the loader.
TEST_F(ShimGamesFiles, AHugeDllIsNotTakenForTheAsiLoader) {
    write(game() / kShimDll, "IsUltimateASILoader");
    std::filesystem::resize_file(game() / kShimDll, 65ULL * 1024 * 1024);   // sparse: no disk used
    EXPECT_FALSE(isAsiLoader(game() / kShimDll));
    EXPECT_EQ(shimDllIn(game()), game() / kShimDll);
    EXPECT_FALSE(isAsiLoader(game() / "missing.dll"));
}

// A folder named dinput8.dll is no shim: the state is Missing and installing says it cannot write there.
TEST_F(ShimGamesFiles, AFolderNamedDinput8CannotBeReplaced) {
    std::filesystem::create_directories(game() / kShimDll / "inside");
    EXPECT_EQ(shimDllIn(game()), game() / kShimDll);
    EXPECT_EQ(shimState(game(), sourceDll()), ShimState::Missing);
    const ShimResult put = installShim(game(), sourceDll(), sourceSettings());
    EXPECT_FALSE(put.ok);
    EXPECT_NE(put.error.find("cannot write"), std::string::npos) << put.error;
    EXPECT_NE(put.error.find("close the game and try again"), std::string::npos) << put.error;
    EXPECT_TRUE(put.changed.empty());
    EXPECT_TRUE(std::filesystem::is_directory(game() / kShimDll / "inside"));
    EXPECT_FALSE(std::filesystem::exists(game() / kShimSettings));
}

// A folder named wheelbri-shim.ini: the DLL goes in, the settings cannot, and the result says which file failed.
TEST_F(ShimGamesFiles, AFolderInTheSettingsPlaceStopsTheInstall) {
    std::filesystem::create_directories(game() / kShimSettings);
    const ShimResult put = installShim(game(), sourceDll(), sourceSettings());
    EXPECT_FALSE(put.ok);
    EXPECT_NE(put.error.find(kShimSettings), std::string::npos) << put.error;
    ASSERT_EQ(put.changed.size(), 1U);
    EXPECT_EQ(read(game() / kShimDll), "our shim v1");
    EXPECT_TRUE(std::filesystem::is_directory(game() / kShimSettings));
}

// Without WheelBri's own wheelbri-shim.ini to copy, only the DLL goes in.
TEST_F(ShimGamesFiles, NoSourceSettingsPutsOnlyTheDll) {
    const ShimResult put = installShim(game(), sourceDll(), dir_ / "shim" / "missing.ini");
    ASSERT_TRUE(put.ok) << put.error;
    ASSERT_EQ(put.changed.size(), 1U);
    EXPECT_FALSE(std::filesystem::exists(game() / kShimSettings));
    // With no settings next to it, our own DLL is still ours: it is byte-identical.
    EXPECT_EQ(shimState(game(), sourceDll()), ShimState::Installed);
}

// WheelBri's own DLL missing (not built, or not installed): installing says so, naming the 32-bit one for a
// 32-bit game; nothing is written.
TEST_F(ShimGamesFiles, AMissingSourceDllIsSaidAndNothingWritten) {
    ShimResult put = installShim(game(), {}, sourceSettings());
    EXPECT_FALSE(put.ok);
    EXPECT_NE(put.error.find("not found in shim/"), std::string::npos) << put.error;
    put = installShim(game(), dir_ / "shim" / "gone.dll", sourceSettings());
    EXPECT_FALSE(put.ok);
    EXPECT_NE(put.error.find("gone.dll"), std::string::npos) << put.error;
    EXPECT_EQ(put.error.find("32-bit"), std::string::npos) << put.error;
    write(game() / "NFSC.exe", peFile(0x014C, 100));
    put = installShim(game(), dir_ / "shim" / "gone.dll", sourceSettings());
    EXPECT_NE(put.error.find("32-bit dinput8.dll is missing"), std::string::npos) << put.error;
    EXPECT_FALSE(std::filesystem::exists(game() / kShimDll));
    EXPECT_FALSE(std::filesystem::exists(game() / kShimSettings));
}

// A folder that is not a game folder (no .exe, or not there) gets nothing.
TEST_F(ShimGamesFiles, NoShimIntoAFolderWithoutAGame) {
    const ShimResult put = installShim(dir_ / "shim", sourceDll(), sourceSettings());
    EXPECT_FALSE(put.ok);
    EXPECT_NE(put.error.find("no .exe"), std::string::npos) << put.error;
    EXPECT_NE(gameFolderProblem({}), "");
    std::filesystem::create_directories(dir_ / "tools" / "setup.exe");   // only a folder named like a program
    EXPECT_NE(gameFolderProblem(dir_ / "tools"), "");
    EXPECT_NE(gameFolderProblem(game() / "Game.exe"), "");               // a file, not a folder
}

// When WheelBri's own DLL cannot be found, a dinput8.dll with our settings next to it still counts as ours
// (installed); one without them is another mod's.
TEST_F(ShimGamesFiles, WithoutTheSourceOurSettingsSayWhoseDllItIs) {
    write(game() / kShimDll, "some build of ours");
    EXPECT_EQ(shimState(game(), dir_ / "missing.dll"), ShimState::Foreign);
    write(game() / kShimSettings, "[shim]\n");
    EXPECT_EQ(shimState(game(), dir_ / "missing.dll"), ShimState::Installed);
    EXPECT_EQ(shimState(game(), {}), ShimState::Installed);
}

// Removing where there is no DLL: only our settings go; with nothing there it is a success that changed nothing.
TEST_F(ShimGamesFiles, RemovingWithNoDllTakesOnlyTheSettings) {
    write(game() / kShimSettings, "[shim]\n");
    ShimResult removed = removeShim(game(), sourceDll());
    ASSERT_TRUE(removed.ok) << removed.error;
    ASSERT_EQ(removed.changed.size(), 1U);
    EXPECT_FALSE(std::filesystem::exists(game() / kShimSettings));
    removed = removeShim(game(), sourceDll());
    EXPECT_TRUE(removed.ok);
    EXPECT_TRUE(removed.changed.empty());
    EXPECT_TRUE(std::filesystem::exists(game() / "Game.exe"));
}

// A list file edited by hand: CRLF, comments, blank lines and lines of the wrong shape are skipped; the same folder
// twice (in another case) is one game; a selection that is not listed falls back to the first game.
TEST_F(ShimGamesFiles, AHandEditedListIsReadSafely) {
    const auto file = dir_ / "shim-games.txt";
    const std::string folder = pathToUtf8(game());
    std::string upper = folder;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    write(file, "# comment\r\n\r\ngame\tOnly two\r\ngame\tEmpty\t\r\nwhat\tever\r\n"
                "selected\ttoo\tmany\r\ngame\tFirst\t" + folder + "\r\ngame\tAgain\t" + upper +
                "\r\ngame\tSecond\t" + pathToUtf8(dir_ / "Other") + "\r\nselected\t" + pathToUtf8(dir_ / "Gone") +
                "\r\n");
    const ShimGameList list = readShimGames(file);
    ASSERT_EQ(list.games.size(), 2U);
    EXPECT_EQ(list.games[0].name, "First");
    EXPECT_EQ(list.games[0].folder, game());
    EXPECT_EQ(list.games[1].name, "Second");
    EXPECT_EQ(list.selected, game());   // "Gone" is not in the list
}

// A folder picked with a trailing separator, a drive's root, a Linux folder with a tab (the list's separator) in its
// name: the name shown stays one field and the folder comes back whole after a restart.
TEST_F(ShimGamesFiles, FolderNamesBecomeOneFieldNames) {
    ShimGameList list;
    EXPECT_EQ(addShimGame(list, dir_ / "Trailing" / ""), 0U);
    EXPECT_EQ(list.games[0].name, "Trailing");
    EXPECT_EQ(list.games[0].folder, dir_ / "Trailing");
    EXPECT_EQ(addShimGame(list, std::filesystem::path("/")), 1U);
#ifdef _WIN32
    EXPECT_EQ(list.games[1].name, "\\");   // Windows writes the root with its own separator
#else
    EXPECT_EQ(list.games[1].name, "/");
#endif
    EXPECT_EQ(addShimGame(list, dir_ / "Tab\tName"), 2U);
    EXPECT_EQ(list.games[2].name, "Tab Name");
    ASSERT_TRUE(writeShimGames(dir_ / "list.txt", list));
    const ShimGameList back = readShimGames(dir_ / "list.txt");
    ASSERT_EQ(back.games.size(), 3U);
    EXPECT_EQ(back.games[2].name, "Tab Name");
    EXPECT_EQ(back.games[2].folder, dir_ / "Tab\tName");
    EXPECT_EQ(back.selected, dir_ / "Tab\tName");
}

// A game folder whose name is not ASCII round-trips through the list as UTF-8.
TEST_F(ShimGamesFiles, NonAsciiFoldersRoundTrip) {
    const auto folder = dir_ / pathFromUtf8("Spiele \xC3\xBC\xE2\x82\xAC \xE6\xB8\xB8\xE6\x88\x8F");
    std::filesystem::create_directories(folder);
    write(folder / "Game.exe", "MZ");
    ShimGameList list;
    addShimGame(list, folder);
    ASSERT_TRUE(writeShimGames(dir_ / "list.txt", list));
    const ShimGameList back = readShimGames(dir_ / "list.txt");
    ASSERT_EQ(back.games.size(), 1U);
    EXPECT_EQ(back.games[0].folder, folder);
    EXPECT_EQ(back.games[0].name, "Spiele \xC3\xBC\xE2\x82\xAC \xE6\xB8\xB8\xE6\x88\x8F");
    ASSERT_TRUE(installShim(folder, sourceDll(), sourceSettings()).ok);
    EXPECT_EQ(shimState(folder, sourceDll()), ShimState::Installed);
}

// The list cannot be written where a folder holds its name: false, nothing replaced.
TEST_F(ShimGamesFiles, AListThatCannotBeWrittenSaysSo) {
    std::filesystem::create_directories(dir_ / "list.txt");
    ShimGameList list;
    addShimGame(list, game());
    EXPECT_FALSE(writeShimGames(dir_ / "list.txt", list));
    EXPECT_TRUE(std::filesystem::is_directory(dir_ / "list.txt"));
    EXPECT_TRUE(readShimGames(dir_ / "list.txt").games.empty());   // a folder reads as an empty list
}

// The game's axes are refused, like the target, where there is no game folder, a folder holds the settings' name,
// the settings cannot be read or cannot be replaced; nothing is written in any of those.
TEST_F(ShimGamesFiles, TheGamesAxesSayWhyTheyCannotBeWritten) {
    const JoystickLayout axes;
    EXPECT_NE(setShimGameAxes(dir_ / "not installed", axes).error.find("is not there"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(dir_ / "not installed"));
    EXPECT_FALSE(setShimGameAxes({}, axes).ok);
    EXPECT_FALSE(setShimGameAxes({}, std::nullopt).ok);   // even taking them out needs the folder

    std::filesystem::create_directories(game() / kShimSettings / "inside");
    EXPECT_NE(setShimGameAxes(game(), axes).error.find("is a folder"), std::string::npos);
    std::filesystem::remove_all(game() / kShimSettings);

    write(game() / kShimSettings, "[shim]\ntarget_profile = g920\n");
    std::filesystem::create_directories(game() / (std::string(kShimSettings) + ".tmp") / "busy");
    const ShimResult blocked = setShimGameAxes(game(), axes);
    EXPECT_FALSE(blocked.ok);
    EXPECT_NE(blocked.error.find("cannot write"), std::string::npos);
    EXPECT_TRUE(blocked.changed.empty());
    EXPECT_EQ(read(game() / kShimSettings), "[shim]\ntarget_profile = g920\n");
    std::filesystem::remove_all(game() / (std::string(kShimSettings) + ".tmp"));

    std::filesystem::permissions(game() / kShimSettings, std::filesystem::perms::none);
    if (!std::ifstream(game() / kShimSettings).good()) {   // not when Administrator / root, or on Windows
        const ShimResult unread = setShimGameAxes(game(), axes);
        EXPECT_NE(unread.error.find("cannot read"), std::string::npos);
    }
    std::filesystem::permissions(game() / kShimSettings, std::filesystem::perms::owner_all);
}

// No settings file yet: the axes make one; taking out axes that were never there writes nothing.
TEST_F(ShimGamesFiles, TheGamesAxesMakeTheSettingsWhenMissing) {
    EXPECT_TRUE(setShimGameAxes(game(), std::nullopt).changed.empty());
    EXPECT_FALSE(std::filesystem::exists(game() / kShimSettings));
    const ShimResult made = setShimGameAxes(game(), JoystickLayout{});
    ASSERT_TRUE(made.ok) << made.error;
    EXPECT_EQ(read(game() / kShimSettings),
              "[shim]\nthrottle_axis = y\nbrake_axis = rz\nclutch_axis = slider\ngame_axes = wheelbri\n");
}

// Folders beside the game's .exe (a subfolder named like a program) are never taken for the program.
TEST_F(ShimGamesFiles, AFolderIsNeverTheGamesProgram) {
    std::filesystem::remove(game() / "Game.exe");
    std::filesystem::create_directories(game() / "game.exe");
    GameProfile mine;
    mine.id = "mine";
    mine.program = "game.exe";
    const std::vector<GameProfile> games{mine};
    EXPECT_EQ(gameInFolder(game(), games), nullptr);
    EXPECT_EQ(gameFolderProblem(game()), "no .exe in that folder: pick the folder that holds the game's .exe");
}

// A game folder Windows (or the system) does not let WheelBri write to: putting the shim in and taking it out say
// what to do instead of only the system's words, and leave the folder as it was.
TEST_F(ShimGamesFiles, AProtectedFolderSaysWhatToDo) {
    const ShimResult put = installShim(game(), sourceDll(), sourceSettings());
    ASSERT_TRUE(put.ok) << put.error;
    write(sourceDll(), "our shim v2");   // outdated: installing again must write
    // As under Program Files: neither the folder nor its files may be written by the user.
    std::filesystem::permissions(game() / kShimDll, std::filesystem::perms::owner_read);
    std::filesystem::permissions(game(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    const bool writable = std::ofstream(game() / "probe.txt").good() ||
                          std::ofstream(game() / kShimDll, std::ios::app).good();
    if (writable) {   // Administrator / root, or Windows: the folder is not protected here
        std::filesystem::permissions(game(), std::filesystem::perms::owner_all);
        std::filesystem::permissions(game() / kShimDll, std::filesystem::perms::owner_all);
        GTEST_SKIP() << "the folder stays writable here";
    }
    const ShimResult again = installShim(game(), sourceDll(), sourceSettings());
    const ShimResult out = removeShim(game(), sourceDll());
    std::filesystem::permissions(game(), std::filesystem::perms::owner_all);
    std::filesystem::permissions(game() / kShimDll, std::filesystem::perms::owner_all);
    EXPECT_FALSE(again.ok);
    EXPECT_NE(again.error.find("Windows does not let WheelBri write"), std::string::npos) << again.error;
    EXPECT_FALSE(out.ok);
    EXPECT_NE(out.error.find("Windows does not let WheelBri delete"), std::string::npos) << out.error;
    EXPECT_EQ(read(game() / kShimDll), "our shim v1");
    EXPECT_TRUE(std::filesystem::exists(game() / kShimSettings));
}

// The same for the settings alone: no DLL left, the settings cannot be deleted, or made.
TEST_F(ShimGamesFiles, ProtectedSettingsSayWhatToDo) {
    write(game() / kShimSettings, "[shim]\n");
    std::filesystem::permissions(game(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    if (std::ofstream(game() / "probe.txt").good()) {
        std::filesystem::permissions(game(), std::filesystem::perms::owner_all);
        GTEST_SKIP() << "the folder stays writable here";
    }
    const ShimResult out = removeShim(game(), sourceDll());
    std::filesystem::permissions(game(), std::filesystem::perms::owner_all);
    EXPECT_FALSE(out.ok);
    EXPECT_NE(out.error.find("delete"), std::string::npos) << out.error;
    EXPECT_TRUE(std::filesystem::exists(game() / kShimSettings));

    // Settings that cannot be made after the DLL went in: said, and the DLL that went in is reported changed.
    std::filesystem::remove(game() / kShimSettings);
    std::filesystem::permissions(game(), std::filesystem::perms::owner_all);
    write(game() / kShimDll, "our shim v1");   // already in place: only the settings are missing
    std::filesystem::permissions(game(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    const ShimResult put = installShim(game(), sourceDll(), sourceSettings());
    std::filesystem::permissions(game(), std::filesystem::perms::owner_all);
    EXPECT_FALSE(put.ok);
    EXPECT_NE(put.error.find("Windows does not let WheelBri write"), std::string::npos) << put.error;
    EXPECT_FALSE(std::filesystem::exists(game() / kShimSettings));
}

}  // namespace
}  // namespace wheelbri
