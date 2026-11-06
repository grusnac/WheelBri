// SPDX-License-Identifier: GPL-3.0-or-later
// Remove on the Games tab (service/src/MappingEditView.cpp, removeGame): a game the user added goes off games.ini
// with its own values, even when the page showed an error before (for something else: here the config's problems),
// and the folder Add game put on the list goes with it (files left), unless WheelBri's shim is in it.

#include "ConfigPicker.hpp"
#include "MappingEditView.hpp"
#include "ServiceLogging.hpp"
#include "ShimGamesView.hpp"

#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/FileText.hpp"
#include "wheelbri/ShimGames.hpp"

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

class GameRemove : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string test = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        dir_ = std::filesystem::temp_directory_path() / ("wheelbri-remove-" + test);
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_ / "config");
        std::filesystem::create_directories(dir_ / "configs");
        std::filesystem::create_directories(dir_ / "rigs");
        for (const char* name : {"g25.ini", "g27.ini", "g923.ini"}) {
            std::ofstream(dir_ / "config" / name, std::ios::binary) << "[emulator]\n";
        }
        // A config with a problem: the page says so (an error) from the start.
        std::ofstream(dir_ / "config" / "g920.ini", std::ios::binary) << "[emulator]\nno_such_key = 1\n";
        home_.emplace(dir_);
        std::filesystem::create_directories(userGamesFile().parent_path());
        std::ofstream(userGamesFile(), std::ios::binary)
            << "[game.needforspeedshift]\nlabel = NeedForSpeedShift\nprogram = needforspeedshift.exe\n";
        // Its folder, on the list as Add game puts it.
        folder_ = dir_ / "games" / "Need for Speed Shift";
        std::filesystem::create_directories(folder_);
        std::ofstream(folder_ / "NeedForSpeedShift.exe", std::ios::binary) << "MZ";
        ShimGameList list;
        addShimGame(list, folder_);
        ASSERT_TRUE(writeShimGames(listFile(), list));
    }
    void TearDown() override {
        home_.reset();
        std::filesystem::remove_all(dir_);
    }

    [[nodiscard]] ConfigPicker::Settings settings() const {
        return {dir_ / "config", dir_ / "configs", dir_ / "app.ini"};
    }
    [[nodiscard]] std::filesystem::path listFile() const { return dir_ / "state" / "shim-games.txt"; }
    [[nodiscard]] ShimGamesView::Settings shimSettings() const {
        return {listFile(), ShimSources{dir_ / "shim" / kShimDll, {}}, dir_ / "shim" / kShimSettings, {}, {}, {}};
    }
    void frame(MappingEditView& view) {
        window_.texts.clear();
        view.update();
        view.draw();
    }
    bool click(MappingEditView& view, const std::string& value) {
        frame(view);
        const RecordingWindow::Text* const text = window_.startingWith(value);
        if (text == nullptr) return false;
        view.onClick(text->x + 1, text->y + 1);
        return true;
    }
    [[nodiscard]] static std::string read(const std::filesystem::path& path) {
        return readFileText(path).value_or("");
    }

    RecordingWindow window_;
    std::filesystem::path dir_;
    std::filesystem::path folder_;
    std::optional<SettingsHome> home_;
};

TEST_F(GameRemove, RemoveWorksAfterAnErrorAndTakesItsFolderOffTheList) {
    ConfigPicker picker(window_, settings());
    ShimGamesView shims(window_, shimSettings());
    MappingEditView view(window_, dir_ / "rigs", dir_ / "app.ini", picker);
    view.setShimGames(&shims);
    view.setPage(MappingEditView::Page::Games);
    frame(view);
    ASSERT_NE(window_.startingWith("The config has problems"), nullptr);   // an error on the page already
    ASSERT_TRUE(click(view, "NeedForSpeed"));   // its row (the name may be cut)
    ASSERT_TRUE(click(view, "Remove"));
    EXPECT_EQ(read(userGamesFile()).find("needforspeedshift"), std::string::npos);
    frame(view);
    EXPECT_NE(window_.startingWith("NeedForSpeedShift removed"), nullptr);
    // Not in the list any more, as a game or as a folder; its files are still there.
    for (const RecordingWindow::Text& text : window_.texts) {
        if (text.value.rfind("NeedForSpeedShift removed", 0) == 0) continue;   // the status line
        EXPECT_NE(text.value.rfind("NeedForSpeedS", 0), 0U) << text.value;
    }
    EXPECT_EQ(window_.startingWith("Need for Speed"), nullptr);
    EXPECT_TRUE(shims.folders().empty());
    EXPECT_TRUE(std::filesystem::exists(folder_ / "NeedForSpeedShift.exe"));
}

TEST_F(GameRemove, AFolderWithTheShimInItStaysListed) {
    std::filesystem::create_directories(dir_ / "shim");
    ASSERT_TRUE(writeFileReplacing(dir_ / "shim" / kShimDll, "WheelBri shim"));
    ConfigPicker picker(window_, settings());
    ShimGamesView shims(window_, shimSettings());
    shims.put(0);
    ASSERT_EQ(shims.stateAt(0), ShimState::Installed);
    MappingEditView view(window_, dir_ / "rigs", dir_ / "app.ini", picker);
    view.setShimGames(&shims);
    view.setPage(MappingEditView::Page::Games);
    frame(view);
    ASSERT_TRUE(click(view, "NeedForSpeed"));   // its row (the name may be cut)
    ASSERT_TRUE(click(view, "Remove"));
    EXPECT_EQ(read(userGamesFile()).find("needforspeedshift"), std::string::npos);
    EXPECT_EQ(shims.folders().size(), 1U);
    frame(view);
    EXPECT_NE(window_.startingWith("NeedForSpeedShift removed, with its own values. Its folder stays listed"), nullptr);
}

}  // namespace
}  // namespace wheelbri::service
