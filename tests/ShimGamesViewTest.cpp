// What the app does with the shim's game at Start (service/src/ShimGamesView.cpp, prepareStart): the shim put in or
// brought up to date, and the target written into the game's wheelbri-shim.ini; each refusal says why.

#include "ShimGamesView.hpp"
#include "wheelbri/ConfigLibrary.hpp"

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
                dir_ / "shim" / kShimSettings, {}};
    }
    void listTheGame() const {
        ShimGameList list;
        addShimGame(list, game());
        ASSERT_TRUE(writeShimGames(settings().listFile, list));
    }

    SilentWindow window_;
    std::filesystem::path dir_;
};

TEST_F(ShimStart, NoGameNoStart) {
    ShimGamesView view(window_, settings());
    std::string error;
    EXPECT_FALSE(view.prepareStart("g25", error));
    EXPECT_NE(error.find("No game yet"), std::string::npos);
}

TEST_F(ShimStart, PutsTheShimInAndSetsTheTarget) {
    listTheGame();
    ShimGamesView view(window_, settings());
    std::string error;
    ASSERT_TRUE(view.prepareStart("g25", error)) << error;
    EXPECT_EQ(read(game() / kShimDll), "our shim");
    EXPECT_EQ(read(game() / kShimSettings), "[shim]\ntarget_profile = g25\n");
    // Again with the G920: only the target changes.
    ASSERT_TRUE(view.prepareStart("g920", error)) << error;
    EXPECT_EQ(read(game() / kShimSettings), "[shim]\ntarget_profile = g920\n");
}

TEST_F(ShimStart, AnInstalledShimKeepsTheUsersSettingsButTheTarget) {
    listTheGame();
    write(game() / kShimDll, "our shim");
    write(game() / kShimSettings, "[shim]\nsource_vid = 30b7 ; mine\ntarget_profile = g920\n");
    ShimGamesView view(window_, settings());
    std::string error;
    ASSERT_TRUE(view.prepareStart("g25", error)) << error;
    EXPECT_EQ(read(game() / kShimSettings), "[shim]\nsource_vid = 30b7 ; mine\ntarget_profile = g25\n");
}

