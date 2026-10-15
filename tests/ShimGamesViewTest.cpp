// What the app does with the shim's game at Start (service/src/ShimGamesView.cpp, prepareStart): the shim put in or
// brought up to date, and the target written into the game's wheelbri-shim.ini; each refusal says why.

#include "ShimGamesView.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

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
                dir_ / "shim" / kShimSettings};
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

}  // namespace
}  // namespace wheelbri::service
