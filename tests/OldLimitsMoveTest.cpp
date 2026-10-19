// WheelBri's older force limits (strength 0.70, cap 0.80) in the user's own mapping: the Mapping tab moves them to
// 1 / 1 when it opens the file (the wheel's own app sets how strong), says so, and leaves the rest of the file alone.
// Other values are the user's choice and stay.

#include "ConfigPicker.hpp"
#include "MappingEditView.hpp"
#include "ServiceLogging.hpp"

#include "wheelbri/FileText.hpp"

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

class OldLimitsMove : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string test = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        dir_ = std::filesystem::temp_directory_path() / ("wheelbri-oldlimits-" + test);
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_ / "config");
        std::filesystem::create_directories(dir_ / "configs");
        std::filesystem::create_directories(dir_ / "rigs");
        for (const char* name : {"g25.ini", "g27.ini", "g920.ini", "g923.ini"}) {
            std::ofstream(dir_ / "config" / name, std::ios::binary) << "[emulator]\n[ffb]\nmaster_gain = 1.0\n";
        }
        home_.emplace(dir_);
    }
    void TearDown() override {
        home_.reset();
        std::filesystem::remove_all(dir_);
    }
    [[nodiscard]] ConfigPicker::Settings settings() const {
        return {dir_ / "config", dir_ / "configs", dir_ / "app.ini"};
    }
    [[nodiscard]] std::filesystem::path own() const { return dir_ / "configs" / "my-g920.ini"; }
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

    RecordingWindow window_;
    std::filesystem::path dir_;
    std::optional<SettingsHome> home_;
};

TEST_F(OldLimitsMove, TheOlderLimitsAreMovedWhenTheMappingOpens) {
    std::ofstream(own(), std::ios::binary)
        << "; my own\r\n[emulator]\r\n[ffb]\r\nmaster_gain = 0.7\r\nmax_magnitude = 0.8   ; hard cap\r\n"
           "invert = true\r\n";
    ConfigPicker picker(window_, settings());
    MappingEditView view(window_, dir_ / "rigs", dir_ / "app.ini", picker);
    frame(view);
    EXPECT_EQ(readFileText(own()).value_or(""),
              "; my own\r\n[emulator]\r\n[ffb]\r\nmaster_gain = 1.0\r\nmax_magnitude = 1.0\r\ninvert = true\r\n");
    EXPECT_NE(window_.startingWith("Force feedback: strength and cap are now 1.00"), nullptr);
}

// The other targets' mappings (and the older per-mode files) are moved too, not only the one shown.
TEST_F(OldLimitsMove, EveryOwnMappingIsMoved) {
    const std::string old = "[emulator]\n[ffb]\nmaster_gain = 0.7\nmax_magnitude = 0.8\n";
    for (const char* name : {"my-g920.ini", "my-g25.ini", "my-g923.ini", "my-g25-in-game-shim.ini"}) {
        std::ofstream(dir_ / "configs" / name, std::ios::binary) << old;
    }
    std::ofstream(dir_ / "configs" / "notes.ini", std::ios::binary) << old;   // not a mapping: left alone
    ConfigPicker picker(window_, settings());
    MappingEditView view(window_, dir_ / "rigs", dir_ / "app.ini", picker);
    frame(view);
    for (const char* name : {"my-g920.ini", "my-g25.ini", "my-g923.ini", "my-g25-in-game-shim.ini"}) {
        EXPECT_EQ(readFileText(dir_ / "configs" / name).value_or(""),
                  "[emulator]\n[ffb]\nmaster_gain = 1.0\nmax_magnitude = 1.0\n")
            << name;
    }
    EXPECT_EQ(readFileText(dir_ / "configs" / "notes.ini").value_or(""), old);
    EXPECT_NE(window_.startingWith("Force feedback: strength and cap are now 1.00 (were 0.70 / 0.80) in 4 mappings"),
              nullptr);
}

TEST_F(OldLimitsMove, ValuesTheUserChoseStay) {
    const std::string mine = "[emulator]\n[ffb]\nmaster_gain = 0.5\nmax_magnitude = 0.8\n";
    std::ofstream(own(), std::ios::binary) << mine;
    ConfigPicker picker(window_, settings());
    MappingEditView view(window_, dir_ / "rigs", dir_ / "app.ini", picker);
    frame(view);
    EXPECT_EQ(readFileText(own()).value_or(""), mine);
    EXPECT_EQ(window_.startingWith("Force feedback: strength and cap are now"), nullptr);
}

// A game profile's own force feedback gain (NFS Payback's x3) is said in its Force feedback sub-tab, not only shown as
// the Steering force and Vibration numbers.
TEST_F(OldLimitsMove, AProfilesGainIsSaidOnItsForceFeedbackTab) {
    std::ofstream(own(), std::ios::binary) << "[emulator]\n[ffb]\nmaster_gain = 1.0\nmax_magnitude = 1.0\n";
    ConfigPicker picker(window_, settings());
    MappingEditView view(window_, dir_ / "rigs", dir_ / "app.ini", picker);
    view.setPage(MappingEditView::Page::Games);
    ASSERT_TRUE(click(view, "NFS Payback"));
    ASSERT_TRUE(click(view, "Force feedback"));
    frame(view);
    EXPECT_NE(window_.startingWith("Its game profile: Steering force and Vibration x3 ("), nullptr);
    ASSERT_NE(window_.startingWith("Steering force"), nullptr);
    EXPECT_NE(window_.startingWith("the profile's; constant force"), nullptr);   // the row says whose 3 it is
}

}  // namespace
}  // namespace wheelbri::service
