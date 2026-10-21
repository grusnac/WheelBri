// The Mapping tab saves as you change (service/src/MappingEditView.cpp, autoSave and undo): a change is written at
// once (WheelBri's default stays as it is: the change goes to the user's own file), Undo goes back one change and
// saves that, and the list starts again with another Target.

#include "ConfigPicker.hpp"
#include "MappingEditView.hpp"
#include "UiStyle.hpp"

#include "wheelbri/ConfigLibrary.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wheelbri::service {
namespace {

class RecordingWindow final : public ui::UiWindow {
public:
    struct Text {
        float x, y;
        std::string value;
    };
    struct Fill {
        float x, y, w, h;
        ui::Colour colour;
    };
    std::vector<Text> texts;
    std::vector<Fill> fills;

    ui::WindowInput pump() override { return {}; }
    [[nodiscard]] bool visible() const override { return true; }
    void begin(ui::Colour) override {}
    void fill(const float x, const float y, const float w, const float h, const ui::Colour colour) override {
        fills.push_back({x, y, w, h, colour});
    }
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
    // The colour of the button under the text `value` (the last fill holding its first character).
    [[nodiscard]] std::optional<ui::Colour> buttonColour(const std::string& value) const {
        const Text* const text = find(value);
        if (text == nullptr) return std::nullopt;
        std::optional<ui::Colour> colour;
        for (const Fill& fill : fills) {
            if (text->x >= fill.x && text->x < fill.x + fill.w && text->y >= fill.y && text->y < fill.y + fill.h) {
                colour = fill.colour;
            }
        }
        return colour;
    }
};

constexpr ui::Colour kOn = style::kHeading;   // a pressed button (MappingEditView's kHeading)
bool same(const std::optional<ui::Colour> a, const ui::Colour b) {
    return a && a->r == b.r && a->g == b.g && a->b == b.b;
}

class MappingAutoSave : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("wheelbri-autosave-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_ / "config");
        std::filesystem::create_directories(dir_ / "configs");
        std::filesystem::create_directories(dir_ / "rigs");
        for (const char* name : {"g920.ini", "g25.ini", "g27.ini", "g923.ini"}) {
            std::ofstream(dir_ / "config" / name, std::ios::binary) << "[emulator]\n";
        }
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    [[nodiscard]] ConfigPicker::Settings settings() const {
        return {dir_ / "config", dir_ / "configs", dir_ / "app.ini"};
    }
    // Draws the tab (the save happens at the start of a draw) and clicks the first label `value`; false if not drawn.
    bool click(MappingEditView& view, const std::string& value) {
        frame(view);
        const RecordingWindow::Text* text = window_.find(value);
        if (text == nullptr) return false;
        view.onClick(text->x + 1, text->y + 1);
        return true;
    }
    void frame(MappingEditView& view) {
        window_.texts.clear();
        window_.fills.clear();
        view.update();
        view.draw();
    }
    // The file's text, its line ends as LF (the first save keeps the default's, later ones the own file's).
    [[nodiscard]] static std::string read(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        std::erase(text, '\r');
        return text;
    }

    RecordingWindow window_;
    std::filesystem::path dir_;
};

// A change is saved at the next draw, into the user's own file (the default untouched); Undo goes back and saves that;
// with nothing left to undo, Undo does nothing.
TEST_F(MappingAutoSave, AChangeIsSavedAndUndoGoesBack) {
    ConfigPicker picker(window_, settings());
    MappingEditView view(window_, dir_ / "rigs", dir_ / "app.ini", picker);
    frame(view);
    ASSERT_NE(window_.find("saved"), nullptr);
    EXPECT_EQ(window_.find("Save"), nullptr);     // no Save button
    EXPECT_EQ(window_.find("Revert"), nullptr);   // Undo instead
    const std::filesystem::path own = picker.ownPath();
    EXPECT_FALSE(std::filesystem::exists(own));

    ASSERT_TRUE(click(view, "Invert"));           // the steering axis
    frame(view);                                  // saved now
    ASSERT_TRUE(std::filesystem::exists(own));
    EXPECT_EQ(read(dir_ / "config" / "g920.ini"), "[emulator]\n");   // WheelBri's default as it was
    EXPECT_TRUE(same(window_.buttonColour("Invert"), kOn));
    const std::string inverted = read(own);
    ASSERT_NE(window_.find("saved"), nullptr);

    // A new view on the same files sees the change: it was written, not just shown.
    {
        ConfigPicker again(window_, settings());
        MappingEditView fresh(window_, dir_ / "rigs", dir_ / "app.ini", again);
        frame(fresh);
        EXPECT_TRUE(same(window_.buttonColour("Invert"), kOn));
    }

    ASSERT_TRUE(click(view, "Undo"));
    frame(view);
    EXPECT_FALSE(same(window_.buttonColour("Invert"), kOn));
    EXPECT_NE(read(own), inverted);               // saved back
    {
        ConfigPicker again(window_, settings());
        MappingEditView fresh(window_, dir_ / "rigs", dir_ / "app.ini", again);
        frame(fresh);
        EXPECT_FALSE(same(window_.buttonColour("Invert"), kOn));
    }
    const std::string undone = read(own);
    ASSERT_TRUE(click(view, "Undo"));             // nothing left: the button is off, nothing changes
    frame(view);
    EXPECT_EQ(read(own), undone);
}

// Several changes go back one at a time, newest first.
TEST_F(MappingAutoSave, UndoGoesBackOneChangeAtATime) {
    ConfigPicker picker(window_, settings());
    MappingEditView view(window_, dir_ / "rigs", dir_ / "app.ini", picker);
    frame(view);
    ASSERT_TRUE(click(view, "Invert"));
    frame(view);
    const std::string one = read(picker.ownPath());
    ASSERT_TRUE(click(view, "Invert"));   // back off: a second change
    frame(view);
    const std::string two = read(picker.ownPath());
    EXPECT_NE(one, two);
    ASSERT_TRUE(click(view, "Undo"));
    frame(view);
    EXPECT_EQ(read(picker.ownPath()), one);
    EXPECT_TRUE(same(window_.buttonColour("Invert"), kOn));
}

// The G25 shows its centring spring under the rotation (Logitech's software had one for it); a preset is saved to
// [ffb] centring_spring in the user's own file, and the row fits above the status line with every button group shown.
// The G920 shows none.
TEST_F(MappingAutoSave, TheG25ShowsItsCentringSpring) {
    // Every G25 button mapped, the shifter's too: every group shown, the tallest the tab gets.
    {
        std::ofstream g25(dir_ / "config" / "g25.ini", std::ios::binary);
        g25 << "[emulator]\ntarget_profile = g25\n[buttons]\n";
        for (int b = 0; b < 19; ++b) g25 << "target_" << b << " = " << b << "\n";
    }
    ConfigPicker picker(window_, settings());
    MappingEditView view(window_, dir_ / "rigs", dir_ / "app.ini", picker);
    frame(view);
    EXPECT_EQ(window_.find("CENTRING SPRING"), nullptr);   // the G920: none

    picker.useTarget(ConfigPicker::Target::G25);
    frame(view);
    const RecordingWindow::Text* heading = window_.find("CENTRING SPRING");
    ASSERT_NE(heading, nullptr);
    EXPECT_LT(heading->y + 32, window_.height() - 30 - 24);   // its row above the status line at the bottom
    EXPECT_TRUE(same(window_.buttonColour("Off"), kOn));

    ASSERT_TRUE(click(view, "25%"));
    frame(view);
    EXPECT_TRUE(same(window_.buttonColour("25%"), kOn));
    const std::string written = read(picker.ownPath());
    EXPECT_NE(written.find("centring_spring = 25"), std::string::npos) << written;
}

// The G25 and G27 show their button order under the centring spring; Native is saved to [buttons] order in the user's
// own file, and the row fits above the status line with every button group shown (the G27's, the most buttons). The
// G920 shows none.
TEST_F(MappingAutoSave, TheG25AndG27ShowTheirButtonOrder) {
    for (const auto& [name, count] : {std::pair<std::string, int>{"g25", 19}, {"g27", 23}}) {
        std::ofstream file(dir_ / "config" / (name + ".ini"), std::ios::binary);
        file << "[emulator]\ntarget_profile = " << name << "\n[buttons]\n";
        for (int b = 0; b < count; ++b) file << "target_" << b << " = " << b << "\n";
    }
    ConfigPicker picker(window_, settings());
    MappingEditView view(window_, dir_ / "rigs", dir_ / "app.ini", picker);
    frame(view);
    EXPECT_EQ(window_.find("BUTTON ORDER"), nullptr);   // the G920: none

    for (const ConfigPicker::Target target : {ConfigPicker::Target::G27, ConfigPicker::Target::G25}) {
        picker.useTarget(target);
        frame(view);
        const RecordingWindow::Text* heading = window_.find("BUTTON ORDER");
        ASSERT_NE(heading, nullptr);
        EXPECT_LT(heading->y + 32, window_.height() - 30 - 24);   // its row above the status line at the bottom
        EXPECT_TRUE(same(window_.buttonColour("Logitech driver"), kOn));
    }

    ASSERT_TRUE(click(view, "Native"));   // the G25's
    frame(view);
    EXPECT_TRUE(same(window_.buttonColour("Native"), kOn));
    const std::string written = read(picker.ownPath());
    EXPECT_NE(written.find("order = native"), std::string::npos) << written;
}

}  // namespace
}  // namespace wheelbri::service
