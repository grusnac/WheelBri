// SPDX-License-Identifier: GPL-3.0-or-later
// The Games tab learns a game's settings file by seeing the game write it (service/src/MappingEditView.cpp,
// setGameNow and learnGameFile): a located file the game was not seen writing is shown as not confirmed (it may be
// another game's: NFS Heat's PROFILEOPTIONS holds NFS Unbound's settings too); once the game, ending, has written it,
// it is kept in app.ini as game_file_seen.<id> and confirmed.

#include "ConfigPicker.hpp"
#include "MappingEditView.hpp"

#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/FileText.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
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

    [[nodiscard]] const Text* startingWith(const std::string& value) const {
        for (const Text& text : texts) {
            if (text.value.rfind(value, 0) == 0) return &text;
        }
        return nullptr;
    }
};

class GameFileSeen : public ::testing::Test {
protected:
    void SetUp() override {
        // Short (the Game file sub-tab shows the file's path, cut at its end when long): one per test.
        const std::string test = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        dir_ = std::filesystem::temp_directory_path() / ("wbfs-" + std::to_string(test.size()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_ / "config");
        std::filesystem::create_directories(dir_ / "configs");
        std::filesystem::create_directories(dir_ / "rigs");
        for (const char* name : {"g920.ini", "g25.ini", "g27.ini", "g923.ini"}) {
            std::ofstream(dir_ / "config" / name, std::ios::binary) << "[emulator]\n";
        }
        // A file in a folder not named like NFS Unbound's (where a game of another name keeps its own).
        file_ = dir_ / "Documents" / "Other" / "settings" / "PROFILEOPTIONS";
        std::filesystem::create_directories(file_.parent_path());
        ASSERT_TRUE(writeFileReplacing(file_, "FBCHUNKS"));
        std::filesystem::last_write_time(file_, std::filesystem::file_time_type::clock::now() - std::chrono::hours(3));
        ASSERT_TRUE(writeAppSetting(dir_ / "app.ini", "game_file.nfsunbound", file_.string()));
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    [[nodiscard]] ConfigPicker::Settings settings() const {
        return {dir_ / "config", dir_ / "configs", dir_ / "app.ini"};
    }
    void frame(MappingEditView& view) {
        window_.texts.clear();
        view.update();
        view.draw();
    }
    // The Games page, NFS Unbound, its Game file sub-tab.
    void showGameFile(MappingEditView& view) {
        view.setPage(MappingEditView::Page::Games);
        frame(view);   // the games are read at the first draw
        view.selectGame("nfsunbound");
        frame(view);
        const RecordingWindow::Text* const tab = window_.startingWith("Game file");
        ASSERT_NE(tab, nullptr);
        view.onClick(tab->x + 1, tab->y + 1);
        frame(view);
        ASSERT_NE(window_.startingWith("GAME CONFIG"), nullptr);
    }
    [[nodiscard]] bool notConfirmed() const {
        return window_.startingWith("not seen written by NFS Unbound") != nullptr;
    }
    [[nodiscard]] std::string seen() const { return readAppSetting(dir_ / "app.ini", "game_file_seen.nfsunbound"); }
    // NFS Unbound runs (seen for `forMs`), then ends; `writes`: it writes its file meanwhile.
    void play(MappingEditView& view, const bool writes) {
        MappingEditView::GameNow running;
        running.session = true;
        running.id = "nfsunbound";
        running.forMs = 60000;
        view.setGameNow(running);
        if (writes) std::filesystem::last_write_time(file_, std::filesystem::file_time_type::clock::now());
        MappingEditView::GameNow ended;
        ended.session = true;
        view.setGameNow(ended);
    }

    RecordingWindow window_;
    std::filesystem::path dir_;
    std::filesystem::path file_;
};

TEST_F(GameFileSeen, AFileTheGameWroteIsConfirmed) {
    ConfigPicker picker(window_, settings());
    MappingEditView view(window_, dir_ / "rigs", dir_ / "app.ini", picker);
    showGameFile(view);
    EXPECT_TRUE(notConfirmed());

    play(view, false);   // the game ran, the file was not written: still not its own
    EXPECT_EQ(seen(), "");
    frame(view);
    EXPECT_TRUE(notConfirmed());

    play(view, true);
    EXPECT_EQ(seen(), file_.string());
    frame(view);
    EXPECT_FALSE(notConfirmed());

    // Kept for the next start.
    ConfigPicker again(window_, settings());
    MappingEditView fresh(window_, dir_ / "rigs", dir_ / "app.ini", again);
    showGameFile(fresh);
    EXPECT_FALSE(notConfirmed());
}

// A located file the game never wrote (another game's) gives way to the one the game was seen writing.
TEST_F(GameFileSeen, TheFileTheGameWroteWinsOverAnotherGamesFile) {
    const std::filesystem::path heat = dir_ / "Documents" / "Need for Speed Heat" / "settings" / "PROFILEOPTIONS";
    std::filesystem::create_directories(heat.parent_path());
    ASSERT_TRUE(writeFileReplacing(heat, "FBCHUNKS"));
    ASSERT_TRUE(writeAppSetting(dir_ / "app.ini", "game_file.nfsunbound", heat.string()));
    ConfigPicker picker(window_, settings());
    MappingEditView view(window_, dir_ / "rigs", dir_ / "app.ini", picker);
    showGameFile(view);
    EXPECT_TRUE(notConfirmed());

    // Seen writing its own file (file_) when it last ended: that one is used, confirmed.
    ASSERT_TRUE(writeAppSetting(dir_ / "app.ini", "game_file_seen.nfsunbound", file_.string()));
    frame(view);
    EXPECT_FALSE(notConfirmed());
    // The File line: the path, maybe cut at its end ("..").
    const std::string start = dir_.string();
    const RecordingWindow::Text* const line = window_.startingWith(start);
    ASSERT_NE(line, nullptr);
    std::string shown = line->value;
    if (shown.size() > 2 && shown.substr(shown.size() - 2) == "..") shown.resize(shown.size() - 2);
    ASSERT_GT(shown.size(), (dir_ / "Documents").string().size() + 1);
    EXPECT_EQ(file_.string().rfind(shown, 0), 0U) << shown;
    EXPECT_NE(heat.string().rfind(shown, 0), 0U) << shown;
}

}  // namespace
}  // namespace wheelbri::service
