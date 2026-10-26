// What the app does with the shim's game at Start (service/src/ShimGamesView.cpp, prepareStart): the shim put in or
// brought up to date, and the target written into the game's wheelbri-shim.ini; each refusal says why.

#include "ShimGamesView.hpp"
#include "UiStyle.hpp"
#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/EmulatedJoystick.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace wheelbri::service {
namespace {

class SilentWindow final : public ui::UiWindow {
public:
    ui::WindowInput pump() override { return {}; }
    [[nodiscard]] bool visible() const override { return true; }
    void begin(ui::Colour) override {}
    void fill(float, float, float, float, ui::Colour) override {}
    void outline(float, float, float, float, ui::Colour) override {}
    void text(float, float, std::string_view, ui::Colour) override {}
    void present() override {}
    bool openFolder(const std::filesystem::path&, std::string&) override { return false; }
    bool pickFolder(const std::string&, std::filesystem::path&, std::string&) override { return false; }
    [[nodiscard]] std::string describe() const override { return "silent"; }
};

class ShimStart : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("wheelbri-shimstart-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(game());
        std::filesystem::create_directories(dir_ / "shim");
        write(game() / "Game.exe", "MZ");
        write(dir_ / "shim" / kShimDll, "our shim");
        write(dir_ / "shim" / kShimSettings, "[shim]\ntarget_profile = g920\n");
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    static void write(const std::filesystem::path& path, const std::string& text) {
        std::ofstream(path, std::ios::binary) << text;
    }
    static std::string read(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    [[nodiscard]] std::filesystem::path game() const { return dir_ / "Need for Speed Undercover"; }
    [[nodiscard]] ShimGamesView::Settings settings() const {
        return {dir_ / "state" / "shim-games.txt", ShimSources{dir_ / "shim" / kShimDll, {}},
                dir_ / "shim" / kShimSettings, {}, {}, {}};
    }
    void listTheGame() const {
        ShimGameList list;
        addShimGame(list, game());
        ASSERT_TRUE(writeShimGames(settings().listFile, list));
    }
    // Start's preparation, without game axes; the folders it readied in prepared_.
    bool prepare(ShimGamesView& view, const std::string& profile, std::string& error) {
        return view.prepareStart(profile, {}, prepared_, error);
    }

    std::vector<std::filesystem::path> prepared_;

    SilentWindow window_;
    std::filesystem::path dir_;
};

TEST_F(ShimStart, NoGameNoStart) {
    ShimGamesView view(window_, settings());
    std::string error;
    EXPECT_FALSE(prepare(view, "g25", error));
    EXPECT_NE(error.find("No game yet"), std::string::npos);
}

TEST_F(ShimStart, PutsTheShimInAndSetsTheTarget) {
    listTheGame();
    ShimGamesView view(window_, settings());
    std::string error;
    ASSERT_TRUE(prepare(view, "g25", error)) << error;
    EXPECT_EQ(read(game() / kShimDll), "our shim");
    EXPECT_EQ(read(game() / kShimSettings), "[shim]\ntarget_profile = g25\n");
    // Again with the G920: only the target changes.
    ASSERT_TRUE(prepare(view, "g920", error)) << error;
    EXPECT_EQ(read(game() / kShimSettings), "[shim]\ntarget_profile = g920\n");
}

TEST_F(ShimStart, AnInstalledShimKeepsTheUsersSettingsButTheTarget) {
    listTheGame();
    write(game() / kShimDll, "our shim");
    write(game() / kShimSettings, "[shim]\nsource_vid = 30b7 ; mine\ntarget_profile = g920\n");
    ShimGamesView view(window_, settings());
    std::string error;
    ASSERT_TRUE(prepare(view, "g25", error)) << error;
    EXPECT_EQ(read(game() / kShimSettings), "[shim]\nsource_vid = 30b7 ; mine\ntarget_profile = g25\n");
}

// Another mod's dinput8.dll is chained at Start: the shim in its place, the mod's renamed to dinput8-chain.dll.
TEST_F(ShimStart, AnotherModsDllIsChained) {
    listTheGame();
    write(game() / kShimDll, "someone else's proxy");
    ShimGamesView view(window_, settings());
    std::string error;
    ASSERT_TRUE(prepare(view, "g25", error)) << error;
    EXPECT_EQ(read(game() / kShimDll), "our shim");
    EXPECT_EQ(read(game() / kShimChainDll), "someone else's proxy");
    const std::string settingsText = read(game() / kShimSettings);
    EXPECT_NE(settingsText.find("chain = dinput8-chain.dll"), std::string::npos) << settingsText;
    EXPECT_NE(settingsText.find("target_profile = g25"), std::string::npos) << settingsText;
    EXPECT_EQ(prepared_, std::vector<std::filesystem::path>{game()});
}

// A mod's DLL that cannot be chained is left out; when it is the only folder, Start cannot go on.
TEST_F(ShimStart, AModThatCannotBeChainedIsLeftOut) {
    listTheGame();
    write(game() / kShimDll, "someone else's proxy");
    std::filesystem::create_directories(game() / kShimChainDll);   // a folder where the chain would go
    ShimGamesView view(window_, settings());
    std::string error;
    EXPECT_FALSE(prepare(view, "g25", error));
    EXPECT_NE(error.find("cannot chain"), std::string::npos) << error;
    EXPECT_FALSE(std::filesystem::exists(game() / kShimSettings));   // nothing written next to it
    EXPECT_EQ(read(game() / kShimDll), "someone else's proxy");
    EXPECT_TRUE(prepared_.empty());
}

// Every listed folder is readied (one whose shim was deleted gets it back), presenting the target, with the pedal axes
// given for it; another mod's folder is chained.
TEST_F(ShimStart, EveryListedFolderIsReady) {
    const auto second = dir_ / "Second";
    const auto modded = dir_ / "Modded";
    for (const auto& folder : {second, modded}) {
        std::filesystem::create_directories(folder);
        write(folder / "Game.exe", "MZ");
    }
    write(modded / kShimDll, "another mod");
    ShimGameList list;
    for (const auto& folder : {game(), second, modded}) addShimGame(list, folder);
    ASSERT_TRUE(writeShimGames(settings().listFile, list));
    ShimGamesView view(window_, settings());
    std::vector<std::filesystem::path> asked;
    std::string error;
    ASSERT_TRUE(view.prepareStart("g923",
                                  [&asked](const std::filesystem::path& folder) {
                                      asked.push_back(folder);
                                      return std::optional<JoystickLayout>();
                                  },
                                  prepared_, error))
        << error;
    EXPECT_EQ(prepared_, (std::vector<std::filesystem::path>{game(), second, modded}));
    EXPECT_EQ(asked, prepared_);
    EXPECT_EQ(read(game() / kShimDll), "our shim");
    EXPECT_EQ(read(second / kShimDll), "our shim");
    EXPECT_NE(read(second / kShimSettings).find("target_profile = g923"), std::string::npos);
    EXPECT_EQ(read(modded / kShimDll), "our shim");
    EXPECT_EQ(read(modded / kShimChainDll), "another mod");
    // Deleted from one: the next Start puts it back.
    std::filesystem::remove(second / kShimDll);
    ASSERT_TRUE(prepare(view, "g923", error)) << error;
    EXPECT_EQ(read(second / kShimDll), "our shim");
}

// What Stop does with the shims: asked unless the user said otherwise; kept in app.ini.
TEST_F(ShimStart, TheAtStopChoiceIsKept) {
    ShimGamesView::Settings kept = settings();
    kept.stateFile = dir_ / "state" / "app.ini";
    {
        ShimGamesView view(window_, kept);
        EXPECT_EQ(view.atStop(), ShimGamesView::AtStop::Ask);
        view.setAtStop(ShimGamesView::AtStop::Keep);
    }
    EXPECT_EQ(readAppSetting(kept.stateFile, "shim_at_stop"), "keep");
    {
        ShimGamesView view(window_, kept);
        EXPECT_EQ(view.atStop(), ShimGamesView::AtStop::Keep);
        view.setAtStop(ShimGamesView::AtStop::Delete);
    }
    EXPECT_EQ(ShimGamesView(window_, kept).atStop(), ShimGamesView::AtStop::Delete);
    ShimGamesView(window_, kept).setAtStop(ShimGamesView::AtStop::Ask);
    EXPECT_EQ(readAppSetting(kept.stateFile, "shim_at_stop"), "ask");
}

TEST_F(ShimStart, ATargetThatCannotBeSetStopsStart) {
    listTheGame();
    write(game() / kShimDll, "our shim");
    std::filesystem::create_directories(game() / kShimSettings / "inside");   // a folder where the settings go
    ShimGamesView view(window_, settings());
    std::string error;
    EXPECT_FALSE(prepare(view, "g25", error));
    EXPECT_NE(error.find("could not be set to the g25"), std::string::npos);
    EXPECT_FALSE(prepare(view, "g29", error));   // and never a wheel WheelBri does not emulate
}

// A window that keeps what was drawn, so a test can find a label and click on it, and answers the folder window.
class RecordingWindow final : public ui::UiWindow {
public:
    struct Text {
        float x, y;
        std::string value;
        ui::Colour colour;
    };

    ui::WindowInput pump() override { return {}; }
    [[nodiscard]] bool visible() const override { return true; }
    void begin(ui::Colour) override {}
    void fill(float, float, float, float, ui::Colour) override {}
    void outline(float, float, float, float, ui::Colour) override {}
    void text(const float x, const float y, const std::string_view value, const ui::Colour colour) override {
        texts.push_back({x, y, std::string(value), colour});
    }
    void present() override {}
    bool openFolder(const std::filesystem::path&, std::string&) override { return false; }
    bool pickFolder(const std::string&, std::filesystem::path& folder, std::string& error) override {
        ++picks;
        pickStart = folder;
        if (!pickAnswer) {
            error = pickError;
            return false;
        }
        folder = *pickAnswer;
        return true;
    }
    [[nodiscard]] std::string describe() const override { return "recording"; }

    void setCanvas(const float width, const float height) {
        canvasWidth_ = width;
        canvasHeight_ = height;
    }
    // The `nth` text drawn exactly `value`, or one containing `part` with `contains`.
    [[nodiscard]] const Text* find(const std::string& value, const int nth = 0, const bool contains = false) const {
        int seen = 0;
        for (const Text& text : texts) {
            const bool match = contains ? text.value.find(value) != std::string::npos : text.value == value;
            if (match && seen++ == nth) return &text;
        }
        return nullptr;
    }
    // The first text drawn at `x`.
    [[nodiscard]] const Text* findAt(const float x) const {
        for (const Text& text : texts) {
            if (text.x == x) return &text;
        }
        return nullptr;
    }
    [[nodiscard]] int count(const std::string& value) const {
        int n = 0;
        for (const Text& text : texts) n += text.value == value ? 1 : 0;
        return n;
    }

    std::vector<Text> texts;
    std::optional<std::filesystem::path> pickAnswer;   // nullopt: cancelled (or no folder window, with pickError)
    std::string pickError;
    std::filesystem::path pickStart;
    int picks = 0;
};

bool sameColour(const ui::Colour a, const ui::Colour b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
constexpr ui::Colour kWarningColour = style::kWarning;
constexpr ui::Colour kCautionColour = style::kCaution;
constexpr ui::Colour kHeadingColour = style::kHeading;
constexpr ui::Colour kTextColour = style::kText;

class ShimFolders : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("wheelbri-shimfolders-" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_ / "shim");
        write(dir_ / "shim" / kShimDll, "our shim");
        write(dir_ / "shim" / kShimSettings, "[shim]\ntarget_profile = g920\n");
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    static void write(const std::filesystem::path& path, const std::string& text) {
        std::ofstream(path, std::ios::binary) << text;
    }
    // A game folder (with its .exe, `program`) named `name`.
    [[nodiscard]] std::filesystem::path makeGame(const std::string& name,
                                                 const std::string& program = "Game.exe") const {
        const auto folder = dir_ / name;
        std::filesystem::create_directories(folder);
        write(folder / program, "MZ");
        return folder;
    }
    [[nodiscard]] ShimGamesView::Settings settings() const {
        return {listFile(), ShimSources{dir_ / "shim" / kShimDll, dir_ / "shim" / "x86" / kShimDll},
                dir_ / "shim" / kShimSettings, {}, {}, {}};
    }
    [[nodiscard]] std::filesystem::path listFile() const { return dir_ / "state" / "shim-games.txt"; }
    void listGames(const std::vector<std::filesystem::path>& folders) const {
        ShimGameList list;
        for (const auto& folder : folders) addShimGame(list, folder);
        list.selected = folders.empty() ? std::filesystem::path() : folders.front();
        ASSERT_TRUE(writeShimGames(listFile(), list));
    }
    [[nodiscard]] static GameProfile known(const std::string& program) {
        GameProfile game;
        game.id = "known";
        game.label = "Known Game";
        game.program = program;
        return game;
    }

    RecordingWindow window_;
    std::filesystem::path dir_;
};

// The row next to Start lists the games (there is none to pick: Start readies them all): nothing listed yet (amber),
// one by its name, several with how many first; cut with ".." to the field; plain text while a session runs.
TEST_F(ShimFolders, TheRowListsTheGames) {
    {
        ShimGamesView view(window_, settings());
        view.draw(0, 0, true);
        const RecordingWindow::Text* none = window_.find("no game: add one on the", 0, true);
        ASSERT_NE(none, nullptr);
        EXPECT_EQ(none->value.size(), 27U);   // cut to the field
        EXPECT_TRUE(sameColour(none->colour, kCautionColour));
        EXPECT_FALSE(view.hasGame());
    }
    const auto game = makeGame("Need for Speed Undercover Collector's Edition");
    listGames({game});
    {
        window_.texts.clear();
        ShimGamesView view(window_, settings());
        EXPECT_TRUE(view.hasGame());
        view.draw(0, 0, true);
        const RecordingWindow::Text* row = window_.find("Need for Speed Undercover..");   // 27 characters fit
        ASSERT_NE(row, nullptr);
        EXPECT_TRUE(sameColour(row->colour, kHeadingColour));
        window_.texts.clear();
        view.draw(0, 0, false);   // a session runs: plain text
        EXPECT_TRUE(sameColour(window_.find("Need for Speed Undercover..")->colour, kTextColour));
        EXPECT_NE(window_.find("Games"), nullptr);
    }
    listGames({makeGame("Heat"), makeGame("Rivals")});
    {
        window_.texts.clear();
        ShimGamesView view(window_, settings());
        view.draw(0, 0, true);
        EXPECT_NE(window_.find("2: Heat, Rivals"), nullptr);
    }
}

// The row's Games button opens the Games tab (Settings::openGames) only while changes are allowed; its edges are exact
// (the right and bottom edge are outside), and a second click before the next draw does nothing.
TEST_F(ShimFolders, TheGamesButtonOpensTheGamesTab) {
    int opened = 0;
    ShimGamesView::Settings counted = settings();
    counted.openGames = [&opened] { ++opened; };
    ShimGamesView view(window_, counted);
    view.draw(0, 0, false);
    const RecordingWindow::Text* games = window_.find("Games");
    ASSERT_NE(games, nullptr);
    EXPECT_FALSE(view.onClick(games->x + 1, games->y + 1));   // a session runs: no button
    EXPECT_EQ(opened, 0);

    // The button is at x 274..370, y 0..16 on a row drawn at (0, 0).
    view.draw(0, 0, true);
    EXPECT_FALSE(view.onClick(273.9F, 8));
    EXPECT_FALSE(view.onClick(370, 8));
    EXPECT_FALSE(view.onClick(300, 16));
    EXPECT_FALSE(view.onClick(300, -0.1F));
    EXPECT_EQ(opened, 0);
    EXPECT_TRUE(view.onClick(274, 0));
    EXPECT_EQ(opened, 1);
    EXPECT_FALSE(view.onClick(274, 0));   // no draw since: the button is not there yet
    EXPECT_EQ(opened, 1);

    ShimGamesView unset(window_, settings());   // no callback: the click is taken, nothing happens
    unset.draw(0, 0, true);
    EXPECT_TRUE(unset.onClick(274, 0));
}

// Choose folder: cancelled, no folder window here, a folder with no .exe; then a real game, which is listed but
// gets no shim yet. The folder window starts where it is told.
TEST_F(ShimFolders, ChooseFolderRefusesWhatIsNotAGameFolder) {
    ShimGamesView view(window_, settings());
    EXPECT_FALSE(view.chooseFolder(nullptr, {}));   // cancelled
    EXPECT_EQ(window_.picks, 1);
    EXPECT_TRUE(window_.pickStart.empty());
    std::string problem;
    EXPECT_FALSE(view.takeProblem(problem));

    window_.pickError = "no folder window here";
    EXPECT_FALSE(view.chooseFolder(nullptr, {}));
    ASSERT_TRUE(view.takeProblem(problem));
    EXPECT_EQ(problem, "no folder window here");
    EXPECT_FALSE(view.takeProblem(problem));     // taken once

    std::filesystem::create_directories(dir_ / "Empty");
    window_.pickAnswer = dir_ / "Empty";
    EXPECT_FALSE(view.chooseFolder(nullptr, {}));
    ASSERT_TRUE(view.takeProblem(problem));
    EXPECT_EQ(problem.rfind("Not added: no .exe", 0), 0U) << problem;

    EXPECT_FALSE(view.hasGame());
    EXPECT_FALSE(std::filesystem::exists(listFile()));   // nothing saved for a refused folder

    const auto game = makeGame("NFS Most Wanted");
    window_.pickAnswer = game;
    EXPECT_TRUE(view.chooseFolder(nullptr, dir_));
    EXPECT_EQ(window_.pickStart, dir_);
    EXPECT_FALSE(view.takeProblem(problem)) << problem;
    EXPECT_EQ(view.takeToast(), "Folder added: NFS Most Wanted");
    EXPECT_TRUE(view.hasGame());
    EXPECT_EQ(shimState(game, dir_ / "shim" / kShimDll), ShimState::Missing);   // no shim put in
    EXPECT_EQ(readShimGames(listFile()).selected, game);
    view.refresh(true);
    ASSERT_EQ(view.folders().size(), 1U);
    EXPECT_EQ(view.stateAt(0), ShimState::Missing);
}

// A folder with another mod's dinput8.dll is listed as it is (the Games tab marks it "other mod") and left alone.
TEST_F(ShimFolders, AFolderWithAnotherModIsListedAndLeftAlone) {
    const auto modded = makeGame("Modded");
    write(modded / kShimDll, "someone else's proxy");
    window_.pickAnswer = modded;
    ShimGamesView view(window_, settings());
    EXPECT_TRUE(view.chooseFolder(nullptr, {}));
    view.refresh(true);
    ASSERT_EQ(view.folders().size(), 1U);
    EXPECT_EQ(view.stateAt(0), ShimState::Foreign);
    std::ifstream in(modded / kShimDll, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()),
              "someone else's proxy");
    EXPECT_FALSE(std::filesystem::exists(modded / kShimSettings));
    view.put(0);   // Put shim refuses it
    std::string problem;
    ASSERT_TRUE(view.takeProblem(problem));
    EXPECT_NE(problem.find("not WheelBri's"), std::string::npos) << problem;
}

// Choose folder for a game WheelBri knows: the folder must have its program; then it is listed.
TEST_F(ShimFolders, ChooseFolderForAKnownGameWantsItsProgram) {
    const GameProfile game = known("known.exe");
    const auto other = makeGame("Other");
    window_.pickAnswer = other;
    ShimGamesView view(window_, settings());
    EXPECT_FALSE(view.chooseFolder(&game, {}));
    std::string problem;
    ASSERT_TRUE(view.takeProblem(problem));
    EXPECT_EQ(problem.rfind("Not added: known.exe is not in that folder", 0), 0U) << problem;
    EXPECT_TRUE(view.folders().empty());

    const auto right = makeGame("Known", "known.exe");
    window_.pickAnswer = right;
    EXPECT_TRUE(view.chooseFolder(&game, {}));
    ASSERT_EQ(view.folders().size(), 1U);
    EXPECT_EQ(view.folders()[0].folder, right);
}

// Adding a folder puts it at the end of the list, and a folder that is not a game's is refused.
TEST_F(ShimFolders, AddFolderAddsItToTheList) {
    const auto first = makeGame("First");
    listGames({first});
    ShimGamesView view(window_, settings());
    const auto second = makeGame("Second");
    std::string problem;
    EXPECT_TRUE(view.addFolder(second, problem)) << problem;
    ASSERT_EQ(view.folders().size(), 2U);
    EXPECT_EQ(view.folders()[1].folder, second);
    EXPECT_EQ(readShimGames(listFile()).games.size(), 2U);
    EXPECT_EQ(readShimGames(listFile()).selected, first);   // the older file's line kept as it was

    std::filesystem::create_directories(dir_ / "Empty");
    EXPECT_FALSE(view.addFolder(dir_ / "Empty", problem));
    EXPECT_EQ(problem.rfind("no .exe", 0), 0U) << problem;
    EXPECT_EQ(view.folders().size(), 2U);
}

// Put and remove do what they say and the states follow: no shim, ours, ours outdated; another mod's DLL is left.
TEST_F(ShimFolders, PutAndRemoveChangeTheState) {
    const auto missing = makeGame("Missing");
    const auto foreign = makeGame("Foreign");
    write(foreign / kShimDll, "another mod");
    listGames({missing, foreign});
    ShimGamesView view(window_, settings());
    view.refresh(true);
    EXPECT_EQ(view.stateAt(0), ShimState::Missing);
    EXPECT_EQ(view.stateAt(1), ShimState::Foreign);
    EXPECT_EQ(view.stateAt(9), ShimState::Missing);   // past the end

    view.put(0);
    EXPECT_EQ(view.stateAt(0), ShimState::Installed);
    EXPECT_EQ(view.targetAt(0), "g920");

    write(dir_ / "shim" / kShimDll, "our newer shim");   // a newer build: outdated, put again
    view.refresh(true);
    EXPECT_EQ(view.stateAt(0), ShimState::Outdated);
    view.put(0);
    EXPECT_EQ(view.stateAt(0), ShimState::Installed);

    view.remove(0);
    EXPECT_EQ(view.stateAt(0), ShimState::Missing);
    EXPECT_EQ(view.takeToast(), "Shim deleted from Missing");
    EXPECT_FALSE(std::filesystem::exists(missing / kShimDll));
    EXPECT_FALSE(std::filesystem::exists(missing / kShimSettings));
    EXPECT_TRUE(std::filesystem::exists(missing / "Game.exe"));
    EXPECT_TRUE(std::filesystem::exists(foreign / kShimDll));
    std::string problem;
    EXPECT_FALSE(view.takeProblem(problem)) << problem;

    view.remove(1);   // another mod's: refused with the reason
    ASSERT_TRUE(view.takeProblem(problem));
    EXPECT_NE(problem.find("not WheelBri's"), std::string::npos) << problem;
    EXPECT_TRUE(std::filesystem::exists(foreign / kShimDll));
}

// Put shim with WheelBri's own DLL missing: the problem says why and names the folder.
TEST_F(ShimFolders, APutThatFailsIsAProblem) {
    const auto game = makeGame("Game");
    listGames({game});
    std::filesystem::remove(dir_ / "shim" / kShimDll);
    ShimGamesView view(window_, settings());
    view.put(0);
    std::string problem;
    ASSERT_TRUE(view.takeProblem(problem));
    EXPECT_NE(problem.find("could not be put into Game"), std::string::npos) << problem;
    EXPECT_NE(problem.find("dinput8.dll is missing"), std::string::npos) << problem;
    EXPECT_NE(problem.find(pathToUtf8(game)), std::string::npos) << problem;
    EXPECT_TRUE(view.takeToast().empty());
}

// Forget takes a game off the list only; when our shim is still in its folder the user is told how to delete it.
TEST_F(ShimFolders, ForgetLeavesTheFilesAndSaysSo) {
    const auto installed = makeGame("Installed");
    const auto bare = makeGame("Bare");
    listGames({installed, bare});
    ASSERT_TRUE(installShim(installed, dir_ / "shim" / kShimDll, dir_ / "shim" / kShimSettings).ok);
    ShimGamesView view(window_, settings());
    view.forget(0);   // Installed
    std::string problem;
    ASSERT_TRUE(view.takeProblem(problem));
    EXPECT_NE(problem.find("still in its folder"), std::string::npos) << problem;
    EXPECT_TRUE(std::filesystem::exists(installed / kShimDll));
    ASSERT_EQ(readShimGames(listFile()).games.size(), 1U);
    EXPECT_EQ(view.folders().front().folder, bare);

    view.forget(0);   // Bare: no shim, nothing to say
    EXPECT_FALSE(view.takeProblem(problem)) << problem;
    EXPECT_FALSE(view.hasGame());
    EXPECT_TRUE(readShimGames(listFile()).games.empty());
    view.forget(0);   // nothing left: nothing happens
    EXPECT_TRUE(view.folders().empty());
}

// Deleting the shim after a session: another mod's DLL is refused with the reason; ours goes.
TEST_F(ShimFolders, RemoveFromRefusesAnotherModsDll) {
    const auto game = makeGame("Game");
    write(game / kShimDll, "another mod");
    ShimGamesView view(window_, settings());
    std::string error;
    EXPECT_FALSE(view.removeFrom(game, error));
    EXPECT_NE(error.find("not WheelBri's"), std::string::npos) << error;
    EXPECT_TRUE(std::filesystem::exists(game / kShimDll));
    write(game / kShimDll, "our shim");
    EXPECT_TRUE(view.removeFrom(game, error)) << error;
    EXPECT_FALSE(std::filesystem::exists(game / kShimDll));
}

// The list file cannot be saved (a folder holds its name): a forget still applies for this run, nothing crashes.
TEST_F(ShimFolders, AListThatCannotBeSavedDoesNotStopAChange) {
    const auto first = makeGame("First");
    const auto second = makeGame("Second");
    listGames({first, second});
    ShimGamesView view(window_, settings());
    std::filesystem::remove(listFile());
    std::filesystem::create_directories(listFile());
    view.forget(0);
    ASSERT_EQ(view.folders().size(), 1U);
    EXPECT_EQ(view.folders().front().folder, second);
    EXPECT_TRUE(std::filesystem::is_directory(listFile()));
}

// Hot swap: the shims in the listed folders that have one (current or older) get the target and the pedal axes given
// for their folder; a listed game without the shim and another mod's dinput8.dll are left as they are; targetAt shows
// each shim's target.
TEST_F(ShimFolders, SwapShimsChangesOnlyTheFoldersWithTheShim) {
    const auto current = makeGame("Current");
    const auto older = makeGame("Older");
    const auto none = makeGame("None");
    const auto foreign = makeGame("Foreign");
    write(foreign / kShimDll, "another mod");
    listGames({current, older, none, foreign});
    ShimGamesView view(window_, settings());
    ASSERT_TRUE(installShim(current, dir_ / "shim" / kShimDll, dir_ / "shim" / kShimSettings).ok);
    ASSERT_TRUE(installShim(older, dir_ / "shim" / kShimDll, dir_ / "shim" / kShimSettings).ok);
    write(older / kShimDll, "our older shim");   // ours (next to its settings), not WheelBri's current one
    ASSERT_EQ(shimState(older, dir_ / "shim" / kShimDll), ShimState::Outdated);

    std::vector<std::filesystem::path> asked;
    JoystickLayout swapped = layoutFor("g923", 0);
    std::swap(swapped.throttle, swapped.brake);
    const std::vector<std::string> problems = view.swapShims("g923", [&](const std::filesystem::path& folder) {
        asked.push_back(folder);
        return folder == current ? std::optional<JoystickLayout>(swapped) : std::nullopt;
    });
    EXPECT_TRUE(problems.empty());
    EXPECT_EQ(asked, (std::vector<std::filesystem::path>{current, older}));
    const auto settingsOf = [](const std::filesystem::path& folder) {
        std::ifstream in(folder / kShimSettings, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    EXPECT_NE(settingsOf(current).find("target_profile = g923"), std::string::npos);
    EXPECT_NE(settingsOf(current).find("game_axes = wheelbri"), std::string::npos);
    EXPECT_NE(settingsOf(older).find("target_profile = g923"), std::string::npos);
    EXPECT_EQ(settingsOf(older).find("game_axes"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(none / kShimSettings));      // no shim there: nothing written
    EXPECT_FALSE(std::filesystem::exists(foreign / kShimSettings));   // another mod's: never touched
    std::ifstream mod(foreign / kShimDll, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(mod), {}), "another mod");

    view.refresh(true);
    EXPECT_EQ(view.targetAt(0), "g923");
    EXPECT_EQ(view.targetAt(1), "g923");
    EXPECT_EQ(view.targetAt(2), "");
    EXPECT_EQ(view.targetAt(3), "");
    EXPECT_EQ(view.targetAt(9), "");
}

// A folder whose shim settings cannot be written is said, by game, and the others are swapped all the same.
TEST_F(ShimFolders, ASwapThatFailsIsSaidAndTheOthersGoOn) {
    const auto broken = makeGame("Broken");
    const auto fine = makeGame("Fine");
    listGames({broken, fine});
    ShimGamesView view(window_, settings());
    ASSERT_TRUE(installShim(broken, dir_ / "shim" / kShimDll, dir_ / "shim" / kShimSettings).ok);
    ASSERT_TRUE(installShim(fine, dir_ / "shim" / kShimDll, dir_ / "shim" / kShimSettings).ok);
    std::filesystem::remove(broken / kShimSettings);
    std::filesystem::create_directories(broken / kShimSettings);   // a folder where the settings go
    const std::vector<std::string> problems = view.swapShims("g25", {});
    ASSERT_EQ(problems.size(), 1U);
    EXPECT_EQ(problems[0].rfind("Broken: ", 0), 0U) << problems[0];
    std::ifstream in(fine / kShimSettings, std::ios::binary);
    EXPECT_NE(std::string(std::istreambuf_iterator<char>(in), {}).find("target_profile = g25"), std::string::npos);
}

// The switch: on unless turned off, kept in app.ini; turning it on again (and every change to the folders) counts as
// a change, so the app swaps the shims then.
TEST_F(ShimFolders, HotSwapSwitchIsKept) {
    const auto game = makeGame("Game");
    listGames({game});
    ShimGamesView::Settings kept = settings();
    kept.stateFile = dir_ / "state" / "app.ini";
    ShimGamesView view(window_, kept);
    EXPECT_TRUE(view.hotSwap());
    std::uint64_t before = view.changes();
    view.setHotSwap(false);
    EXPECT_FALSE(view.hotSwap());
    EXPECT_EQ(readAppSetting(kept.stateFile, "shim_hot_swap"), "off");
    EXPECT_FALSE(ShimGamesView(window_, kept).hotSwap());   // kept for the next run
    view.setHotSwap(true);
    EXPECT_TRUE(view.hotSwap());
    EXPECT_GT(view.changes(), before);
    before = view.changes();
    view.setHotSwap(true);   // already on: no change
    EXPECT_EQ(view.changes(), before);
    view.put(0);
    EXPECT_GT(view.changes(), before);
}

// A game WheelBri knows in a folder is shown by its name (the folder's after it); put and hot swap leave a short
// message.
TEST_F(ShimFolders, KnownGamesShowTheirNameAndActionsSayWhatTheyDid) {
    const auto excalibur = makeGame("Excalibur");
    const auto other = makeGame("Other");
    listGames({excalibur, other});
    ShimGamesView::Settings named = settings();
    named.gameLabel = [&excalibur](const std::filesystem::path& folder) {
        return folder == excalibur ? std::string("NFS Unbound") : std::string();
    };
    ShimGamesView view(window_, named);
    EXPECT_EQ(view.displayName(ShimGame{"Excalibur", excalibur}), "NFS Unbound (Excalibur)");
    EXPECT_EQ(view.displayName(ShimGame{"Other", other}), "Other");
    window_.texts.clear();
    view.draw(0, 0, true);
    EXPECT_NE(window_.find("2: NFS Unbound, Other"), nullptr);   // the row: the game's own name
    EXPECT_TRUE(view.takeToast().empty());
    view.put(0);
    EXPECT_EQ(view.takeToast(), "Shim put into NFS Unbound (Excalibur)");
    EXPECT_TRUE(view.takeToast().empty());   // taken once
    EXPECT_TRUE(view.swapShims("g923", {}).empty());
    EXPECT_EQ(view.takeToast(), "Hot swap: 1 game presents the G923");
    EXPECT_TRUE(view.swapShims("g923", {}).empty());   // nothing changed: no message
    EXPECT_TRUE(view.takeToast().empty());
}


// Without WheelBri's shim at all (sources not found) the view still lists and says so; put fails with why.
TEST_F(ShimFolders, NoShimToPutIn) {
    const auto game = makeGame("Heat");
    listGames({game});
    ShimGamesView::Settings without = settings();
    without.sources = ShimSources{};
    ShimGamesView view(window_, without);
    view.put(0);
    std::string problem;
    ASSERT_TRUE(view.takeProblem(problem));
    EXPECT_NE(problem.find("could not be put into"), std::string::npos) << problem;
    std::vector<std::filesystem::path> prepared;
    std::string error;
    EXPECT_FALSE(view.prepareStart("g920", {}, prepared, error));
    EXPECT_NE(error.find("The shim could not be put into"), std::string::npos) << error;
}

// put, remove and forget for a folder that is not listed do nothing.
TEST_F(ShimFolders, IndexesOutOfTheListDoNothing) {
    ShimGamesView view(window_, settings());
    view.put(3);
    view.remove(3);
    view.forget(3);
    std::string problem;
    EXPECT_FALSE(view.takeProblem(problem));
    EXPECT_EQ(view.changes(), 0U);
}

// Forgetting a folder with an older shim of ours says it is still there.
TEST_F(ShimFolders, ForgettingAnOlderShim) {
    const auto game = makeGame("Heat");
    write(game / kShimDll, "an older shim");
    write(game / kShimSettings, "[shim]\n");
    listGames({game});
    ShimGamesView view(window_, settings());
    view.forget(0);
    std::string problem;
    ASSERT_TRUE(view.takeProblem(problem));
    EXPECT_NE(problem.find("WheelBri's shim is still in its folder"), std::string::npos) << problem;
}

// Without a state file the hot swap and the at-Stop choice change for this run only.
TEST_F(ShimFolders, NoStateFile) {
    ShimGamesView view(window_, settings());
    view.setHotSwap(false);
    view.setAtStop(ShimGamesView::AtStop::Keep);
    EXPECT_FALSE(view.hotSwap());
    EXPECT_EQ(view.atStop(), ShimGamesView::AtStop::Keep);
}

// A game whose name is its folder's: shown once, not "Heat (Heat)"; the row lists one game without a count.
TEST_F(ShimFolders, AGameNamedAsItsFolder) {
    const auto game = makeGame("Heat");
    listGames({game});
    ShimGamesView::Settings named = settings();
    named.gameLabel = [](const std::filesystem::path&) { return std::string("Heat"); };
    ShimGamesView view(window_, named);
    EXPECT_EQ(view.displayName(view.folders()[0]), "Heat");
    view.draw(0, 0, true);
    EXPECT_NE(window_.find("Heat"), nullptr);
}

// Hot swap: a game whose pedal axes were set gets them taken out when it has none now; axes that cannot be written
// are said, the others swapped all the same.
TEST_F(ShimFolders, HotSwapPedalAxes) {
    const auto game = makeGame("Heat");
    write(game / kShimDll, "our shim");
    write(game / kShimSettings, "[shim]\ntarget_profile = g920\ngame_axes = wheelbri\nthrottle_axis = y\n"
                                "brake_axis = rz\nclutch_axis = slider\n");
    listGames({game});
    ShimGamesView view(window_, settings());
    EXPECT_TRUE(view.swapShims("g920", [](const std::filesystem::path&) { return std::nullopt; }).empty());
    {
        std::ifstream in(game / kShimSettings, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        EXPECT_EQ(text.find("game_axes"), std::string::npos) << text;
    }
    // The target already as asked (nothing written for it), the axes not writable: said.
    std::filesystem::create_directories(game / (std::string(kShimSettings) + ".tmp") / "x");
    const std::vector<std::string> problems = view.swapShims(
        "g920", [](const std::filesystem::path&) { return std::optional<JoystickLayout>(layoutFor("g920", 0)); });
    ASSERT_EQ(problems.size(), 1U);
    EXPECT_NE(problems[0].find("its pedal axes"), std::string::npos) << problems[0];
    // At Start the same is a reason not to start.
    std::vector<std::filesystem::path> prepared;
    std::string error;
    EXPECT_FALSE(view.prepareStart(
        "g920", [](const std::filesystem::path&) { return std::optional<JoystickLayout>(layoutFor("g920", 0)); },
        prepared, error));
    EXPECT_NE(error.find("The pedal axes for"), std::string::npos) << error;
}

// Start with the game's pedal axes written; two folders with other mods' DLLs: none can take the shim.
TEST_F(ShimFolders, StartWritesTheAxesAndOtherModsAreCounted) {
    const auto game = makeGame("Heat");
    listGames({game});
    {
        ShimGamesView view(window_, settings());
        std::vector<std::filesystem::path> prepared;
        std::string error;
        ASSERT_TRUE(view.prepareStart(
            "g920", [](const std::filesystem::path&) { return std::optional<JoystickLayout>(layoutFor("g920", 0)); },
            prepared, error))
            << error;
        std::ifstream in(game / kShimSettings, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        EXPECT_NE(text.find("game_axes"), std::string::npos) << text;
    }
    const auto a = makeGame("A");
    const auto b = makeGame("B");
    write(a / kShimDll, "another mod");
    write(b / kShimDll, "another mod");
    listGames({a, b});
    ShimGamesView view(window_, settings());
    std::vector<std::filesystem::path> prepared;
    std::string error;
    EXPECT_FALSE(view.prepareStart("g920", {}, prepared, error));
    EXPECT_NE(error.find("and the others hold"), std::string::npos) << error;
}

}  // namespace
}  // namespace wheelbri::service