TEST_F(ShimStart, AnotherModsDllStopsStart) {
    listTheGame();
    write(game() / kShimDll, "someone else's proxy");
    ShimGamesView view(window_, settings());
    std::string error;
    EXPECT_FALSE(view.prepareStart("g25", error));
    EXPECT_NE(error.find("could not be put into"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(game() / kShimSettings));   // nothing written next to it
}

TEST_F(ShimStart, ATargetThatCannotBeSetStopsStart) {
    listTheGame();
    write(game() / kShimDll, "our shim");
    std::filesystem::create_directories(game() / kShimSettings / "inside");   // a folder where the settings go
    ShimGamesView view(window_, settings());
    std::string error;
    EXPECT_FALSE(view.prepareStart("g25", error));
    EXPECT_NE(error.find("could not be set to the g25"), std::string::npos);
    EXPECT_FALSE(view.prepareStart("g29", error));   // and never a wheel WheelBri does not emulate
}

TEST_F(ShimStart, AClickOutsideTheGamesPanelClosesIt) {
    listTheGame();
    ShimGamesView view(window_, settings());
    // Open it from the row's Games button (wherever it is on the 370-wide row).
    for (float x = 0; x < 370 && !view.panelOpen(); x += 4) {
        view.draw(0, 0, true);
        view.onClick(x, 8);
    }
    ASSERT_TRUE(view.panelOpen());
    view.drawPanel();
    view.onPanelClick(60, 60);   // inside, on no button: stays open
    EXPECT_TRUE(view.panelOpen());
    view.drawPanel();
    view.onPanelClick(5, 5);     // outside (above and left of it): closed
    EXPECT_FALSE(view.panelOpen());
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
constexpr ui::Colour kWarningColour{214, 72, 64};
constexpr ui::Colour kCautionColour{206, 164, 58};
constexpr ui::Colour kHeadingColour{120, 190, 255};
constexpr ui::Colour kTextColour{222, 226, 232};

class ShimPanel : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("wheelbri-shimpanel-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_ / "shim");
        write(dir_ / "shim" / kShimDll, "our shim");
        write(dir_ / "shim" / kShimSettings, "[shim]\ntarget_profile = g920\n");
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    static void write(const std::filesystem::path& path, const std::string& text) {
        std::ofstream(path, std::ios::binary) << text;
    }
    // A game folder (with its .exe) named `name`.
    [[nodiscard]] std::filesystem::path makeGame(const std::string& name) const {
        const auto folder = dir_ / name;
        std::filesystem::create_directories(folder);
        write(folder / "Game.exe", "MZ");
        return folder;
    }
    [[nodiscard]] ShimGamesView::Settings settings() const {
        return {listFile(), ShimSources{dir_ / "shim" / kShimDll, dir_ / "shim" / "x86" / kShimDll},
                dir_ / "shim" / kShimSettings, {}};
    }
    [[nodiscard]] std::filesystem::path listFile() const { return dir_ / "state" / "shim-games.txt"; }
    void listGames(const std::vector<std::filesystem::path>& folders) const {
        ShimGameList list;
        for (const auto& folder : folders) addShimGame(list, folder);
        list.selected = folders.empty() ? std::filesystem::path() : folders.front();
        ASSERT_TRUE(writeShimGames(listFile(), list));
    }
    // Opens the panel from the row's Games button.
    void openPanel(ShimGamesView& view) {
        window_.texts.clear();
        view.draw(0, 0, true);
        const RecordingWindow::Text* games = window_.find("Games (", 0, true);
        ASSERT_NE(games, nullptr);
        ASSERT_TRUE(view.onClick(games->x + 1, games->y + 1));
        ASSERT_TRUE(view.panelOpen());
    }
    // Draws the panel and clicks the `nth` label `value` on it; false when the panel does not show it.
    bool clickPanel(ShimGamesView& view, const std::string& value, const int nth = 0) {
        window_.texts.clear();
        view.drawPanel();
        const RecordingWindow::Text* text = window_.find(value, nth);
        if (text == nullptr) return false;
        view.onPanelClick(text->x + 1, text->y + 1);
        return true;
    }
    void drawPanel(ShimGamesView& view) {
        window_.texts.clear();
        view.drawPanel();
    }

    RecordingWindow window_;
    std::filesystem::path dir_;
};

// The row next to Start says which game Start uses and what is in its folder: nothing listed yet, no shim, another
// mod's DLL (in the warning colour), or ours; a name too long for the field is cut with "..".
TEST_F(ShimPanel, TheRowSaysWhatIsInTheGameFolder) {
    {
        ShimGamesView view(window_, settings());
        view.draw(0, 0, true);
        EXPECT_NE(window_.find("no game: Games, Add game"), nullptr);   // whole: it fits the field
        EXPECT_EQ(view.selectedName(), "");
    }
    const auto game = makeGame("Need for Speed Undercover Collector's Edition");
    listGames({game});
    {
        window_.texts.clear();
        ShimGamesView view(window_, settings());
        view.draw(0, 0, true);
        const RecordingWindow::Text* row = window_.find(" (no shim)", 0, true);
        ASSERT_NE(row, nullptr);
        EXPECT_EQ(row->value, "Need for Speed .. (no shim)");   // 27 characters fit in the field
        EXPECT_TRUE(sameColour(row->colour, kCautionColour));
    }
    write(game / kShimDll, "another mod");
    {
        window_.texts.clear();
        ShimGamesView view(window_, settings());
        view.draw(0, 0, true);
        const RecordingWindow::Text* row = window_.find(" (not ours)", 0, true);
        ASSERT_NE(row, nullptr);
        EXPECT_TRUE(sameColour(row->colour, kWarningColour));
    }
    write(game / kShimDll, "our shim");
    {
        window_.texts.clear();
        ShimGamesView view(window_, settings());
        view.draw(0, 0, true);
        ASSERT_NE(window_.find("Need for Speed Undercover.."), nullptr);
        EXPECT_TRUE(sameColour(window_.find("Need for Speed Undercover..")->colour, kHeadingColour));
        window_.texts.clear();
        view.draw(0, 0, false);   // a session runs: plain text
        EXPECT_TRUE(sameColour(window_.find("Need for Speed Undercover..")->colour, kTextColour));
        EXPECT_NE(window_.find("Games (1)"), nullptr);
    }
}

// The Games button opens the panel only while changes are allowed; its edges are exact (the right and bottom edge
// are outside), and a second click before the next draw does nothing.
TEST_F(ShimPanel, TheGamesButtonHasExactEdgesAndOnlyWorksWhenAllowed) {
    ShimGamesView view(window_, settings());
    view.draw(0, 0, false);
    const RecordingWindow::Text* games = window_.find("Games (0)");
    ASSERT_NE(games, nullptr);
    EXPECT_FALSE(view.onClick(games->x + 1, games->y + 1));   // a session runs: no button
    EXPECT_FALSE(view.panelOpen());

    // The button is at x 274..370, y 0..16 on a row drawn at (0, 0).
    view.draw(0, 0, true);
    EXPECT_FALSE(view.onClick(273.9F, 8));
    EXPECT_FALSE(view.onClick(370, 8));
    EXPECT_FALSE(view.onClick(300, 16));
    EXPECT_FALSE(view.onClick(300, -0.1F));
    EXPECT_FALSE(view.panelOpen());
    EXPECT_TRUE(view.onClick(274, 0));
    EXPECT_TRUE(view.panelOpen());
    view.closePanel();
    EXPECT_FALSE(view.onClick(274, 0));   // no draw since: the button is not there yet
    EXPECT_FALSE(view.panelOpen());
}

// The panel's edges: its top-left corner is inside (stays open), its right and bottom edges are outside (closes).
TEST_F(ShimPanel, ThePanelsEdgesAreExact) {
    ShimGamesView view(window_, settings());
    openPanel(view);
    drawPanel(view);
    view.onPanelClick(24, 40);            // the top-left corner, on no button
    EXPECT_TRUE(view.panelOpen());
    drawPanel(view);
    view.onPanelClick(24 + 912, 100);     // just right of it (912 wide on the 960 design)
    EXPECT_FALSE(view.panelOpen());
    openPanel(view);
    drawPanel(view);
    view.onPanelClick(100, 40 + 80 + 22 + 30);   // just below it (one row: 80 + 22 + 30 tall)
    EXPECT_FALSE(view.panelOpen());
    openPanel(view);
    EXPECT_TRUE(clickPanel(view, "Close"));
    EXPECT_FALSE(view.panelOpen());
}

// Add game: cancelled, no folder window here, a folder with no .exe, a folder with another mod's dinput8.dll; then a
// real game, which gets the shim, is picked and saved. The folder window starts next to the last game.
TEST_F(ShimPanel, AddGameRefusesWhatIsNotAGameFolder) {
    ShimGamesView view(window_, settings());
    openPanel(view);
    drawPanel(view);
    EXPECT_NE(window_.find("No game yet", 0, true), nullptr);

    ASSERT_TRUE(clickPanel(view, "Add game"));   // cancelled
    EXPECT_EQ(window_.picks, 1);
    EXPECT_TRUE(window_.pickStart.empty());      // no game yet: the window starts wherever the system likes
    std::string problem;
    EXPECT_FALSE(view.takeProblem(problem));

    window_.pickError = "no folder window here";
    ASSERT_TRUE(clickPanel(view, "Add game"));
    ASSERT_TRUE(view.takeProblem(problem));
    EXPECT_EQ(problem, "no folder window here");
    EXPECT_FALSE(view.takeProblem(problem));     // taken once

    std::filesystem::create_directories(dir_ / "Empty");
    window_.pickAnswer = dir_ / "Empty";
    ASSERT_TRUE(clickPanel(view, "Add game"));
    ASSERT_TRUE(view.takeProblem(problem));
    EXPECT_EQ(problem.rfind("Not added: no .exe", 0), 0U) << problem;

    const auto modded = makeGame("Modded");
    write(modded / kShimDll, "someone else's proxy");
    window_.pickAnswer = modded;
    ASSERT_TRUE(clickPanel(view, "Add game"));
    ASSERT_TRUE(view.takeProblem(problem));
    EXPECT_NE(problem.find("not WheelBri's"), std::string::npos) << problem;
    std::ifstream in(modded / kShimDll, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()),
              "someone else's proxy");
    EXPECT_FALSE(view.hasGame());
    EXPECT_FALSE(std::filesystem::exists(listFile()));   // nothing saved for a refused game

    const auto game = makeGame("NFS Most Wanted");
    window_.pickAnswer = game;
    ASSERT_TRUE(clickPanel(view, "Add game"));
    EXPECT_FALSE(view.takeProblem(problem)) << problem;
    EXPECT_EQ(view.selectedFolder(), game);
    EXPECT_EQ(view.selectedName(), "NFS Most Wanted");
    EXPECT_EQ(shimState(game, dir_ / "shim" / kShimDll), ShimState::Installed);
    EXPECT_EQ(readShimGames(listFile()).selected, game);
    drawPanel(view);
    EXPECT_NE(window_.find("shim in place"), nullptr);
    EXPECT_NE(window_.find("Remove shim"), nullptr);

    window_.pickAnswer.reset();
    ASSERT_TRUE(clickPanel(view, "Add game"));
    EXPECT_EQ(window_.pickStart, dir_);          // next to the last game
}

// A click on a game's name picks the one Start uses, and the choice is saved.
TEST_F(ShimPanel, ClickingAGamePicksIt) {
    const auto first = makeGame("First");
    const auto second = makeGame("Second");
    listGames({first, second});
    ShimGamesView view(window_, settings());
    EXPECT_EQ(view.selectedFolder(), first);
    openPanel(view);
    ASSERT_TRUE(clickPanel(view, "Second"));
    EXPECT_EQ(view.selectedFolder(), second);
    EXPECT_EQ(readShimGames(listFile()).selected, second);
    EXPECT_TRUE(view.panelOpen());
}

// Each game's buttons follow its state: Put shim when there is none or ours is outdated, Remove shim when ours is
// current, neither for another mod's DLL (shown in the warning colour); the buttons do what they say.
TEST_F(ShimPanel, PutAndRemoveFollowTheState) {
    const auto missing = makeGame("Missing");
    const auto foreign = makeGame("Foreign");
    write(foreign / kShimDll, "another mod");
    listGames({missing, foreign});
    ShimGamesView view(window_, settings());
    openPanel(view);
    drawPanel(view);
    EXPECT_EQ(window_.count("Put shim"), 1);
    EXPECT_EQ(window_.count("Remove shim"), 0);
    EXPECT_EQ(window_.count("Forget"), 2);
    const RecordingWindow::Text* other = window_.find("another dinput8.dll");
    ASSERT_NE(other, nullptr);
    EXPECT_TRUE(sameColour(other->colour, kWarningColour));

    ASSERT_TRUE(clickPanel(view, "Put shim"));
    EXPECT_EQ(shimState(missing, dir_ / "shim" / kShimDll), ShimState::Installed);
    drawPanel(view);
    EXPECT_EQ(window_.count("Put shim"), 0);
    EXPECT_EQ(window_.count("Remove shim"), 1);

    write(dir_ / "shim" / kShimDll, "our newer shim");   // a newer build: outdated, Put shim again
    view.closePanel();
    openPanel(view);
    drawPanel(view);
    EXPECT_NE(window_.find("shim outdated"), nullptr);
    ASSERT_TRUE(clickPanel(view, "Put shim"));
    EXPECT_EQ(shimState(missing, dir_ / "shim" / kShimDll), ShimState::Installed);

    ASSERT_TRUE(clickPanel(view, "Remove shim"));
    EXPECT_FALSE(std::filesystem::exists(missing / kShimDll));
    EXPECT_FALSE(std::filesystem::exists(missing / kShimSettings));
    EXPECT_TRUE(std::filesystem::exists(missing / "Game.exe"));
    EXPECT_TRUE(std::filesystem::exists(foreign / kShimDll));
    std::string problem;
    EXPECT_FALSE(view.takeProblem(problem)) << problem;
}

// Put shim with WheelBri's own DLL missing: the panel's problem says why and names the folder.
TEST_F(ShimPanel, APutThatFailsIsAProblem) {
    const auto game = makeGame("Game");
    listGames({game});
    std::filesystem::remove(dir_ / "shim" / kShimDll);
    ShimGamesView view(window_, settings());
    openPanel(view);
    ASSERT_TRUE(clickPanel(view, "Put shim"));
    std::string problem;
    ASSERT_TRUE(view.takeProblem(problem));
    EXPECT_NE(problem.find("could not be put into Game"), std::string::npos) << problem;
    EXPECT_NE(problem.find("dinput8.dll is missing"), std::string::npos) << problem;
    EXPECT_NE(problem.find(pathToUtf8(game)), std::string::npos) << problem;
}

// Forget takes a game off the list only; when our shim is still in its folder the user is told how to delete it.
TEST_F(ShimPanel, ForgetLeavesTheFilesAndSaysSo) {
    const auto installed = makeGame("Installed");
    const auto bare = makeGame("Bare");
    listGames({installed, bare});
    ASSERT_TRUE(installShim(installed, dir_ / "shim" / kShimDll, dir_ / "shim" / kShimSettings).ok);
    ShimGamesView view(window_, settings());
    openPanel(view);
    ASSERT_TRUE(clickPanel(view, "Forget", 0));   // Installed
    std::string problem;
    ASSERT_TRUE(view.takeProblem(problem));
    EXPECT_NE(problem.find("still in its folder"), std::string::npos) << problem;
    EXPECT_TRUE(std::filesystem::exists(installed / kShimDll));
    EXPECT_EQ(view.selectedFolder(), bare);       // the first game left
    ASSERT_EQ(readShimGames(listFile()).games.size(), 1U);

    ASSERT_TRUE(clickPanel(view, "Forget", 0));   // Bare: no shim, nothing to say
    EXPECT_FALSE(view.takeProblem(problem)) << problem;
    EXPECT_FALSE(view.hasGame());
    EXPECT_TRUE(readShimGames(listFile()).games.empty());
}

// More games than the panel has room for: it stops at its bottom edge; a wider window gives the folders more room.
TEST_F(ShimPanel, ManyGamesStayInsideThePanel) {
    std::vector<std::filesystem::path> folders;
    for (int i = 0; i < 40; ++i) {
        folders.push_back(dir_ / ("a folder with a rather long name to show how much of it fits " + std::to_string(i)));
    }
    listGames(folders);
    ShimGamesView view(window_, settings());
    openPanel(view);
    drawPanel(view);
    const int rows = window_.count("Forget");
    EXPECT_GT(rows, 10);
    EXPECT_LT(rows, 40);   // the 748-tall design has room for about 27
    const RecordingWindow::Text* folder = window_.findAt(194);   // the first game's folder
    ASSERT_NE(folder, nullptr);
    EXPECT_EQ(folder->value.size(), 39U);   // room left for the state and the wheel its shim presents
    EXPECT_EQ(folder->value.substr(37), "..");

    window_.setCanvas(1200, 1000);   // 240 wider: 30 more characters of the folder; taller: more rows
    drawPanel(view);
    EXPECT_GT(window_.count("Forget"), rows);
    folder = window_.findAt(194);
    ASSERT_NE(folder, nullptr);
    EXPECT_EQ(folder->value.size(), 69U);
}

// Deleting the shim after a session: another mod's DLL is refused with the reason; ours goes.
TEST_F(ShimPanel, RemoveFromRefusesAnotherModsDll) {
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

// The list file cannot be saved (a folder holds its name): the pick still applies for this run, nothing crashes.
TEST_F(ShimPanel, AListThatCannotBeSavedDoesNotStopThePick) {
    const auto first = makeGame("First");
    const auto second = makeGame("Second");
    listGames({first, second});
    ShimGamesView view(window_, settings());
    std::filesystem::remove(listFile());
    std::filesystem::create_directories(listFile());
    openPanel(view);
    ASSERT_TRUE(clickPanel(view, "Second"));
    EXPECT_EQ(view.selectedFolder(), second);
    EXPECT_TRUE(std::filesystem::is_directory(listFile()));
}

// Hot swap: the shims in the listed folders that have one (current or older) get the target and the pedal axes given
// for their folder; a listed game without the shim and another mod's dinput8.dll are left as they are; the panel shows
// each shim's target.
TEST_F(ShimPanel, SwapShimsChangesOnlyTheFoldersWithTheShim) {
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

    openPanel(view);
    drawPanel(view);
    EXPECT_EQ(window_.count("g923"), 2);
}

// A folder whose shim settings cannot be written is said, by game, and the others are swapped all the same.
TEST_F(ShimPanel, ASwapThatFailsIsSaidAndTheOthersGoOn) {
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

// The switch: on unless turned off, kept in app.ini; turning it on again (and every change to the list) counts as a
// change, so the app swaps the shims then.
TEST_F(ShimPanel, HotSwapSwitchIsKept) {
    const auto game = makeGame("Game");
    listGames({game});
    ShimGamesView::Settings kept = settings();
    kept.stateFile = dir_ / "state" / "app.ini";
    ShimGamesView view(window_, kept);
    EXPECT_TRUE(view.hotSwap());
    openPanel(view);
    std::uint64_t before = view.changes();
    ASSERT_TRUE(clickPanel(view, "Hot swap: on"));
    EXPECT_FALSE(view.hotSwap());
    EXPECT_EQ(readAppSetting(kept.stateFile, "shim_hot_swap"), "off");
    EXPECT_FALSE(ShimGamesView(window_, kept).hotSwap());   // kept for the next run
    ASSERT_TRUE(clickPanel(view, "Hot swap: off"));
    EXPECT_TRUE(view.hotSwap());
    EXPECT_GT(view.changes(), before);
    before = view.changes();
    ASSERT_TRUE(clickPanel(view, "Put shim"));
    EXPECT_GT(view.changes(), before);
}

}  // namespace
}  // namespace wheelbri::service
