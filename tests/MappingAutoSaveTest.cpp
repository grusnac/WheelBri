// SPDX-License-Identifier: GPL-3.0-or-later
// The Mapping tab saves as you change (service/src/MappingEditView.cpp, autoSave and undo): a change is written at
// once (WheelBri's default stays as it is: the change goes to the user's own file), Undo goes back one change and
// saves that, and the list starts again with another Target.

#include "ConfigPicker.hpp"
#include "MappingEditView.hpp"
#include "UiStyle.hpp"

#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/DeviceProfile.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
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
        ui::Colour colour;
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
    void text(const float x, const float y, const std::string_view value, const ui::Colour colour) override {
        texts.push_back({x, y, std::string(value), colour});
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

// The G25 and G27 pick their button order on the top row (no section of its own below); each order has its own
// mapping: a change with Native goes to my-g25-native.ini, with order = native, WheelBri's default untouched.
TEST_F(MappingAutoSave, TheG25AndG27PickTheirButtonOrderOnTheTopRow) {
    std::ofstream(dir_ / "config" / "g25.ini", std::ios::binary) << "[emulator]\ntarget_profile = g25\n";
    std::ofstream(dir_ / "config" / "g27.ini", std::ios::binary) << "[emulator]\ntarget_profile = g27\n";
    ConfigPicker picker(window_, settings());
    MappingEditView view(window_, dir_ / "rigs", dir_ / "app.ini", picker);
    frame(view);
    EXPECT_EQ(window_.find("Logitech"), nullptr);   // the G920: one order

    for (const ConfigPicker::Target target : {ConfigPicker::Target::G27, ConfigPicker::Target::G25}) {
        picker.useTarget(target);
        frame(view);
        EXPECT_EQ(window_.find("BUTTON ORDER"), nullptr);
        EXPECT_TRUE(same(window_.buttonColour("Logitech"), kOn));
    }

    ASSERT_TRUE(click(view, "Native"));   // the G25's
    frame(view);
    EXPECT_TRUE(same(window_.buttonColour("Native"), kOn));
    EXPECT_EQ(picker.ownPath().filename(), "my-g25-native.ini");
    ASSERT_TRUE(click(view, "Invert"));   // the steering axis
    frame(view);
    const std::string written = read(picker.ownPath());
    EXPECT_NE(written.find("order = native"), std::string::npos) << written;
    EXPECT_NE(written.find("with Native button order"), std::string::npos) << written;
    EXPECT_FALSE(std::filesystem::exists(dir_ / "configs" / "my-g25.ini"));
    EXPECT_EQ(read(dir_ / "config" / "g25.ini"), "[emulator]\ntarget_profile = g25\n");
}

// The user's Logitech driver mapping copied for Native: its mapped buttons' numbers changed, so their rows are amber
// and a note says so; changing a row takes its mark off, "Got it" all of them; kept in app.ini until then.
TEST_F(MappingAutoSave, ACopiedNativeMappingMarksTheRenumberedButtons) {
    std::ofstream(dir_ / "config" / "g25.ini", std::ios::binary) << "[emulator]\ntarget_profile = g25\n";
    std::ofstream(dir_ / "configs" / "my-g25.ini", std::ios::binary)
        << "[emulator]\ntarget_profile = g25\nfrom_rig = true\n[buttons]\ntarget_0 = 3\ntarget_15 = 7\n";
    ASSERT_TRUE(writeAppSetting(dir_ / "app.ini", "target", "g25"));
    ConfigPicker picker(window_, settings());
    MappingEditView view(window_, dir_ / "rigs", dir_ / "app.ini", picker);
    const auto g25 = makeProfile("g25");
    ASSERT_NE(g25, nullptr);
    const auto label = [&g25](const ButtonOrder order, const std::size_t b) {
        char text[32];
        std::snprintf(text, sizeof(text), "%2d %.10s", gameButtonNumber(*g25, order, b),
                      std::string(g25->buttonName(b)).c_str());
        return std::string(text);
    };
    const auto colourOf = [this](const std::string& value) -> std::optional<ui::Colour> {
        const RecordingWindow::Text* text = window_.find(value);
        if (text == nullptr) return std::nullopt;
        return text->colour;
    };
    const auto noted = [this] {
        return std::any_of(window_.texts.begin(), window_.texts.end(), [](const RecordingWindow::Text& text) {
            return text.value.rfind("copied from Logitech", 0) == 0;
        });
    };
    frame(view);
    EXPECT_TRUE(same(colourOf(label(ButtonOrder::Driver, 0)), style::kText));
    EXPECT_FALSE(noted());

    picker.useButtonOrder(ButtonOrder::Native);
    frame(view);
    EXPECT_EQ(readAppSetting(dir_ / "app.ini", "renumbered_g25"), "0,15");
    EXPECT_TRUE(noted());
    EXPECT_TRUE(same(colourOf(label(ButtonOrder::Native, 0)), style::kCaution));
    EXPECT_TRUE(same(colourOf(label(ButtonOrder::Native, 15)), style::kCaution));
    EXPECT_TRUE(same(colourOf(label(ButtonOrder::Native, 1)), style::kText));   // nothing mapped there

    // Kept: a new view (the next start) shows them too.
    MappingEditView again(window_, dir_ / "rigs", dir_ / "app.ini", picker);
    frame(again);
    EXPECT_TRUE(same(colourOf(label(ButtonOrder::Native, 15)), style::kCaution));

    // Red 1's row changed (unmapped): its mark goes, the other stays.
    const auto source = std::find_if(window_.texts.begin(), window_.texts.end(), [](const RecordingWindow::Text& t) {
        return t.value.find("button 3") != std::string::npos;
    });
    ASSERT_NE(source, window_.texts.end());
    view.onClick(source->x + 1, source->y + 1);
    frame(view);
    const auto nothing = std::find_if(window_.texts.rbegin(), window_.texts.rend(),   // the popup's: drawn last
                                      [](const RecordingWindow::Text& t) { return t.value == "(nothing)"; });
    ASSERT_NE(nothing, window_.texts.rend());
    view.onClick(nothing->x + 1, nothing->y + 1);
    frame(view);
    EXPECT_EQ(readAppSetting(dir_ / "app.ini", "renumbered_g25"), "15");
    EXPECT_FALSE(same(colourOf(label(ButtonOrder::Native, 0)), style::kCaution));   // its group folded: not shown
    EXPECT_TRUE(noted());

    ASSERT_TRUE(click(view, "Got it"));
    frame(view);
    EXPECT_TRUE(readAppSetting(dir_ / "app.ini", "renumbered_g25").empty());
    EXPECT_FALSE(noted());
    EXPECT_TRUE(same(colourOf(label(ButtonOrder::Native, 15)), style::kText));
}

}  // namespace
}  // namespace wheelbri::service
