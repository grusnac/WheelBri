// SPDX-License-Identifier: GPL-3.0-or-later
// The Games tab's list (service/src/MappingEditView.cpp, drawGames): a name is cut only where its tag leaves no room
// (its hover text has it whole), the bar right of the list drags its width between 196 and what the canvas allows, a
// double-click on the bar fits the longest name, and the width is kept in app.ini (games_list_width).

#include "ConfigPicker.hpp"
#include "MappingEditView.hpp"
#include "ServiceLogging.hpp"

#include "wheelbri/ConfigLibrary.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace wheelbri::service {
namespace {

class RecordingWindow final : public ui::UiWindow {
public:
    struct Text {
        float x, y;
        std::string value;
    };
    std::vector<Text> texts;

    ui::WindowInput pump() override { return {}; }
    [[nodiscard]] bool visible() const override { return true; }
    void begin(ui::Colour) override {}
    void fill(float, float, float, float, ui::Colour) override {}
    void outline(float, float, float, float, ui::Colour) override {}
    void text(const float x, const float y, const std::string_view value, ui::Colour) override {
        texts.push_back({x, y, std::string(value)});
    }
    void present() override {}
    bool openFolder(const std::filesystem::path&, std::string&) override { return false; }
    bool pickFolder(const std::string&, std::filesystem::path&, std::string&) override { return false; }
    [[nodiscard]] std::string describe() const override { return "recording"; }

    [[nodiscard]] const Text* find(const std::string& value) const {
        for (const Text& text : texts) {
            if (text.value == value) return &text;
        }
        return nullptr;
    }
    [[nodiscard]] const Text* startingWith(const std::string& value) const {
        for (const Text& text : texts) {
            if (text.value.rfind(value, 0) == 0) return &text;
        }
        return nullptr;
    }
};

// Points the user's settings folder (where games.ini is) at a test folder while it lives.
class SettingsHome {
public:
    explicit SettingsHome(const std::filesystem::path& dir) {
        if (const char* value = std::getenv(kName)) before_ = value;
        set(dir.string());
    }
    ~SettingsHome() { set(before_.value_or("")); }
    SettingsHome(const SettingsHome&) = delete;
    SettingsHome& operator=(const SettingsHome&) = delete;

private:
#if defined(_WIN32)
    static constexpr const char* kName = "LOCALAPPDATA";
    static void set(const std::string& value) { _putenv_s(kName, value.c_str()); }
#else
    static constexpr const char* kName = "XDG_CONFIG_HOME";
    static void set(const std::string& value) {
        if (value.empty()) {
            unsetenv(kName);
        } else {
            setenv(kName, value.c_str(), 1);
        }
    }
#endif
    std::optional<std::string> before_;
};

constexpr const char* kLongName = "Need for Speed Most Wanted";

class GamesListWidth : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("wheelbri-listwidth-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_ / "config");
        std::filesystem::create_directories(dir_ / "configs");
        std::filesystem::create_directories(dir_ / "rigs");
        for (const char* name : {"g920.ini", "g25.ini", "g27.ini", "g923.ini"}) {
            std::ofstream(dir_ / "config" / name, std::ios::binary) << "[emulator]\n";
        }
        home_.emplace(dir_);
        std::filesystem::create_directories(userGamesFile().parent_path());
        std::ofstream(userGamesFile(), std::ios::binary)
            << "[game.nfs13]\nlabel = " << kLongName << "\nprogram = nfs13.exe\n";
    }
    void TearDown() override {
        home_.reset();
        std::filesystem::remove_all(dir_);
    }

    [[nodiscard]] ConfigPicker::Settings settings() const {
        return {dir_ / "config", dir_ / "configs", dir_ / "app.ini"};
    }
    void frame(MappingEditView& view) {
        window_.texts.clear();
        view.setPage(MappingEditView::Page::Games);
        view.update();
        view.draw();
    }
    // Where the list's bar is: right of the list, from the top of the Games page.
    [[nodiscard]] float barX() const {
        const RecordingWindow::Text* const tag = window_.find("yours");
        return tag != nullptr ? tag->x + 5 * 8 + 6 + 4 : 0;   // the tag ends 6 before the list's edge, the bar 4 after
    }
    [[nodiscard]] std::string kept() const { return readAppSetting(dir_ / "app.ini", "games_list_width"); }

    RecordingWindow window_;
    std::filesystem::path dir_;
    std::optional<SettingsHome> home_;
};

// At 196 the long name is cut (its tag stays whole); a double-click on the bar fits it and keeps the width.
TEST_F(GamesListWidth, ADoubleClickFitsTheLongestName) {
    ConfigPicker picker(window_, settings());
    MappingEditView view(window_, dir_ / "rigs", dir_ / "app.ini", picker);
    frame(view);
    ASSERT_NE(window_.find("yours"), nullptr);
    EXPECT_EQ(window_.find(kLongName), nullptr);
    const RecordingWindow::Text* const cut = window_.startingWith("Need for Speed");
    ASSERT_NE(cut, nullptr);
    EXPECT_EQ(cut->value.substr(cut->value.size() - 2), "..");

    const float x = barX();
    view.onClick(x + 2, 300);
    frame(view);
    view.onClick(x + 2, 300);   // the second press, at once: a double-click
    frame(view);
    ASSERT_NE(window_.find(kLongName), nullptr);
    // 6 + 26 characters + 8 + "yours" + 6 = 268
    EXPECT_EQ(kept(), "268");

    // A new view (the next start) has the same width.
    ConfigPicker again(window_, settings());
    MappingEditView fresh(window_, dir_ / "rigs", dir_ / "app.ini", again);
    frame(fresh);
    EXPECT_NE(window_.find(kLongName), nullptr);
}

// Dragging the bar moves the list's edge with the mouse, between 196 and what leaves the game's side room (320 on the
// 960 canvas); the width is kept when the button is let go.
TEST_F(GamesListWidth, DraggingTheBarSetsTheWidth) {
    ConfigPicker picker(window_, settings());
    MappingEditView view(window_, dir_ / "rigs", dir_ / "app.ini", picker);
    frame(view);
    const float x = barX();
    view.onClick(x + 2, 300);
    view.onPointer(x + 2 + 40, 300, true);
    view.onPointer(x + 2 + 80, 300, true);
    EXPECT_EQ(kept(), "");   // not yet: the button is still held
    view.onPointer(x + 2 + 80, 300, false);
    EXPECT_EQ(kept(), "276");
    frame(view);
    EXPECT_NE(window_.find(kLongName), nullptr);

    // Far right: as wide as the canvas allows; far left: never narrower than 196.
    view.onClick(barX() + 2, 300);
    view.onPointer(900, 300, true);
    view.onPointer(900, 300, false);
    EXPECT_EQ(kept(), "320");
    frame(view);
    view.onClick(barX() + 2, 500);
    view.onPointer(0, 500, true);
    view.onPointer(0, 500, false);
    EXPECT_EQ(kept(), "196");
}

}  // namespace
}  // namespace wheelbri::service
