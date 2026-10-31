// The Mapping tab's editor and the Games page (service/src/MappingEditView.cpp), driven as the app drives them: a
// draw, then a click on what it drew, keys and typed text, the pointer. A rig of four devices with named controls, the
// shipped configs, the user's settings in a temporary folder, and the system's dialogs, Documents folder, running
// programs and clock replaced (MappingEditView::System), so every path runs the same on Windows and Linux.

#include "ConfigPicker.hpp"
#include "MappingEditView.hpp"
#include "ServiceLogging.hpp"
#include "ShimGamesView.hpp"
#include "UiStyle.hpp"

#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/FileText.hpp"
#include "wheelbri/GameProfiles.hpp"
#include "wheelbri/Rig.hpp"
#include "wheelbri/ShimGames.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace wheelbri::service {
namespace {

bool sameColour(const ui::Colour a, const ui::Colour b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

// Records what a frame draws; the canvas can be made bigger, as a bigger window makes it.
class Window final : public ui::UiWindow {
public:
    struct Text {
        float x, y;
        std::string value;
        ui::Colour colour;
    };
    struct Box {
        float x, y, w, h;
        ui::Colour colour;
    };
    std::vector<Text> texts;
    std::vector<Box> fills;
    std::vector<Box> outlines;
    // What pickFolder answers: a folder, or nothing (cancelled, with `pickError` when set).
    std::optional<std::filesystem::path> folderAnswer;
    std::string pickError;

    void setCanvas(const float w, const float h) {
        canvasWidth_ = w;
        canvasHeight_ = h;
    }
    void clear() {
        texts.clear();
        fills.clear();
        outlines.clear();
    }
    ui::WindowInput pump() override { return {}; }
    [[nodiscard]] bool visible() const override { return true; }
    void begin(ui::Colour) override {}
    void fill(const float x, const float y, const float w, const float h, const ui::Colour colour) override {
        fills.push_back({x, y, w, h, colour});
    }
    void outline(const float x, const float y, const float w, const float h, const ui::Colour colour) override {
        outlines.push_back({x, y, w, h, colour});
    }
    void text(const float x, const float y, const std::string_view value, const ui::Colour colour) override {
        texts.push_back({x, y, std::string(value), colour});
    }
    void present() override {}
    bool openFolder(const std::filesystem::path&, std::string&) override { return false; }
    bool pickFolder(const std::string&, std::filesystem::path& folder, std::string& error) override {
        if (!folderAnswer) {
            error = pickError;
            return false;
        }
        folder = *folderAnswer;
        return true;
    }
    [[nodiscard]] std::string describe() const override { return "test"; }

    // The `nth` text drawn exactly `value` (0: the first), or null.
    [[nodiscard]] const Text* find(const std::string& value, const std::size_t nth = 0) const {
        std::size_t seen = 0;
        for (const Text& text : texts) {
            if (text.value == value && seen++ == nth) return &text;
        }
        return nullptr;
    }
    // The first text holding `part`, or null.
    [[nodiscard]] const Text* holding(const std::string& part) const {
        for (const Text& text : texts) {
            if (text.value.find(part) != std::string::npos) return &text;
        }
        return nullptr;
    }
    [[nodiscard]] bool has(const std::string& value) const { return find(value) != nullptr; }
    [[nodiscard]] bool hasPart(const std::string& part) const { return holding(part) != nullptr; }
    [[nodiscard]] std::size_t count(const std::string& value) const {
        std::size_t n = 0;
        for (const Text& text : texts) n += text.value == value ? 1 : 0;
        return n;
    }
    // Whether a box outlined in `colour` holds the point.
    [[nodiscard]] bool outlinedAt(const float x, const float y, const ui::Colour colour) const {
        for (const Box& box : outlines) {
            if (x >= box.x && x < box.x + box.w && y >= box.y && y < box.y + box.h && sameColour(box.colour, colour)) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] bool filledAt(const float x, const float y, const ui::Colour colour) const {
        for (const Box& box : fills) {
            if (x >= box.x && x < box.x + box.w && y >= box.y && y < box.y + box.h && sameColour(box.colour, colour)) {
                return true;
            }
        }
        return false;
    }
};

// Points the user's settings folder (app.ini, games.ini, the configs, the rigs) at a test folder while it lives.
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

std::string read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::erase(text, '\r');
    return text;
}

void write(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

constexpr DeviceId kWheelId{0x346e, 0x0005};
constexpr DeviceId kPedalsId{0x1234, 0x0001};
constexpr DeviceId kBoxId{0x1234, 0x0002};
constexpr DeviceId kShifterId{0x1234, 0x0003};

// The rig: a wheel with the axes and buttons the shipped G920 mapping uses (and more), pedals, a button box with many
// buttons, and a shifter with nothing named yet.
Rig testRig() {
    Rig rig;
    RigDevice wheel;
    wheel.name = "Moza R3";
    wheel.id = kWheelId;
    wheel.kind = DeviceKind::Wheel;
    wheel.axes[0] = {"Wheel", "Steering"};
    wheel.axes[2] = {"Gas", "Throttle"};
    wheel.axes[3] = {"Hand", "Handbrake"};
    wheel.axes[5] = {"Brake", "Brake"};
    wheel.axes[6] = {"Clutch", "Clutch"};
    for (const int b : {0, 1, 2, 3, 4, 5, 6, 7, 12, 13, 21, 22, 34, 35, 37}) {
        wheel.buttons[static_cast<std::size_t>(b)] = {"B" + std::to_string(b), "Button"};
    }
    // What the Rig maps by itself (AutoMap.hpp): an Xbox-style face, the D-pad as four buttons, the paddles.
    const std::array<std::pair<int, const char*>, 10> core = {{{0, "A"}, {1, "B"}, {2, "X"}, {3, "Y"}, {4, "D Up"},
                                                                {5, "D Right"}, {6, "D Down"}, {7, "D Left"},
                                                                {12, "L Paddle"}, {13, "R Paddle"}}};
    for (const auto& [b, kind] : core) wheel.buttons[static_cast<std::size_t>(b)].kind = kind;
    rig.devices.push_back(wheel);
    RigDevice pedals;
    pedals.name = "Pedals";
    pedals.id = kPedalsId;
    pedals.kind = DeviceKind::Pedals;
    pedals.axes[0] = {"Left", ""};   // named only: the wheel's Clutch and Throttle are the ones the Rig maps
    pedals.axes[1] = {"Right", ""};
    pedals.buttons[0] = {"Pb0", "Button"};
    rig.devices.push_back(pedals);
    RigDevice box;
    box.name = "Button box with a very long name indeed, longer than forty";
    box.id = kBoxId;
    box.kind = DeviceKind::ButtonBox;
    for (std::size_t b = 0; b < MapperConfig::kMaxSourceButtons; ++b) box.buttons[b] = {"K" + std::to_string(b), ""};
    rig.devices.push_back(box);
    RigDevice shifter;
    shifter.name = "Shifter";
    shifter.id = kShifterId;
    shifter.kind = DeviceKind::Shifter;
    rig.devices.push_back(shifter);
    return rig;
}

class EditView : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("wheelbri-editview-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
        for (const char* name : {"g920.ini", "g25.ini", "g27.ini", "g923.ini"}) {
            std::filesystem::create_directories(dir_ / "config");
            std::filesystem::copy_file(std::filesystem::path(WHEELBRI_CONFIG_DIR) / name, dir_ / "config" / name);
        }
        home_.emplace(dir_);
        std::filesystem::create_directories(userConfigFolder());
        documents_ = dir_ / "Documents";
        std::filesystem::create_directories(documents_);
    }
    void TearDown() override {
        view_.reset();
        shims_.reset();
        picker_.reset();
        home_.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    // config/g920.ini as it was before from_rig (2026-10-23): the Moza R3's numbers written out, every unlisted button
    // the wheel's of the same number. For the tests of what the tab does with any mapping's sources.
    void useOlderDefault() {
        std::string text = "[emulator]\nsource_device =\ntarget_profile = g920\n[steering]\naxis = 0\n[throttle]\n"
                           "axis = 2\n[brake]\naxis = 5\n[clutch]\naxis = 6\n[buttons]\ntarget_4 = 13\ntarget_5 = 12\n"
                           "target_6 = 35\ntarget_7 = 22\ntarget_8 = 34\ntarget_9 = 21\ntarget_10 = 37\n";
        for (int t = 11; t < 32; ++t) text += "target_" + std::to_string(t) + " = -1\n";
        text += "[hat]\nup = 4\nright = 5\ndown = 6\nleft = 7\n";
        write(dir_ / "config" / "g920.ini", text);
    }

    // The rig saved and made the active one.
    void useRig(const Rig& rig) {
        ASSERT_TRUE(saveRig(rigsFolder(), "My rig", rig));
        ASSERT_TRUE(writeAppSetting(appStateFile(), "rig", "My rig"));
    }
    [[nodiscard]] ConfigPicker::Settings pickerSettings() const {
        return {dir_ / "config", userConfigFolder(), appStateFile()};
    }
    // A view on the settings (made again: as at the app's next start), with the test's system.
    MappingEditView& open(const bool withShims = false) {
        view_.reset();
        picker_ = std::make_unique<ConfigPicker>(window_, pickerSettings());
        view_ = std::make_unique<MappingEditView>(window_, rigsFolder(), appStateFile(), *picker_);
        if (withShims) {
            shims_ = std::make_unique<ShimGamesView>(
                window_, ShimGamesView::Settings{shimGamesFile(), ShimSources{dir_ / "shim" / kShimDll, {}},
                                                 dir_ / "shim" / kShimSettings, appStateFile(), {}, {}});
            view_->setShimGames(shims_.get());
        }
        MappingEditView::System system;
        system.pickFile = [this](const std::string& title, const std::filesystem::path&, const std::string&) {
            pickTitles_.push_back(title);
            if (fileAnswers_.empty()) return std::optional<std::filesystem::path>();
            const std::optional<std::filesystem::path> answer = fileAnswers_.front();
            fileAnswers_.pop_front();
            return answer;
        };
        system.canPickFile = [this] { return canPick_; };
        system.documentsFolder = [this] { return documents_; };
        system.runningPrograms = [this] { return running_; };
        system.nowMs = [this] { return nowMs_; };
        view_->setSystem(system);
        return *view_;
    }
    void frame() {
        window_.clear();
        view_->update();
        view_->draw();
        view_->drawTooltip();
    }
    void games() {
        view_->setPage(MappingEditView::Page::Games);
        frame();
    }
    // Draws, then clicks the `nth` text `value`; false when it was not drawn.
    bool click(const std::string& value, const std::size_t nth = 0) {
        frame();
        const Window::Text* const text = window_.find(value, nth);
        if (text == nullptr) return false;
        view_->onClick(text->x + 1, text->y + 1);
        return true;
    }
    // The same on the text holding `part`.
    bool clickPart(const std::string& part) {
        frame();
        const Window::Text* const text = window_.holding(part);
        if (text == nullptr) return false;
        view_->onClick(text->x + 1, text->y + 1);
        return true;
    }
    // Draws, then clicks the `nth` text right of the text `label` on its row (a row's field or button).
    bool clickRight(const std::string& label, const std::size_t nth = 0) {
        frame();
        const Window::Text* const name = window_.find(label);
        if (name == nullptr) return false;
        std::vector<const Window::Text*> row;
        for (const Window::Text& text : window_.texts) {
            if (text.y == name->y && text.x > name->x + 8) row.push_back(&text);
        }
        std::sort(row.begin(), row.end(), [](const Window::Text* a, const Window::Text* b) { return a->x < b->x; });
        if (nth >= row.size()) return false;
        view_->onClick(row[nth]->x + 1, row[nth]->y + 1);
        return true;
    }
    void type(const std::string& text) { view_->onText(text); }
    [[nodiscard]] std::string own() const { return read(picker_->ownPath()); }
    [[nodiscard]] std::string status() const {
        // The status line: the last text drawn at the bottom of the tab.
        const float y = window_.height() - 30 - 24;
        for (auto it = window_.texts.rbegin(); it != window_.texts.rend(); ++it) {
            if (it->y == y && it->x == 4) return it->value;
        }
        return {};
    }

    Window window_;
    std::filesystem::path dir_;
    std::filesystem::path documents_;
    std::optional<SettingsHome> home_;
    std::unique_ptr<ConfigPicker> picker_;
    std::unique_ptr<ShimGamesView> shims_;
    std::unique_ptr<MappingEditView> view_;
    std::deque<std::optional<std::filesystem::path>> fileAnswers_;
    std::vector<std::string> pickTitles_;
    bool canPick_ = true;
    std::optional<std::vector<RunningProgram>> running_ = std::vector<RunningProgram>{};
    std::uint64_t nowMs_ = 1'000'000;
};

// --- the Mapping tab --------------------------------------------------------------------------------------------

// Without a rig the tab says so and still shows the default mapping; with a rig that has no devices it says that.
TEST_F(EditView, SaysWhenThereIsNoRig) {
    useOlderDefault();
    open();
    frame();
    EXPECT_TRUE(window_.hasPart("No rig yet"));
    EXPECT_TRUE(window_.has("AXES"));
    EXPECT_TRUE(window_.hasPart("the wheel: axis 0"));   // a source the rig cannot name: the wheel and its number
    useRig(Rig{});
    nowMs_ += 2000;   // the rig is read again every 2 s
    frame();
    EXPECT_TRUE(window_.hasPart("The rig has no devices yet"));
}

// With the rig, sources show the device and the control's name; a source the rig has not named says so in amber.
TEST_F(EditView, SourcesAreNamedFromTheRig) {
    useOlderDefault();
    Rig rig = testRig();
    rig.devices[0].axes[6] = {};   // the clutch's axis not named
    useRig(rig);
    open();
    frame();
    EXPECT_TRUE(window_.has("Moza R3: Wheel"));
    EXPECT_TRUE(window_.has("Moza R3: Gas"));
    const Window::Text* const missing = window_.find("Moza R3: axis 6  (not in the rig)");
    ASSERT_NE(missing, nullptr);
    EXPECT_TRUE(sameColour(missing->colour, style::kCaution));
    // Its hover text: the device, the control's name and its number.
    view_->setHover(missing->x, missing->y, true);
    frame();
    EXPECT_TRUE(window_.hasPart(", not in the rig"));
    const Window::Text* const wheel = window_.find("Moza R3: Wheel");
    ASSERT_NE(wheel, nullptr);
    view_->setHover(wheel->x, wheel->y, true);
    frame();
    EXPECT_TRUE(window_.has("Moza R3: 'Wheel', axis 0"));
    view_->setHover(wheel->x, wheel->y, false);   // the pointer left the window: no hover text
    frame();
    EXPECT_FALSE(window_.has("Moza R3: 'Wheel', axis 0"));
}

// A source is picked in a popup: the rig's named controls, the wheel's first; "(nothing)" unmaps it; Cancel and
// Escape close it unchanged; a click outside it does nothing.
TEST_F(EditView, PickingASourceForAnAxis) {
    useRig(testRig());
    open();
    ASSERT_TRUE(click("Moza R3: Wheel"));
    frame();
    EXPECT_TRUE(window_.has("What drives Steering?"));
    EXPECT_TRUE(view_->popupOpen());
    EXPECT_TRUE(window_.hasPart("(the wheel)"));
    EXPECT_TRUE(window_.hasPart("Pedals  1234:0001  pedals"));   // an accessory, its kind
    EXPECT_TRUE(window_.has("no axis named: name them in the Rig tab"));   // the shifter
    view_->onClick(2, 2);   // outside the popup: nothing
    frame();
    EXPECT_TRUE(view_->popupOpen());
    ASSERT_TRUE(click("Cancel"));
    frame();
    EXPECT_FALSE(view_->popupOpen());

    ASSERT_TRUE(click("Moza R3: Wheel"));
    frame();
    view_->onKey('x');   // not Escape: nothing
    EXPECT_TRUE(view_->popupOpen());
    view_->onKey(ui::kEscape);
    EXPECT_FALSE(view_->popupOpen());

    ASSERT_TRUE(click("Moza R3: Wheel"));
    ASSERT_TRUE(click("1 Right"));   // the pedals' right pedal
    frame();
    EXPECT_FALSE(view_->popupOpen());
    EXPECT_TRUE(window_.has("Pedals: Right"));
    EXPECT_NE(own().find("1234:0001/1"), std::string::npos) << own();

    ASSERT_TRUE(click("Pedals: Right"));
    ASSERT_TRUE(click("(nothing)"));
    frame();
    EXPECT_TRUE(window_.has("(nothing)"));
}

// A control used elsewhere is greyed in the picker with where in its hover text, and a click moves it here (taken from
// the other use); the one picked now is highlighted.
TEST_F(EditView, AControlUsedElsewhereIsGreyedAndMoves) {
    useRig(testRig());
    open();
    ASSERT_TRUE(click("Moza R3: Wheel"));
    frame();
    const Window::Text* const gas = window_.find("2 Gas");   // the throttle's
    ASSERT_NE(gas, nullptr);
    EXPECT_TRUE(sameColour(gas->colour, style::kDim));
    view_->setHover(gas->x, gas->y, true);
    frame();
    EXPECT_TRUE(window_.hasPart("Throttle"));
    EXPECT_TRUE(window_.hasPart("Click to move it here."));
    const Window::Text* const mine = window_.find("0 Wheel");
    ASSERT_NE(mine, nullptr);
    EXPECT_TRUE(sameColour(mine->colour, style::kDark));
    // The tab's own hover texts under the popup are not shown.
    const Window::Text* const heading = window_.find("What drives Steering?");
    ASSERT_NE(heading, nullptr);
    view_->setHover(300, 50, true);   // over the AXES heading under the popup
    frame();
    EXPECT_FALSE(window_.hasPart("Sources are the rig's controls"));
    // A click on the greyed one moves it here: the throttle has nothing then.
    view_->setHover(0, 0, false);
    ASSERT_TRUE(click("2 Gas"));
    frame();
    EXPECT_FALSE(view_->popupOpen());
    EXPECT_TRUE(window_.has("Moza R3: Gas"));
    EXPECT_EQ(window_.count("Moza R3: Gas"), 1U);   // on the steering's row only
}

// The source picker as the secondary popup: a find box (typing filters, Escape clears it, then closes), a list that
// scrolls with the line at the bottom saying what is out of view, and controls touched on the rig: the right kind,
// named and free is picked (the popup closes); the wrong kind, an unnamed one or one used elsewhere says why.
TEST_F(EditView, TheSourcePickerFindsScrollsAndTakesTouches) {
    Rig rig = testRig();
    for (std::size_t b = 0; b < MapperConfig::kMaxSourceButtons; ++b) {   // more than the popup holds
        rig.devices[1].buttons[b] = {"P" + std::to_string(b), ""};
        rig.devices[3].buttons[b] = {"S" + std::to_string(b), ""};
    }
    rig.devices[2].buttons[63] = {};   // the box's last button has no name
    useRig(rig);
    open();
    ASSERT_TRUE(click("Moza R3: B13"));   // R Paddle
    frame();
    EXPECT_TRUE(view_->editingText());
    EXPECT_TRUE(view_->wantsRigTouches());
    ASSERT_TRUE(window_.hasPart(" more below: scroll with the mouse wheel."));
    EXPECT_FALSE(window_.has("63 S63"));
    for (int i = 0; i < 40; ++i) view_->onScroll(-1);
    frame();
    EXPECT_TRUE(window_.has("63 S63"));
    EXPECT_TRUE(window_.hasPart(" above: scroll with the mouse wheel."));
    type("k4");
    frame();
    EXPECT_TRUE(window_.has("4 K4"));
    EXPECT_TRUE(window_.has("41 K41"));
    EXPECT_FALSE(window_.has("1 K1"));
    EXPECT_FALSE(window_.hasPart("more below"));
    view_->onKey(ui::kEscape);   // clears the find box
    frame();
    EXPECT_TRUE(view_->popupOpen());
    EXPECT_TRUE(window_.has("1 K1"));
    // Touches.
    view_->onRigTouch(kWheelId, true, 3);   // an axis, for a button
    frame();
    EXPECT_TRUE(window_.hasPart("That was an axis: button 5 R Paddle takes a button."));
    view_->onRigTouch(kBoxId, false, 63);
    frame();
    EXPECT_TRUE(window_.hasPart("has no name in the Rig tab yet"));
    view_->onRigTouch(kWheelId, false, 1);   // the G920 B's
    frame();
    EXPECT_TRUE(view_->popupOpen());
    EXPECT_TRUE(window_.hasPart("1 B1: used elsewhere"));
    EXPECT_TRUE(window_.hasPart("click it to move it here."));
    view_->onRigTouch(kBoxId, false, 7);
    frame();
    EXPECT_FALSE(view_->popupOpen());
    EXPECT_TRUE(window_.has("Button box with a very long name indeed, longer than forty: K7") ||
                window_.hasPart(": K7"));
}

// Moving a control here takes it from a secondary too.
TEST_F(EditView, MovingHereTakesItFromASecondary) {
    useOlderDefault();
    std::string text = read(dir_ / "config" / "g920.ini");
    text += "\n[buttons]\ntarget_0_or = 21\n";
    write(userConfigFolder() / "my-g920.ini", text);
    useRig(testRig());
    open();
    frame();
    ASSERT_TRUE(window_.hasPart("B21"));   // A's "or"
    ASSERT_TRUE(click("Moza R3: B13"));   // R Paddle
    ASSERT_TRUE(click("21 B21"));         // greyed: A's secondary; moved here
    frame();
    EXPECT_FALSE(view_->popupOpen());
    EXPECT_EQ(own().find("target_0_or"), std::string::npos) << own();
}

// A control used twice (an older mapping) is red, says where in its hover text, and is not saved; Undo drops the
// change. A change that could not be saved is not tried again every frame.
TEST_F(EditView, AControlUsedTwiceIsNotSaved) {
    useOlderDefault();
    useRig(testRig());
    open();
    frame();
    // The brake on the throttle's axis: picked with the throttle's entry greyed, so through the file instead.
    write(picker_->ownPath(), read(dir_ / "config" / "g920.ini") + "\n");
    {
        std::string text = read(picker_->ownPath());
        const std::size_t brake = text.find("axis = 5");
        ASSERT_NE(brake, std::string::npos);
        text.replace(brake, 8, "axis = 2");
        write(picker_->ownPath(), text);
    }
    picker_->refresh();
    frame();
    EXPECT_EQ(window_.count("Moza R3: Gas"), 2U);
    const Window::Text* const gas = window_.find("Moza R3: Gas");
    ASSERT_NE(gas, nullptr);
    EXPECT_TRUE(window_.outlinedAt(gas->x, gas->y, style::kWarning));
    view_->setHover(gas->x, gas->y, true);
    frame();
    EXPECT_TRUE(window_.hasPart("Used twice"));

    ASSERT_TRUE(click("Invert"));   // a change: Save refuses it
    frame();
    EXPECT_TRUE(window_.hasPart("Not saved: a control is used twice"));
    EXPECT_TRUE(window_.has("not saved"));
    const std::string before = own();
    frame();   // not tried again
    EXPECT_EQ(own(), before);
    ASSERT_TRUE(click("Undo"));
    frame();
    EXPECT_TRUE(window_.has("Change dropped."));
    EXPECT_TRUE(window_.has("saved"));
}

// Two and more controls used twice: the status counts the others.
TEST_F(EditView, SeveralControlsUsedTwiceAreCounted) {
    useOlderDefault();
    useRig(testRig());
    std::string text = read(dir_ / "config" / "g920.ini");
    text.replace(text.find("axis = 5"), 8, "axis = 2");
    text.replace(text.find("axis = 6"), 8, "axis = 0");
    write(userConfigFolder() / "my-g920.ini", text);
    window_.setCanvas(1600, 748);   // room for the whole status line
    open();
    ASSERT_TRUE(click("Invert"));
    frame();
    EXPECT_TRUE(window_.hasPart("(+1 more)"));
}

// The D-pad and a target button are picked the same way; the button's popup names it.
TEST_F(EditView, PickingTheDpadAndAButton) {
    useRig(testRig());
    open();
    ASSERT_TRUE(click("Moza R3: B4"));   // D-pad Up
    frame();
    EXPECT_TRUE(window_.has("What drives D-pad Up?"));
    EXPECT_TRUE(window_.hasPart("1 B1"));   // B1 is the G920's B: greyed (a click would move it here)
    ASSERT_TRUE(click("Cancel"));
    ASSERT_TRUE(click("Moza R3: B13"));   // R Paddle
    frame();
    EXPECT_TRUE(window_.hasPart("What drives button 5 "));
    ASSERT_TRUE(click("5 K5"));   // the button box
    frame();
    EXPECT_TRUE(window_.hasPart(": K5"));
}

// from_rig (the built-in mapping): the steering, pedals, D-pad, A, B, X, Y and paddles come from what the rig's
// controls are, each marked RIG; picking a source makes it the user's, a line in the file, while the others stay
// without one.
TEST_F(EditView, TheRigMapsTheCoreInputs) {
    useRig(testRig());
    open();
    frame();
    EXPECT_TRUE(window_.has("Moza R3: Wheel"));
    EXPECT_TRUE(window_.has("Moza R3: Gas"));
    EXPECT_TRUE(window_.has("Moza R3: B0"));          // A
    EXPECT_FALSE(window_.has("Moza R3: B35"));        // a Button: Menu is the user's to map
    EXPECT_EQ(window_.count("RIG"), 14U);             // 4 axes, 4 D-pad directions, A, B, X, Y, 2 paddles
    ASSERT_TRUE(click("Moza R3: B13"));               // R Paddle
    frame();
    ASSERT_TRUE(click("5 K5"));                       // the button box
    nowMs_ += 2000;
    frame();
    EXPECT_EQ(window_.count("RIG"), 13U);
    const std::string text = read(picker_->ownPath());
    EXPECT_NE(text.find("from_rig = true"), std::string::npos) << text;
    EXPECT_NE(text.find("target_4 = 1234:0002/5"), std::string::npos) << text;
    EXPECT_EQ(text.find("target_0 ="), std::string::npos) << text;
    EXPECT_EQ(text.find("\naxis ="), std::string::npos) << text;
}

// The popup lists as many devices as fit: a button box with 64 named buttons fills it, the shifter after it is left
// out on the smallest canvas; a bigger canvas has more columns.
TEST_F(EditView, ThePickerFitsTheCanvas) {
    useOlderDefault();
    Rig rig = testRig();
    for (std::size_t b = 0; b < MapperConfig::kMaxSourceButtons; ++b) {
        rig.devices[0].buttons[b] = {"B" + std::to_string(b), ""};
        rig.devices[1].buttons[b] = {"P", ""};
        rig.devices[3].buttons[b] = {"S", ""};
    }
    useRig(rig);
    open();
    ASSERT_TRUE(click("Moza R3: B13"));
    frame();
    EXPECT_TRUE(window_.hasPart("Button box"));
    EXPECT_FALSE(window_.hasPart("Shifter  1234:0003"));
    window_.setCanvas(1600, 1000);
    frame();
    EXPECT_TRUE(window_.hasPart("Shifter  1234:0003"));
}

// Invert, the folded button groups (Show) and the steering rotation: slider (press, drag, let go), presets, typed.
TEST_F(EditView, InvertShowAndTheRotation) {
    useOlderDefault();
    useRig(testRig());
    open();
    frame();
    ASSERT_TRUE(window_.hasPart("SHIFTER"));
    ASSERT_TRUE(window_.has("nothing mapped"));
    ASSERT_TRUE(click("Show"));
    frame();
    EXPECT_FALSE(window_.has("nothing mapped"));
    EXPECT_TRUE(window_.hasPart("Gear 1"));

    ASSERT_TRUE(click("Invert", 1));   // the throttle
    frame();
    EXPECT_NE(own().find("[throttle]"), std::string::npos);

    ASSERT_TRUE(click("540"));
    frame();
    EXPECT_NE(own().find("target_rotation_deg = 540"), std::string::npos) << own();

    // The slider: pressed at its left end (90), dragged to its right end (1080), let go.
    frame();
    const Window::Text* const left = window_.find("90");
    ASSERT_NE(left, nullptr);
    view_->onClick(left->x - 2, left->y - 20 + 6);
    view_->onPointer(left->x + 2000, 0, true);
    frame();   // still dragging: not saved yet
    EXPECT_NE(own().find("target_rotation_deg = 540"), std::string::npos);
    view_->onPointer(left->x + 2000, 0, false);
    view_->onPointer(left->x, 0, false);   // let go: no longer dragged
    frame();
    EXPECT_NE(own().find("target_rotation_deg = 1080"), std::string::npos) << own();

    // Typed: digits, backspace (also on nothing), a character not taken, Enter.
    ASSERT_TRUE(click("1080"));   // the field (drawn before the presets and the slider's end label)
    type(std::string(1, ui::kBackspace) + std::string(8, ui::kBackspace) + "x7f2o0");
    type(std::string(1, ui::kEnter));
    frame();
    EXPECT_NE(own().find("target_rotation_deg = 720"), std::string::npos) << own();
}

// A typed rotation out of range says why and keeps typing; Escape drops it; a click elsewhere takes a usable one.
TEST_F(EditView, TypingTheRotation) {
    useRig(testRig());
    open();
    ASSERT_TRUE(click("900"));   // the field (drawn before the presets)
    EXPECT_TRUE(view_->editingText());
    type(std::string(4, ui::kBackspace) + "5");
    type(std::string(1, ui::kEnter));
    EXPECT_TRUE(view_->editingText());   // not usable: still typing
    frame();
    EXPECT_TRUE(window_.hasPart("Not changed:"));
    type(std::string(1, ui::kBackspace) + "." + std::string(1, ui::kEnter));
    frame();
    EXPECT_TRUE(window_.hasPart("is not a number"));
    view_->onKey(ui::kEscape);   // typing dropped
    EXPECT_FALSE(view_->editingText());
    view_->onText("1");   // not typing: nothing
    frame();
    EXPECT_FALSE(std::filesystem::exists(picker_->ownPath()));

    ASSERT_TRUE(click("900"));
    type(std::string(4, ui::kBackspace) + "1234567890123456");   // at most 12 characters
    frame();
    EXPECT_TRUE(window_.has("89012_"));   // the field shows the end of what is typed
    type(std::string(12, ui::kBackspace) + "360");
    view_->onClick(2, 700);   // a click elsewhere takes it
    frame();
    EXPECT_FALSE(view_->editingText());
    EXPECT_NE(own().find("target_rotation_deg = 360"), std::string::npos) << own();

    ASSERT_TRUE(click("360"));
    type(std::string(4, ui::kBackspace) + "9");
    view_->onClick(2, 700);   // a click with an unusable value: the status says why, the value stays
    frame();
    EXPECT_TRUE(window_.hasPart("Not changed:"));
    EXPECT_NE(own().find("target_rotation_deg = 360"), std::string::npos);
    // The next usable value answers the complaint: the status no longer says it.
    ASSERT_TRUE(click("360"));
    type(std::string(4, ui::kBackspace) + "270" + std::string(1, ui::kEnter));
    frame();
    EXPECT_FALSE(window_.hasPart("Not changed:"));
}

// The G25's centring spring: typed (whole percent, 0 to 100) and its presets.
TEST_F(EditView, TheCentringSpring) {
    useRig(testRig());
    open();
    picker_->useTarget(ConfigPicker::Target::G25);
    frame();
    ASSERT_TRUE(window_.has("CENTRING SPRING"));
    ASSERT_TRUE(click("0", 0));   // the field, before the presets ("Off")
    type("150");
    type(std::string(1, ui::kEnter));
    frame();
    EXPECT_TRUE(window_.hasPart("the centring spring goes from 0 (off) to 100%"));
    type(std::string(3, ui::kBackspace) + "33.6");
    type(std::string(1, ui::kEnter));
    frame();
    EXPECT_NE(own().find("centring_spring = 34"), std::string::npos) << own();
    ASSERT_TRUE(click("Off"));
    frame();
    EXPECT_NE(own().find("centring_spring = 0"), std::string::npos) << own();
}

// Undo keeps at most 50 changes; another Target starts the list again.
TEST_F(EditView, UndoKeepsFiftyChangesPerTarget) {
    useRig(testRig());
    open();
    for (int i = 0; i < 52; ++i) {
        ASSERT_TRUE(click("Invert"));
        frame();
    }
    int undone = 0;
    while (true) {
        frame();
        const std::string before = own();
        ASSERT_TRUE(click("Undo"));
        frame();
        if (own() == before) break;
        ++undone;
        ASSERT_LT(undone, 60);
    }
    EXPECT_EQ(undone, 50);

    ASSERT_TRUE(click("Invert"));
    frame();
    picker_->useTarget(ConfigPicker::Target::G27);
    frame();
    picker_->useTarget(ConfigPicker::Target::G920);
    frame();
    const std::string now = own();
    ASSERT_TRUE(click("Undo"));   // nothing to undo for this Target any more
    frame();
    EXPECT_EQ(own(), now);
}

// Default deletes the user's own mapping; when it cannot be deleted, the status says why.
TEST_F(EditView, DefaultDropsTheOwnMapping) {
    useRig(testRig());
    open();
    frame();
    EXPECT_FALSE(window_.has("custom"));
    ASSERT_TRUE(click("Invert"));
    frame();
    ASSERT_TRUE(std::filesystem::exists(picker_->ownPath()));
    ASSERT_TRUE(window_.has("custom"));
    EXPECT_TRUE(sameColour(window_.find("custom")->colour, style::kMine));   // violet: yours
    ASSERT_TRUE(click("Default"));
    frame();
    EXPECT_FALSE(std::filesystem::exists(picker_->ownPath()));
    EXPECT_EQ(view_->takeToast(), "Back to WheelBri's mapping");
    EXPECT_EQ(view_->takeToast(), "");

    ASSERT_TRUE(click("Invert"));
    frame();
    frame();
    // The own file replaced by a folder with something in it: it cannot be deleted.
    std::filesystem::remove(picker_->ownPath());
    write(picker_->ownPath() / "inside", "x");
    const Window::Text* const button = window_.find("Default");
    ASSERT_NE(button, nullptr);
    view_->onClick(button->x + 1, button->y + 1);
    frame();
    EXPECT_TRUE(window_.hasPart("Not dropped:"));
}

// A mapping that cannot be written says so, and is not tried again until the next change.
TEST_F(EditView, AMappingThatCannotBeWritten) {
    useRig(testRig());
    std::filesystem::remove_all(userConfigFolder());
    write(userConfigFolder(), "a file where the folder should be");
    open();
    ASSERT_TRUE(click("Invert"));
    frame();
    EXPECT_TRUE(window_.hasPart("Could not write"));
    EXPECT_TRUE(window_.has("not saved"));
}

// The mapping names a device for the wheel that the rig does not have: every rig device is then an accessory, and
// a mapping with more than three accessories is not saved (writeMapping says why).
TEST_F(EditView, AWheelTheRigDoesNotHave) {
    useOlderDefault();
    Rig rig = testRig();
    rig.devices[3].buttons[0] = {"G1", ""};
    useRig(rig);
    std::string text = read(dir_ / "config" / "g920.ini");
    text.replace(text.find("source_device ="), 15, "source_device = 9999:9999");
    write(userConfigFolder() / "my-g920.ini", text);
    open();
    frame();
    EXPECT_TRUE(window_.hasPart("the wheel: axis 0"));
    // Steering from the wheel (now an accessory), throttle from the pedals, a button from the box, the D-pad Up from the
    // shifter: four accessories.
    ASSERT_TRUE(clickPart("the wheel: axis 0"));
    ASSERT_TRUE(click("0 Wheel"));
    frame();
    ASSERT_TRUE(clickPart("the wheel: axis 2"));
    ASSERT_TRUE(click("1 Right"));
    frame();
    ASSERT_TRUE(clickPart("the wheel: button 13"));
    ASSERT_TRUE(click("7 K7"));
    frame();
    ASSERT_TRUE(clickPart("the wheel: button 4"));
    ASSERT_TRUE(click("0 G1"));
    frame();
    EXPECT_TRUE(window_.hasPart("accessories besides the wheel"));
}

// source_device naming the wheel by name or by id: its controls are the wheel's.
TEST_F(EditView, TheWheelByNameOrId) {
    useRig(testRig());
    std::string text = read(dir_ / "config" / "g920.ini");
    const std::size_t at = text.find("source_device =");
    text.replace(at, 15, "source_device = Moza");
    write(userConfigFolder() / "my-g920.ini", text);
    open();
    frame();
    EXPECT_TRUE(window_.has("Moza R3: Wheel"));
    text.replace(at, 20, "source_device = 346e:0005");
    write(userConfigFolder() / "my-g920.ini", text);
    picker_->refresh();
    frame();
    EXPECT_TRUE(window_.has("Moza R3: Wheel"));
}

// A rig with no device of kind wheel: its first device is the wheel.
TEST_F(EditView, ARigWithoutAWheelUsesItsFirstDevice) {
    Rig rig = testRig();
    rig.devices[0].kind = DeviceKind::Other;
    useRig(rig);
    open();
    frame();
    EXPECT_TRUE(window_.has("Moza R3: Wheel"));
}

// What can go wrong loading the config: none for the Target (and the way back to the G920), an empty file, a config
// with problems; the G920 itself has no way back.
TEST_F(EditView, ConfigsThatCannotBeUsed) {
    useRig(testRig());
    std::filesystem::remove(dir_ / "config" / "g27.ini");
    write(dir_ / "config" / "g25.ini", "");
    open();
    picker_->useTarget(ConfigPicker::Target::G27);
    frame();
    EXPECT_TRUE(window_.hasPart("No config: config/g27.ini is not next to the program."));
    view_->setPage(MappingEditView::Page::Games);
    frame();
    EXPECT_TRUE(window_.has("Pick another Target on the Mapping tab (M)."));
    view_->setPage(MappingEditView::Page::Mapping);
    ASSERT_TRUE(click("Back to the G920"));
    frame();
    EXPECT_EQ(picker_->target(), ConfigPicker::Target::G920);

    picker_->useTarget(ConfigPicker::Target::G25);
    frame();
    EXPECT_TRUE(window_.hasPart("Cannot read"));

    std::filesystem::remove(dir_ / "config" / "g920.ini");
    picker_->useTarget(ConfigPicker::Target::G920);
    frame();
    EXPECT_TRUE(window_.hasPart("No config"));
    EXPECT_FALSE(window_.has("Back to the G920"));

    write(dir_ / "config" / "g920.ini", "[emulator]\ntarget_profile = g29\nno_such_key = 1\n");
    picker_->refresh();
    frame();
    EXPECT_TRUE(window_.hasPart("The config has problems"));
    EXPECT_TRUE(window_.hasPart("G920"));   // an unknown target: the G920's inputs
}

// The user's own mappings at WheelBri's older force limits are moved to 1 / 1 when the tab loads, said in the status
// and a toast; with a config that has problems the problems stay in the status.
TEST_F(EditView, OlderForceLimitsAreMoved) {
    useRig(testRig());
    std::string text = read(dir_ / "config" / "g920.ini");
    text += "\n[ffb]\nmaster_gain = 0.7\nmax_magnitude = 0.8\n";
    write(userConfigFolder() / "my-g920.ini", text);
    write(userConfigFolder() / "my-g25.ini", text);
    write(userConfigFolder() / "notes.ini", text);        // not my-*: left alone
    write(userConfigFolder() / "my-old.txt", text);        // not .ini: left alone
    std::filesystem::create_directories(userConfigFolder() / "my-folder.ini");
    open();
    frame();
    EXPECT_TRUE(window_.hasPart("Force feedback: strength and cap are now 1.00"));
    EXPECT_TRUE(window_.hasPart("2 mappings"));
    EXPECT_EQ(view_->takeToast(), "Force limits now 1.00 / 1.00");
    EXPECT_NE(read(userConfigFolder() / "notes.ini").find("0.7"), std::string::npos);

    write(userConfigFolder() / "my-g920.ini", text + "[emulator]\nno_such_key = 1\n");
    picker_->refresh();
    picker_->useTarget(ConfigPicker::Target::G27);
    picker_->useTarget(ConfigPicker::Target::G920);
    frame();
    EXPECT_TRUE(window_.hasPart("The config has problems"));
    EXPECT_EQ(view_->takeToast(), "Force limits now 1.00 / 1.00");

    write(userConfigFolder() / "my-g920.ini", text);
    picker_->refresh();
    picker_->useTarget(ConfigPicker::Target::G27);
    picker_->useTarget(ConfigPicker::Target::G920);
    frame();
    EXPECT_TRUE(window_.hasPart("1 mapping."));
}

// The rig is read again every 2 s (the Rig tab may have saved it), not every frame.
TEST_F(EditView, TheRigIsReadAgainEveryTwoSeconds) {
    useRig(testRig());
    open();
    frame();
    Rig renamed = testRig();
    renamed.devices[0].axes[0].label = "Rim";
    useRig(renamed);
    nowMs_ += 1000;
    frame();
    EXPECT_TRUE(window_.has("Moza R3: Wheel"));
    nowMs_ += 1000;
    frame();
    EXPECT_TRUE(window_.has("Moza R3: Rim"));
}

// --- secondary sources ("or") ---------------------------------------------------------------------------------------

// The secondary popup: AXES (one a row, each with the slider of the travel it presses past, greyed until the axis is
// added), then BUTTONS. The slider pressed at its ends (0%, 100%) and dragged; a typed number from 0 to 100 (whole
// percent; past 100 refused).
TEST_F(EditView, AnAxisThresholdFromZeroToAHundred) {
    useRig(testRig());
    open();
    frame();
    ASSERT_TRUE(click("+"));
    frame();
    // The popup's headings: drawn last (the tab under it has its own AXES and BUTTONS).
    const auto lastOf = [&](const std::string& value) -> const Window::Text* {
        const Window::Text* found = nullptr;
        for (const auto& t : window_.texts) {
            if (t.value == value) found = &t;
        }
        return found;
    };
    const Window::Text* const axes = lastOf("AXES");
    const Window::Text* const buttons = lastOf("BUTTONS");
    ASSERT_NE(axes, nullptr);
    ASSERT_NE(buttons, nullptr);
    EXPECT_LT(axes->y, buttons->y);
    const Window::Text* hand = window_.find("axis 3 Hand");
    ASSERT_NE(hand, nullptr);
    EXPECT_LT(hand->y, buttons->y);   // the axes above the buttons
    // The text drawn on the Hand row, `value` (its slider's ends, its field).
    const auto onRow = [&](const std::string& value) -> const Window::Text* {
        for (const auto& t : window_.texts) {
            if (t.value == value && std::fabs(t.y - window_.find("axis 3 Hand")->y) < 1.0F) return &t;
        }
        return nullptr;
    };
    ASSERT_NE(onRow("--"), nullptr);   // not added: greyed, nothing to set
    ASSERT_TRUE(click("axis 3 Hand"));   // added: its row comes alive at 50%
    frame();
    ASSERT_NE(onRow("50%"), nullptr);
    const float zeroX = onRow("0%")->x;
    const float zeroY = onRow("0%")->y;
    const float hundredX = onRow("100%")->x;
    view_->onClick(zeroX + 22, zeroY);   // the slider's left end
    view_->onPointer(zeroX + 22, 0, false);
    frame();
    EXPECT_TRUE(window_.has("Hand > 0%"));
    view_->onClick(zeroX + 24, zeroY);
    view_->onPointer(hundredX + 400, 0, true);   // dragged past its right end
    view_->onPointer(hundredX + 400, 0, false);
    frame();
    EXPECT_TRUE(window_.has("Hand > 100%"));
    int found = 0;   // the second "100%" on the row: the field (the first is the slider's end)
    for (const auto& t : window_.texts) {
        if (t.value == "100%" && std::fabs(t.y - zeroY) < 1.0F && ++found == 2) view_->onClick(t.x + 1, t.y + 1);
    }
    ASSERT_EQ(found, 2);
    type(std::string(4, ui::kBackspace) + "101" + std::string(1, ui::kEnter));
    frame();
    EXPECT_TRUE(window_.hasPart("Not changed: an axis presses a button from 0%"));
    type(std::string(3, ui::kBackspace) + "37" + std::string(1, ui::kEnter));
    frame();
    EXPECT_TRUE(window_.has("Hand > 37%"));
    // Its Inverted, on its own row.
    const Window::Text* inverted = nullptr;
    for (const auto& t : window_.texts) {
        if (t.value == "Inverted" && std::fabs(t.y - zeroY) < 1.0F) inverted = &t;
    }
    ASSERT_NE(inverted, nullptr);
    view_->onClick(inverted->x + 1, inverted->y + 1);
    ASSERT_TRUE(click("Done"));
    frame();
    EXPECT_NE(own().find("axis 3 inverted > 37%"), std::string::npos) << own();
}

// A target button's secondaries: buttons and axes toggled in their popup, an axis' threshold and direction, at most 8,
// cleared all at once; the field shows them as chips.
TEST_F(EditView, SecondarySources) {
    useRig(testRig());
    open();
    frame();
    ASSERT_TRUE(click("+"));   // the first button row's "+" (none yet)
    frame();
    ASSERT_TRUE(window_.hasPart("What else presses"));
    EXPECT_TRUE(window_.hasPart("0 of 8."));
    ASSERT_TRUE(click("axis 3 Hand"));   // an axis: its row's slider and field come alive
    frame();
    ASSERT_TRUE(click("50%"));   // the threshold typed: any whole percent of the travel
    type(std::string(3, ui::kBackspace) + "25" + std::string(1, ui::kEnter));
    frame();
    ASSERT_TRUE(click("0 K0"));
    ASSERT_TRUE(click("1 K1"));
    frame();
    ASSERT_TRUE(click("Done"));
    frame();
    EXPECT_TRUE(window_.has("or Hand > 25%"));
    EXPECT_TRUE(window_.has("+2"));   // K0 and K1: no room for their chips on this canvas
    const Window::Text* const chip = window_.find("or Hand > 25%");
    ASSERT_NE(chip, nullptr);
    view_->setHover(chip->x, chip->y, true);
    frame();
    EXPECT_TRUE(window_.hasPart("Also pressed by: Hand > 25%"));
    EXPECT_NE(own().find("_or"), std::string::npos) << own();

    // Full: 8, the rest greyed. A chip opens the popup too.
    ASSERT_TRUE(click("or Hand > 25%"));
    for (int k = 2; k < 8; ++k) ASSERT_TRUE(click(std::to_string(k) + " K" + std::to_string(k)));
    frame();
    EXPECT_TRUE(window_.hasPart("8 of 8."));
    const Window::Text* const ninth = window_.find("9 K9");
    ASSERT_NE(ninth, nullptr);
    view_->setHover(ninth->x, ninth->y, true);
    frame();
    EXPECT_TRUE(window_.hasPart("At most 8 secondary assignments per button"));
    // A chosen one again: taken off.
    ASSERT_TRUE(click("axis 3 Hand"));
    frame();
    EXPECT_TRUE(window_.hasPart("7 of 8."));
    ASSERT_TRUE(click("(clear all)"));
    frame();
    EXPECT_TRUE(window_.hasPart("0 of 8."));
    view_->onKey(ui::kEscape);
    frame();
    EXPECT_FALSE(view_->popupOpen());
}

// The app's notice above the tab bar (UiWindow::bottomReserve): the tab's status line moves up out of its way.
TEST_F(EditView, RoomForTheAppsNotice) {
    useRig(testRig());
    open();
    // The lowest text at the left edge: the status line.
    const auto lowest = [this] {
        float y = 0;
        for (const auto& t : window_.texts) {
            if (t.x == 4) y = std::max(y, t.y);
        }
        return y;
    };
    frame();
    const float y = lowest();
    ASSERT_GT(y, 600.0F);
    window_.setBottomReserve(26);
    frame();
    EXPECT_FLOAT_EQ(lowest(), y - 26);
}

// Section headings are in the text colour (drawn bold by the real windows): blue means selected or an action.
TEST_F(EditView, HeadingsAreTextColoured) {
    useRig(testRig());
    open();
    frame();
    for (const char* name : {"AXES", "BUTTONS", "STEERING ROTATION"}) {
        const Window::Text* const heading = window_.find(name);
        ASSERT_NE(heading, nullptr) << name;
        EXPECT_TRUE(sameColour(heading->colour, style::kText)) << name;
    }
}

// Every button row ends in a "+" button (blue, tinted at rest, filled while pointed at; a hover text says what it does)
// that opens the secondary popup. What is added shows as "or ..." chips in the row's field; the "+" stays.
TEST_F(EditView, EveryButtonRowHasAPlus) {
    useRig(testRig());
    open();
    frame();
    const std::size_t plusses = window_.count("+");
    EXPECT_GT(plusses, 0U);
    const Window::Text* plus = window_.find("+");
    ASSERT_NE(plus, nullptr);
    EXPECT_TRUE(sameColour(plus->colour, style::kHeading));
    view_->setHover(plus->x, plus->y, true);
    frame();
    plus = window_.find("+");
    ASSERT_NE(plus, nullptr);
    EXPECT_TRUE(sameColour(plus->colour, style::kDark));
    EXPECT_TRUE(window_.hasPart("Add a second control that activates it too"));
    view_->setHover(0, 0, false);
    ASSERT_TRUE(click("+"));
    frame();
    ASSERT_TRUE(click("0 K0"));
    ASSERT_TRUE(click("Done"));
    frame();
    EXPECT_EQ(window_.count("+"), plusses);
    EXPECT_TRUE(window_.has("or K0"));
    plus = window_.find("+");
    view_->setHover(plus->x, plus->y, true);
    frame();
    EXPECT_TRUE(window_.hasPart("Add another control that activates it too (1 of 8)"));
}

// The secondary popup's find box takes what is typed while it is open: only the entries holding it (any case) are
// listed. Escape clears it first, then closes the popup.
TEST_F(EditView, TheSecondaryPopupFinds) {
    useRig(testRig());
    open();
    frame();
    ASSERT_TRUE(click("+"));
    frame();
    EXPECT_TRUE(view_->editingText());   // letters are not the window's shortcuts while it is open
    EXPECT_TRUE(window_.has("1 K1"));
    type("k3");
    frame();
    EXPECT_TRUE(window_.has("k3_"));
    EXPECT_TRUE(window_.has("3 K3"));
    EXPECT_TRUE(window_.has("37 K37"));
    EXPECT_FALSE(window_.has("1 K1"));
    EXPECT_FALSE(window_.has("axis 3 Hand"));
    EXPECT_EQ(window_.count("AXES"), 1U);   // the tab's own only: the popup's AXES has nothing to list
    type(std::string(1, ui::kBackspace) + std::string(1, ui::kBackspace) + "HAND");
    frame();
    EXPECT_TRUE(window_.has("axis 3 Hand"));
    EXPECT_FALSE(window_.has("3 K3"));
    type("zz");
    frame();
    EXPECT_TRUE(window_.hasPart("No control holds \"HANDzz\"."));
    view_->onKey(ui::kEscape);
    frame();
    EXPECT_TRUE(view_->popupOpen());
    EXPECT_TRUE(window_.has("1 K1"));
    view_->onKey(ui::kEscape);
    frame();
    EXPECT_FALSE(view_->popupOpen());
}

// What does not fit the popup scrolls (the mouse wheel), and the line at the bottom says how many are out of view;
// the button box's name is shown whole.
TEST_F(EditView, TheSecondaryPopupScrolls) {
    useRig(testRig());
    open();
    frame();
    ASSERT_TRUE(click("+"));
    frame();
    EXPECT_TRUE(window_.hasPart("Button box with a very long name indeed, longer than forty"));
    ASSERT_TRUE(window_.hasPart(" more below: scroll with the mouse wheel."));
    EXPECT_FALSE(window_.has("63 K63"));
    for (int i = 0; i < 20; ++i) view_->onScroll(-1);
    frame();
    EXPECT_TRUE(window_.has("63 K63"));
    EXPECT_FALSE(window_.hasPart("more below"));
    ASSERT_TRUE(click("63 K63"));
    frame();
    EXPECT_TRUE(window_.hasPart("Has 1 of 8."));
    for (int i = 0; i < 20; ++i) view_->onScroll(1);   // back to the top
    frame();
    EXPECT_TRUE(window_.hasPart(" more below"));
}

// A control touched on the rig while the popup is open: a named one is added (an axis with its row alive) and says so,
// touched again it stays; one with no name in the Rig tab says to name it there.
TEST_F(EditView, TheSecondaryPopupTakesTouches) {
    Rig rig = testRig();
    rig.devices[2].buttons[63] = {};   // the box's last button has no name
    useRig(rig);
    open();
    frame();
    EXPECT_FALSE(view_->wantsRigTouches());
    ASSERT_TRUE(click("+"));
    frame();
    ASSERT_TRUE(view_->wantsRigTouches());
    view_->onRigTouch(kBoxId, false, 37);
    frame();
    EXPECT_TRUE(window_.hasPart("Added 37 K37."));
    EXPECT_TRUE(window_.hasPart("Has 1 of 8."));
    view_->onRigTouch(kBoxId, false, 37);
    frame();
    EXPECT_TRUE(window_.hasPart("37 K37: already here"));
    EXPECT_TRUE(window_.hasPart("Has 1 of 8."));
    view_->onRigTouch(kWheelId, true, 3);   // the wheel's Hand lever
    frame();
    EXPECT_TRUE(window_.hasPart("Added axis 3 Hand."));
    EXPECT_TRUE(window_.has("50%"));
    view_->onRigTouch(kBoxId, false, 63);
    frame();
    EXPECT_TRUE(window_.hasPart("has no name in the Rig tab yet"));
    EXPECT_TRUE(window_.hasPart("Has 2 of 8."));
    nowMs_ += 4000;   // the note goes after a few seconds
    frame();
    EXPECT_FALSE(window_.hasPart("has no name"));
    ASSERT_TRUE(click("Done"));
    frame();
    EXPECT_TRUE(window_.has("or K37"));
}

// A secondary chip the rig has not named shows in amber; one used twice is outlined red; with no rig the popup says so.
TEST_F(EditView, SecondariesThatCannotBeUsed) {
    useOlderDefault();
    std::string text = read(dir_ / "config" / "g920.ini");
    text += "\n[buttons]\ntarget_0_or = 13, 40, axis 2 > 50%\n";
    write(userConfigFolder() / "my-g920.ini", text);
    open();
    frame();
    const Window::Text* const chip = window_.find("or button 13");
    ASSERT_NE(chip, nullptr);
    EXPECT_TRUE(sameColour(chip->colour, style::kText));
    EXPECT_TRUE(window_.outlinedAt(chip->x, chip->y, style::kWarning));   // 13 is also button 4's
    view_->onClick(chip->x + 1, chip->y + 1);
    frame();
    EXPECT_TRUE(window_.hasPart("The rig has no devices yet"));
    ASSERT_TRUE(click("Done"));

    useRig(testRig());
    nowMs_ += 2000;
    frame();
    const Window::Text* const named = window_.find("or B13");
    ASSERT_NE(named, nullptr);
    EXPECT_TRUE(sameColour(named->colour, style::kText));
    const Window::Text* const absent = window_.find("or button 40");
    ASSERT_NE(absent, nullptr);
    EXPECT_TRUE(sameColour(absent->colour, style::kCaution));   // button 40 is not in the rig
    view_->setHover(named->x, named->y, true);
    frame();
    EXPECT_TRUE(window_.hasPart("Used twice"));
    EXPECT_TRUE(window_.hasPart("50%"));
}


// --- the Games page ---------------------------------------------------------------------------------------------

void putU32(std::string& out, const std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFU));
}

// NFS Unbound's settings file (Frostbite FBCHUNKS, as GameSettingsTest builds it) with the three values its profile
// changes.
std::string unboundFile(const std::string& ffb = "0.500000", const std::string& spring = "0.500000",
                        const std::string& rotation = "270.000000") {
    std::string out = "FBCHUNKS";
    out += std::string("\x01\x00\x08\x00\x00\x00\x00\x00\x02\x00", 10);
    putU32(out, 3);
    const auto entry = [&out](const std::string& name, const std::string& value) {
        putU32(out, 1);
        putU32(out, static_cast<std::uint32_t>(name.size() + 1));
        out += name;
        out.push_back('\0');
        putU32(out, static_cast<std::uint32_t>(value.size() + 1));
        out += value;
        out.push_back('\0');
    };
    entry("WheelFFBMagnitude", ffb);
    entry("WheelSpringMagnitude", spring);
    entry("WheelMaxRotation", rotation);
    out += std::string(64, '\0');
    return out;
}

constexpr const char* kUnboundRelative = "Need For Speed(TM) Unbound/settings/PROFILEOPTIONS";

// The list: the built-in profiles with their tags, the first one for the target picked; a click picks another; its
// name, program and how it is found; the sub-tabs (Game file only for a game with settings).
TEST_F(EditView, TheGamesList) {
    useRig(testRig());
    open();
    games();
    EXPECT_TRUE(window_.has("GAMES"));
    EXPECT_EQ(window_.count("built in"), 4U);
    EXPECT_TRUE(window_.has("program nfs16.exe"));
    EXPECT_TRUE(window_.hasPart("While WheelBri runs (Start) it finds the game"));
    EXPECT_TRUE(window_.has("Axes"));
    EXPECT_FALSE(window_.has("Game file"));
    EXPECT_TRUE(window_.has("change them on the Mapping tab (M)"));
    ASSERT_TRUE(click("NFS Unbound"));
    frame();
    EXPECT_TRUE(window_.has("Game file"));
    view_->selectGame("no-such-game");   // unknown: nothing changes
    frame();
    EXPECT_TRUE(window_.has("program needforspeedunbound.exe"));
    view_->selectGame("nfspayback");
    frame();
    EXPECT_TRUE(window_.has("program needforspeedpayback.exe"));
    EXPECT_EQ(view_->page(), MappingEditView::Page::Games);
}

// A game that runs: "running" in the list, and what is in use for it; while a session runs, the others say they are
// not running; nothing picked yet: the one that runs is shown.
TEST_F(EditView, TheGameThatRuns) {
    useRig(testRig());
    open();
    MappingEditView::GameNow now;
    now.session = true;
    now.id = "nfspayback";
    now.profileApplies = true;
    now.pid = 42;
    now.forMs = 3'723'000;
    now.rotation = "180 deg";
    now.ffbGain = 3.0F;
    now.warning = "SOMETHING TO FIX";
    view_->setGameNow(now);
    games();
    EXPECT_TRUE(window_.has("running"));
    EXPECT_TRUE(window_.has("RUNNING, pid 42, for 1:02:03: its profile and your values are in use"));
    EXPECT_TRUE(window_.has("now: steering 180 deg, force feedback x3"));
    EXPECT_TRUE(window_.has("SOMETHING TO FIX"));
    ASSERT_TRUE(click("NFS 2015"));
    frame();
    EXPECT_TRUE(window_.has("not running (another game with a profile runs)"));

    now.id = "nfs2015";
    now.profileApplies = false;
    now.rotation.clear();
    now.ffbGain = 1.0F;
    now.warning.clear();
    view_->setGameNow(now);
    frame();
    EXPECT_TRUE(window_.hasPart(": your values are in use"));
    EXPECT_TRUE(window_.has("now: pedals as below"));

    now.id.clear();
    view_->setGameNow(now);
    frame();
    EXPECT_TRUE(window_.has("not running: WheelBri looks for it every 2 s"));
}

// A game the target does not support: "! not G923" in the list and an amber box saying what goes wrong; no profile
// fits the G25: the first game is picked.
TEST_F(EditView, AGameTheTargetDoesNotSupport) {
    useRig(testRig());
    open();
    picker_->useTarget(ConfigPicker::Target::G923);
    games();
    EXPECT_TRUE(window_.has("! not G923"));
    ASSERT_TRUE(click("NFS Unbound"));
    frame();
    EXPECT_TRUE(window_.hasPart("! NFS Unbound does not support the G923"));
    picker_->useTarget(ConfigPicker::Target::G25);
    open();   // nothing picked yet
    games();
    EXPECT_TRUE(window_.has("program nfs16.exe"));
}

// The Axes sub-tab: the game's own steering rotation (typed, "off", Default), each pedal's axis (< >, the clutch also
// none) and direction, Default; two pedals on one axis, and an axis the virtual wheel has not, are said.
TEST_F(EditView, TheAxesSubTab) {
    useRig(testRig());
    open();
    games();
    ASSERT_TRUE(click("NFS Payback"));
    frame();
    EXPECT_TRUE(window_.hasPart("Set your wheel's own software to 180 deg"));
    ASSERT_TRUE(click("180"));   // its rotation (the profile's)
    type(std::string(3, ui::kBackspace) + "zof" + std::string(1, ui::kEnter));
    frame();
    EXPECT_TRUE(window_.hasPart("expected degrees (180) or off"));
    type(std::string(3, ui::kBackspace) + "OFF" + std::string(1, ui::kEnter));
    frame();
    EXPECT_NE(own().find("nfspayback = off"), std::string::npos) << own();
    EXPECT_EQ(view_->takeToast(), "Saved");
    ASSERT_TRUE(click("Default"));   // the rotation's
    frame();
    EXPECT_EQ(own().find("nfspayback = off"), std::string::npos) << own();

    // The throttle: < and > through the axes; Inverted; Default.
    ASSERT_TRUE(click(">"));
    frame();
    EXPECT_NE(own().find("nfspayback.throttle_axis"), std::string::npos) << own();
    ASSERT_TRUE(click("<"));
    ASSERT_TRUE(click("<"));
    ASSERT_TRUE(click("Normal"));
    frame();
    EXPECT_NE(own().find("nfspayback.throttle_invert = true"), std::string::npos) << own();
    ASSERT_TRUE(window_.has("yours"));
    EXPECT_TRUE(sameColour(window_.find("yours")->colour, style::kMine));   // violet: yours, for this game
    ASSERT_TRUE(click("Default", 1));
    frame();
    EXPECT_EQ(own().find("nfspayback.throttle_invert"), std::string::npos) << own();
    // The clutch goes on to none after the slider.
    ASSERT_TRUE(click(">", 2));
    frame();
    EXPECT_NE(own().find("nfspayback.clutch_axis = none"), std::string::npos) << own();
    // The throttle on Rx: the virtual wheel has no such pedal axis (said in Mode Virtual only).
    ASSERT_TRUE(click(">"));
    ASSERT_TRUE(click(">"));
    frame();
    EXPECT_TRUE(window_.hasPart("Mode Virtual: the virtual G920 has only"));
    // The throttle on the brake's axis: two pedals on one axis.
    ASSERT_TRUE(click(">"));
    ASSERT_TRUE(click(">"));
    frame();
    EXPECT_TRUE(window_.hasPart("Two pedals on one axis"));
}

// The Force feedback sub-tab: every number typed (checked as the config reader checks it; cap and slew lower only),
// each Default, the switches; NFS Payback's profile gain said; while the game runs, what it asks and what is sent.
TEST_F(EditView, TheForceFeedbackSubTab) {
    useRig(testRig());
    open();
    games();
    ASSERT_TRUE(click("NFS Payback"));
    ASSERT_TRUE(click("Force feedback"));
    frame();
    EXPECT_TRUE(window_.hasPart("Its game profile: Steering force and Vibration x3"));
    EXPECT_TRUE(window_.hasPart("Default below goes back to it"));
    EXPECT_TRUE(window_.hasPart("the profile's; constant force"));
    const auto setField = [this](const std::string& row, const std::string& typed) {
        ASSERT_TRUE(clickRight(row));
        type(std::string(8, ui::kBackspace) + typed + std::string(1, ui::kEnter));
        frame();
    };
    setField("Overall strength", "0.5");
    EXPECT_NE(own().find("nfspayback.ffb_strength = 0.5"), std::string::npos) << own();
    setField("Overall strength", "5");
    EXPECT_TRUE(window_.hasPart("Not changed: '5': "));
    view_->onKey(ui::kEscape);
    setField("Steering force", "2");
    EXPECT_NE(own().find("nfspayback.ffb_constant = 2"), std::string::npos) << own();
    EXPECT_TRUE(window_.hasPart("your own values below replace it"));
    setField("Vibration", "1.5");
    EXPECT_NE(own().find("nfspayback.ffb_periodic = 1.5"), std::string::npos) << own();
    setField("Damper", "0.5");
    setField("Cap", "0.5");
    EXPECT_NE(own().find("nfspayback.ffb_cap = 0.5"), std::string::npos) << own();
    setField("Cap", "1");   // back up to the mapping's: allowed
    setField("Cap", "2");   // over 1: the reader's own check
    EXPECT_TRUE(window_.hasPart("Not changed: '2': "));
    view_->onKey(ui::kEscape);
    setField("Slew limit", "100");   // over the mapping's
    EXPECT_TRUE(window_.hasPart("a game's limit can only be lower than the mapping's (50)"));
    type(std::string(8, ui::kBackspace) + "20" + std::string(1, ui::kEnter));
    frame();
    EXPECT_NE(own().find("nfspayback.ffb_slew = 20"), std::string::npos) << own();
    setField("Fade a frozen push", "1");
    EXPECT_NE(own().find("nfspayback.ffb_fade_s = 1"), std::string::npos) << own();
    ASSERT_TRUE(clickRight("Turn the other way"));
    ASSERT_TRUE(clickRight("Game effects"));
    frame();
    EXPECT_NE(own().find("nfspayback.ffb_invert = true"), std::string::npos) << own();
    EXPECT_NE(own().find("nfspayback.ffb_effects = false"), std::string::npos) << own();
    // A typed gain replaces the older [game_ffb_gain] line for the game.
    {
        write(picker_->ownPath(), own() + "\n[game_ffb_gain]\nnfspayback = 2\n");
        picker_->refresh();
        frame();
        setField("Steering force", "2.5");
        EXPECT_EQ(own().find("[game_ffb_gain]\nnfspayback"), std::string::npos) << own();
    }

    // Every Default: back to where it came from, and no value left for the game.
    for (int i = 0; i < 12; ++i) {
        frame();
        const Window::Text* button = nullptr;
        for (const Window::Text& text : window_.texts) {
            if (text.value == "Default" && !sameColour(text.colour, style::kFaint)) button = &text;
        }
        if (button == nullptr) break;
        view_->onClick(button->x + 1, button->y + 1);
    }
    frame();
    EXPECT_EQ(own().find("nfspayback.ffb"), std::string::npos) << own();

    // While it runs: per kind what it asks and what reaches the wheel.
    MappingEditView::GameNow now;
    now.session = true;
    now.id = "nfspayback";
    now.ffbLive = true;
    now.asked[0] = 0.5F;
    now.sent[0] = 0.25F;
    view_->setGameNow(now);
    frame();
    EXPECT_TRUE(window_.hasPart("50% -> 25%"));
    EXPECT_TRUE(window_.hasPart("not used now"));
    EXPECT_TRUE(window_.hasPart("running: blue what the game asks"));
}

// The Buttons sub-tab: none changed; Add button... (Cancel; a button, then its control for this game only, any
// control; one the Mapping tab gives it already adds nothing), its row (click to change, Remove).
TEST_F(EditView, TheButtonsSubTab) {
    useRig(testRig());
    open();
    games();
    ASSERT_TRUE(click("Buttons"));
    frame();
    EXPECT_TRUE(window_.hasPart("No button changed for this game"));
    ASSERT_TRUE(click("Add button..."));
    frame();
    EXPECT_TRUE(window_.hasPart("Add button: which G920 button"));
    view_->onClick(2, 2);   // outside: nothing
    ASSERT_TRUE(click("Cancel"));
    ASSERT_TRUE(click("Add button..."));
    ASSERT_TRUE(click("1 A"));
    frame();
    EXPECT_TRUE(window_.hasPart("What drives button 1 A in NFS 2015?"));
    EXPECT_TRUE(window_.hasPart("For this game only"));
    ASSERT_TRUE(click("0 B0"));   // what the Mapping tab gives it: nothing added
    frame();
    EXPECT_TRUE(window_.hasPart("already comes from that control in the Mapping tab: nothing added"));
    EXPECT_TRUE(window_.hasPart("No button changed for this game"));

    ASSERT_TRUE(click("Add button..."));
    ASSERT_TRUE(click("1 A"));
    ASSERT_TRUE(click("1 B1"));
    frame();
    EXPECT_TRUE(window_.has("this game"));
    EXPECT_TRUE(window_.has("Moza R3: B1"));
    EXPECT_NE(own().find("nfs2015.button_0 = 1"), std::string::npos) << own();
    // The Add popup marks it as yours.
    ASSERT_TRUE(click("Add button..."));
    frame();
    EXPECT_TRUE(window_.has("1 A  yours"));
    ASSERT_TRUE(click("Cancel"));
    // Its field: the picker again; the Mapping tab's own control takes the game's value away.
    ASSERT_TRUE(click("Moza R3: B1"));
    ASSERT_TRUE(click("(nothing)"));
    frame();
    EXPECT_TRUE(window_.has("nothing"));
    ASSERT_TRUE(click("nothing"));
    ASSERT_TRUE(click("0 B0"));
    frame();
    EXPECT_TRUE(window_.hasPart("No button changed for this game"));

    ASSERT_TRUE(click("Add button..."));
    ASSERT_TRUE(click("2 B"));
    ASSERT_TRUE(click("2 B2"));
    ASSERT_TRUE(click("Remove", 1));   // the row's (the first is the list's Remove game)
    frame();
    EXPECT_TRUE(window_.hasPart("No button changed for this game"));
}

// Many buttons changed for a game: the ones that do not fit are counted; a button in none of the target's groups is
// under OTHER.
TEST_F(EditView, ManyChangedButtons) {
    useRig(testRig());
    std::string text = read(dir_ / "config" / "g923.ini") + "\n[game_tuning]\n";
    for (int b = 0; b < 24; ++b) text += "nfs2015.button_" + std::to_string(b) + " = 3\n";
    write(userConfigFolder() / "my-g923.ini", text);
    open();
    picker_->useTarget(ConfigPicker::Target::G923);
    games();
    ASSERT_TRUE(click("Buttons"));
    frame();
    EXPECT_TRUE(window_.hasPart("more not shown: a bigger window shows them"));
    window_.setCanvas(1200, 1400);
    frame();
    EXPECT_FALSE(window_.hasPart("more not shown"));
    EXPECT_TRUE(window_.has("OTHER"));
}

// The Game file sub-tab: the file under Documents not there yet, there with other values (Save to file writes them,
// then it holds them), not readable; no Documents folder; the game's own settings typed, with Default.
TEST_F(EditView, TheGameFileSubTab) {
    useRig(testRig());
    open();
    games();
    ASSERT_TRUE(click("NFS Unbound"));
    ASSERT_TRUE(click("Game file"));
    frame();
    EXPECT_TRUE(window_.has("GAME CONFIG"));
    EXPECT_TRUE(window_.hasPart("not found: start the game once"));
    EXPECT_TRUE(window_.has("Locate file"));
    EXPECT_TRUE(window_.hasPart("find the game's PROFILEOPTIONS file"));
    canPick_ = false;
    frame();
    EXPECT_TRUE(window_.has("no file dialog on this system yet"));
    canPick_ = true;

    const std::filesystem::path file = documents_ / kUnboundRelative;
    write(file, unboundFile());
    nowMs_ += 2000;   // read again every 2 s
    frame();
    EXPECT_TRUE(window_.hasPart("not the values below yet"));
    EXPECT_TRUE(window_.has("in the file"));
    running_ = std::vector<RunningProgram>{{5, "NeedForSpeedUnbound.exe"}};
    ASSERT_TRUE(click("Save to file"));
    frame();
    EXPECT_TRUE(window_.hasPart("is running: close it first"));
    running_ = std::vector<RunningProgram>{};
    ASSERT_TRUE(click("Save to file"));
    frame();
    EXPECT_TRUE(window_.has("Written to NFS Unbound's settings file."));
    EXPECT_TRUE(window_.hasPart("holds the values below"));
    EXPECT_TRUE(std::filesystem::exists(file.string() + ".before-wheelbri"));
    ASSERT_TRUE(click("Save to file"));
    frame();
    EXPECT_TRUE(window_.hasPart("already holds these values"));

    // The game's own settings: typed (0 to 1), Default; the rotation, the same value as the Axes sub-tab's.
    ASSERT_TRUE(clickRight("force feedback"));   // the profile's 1
    type(std::string(8, ui::kBackspace) + "2" + std::string(1, ui::kEnter));
    frame();
    EXPECT_TRUE(window_.hasPart("expected a number from 0 to 1"));
    type(std::string(8, ui::kBackspace) + "0.7" + std::string(1, ui::kEnter));
    frame();
    EXPECT_NE(own().find("nfsunbound.force_feedback = 0.7"), std::string::npos) << own();
    EXPECT_TRUE(window_.hasPart("not the values below yet"));
    ASSERT_TRUE(clickRight("force feedback", 1));   // its Default
    frame();
    EXPECT_EQ(own().find("nfsunbound.force_feedback"), std::string::npos) << own();
    ASSERT_TRUE(clickRight("steering rotation"));   // the mapping's
    type(std::string(3, ui::kBackspace) + "360" + std::string(1, ui::kEnter));
    frame();
    EXPECT_NE(own().find("nfsunbound = 360"), std::string::npos) << own();

    // The file gone between the draw and the press: nothing there to write.
    frame();
    std::filesystem::remove(file);
    const Window::Text* const save = window_.find("Save to file");
    ASSERT_NE(save, nullptr);
    view_->onClick(save->x + 1, save->y + 1);
    frame();
    EXPECT_TRUE(window_.hasPart("No settings file there"));

    // Not readable as the game's: said, Save greyed.
    write(file, std::string(2 * 1024 * 1024, 'x'));   // over 1 MB: not the game's
    nowMs_ += 2000;
    frame();
    EXPECT_TRUE(window_.hasPart("cannot be read:"));
    // Damaged between the draw and the press: not written.
    write(file, unboundFile());
    nowMs_ += 2000;
    frame();
    write(file, "FBCHUNKS damaged");
    const Window::Text* const again = window_.find("Save to file");
    ASSERT_NE(again, nullptr);
    view_->onClick(again->x + 1, again->y + 1);
    frame();
    EXPECT_TRUE(window_.hasPart("Not written:"));

    // No Documents folder (not Windows): not found here.
    documents_.clear();
    nowMs_ += 2000;
    frame();
    EXPECT_TRUE(window_.hasPart("not found here: Locate it"));
}

// Locate: cancelled; a file that is not the game's; one that is, where its profile says (its own) or elsewhere (kept,
// but Save to file asks first: the second press writes); one the game was seen writing wins over one located.
TEST_F(EditView, LocatingTheSettingsFile) {
    useRig(testRig());
    window_.setCanvas(1400, 748);   // room for the whole status line
    open();
    games();
    ASSERT_TRUE(click("NFS Unbound"));
    ASSERT_TRUE(click("Game file"));
    fileAnswers_.push_back(std::nullopt);   // cancelled
    ASSERT_TRUE(click("Locate file"));
    frame();
    ASSERT_EQ(pickTitles_.size(), 1U);
    EXPECT_NE(pickTitles_[0].find("Locate NFS Unbound's settings file (PROFILEOPTIONS)"), std::string::npos);

    const std::filesystem::path wrong = dir_ / "elsewhere" / "notes.txt";
    write(wrong, "hello");
    fileAnswers_.push_back(wrong);
    ASSERT_TRUE(click("Locate file"));
    frame();
    EXPECT_TRUE(window_.hasPart("Not NFS Unbound's settings file:"));
    std::string other = unboundFile();
    other.replace(other.find("WheelSpringMagnitude"), 20, "WheelOtherMagnitudes");
    write(wrong, other);
    fileAnswers_.push_back(wrong);
    ASSERT_TRUE(click("Locate file"));
    frame();
    EXPECT_TRUE(window_.hasPart("its settings are not in it"));

    // Elsewhere: kept, not confirmed.
    const std::filesystem::path heat = dir_ / "Heat" / "settings" / "PROFILEOPTIONS";
    write(heat, unboundFile());
    fileAnswers_.push_back(heat);
    ASSERT_TRUE(click("Locate file"));
    frame();
    EXPECT_TRUE(window_.hasPart("Kept, but NFS Unbound has not been seen writing this file"));
    EXPECT_TRUE(window_.hasPart("not seen written by NFS Unbound yet"));
    EXPECT_TRUE(window_.hasPart("Heat"));
    ASSERT_TRUE(click("Save to file"));
    frame();
    EXPECT_TRUE(window_.hasPart("Save to file again to write it anyway"));
    ASSERT_TRUE(click("Save to file"));
    frame();
    EXPECT_TRUE(window_.has("Written to NFS Unbound's settings file."));

    // Where its profile says (any Documents): its own.
    const std::filesystem::path fits = dir_ / "OneDrive" / kUnboundRelative;
    write(fits, unboundFile());
    fileAnswers_.push_back(fits);
    ASSERT_TRUE(click("Locate..."));
    frame();
    EXPECT_TRUE(window_.hasPart("Found NFS Unbound's settings file"));

    // The one it was seen writing, not the one located: that one is used.
    const std::filesystem::path seen = dir_ / "Seen" / "PROFILEOPTIONS";
    write(seen, unboundFile());
    ASSERT_TRUE(writeAppSetting(appStateFile(), "game_file_seen.nfsunbound", pathToUtf8(seen)));
    ASSERT_TRUE(writeAppSetting(appStateFile(), "game_file.nfsunbound", pathToUtf8(heat)));
    nowMs_ += 2000;
    frame();
    EXPECT_TRUE(window_.hasPart("Seen"));
    // Seen writing the one under Documents: Documents' is used.
    write(documents_ / kUnboundRelative, unboundFile());
    ASSERT_TRUE(writeAppSetting(appStateFile(), "game_file_seen.nfsunbound",
                                pathToUtf8(documents_ / kUnboundRelative)));
    ASSERT_TRUE(writeAppSetting(appStateFile(), "game_file.nfsunbound", ""));
    nowMs_ += 2000;
    frame();
    EXPECT_TRUE(window_.hasPart("Documents/Need For Speed(TM) Unbound"));

    // The state file cannot be written: the file is not remembered.
    std::filesystem::remove(appStateFile());
    std::filesystem::create_directories(appStateFile());
    fileAnswers_.push_back(fits);
    ASSERT_TRUE(click("Locate..."));
    frame();
    EXPECT_TRUE(window_.hasPart("Could not remember the file in"));
}

// When a game with a settings file ends, the file it wrote while it ran is its own (game_file_seen); none or several
// written, or the game's program still running, nothing is learnt.
TEST_F(EditView, LearningTheGamesFile) {
    useRig(testRig());
    open();
    games();
    MappingEditView::GameNow now;
    now.session = true;
    now.id = "nfsunbound";
    view_->setGameNow(now);
    // As the folder listing names it (Windows: '\' throughout), which is what is kept.
    const std::filesystem::path file = (documents_ / kUnboundRelative).make_preferred();
    write(file, unboundFile());
    running_ = std::vector<RunningProgram>{{5, "NeedForSpeedUnbound.exe"}};
    view_->setGameNow(MappingEditView::GameNow{});   // the session ended, the game still runs: not yet
    EXPECT_EQ(readAppSetting(appStateFile(), "game_file_seen.nfsunbound"), "");

    running_ = std::vector<RunningProgram>{};
    view_->setGameNow(now);
    write(file, unboundFile());
    view_->setGameNow(MappingEditView::GameNow{});
    EXPECT_EQ(readAppSetting(appStateFile(), "game_file_seen.nfsunbound"), pathToUtf8(file));
    view_->setGameNow(now);   // again: already known
    view_->setGameNow(MappingEditView::GameNow{});
    EXPECT_EQ(readAppSetting(appStateFile(), "game_file_seen.nfsunbound"), pathToUtf8(file));

    // Two written: neither is taken.
    write(documents_ / "Other" / "settings" / "PROFILEOPTIONS", unboundFile());
    ASSERT_TRUE(writeAppSetting(appStateFile(), "game_file_seen.nfsunbound", ""));
    view_->setGameNow(now);
    write(file, unboundFile());
    view_->setGameNow(MappingEditView::GameNow{});
    EXPECT_EQ(readAppSetting(appStateFile(), "game_file_seen.nfsunbound"), "");

    // A game without settings, and the programs that cannot be listed: nothing.
    now.id = "nfs2015";
    view_->setGameNow(now);
    view_->setGameNow(MappingEditView::GameNow{});
    running_.reset();
    now.id = "nfsunbound";
    std::filesystem::remove(documents_ / "Other" / "settings" / "PROFILEOPTIONS");
    view_->setGameNow(now);
    write(file, unboundFile());
    // The state file cannot be written: said in the log only.
    std::filesystem::remove(appStateFile());
    std::filesystem::create_directories(appStateFile());
    view_->setGameNow(MappingEditView::GameNow{});
    EXPECT_EQ(readAppSetting(appStateFile(), "game_file_seen.nfsunbound"), "");
}


// A game the user added gets its own steering rotation like any other: saved, read back without a problem, and gone
// with the game when it is removed.
TEST_F(EditView, AnAddedGamesRotation) {
    useRig(testRig());
    write(userGamesFile(), "[game.shift]\nlabel = NFS Shift\nprogram = shift.exe\n");
    open();
    games();
    ASSERT_TRUE(click("NFS Shift"));
    ASSERT_TRUE(clickRight("Steering", 1));   // its degrees (after "x, the wheel")
    type(std::string(4, ui::kBackspace) + "360" + std::string(1, ui::kEnter));
    frame();
    EXPECT_NE(own().find("shift = 360"), std::string::npos) << own();
    open();   // the next start reads it back
    games();
    EXPECT_FALSE(window_.hasPart("The config has problems")) << status();
    ASSERT_TRUE(click("NFS Shift"));
    frame();
    EXPECT_TRUE(window_.has("360"));
    ASSERT_TRUE(click("Remove"));
    frame();
    EXPECT_EQ(own().find("shift = 360"), std::string::npos) << own();
    open();
    games();
    EXPECT_FALSE(window_.hasPart("The config has problems")) << status();
}


// Add game...: cancelled; not a program; a program WheelBri knows already (picked, its folder listed); a new one
// (games.ini, its own id even when the name is taken, its name cut to 20 bytes at a character's end); games.ini that
// cannot be written. Without a file window the button is off.
TEST_F(EditView, AddingAGame) {
    useRig(testRig());
    write(dir_ / "shim" / kShimDll, "our shim");
    write(dir_ / "shim" / kShimSettings, "[shim]\n");
    open(true);
    games();
    fileAnswers_.push_back(std::nullopt);
    ASSERT_TRUE(click("Add game..."));
    ASSERT_EQ(pickTitles_.size(), 1U);

    fileAnswers_.push_back(dir_ / "Games" / "readme.txt");
    ASSERT_TRUE(click("Add game..."));
    frame();
    EXPECT_TRUE(window_.has("Not added: pick the game's program, a .exe file."));

    // A game it knows: picked, its folder listed.
    write(dir_ / "Games" / "NFS" / "NFS16.exe", "MZ");
    fileAnswers_.push_back(dir_ / "Games" / "NFS" / "NFS16.exe");
    ASSERT_TRUE(click("Add game..."));
    frame();
    EXPECT_TRUE(window_.has("NFS 2015 is in the list already (nfs16.exe)."));
    ASSERT_EQ(shims_->folders().size(), 1U);
    // The same folder again: listed once.
    fileAnswers_.push_back(dir_ / "Games" / "NFS" / "NFS16.exe");
    ASSERT_TRUE(click("Add game..."));
    frame();
    EXPECT_EQ(shims_->folders().size(), 1U);

    // New games: a folder that is listed, one that cannot be (no .exe in it), a name already taken, a long name.
    const std::filesystem::path shift = dir_ / "Games" / "Shift" / "NeedForSpeedShift.exe";
    write(shift, "MZ");
    fileAnswers_.push_back(shift);
    ASSERT_TRUE(click("Add game..."));
    frame();
    EXPECT_TRUE(window_.hasPart("NeedForSpeedShift added: WheelBri finds needforspeedshift.exe while it runs"));
    EXPECT_NE(read(userGamesFile()).find("[game.needforspeedshift]"), std::string::npos) << read(userGamesFile());
    EXPECT_TRUE(window_.has("yours") || window_.hasPart("no shim"));
    fileAnswers_.push_back(dir_ / "Nowhere" / "need-for-speed-shift.exe");   // the same id: another; no folder listed
    ASSERT_TRUE(click("Add game..."));
    frame();
    EXPECT_NE(read(userGamesFile()).find("[game.needforspeedshift2]"), std::string::npos) << read(userGamesFile());
    const std::filesystem::path longName = dir_ / "Games" / "abcdefghijklmnopqrs\xc3\x89tu.exe";
    fileAnswers_.push_back(longName);
    ASSERT_TRUE(click("Add game..."));
    frame();
    EXPECT_NE(read(userGamesFile()).find("label = abcdefghijklmnopqrs\n"), std::string::npos) << read(userGamesFile());
    fileAnswers_.push_back(dir_ / "Games" / "!!!.exe");   // no letters for an id: "game"
    ASSERT_TRUE(click("Add game..."));
    frame();
    EXPECT_NE(read(userGamesFile()).find("[game.game]"), std::string::npos) << read(userGamesFile());

    // games.ini cannot be written.
    std::filesystem::remove(userGamesFile());
    std::filesystem::create_directories(userGamesFile() / "x");
    fileAnswers_.push_back(dir_ / "Games" / "Other.exe");
    ASSERT_TRUE(click("Add game..."));
    frame();
    EXPECT_TRUE(window_.hasPart("Not added: cannot write"));

    canPick_ = false;
    frame();
    const Window::Text* const add = window_.find("Add game...");
    ASSERT_NE(add, nullptr);
    EXPECT_TRUE(sameColour(add->colour, style::kFaint));
}

// Remove: a built-in profile never; a game the user added goes with its values, and its folder too unless WheelBri's
// shim is in it; games.ini that cannot be written, or values that cannot be saved, keep it.
TEST_F(EditView, RemovingAGame) {
    useRig(testRig());
    write(dir_ / "shim" / kShimDll, "our shim");
    write(dir_ / "shim" / kShimSettings, "[shim]\n");
    write(userGamesFile(), "[game.shift]\nlabel = NFS Shift\nprogram = shift.exe\n[game.rivals]\nlabel = NFS Rivals\n"
                           "program = rivals.exe\n");
    write(dir_ / "Games" / "Shift" / "shift.exe", "MZ");
    write(dir_ / "Games" / "Rivals" / "rivals.exe", "MZ");
    open(true);
    std::string problem;
    ASSERT_TRUE(shims_->addFolder(dir_ / "Games" / "Shift", problem)) << problem;
    ASSERT_TRUE(shims_->addFolder(dir_ / "Games" / "Rivals", problem)) << problem;
    shims_->put(1);   // the shim in Rivals' folder
    games();
    const Window::Text* const remove = window_.find("Remove");
    ASSERT_NE(remove, nullptr);
    EXPECT_TRUE(sameColour(remove->colour, style::kFaint));   // a built-in profile is picked: never removed

    ASSERT_TRUE(click("NFS Shift"));
    ASSERT_TRUE(click("Remove"));
    frame();
    EXPECT_TRUE(window_.has("NFS Shift removed, with its own values."));
    EXPECT_EQ(read(userGamesFile()).find("shift"), std::string::npos);
    EXPECT_EQ(shims_->folders().size(), 1U);   // its folder went too

    ASSERT_TRUE(click("NFS Rivals"));
    ASSERT_TRUE(click("Remove"));
    frame();
    EXPECT_TRUE(window_.hasPart("Its folder stays listed: WheelBri's shim is in it"));
    EXPECT_EQ(shims_->folders().size(), 1U);

    // games.ini cannot be written.
    write(userGamesFile(), "[game.shift]\nlabel = NFS Shift\nprogram = shift.exe\n");
    open(true);
    games();
    ASSERT_TRUE(click("NFS Shift"));
    frame();
    std::filesystem::remove(userGamesFile());
    std::filesystem::create_directories(userGamesFile() / "x");
    const Window::Text* const button = window_.find("Remove");
    ASSERT_NE(button, nullptr);
    view_->onClick(button->x + 1, button->y + 1);
    frame();
    EXPECT_TRUE(window_.hasPart("Not removed: cannot write"));

    // Its values cannot be saved: it stays.
    std::filesystem::remove_all(userGamesFile());
    write(userGamesFile(), "[game.shift]\nlabel = NFS Shift\nprogram = shift.exe\n");
    std::string text = read(dir_ / "config" / "g920.ini") + "\n[game_tuning]\nshift.ffb_strength = 0.5\n";
    write(userConfigFolder() / "my-g920.ini", text);
    open(true);
    games();
    ASSERT_TRUE(click("NFS Shift"));
    frame();
    std::filesystem::remove_all(userConfigFolder());
    write(userConfigFolder(), "a file where the folder should be");
    const Window::Text* const again = window_.find("Remove");
    ASSERT_NE(again, nullptr);
    view_->onClick(again->x + 1, again->y + 1);
    frame();
    EXPECT_NE(read(userGamesFile()).find("shift"), std::string::npos);
}

// The FOLDER section and the list's folders: none yet (Mode Shim needs one, Mode Virtual does not), Choose folder...
// (cancelled, refused, the game's own), each shim state with what can be done (Put shim, Remove shim, Forget folder),
// a folder whose game WheelBri does not know (Add as your game...).
TEST_F(EditView, GameFolders) {
    useRig(testRig());
    write(dir_ / "shim" / kShimDll, "our shim");
    write(dir_ / "shim" / kShimSettings, "[shim]\n");
    open(true);
    games();
    EXPECT_TRUE(window_.has("FOLDER"));
    EXPECT_TRUE(window_.hasPart("none needed in Mode Virtual"));
    ASSERT_TRUE(picker_->way() == ConfigPicker::Way::VirtualDevice);
    view_->setPage(MappingEditView::Page::Mapping);
    ASSERT_TRUE(click("Shim"));   // Mode: Shim
    games();
    EXPECT_TRUE(window_.hasPart("none yet: Mode Shim puts its shim in the game's folder"));

    window_.folderAnswer.reset();
    window_.pickError = "no folder window here";
    ASSERT_TRUE(click("Choose folder..."));
    std::string problem;
    EXPECT_TRUE(shims_->takeProblem(problem));
    write(dir_ / "Games" / "NFS" / "NFS16.exe", "MZ");
    write(dir_ / "Games" / "Other" / "other.exe", "MZ");
    window_.folderAnswer = dir_ / "Games" / "Other";   // not NFS 2015's
    ASSERT_TRUE(click("Choose folder..."));
    EXPECT_TRUE(shims_->takeProblem(problem));
    EXPECT_NE(problem.find("nfs16.exe is not in that folder"), std::string::npos) << problem;
    window_.folderAnswer = dir_ / "Games" / "NFS";
    ASSERT_TRUE(click("Choose folder..."));
    games();
    EXPECT_TRUE(window_.has("no shim"));
    EXPECT_TRUE(window_.hasPart("no shim (Start puts it in)"));
    ASSERT_TRUE(click("Put shim"));
    games();
    nowMs_ += 2000;
    games();
    EXPECT_TRUE(window_.hasPart("shim in place, presents the G920"));
    EXPECT_TRUE(window_.has("shim g920"));
    ASSERT_TRUE(click("Remove shim"));
    nowMs_ += 2000;
    games();
    EXPECT_TRUE(window_.hasPart("no shim (Start puts it in)"));
    // An older shim of ours: Start replaces it; another mod's: left alone.
    write(dir_ / "Games" / "NFS" / kShimDll, "an older shim");
    write(dir_ / "Games" / "NFS" / kShimSettings, "[shim]\ntarget_profile = g25\n");
    shims_->refresh(true);
    nowMs_ += 2000;
    games();
    EXPECT_TRUE(window_.hasPart("older shim (Start replaces it), presents the G25"));
    EXPECT_TRUE(window_.has("Put shim"));
    std::filesystem::remove(dir_ / "Games" / "NFS" / kShimSettings);
    shims_->refresh(true);
    nowMs_ += 2000;
    games();
    EXPECT_TRUE(window_.hasPart("another mod's dinput8.dll: Start chains it"));
    EXPECT_TRUE(window_.has("other mod"));
    // Chained from here too, and given back.
    ASSERT_TRUE(click("Chain mod"));
    games();
    nowMs_ += 2000;
    games();
    EXPECT_TRUE(window_.hasPart("shim in place, chained to another mod's DLL, presents the G920"));
    EXPECT_TRUE(window_.has("shim g920 +mod"));
    ASSERT_TRUE(click("Remove shim"));
    nowMs_ += 2000;
    games();
    EXPECT_TRUE(window_.hasPart("another mod's dinput8.dll: Start chains it"));

    // A folder whose game WheelBri does not know: listed after the games; Add as your game... from there.
    ASSERT_TRUE(shims_->addFolder(dir_ / "Games" / "Other", problem)) << problem;
    nowMs_ += 2000;
    games();
    ASSERT_TRUE(click("Other"));
    games();
    EXPECT_TRUE(window_.hasPart("WheelBri does not know the game in this folder"));
    fileAnswers_.push_back(dir_ / "Games" / "Other" / "other.exe");
    ASSERT_TRUE(click("Add as your game..."));
    nowMs_ += 2000;
    games();
    EXPECT_TRUE(window_.hasPart("other added"));
    EXPECT_FALSE(window_.hasPart("WheelBri does not know the game in this folder"));

    // Forget folder: off the list.
    ASSERT_TRUE(click("Forget folder"));
    nowMs_ += 2000;
    games();
    EXPECT_EQ(shims_->folders().size(), 1U);
}

// What Stop does with the shims, once the user said: shown with Ask again; the Hot swap switch (not while running).
TEST_F(EditView, AtStopAndHotSwap) {
    useRig(testRig());
    open(true);
    shims_->setAtStop(ShimGamesView::AtStop::Keep);
    games();
    EXPECT_TRUE(window_.has("At Stop: the shims stay"));
    shims_->setAtStop(ShimGamesView::AtStop::Delete);
    games();
    EXPECT_TRUE(window_.has("At Stop: shims deleted"));
    ASSERT_TRUE(click("Ask again"));
    EXPECT_EQ(shims_->atStop(), ShimGamesView::AtStop::Ask);
    EXPECT_TRUE(shims_->hotSwap());
    ASSERT_TRUE(click("Hot swap"));
    EXPECT_FALSE(shims_->hotSwap());
    view_->setLocked(true);
    ASSERT_TRUE(click("Hot swap"));
    EXPECT_FALSE(shims_->hotSwap());   // fixed while running
    games();
    EXPECT_TRUE(window_.has("fixed while running"));
}

// While a session runs the Games page's values stay editable and are used at once; the status says so, or that the
// session runs on WheelBri's default (the changes then go to the user's own file for the next Start). The Mapping tab
// ends its typing and its popups when a session starts.
TEST_F(EditView, WhileASessionRuns) {
    useRig(testRig());
    open();
    games();
    ASSERT_TRUE(click("Force feedback"));
    // A value set, then typed again after a complaint: nothing to save, the status line says when values apply.
    const auto again = [this](const std::string& value) {
        ASSERT_TRUE(clickRight("Overall strength"));
        type(std::string(8, ui::kBackspace) + "5" + std::string(1, ui::kEnter));
        type(std::string(8, ui::kBackspace) + value + std::string(1, ui::kEnter));
        games();
    };
    view_->setLocked(true);   // the session runs on WheelBri's default
    ASSERT_TRUE(clickRight("Overall strength"));
    type(std::string(8, ui::kBackspace) + "0.5" + std::string(1, ui::kEnter));
    games();
    EXPECT_EQ(view_->takeToast(), "Saved");
    again("0.5");
    EXPECT_TRUE(window_.hasPart("Running with WheelBri's default mapping"));

    // A session on the user's own mapping: changes apply at once.
    view_->setLocked(false);
    view_->setLocked(true);
    ASSERT_TRUE(clickRight("Overall strength"));
    type(std::string(8, ui::kBackspace) + "0.6" + std::string(1, ui::kEnter));
    games();
    EXPECT_EQ(view_->takeToast(), "Saved: in use now");
    again("0.6");
    EXPECT_TRUE(window_.hasPart("Running: changes apply at once"));
    const Window::Text* const add = window_.find("Add game...");
    ASSERT_NE(add, nullptr);
    EXPECT_TRUE(sameColour(add->colour, style::kFaint));

    // Stopped: the hint about when values apply.
    view_->setLocked(false);
    again("0.6");
    EXPECT_TRUE(window_.hasPart("Saved as you change them"));

    // The Mapping tab: a session that starts ends its typing and its picker.
    view_->setPage(MappingEditView::Page::Mapping);
    ASSERT_TRUE(click("Moza R3: Wheel"));
    frame();
    ASSERT_TRUE(view_->popupOpen());
    view_->setLocked(true);
    EXPECT_FALSE(view_->popupOpen());
    view_->setLocked(false);
    ASSERT_TRUE(click("900"));
    view_->setLocked(true);
    EXPECT_FALSE(view_->editingText());
    view_->setLocked(false);
    frame();
    // The Games page keeps its typing and its button picker when a session starts.
    games();
    ASSERT_TRUE(click("Buttons"));
    ASSERT_TRUE(click("Add button..."));
    view_->setLocked(true);
    EXPECT_TRUE(view_->popupOpen());
    view_->setLocked(false);
    view_->onKey(ui::kEscape);
    ASSERT_TRUE(click("Force feedback"));
    ASSERT_TRUE(clickRight("Overall strength"));
    view_->setLocked(true);
    EXPECT_TRUE(view_->editingText());
}

// The Games page's values that cannot be saved: back to the saved ones, and the page says why.
TEST_F(EditView, GameValuesThatCannotBeSaved) {
    useRig(testRig());
    std::filesystem::remove_all(userConfigFolder());
    write(userConfigFolder(), "a file where the folder should be");
    open();
    games();
    ASSERT_TRUE(clickRight("Steering", 1));
    type(std::string(4, ui::kBackspace) + "360" + std::string(1, ui::kEnter));
    games();
    EXPECT_TRUE(window_.hasPart("Could not write"));
    EXPECT_FALSE(window_.has("360"));
}

// Many games: the list stops above its buttons; a long name is cut, its hover text has it whole.
TEST_F(EditView, AListLongerThanTheCanvas) {
    useRig(testRig());
    std::string list;
    for (int g = 0; g < 30; ++g) {
        list += "[game.g" + std::to_string(g) + "]\nlabel = Game number " + std::to_string(g) + " long\nprogram = g" +
                std::to_string(g) + ".exe\n";
    }
    write(userGamesFile(), list);
    write(dir_ / "shim" / kShimDll, "our shim");
    for (int f = 0; f < 3; ++f) write(dir_ / "Games" / ("F" + std::to_string(f)) / "x.exe", "MZ");
    open(true);
    std::string problem;
    for (int f = 0; f < 3; ++f) ASSERT_TRUE(shims_->addFolder(dir_ / "Games" / ("F" + std::to_string(f)), problem));
    games();
    EXPECT_TRUE(window_.hasPart("Game number 0"));
    EXPECT_FALSE(window_.hasPart("Game number 29"));
    EXPECT_FALSE(window_.has("F2"));
    const Window::Text* const cut = window_.holding("Game number 0");
    ASSERT_NE(cut, nullptr);
    view_->setHover(cut->x, cut->y, true);
    games();
    EXPECT_TRUE(window_.hasPart("Game number 0 long (yours)"));
    window_.setCanvas(1200, 1400);
    games();
    EXPECT_TRUE(window_.has("F2"));
}

// The list's width: read from app.ini once; a drag that cannot be kept (app.ini cannot be written) says so; with no
// state file it is not kept at all.
TEST_F(EditView, TheListsWidthKept) {
    useRig(testRig());
    ASSERT_TRUE(writeAppSetting(appStateFile(), "games_list_width", "250"));
    open();
    games();
    const Window::Text* const tag = window_.find("built in");
    ASSERT_NE(tag, nullptr);
    const float bar = tag->x + 8 * 8 + 6 + 4;
    EXPECT_FLOAT_EQ(bar, 250 + 4 + 4);
    view_->setHover(bar + 2, 300, true);   // hovered: the bar lights up, its hover text
    games();
    EXPECT_TRUE(window_.hasPart("Drag to make the games list wider"));
    view_->onClick(bar + 2, 300);
    view_->onPointer(bar + 2, 300, true);    // not moved yet
    view_->onPointer(bar + 30, 300, true);
    view_->setPage(MappingEditView::Page::Mapping);
    view_->onPointer(bar + 60, 300, true);   // the page changed: the drag ends
    EXPECT_EQ(readAppSetting(appStateFile(), "games_list_width"), "278");
    games();
    view_->onPointer(0, 0, true);   // no drag: nothing
    std::filesystem::remove(appStateFile());
    std::filesystem::create_directories(appStateFile());
    view_->onClick(bar + 30, 300);
    view_->onPointer(bar + 50, 300, false);
    games();
    EXPECT_TRUE(window_.hasPart("Could not keep the games list's width"));

    // No state file: the width is not kept.
    std::filesystem::remove_all(appStateFile());
    picker_ = std::make_unique<ConfigPicker>(window_, pickerSettings());
    view_ = std::make_unique<MappingEditView>(window_, rigsFolder(), std::filesystem::path(), *picker_);
    games();
    const Window::Text* const tag2 = window_.find("built in");
    ASSERT_NE(tag2, nullptr);
    const float bar2 = tag2->x + 8 * 8 + 6 + 4;
    view_->onClick(bar2 + 2, 300);
    view_->onPointer(bar2 + 40, 300, true);
    view_->onPointer(bar2 + 40, 300, false);
    EXPECT_EQ(readAppSetting(appStateFile(), "games_list_width"), "");
}


// The list's bar: two presses far apart are two presses, not a double-click; a press let go where it was keeps the
// width as it was; hovered off the bar, it does not light up.
TEST_F(EditView, TheListsBarPressedTwiceSlowly) {
    useRig(testRig());
    open();
    games();
    const Window::Text* const tag = window_.find("built in");
    ASSERT_NE(tag, nullptr);
    const float bar = tag->x + 8 * 8 + 6 + 4;
    view_->setHover(bar - 20, 300, true);
    games();
    view_->setHover(bar + 2, 10, true);   // above the bar
    games();
    view_->setHover(bar + 2, 740, true);   // below it
    games();
    view_->onClick(bar + 2, 300);
    view_->onPointer(bar + 2, 300, false);   // let go where it was: the width shown is kept
    EXPECT_EQ(readAppSetting(appStateFile(), "games_list_width"), "196");
    nowMs_ += 1000;   // much later: a press again, not a double-click (the width stays, nothing written)
    games();
    view_->onClick(bar + 2, 300);
    view_->onPointer(bar + 2, 300, false);
    games();
    EXPECT_EQ(readAppSetting(appStateFile(), "games_list_width"), "196");
}

// A folder whose game WheelBri does not know, picked, then forgotten (the page shows the picked game again); while
// it is shown, Remove is off; with two folders of one game only the first counts.
TEST_F(EditView, AnUnknownFolderPickedThenForgotten) {
    useRig(testRig());
    write(userGamesFile(), "[game.shift]\nlabel = NFS Shift\nprogram = shift.exe\n");
    write(dir_ / "shim" / kShimDll, "our shim");
    write(dir_ / "Games" / "Other" / "other.exe", "MZ");
    write(dir_ / "Games" / "NFS" / "nfs16.exe", "MZ");
    write(dir_ / "Games" / "NFS copy" / "nfs16.exe", "MZ");
    open(true);
    std::string problem;
    ASSERT_TRUE(shims_->addFolder(dir_ / "Games" / "Other", problem)) << problem;
    ASSERT_TRUE(shims_->addFolder(dir_ / "Games" / "NFS", problem)) << problem;
    ASSERT_TRUE(shims_->addFolder(dir_ / "Games" / "NFS copy", problem)) << problem;
    games();
    ASSERT_TRUE(click("NFS Shift"));
    ASSERT_TRUE(click("Other"));
    games();
    const Window::Text* const remove = window_.find("Remove");
    ASSERT_NE(remove, nullptr);
    EXPECT_TRUE(sameColour(remove->colour, style::kFaint));
    canPick_ = false;
    games();
    const Window::Text* const add = window_.find("Add as your game...");
    ASSERT_NE(add, nullptr);
    EXPECT_TRUE(sameColour(add->colour, style::kFaint));
    shims_->forget(0);
    nowMs_ += 2000;
    games();
    EXPECT_FALSE(window_.hasPart("does not know the game"));
    EXPECT_TRUE(window_.has("program shift.exe"));
    ASSERT_TRUE(click("NFS 2015"));
    games();
    EXPECT_TRUE(window_.hasPart("NFS"));   // its first folder
}

// A shim of ours with no settings next to it presents no target; the game file tab goes back to Axes for a game
// without one.
TEST_F(EditView, AShimWithoutItsSettings) {
    useRig(testRig());
    write(dir_ / "shim" / kShimDll, "our shim");
    write(dir_ / "Games" / "NFS" / "nfs16.exe", "MZ");
    write(dir_ / "Games" / "NFS" / kShimDll, "our shim");
    open(true);
    std::string problem;
    ASSERT_TRUE(shims_->addFolder(dir_ / "Games" / "NFS", problem)) << problem;
    games();
    EXPECT_TRUE(window_.has("shim in place"));
    EXPECT_TRUE(window_.has("shim "));
    ASSERT_TRUE(click("NFS Unbound"));
    ASSERT_TRUE(click("Game file"));
    ASSERT_TRUE(click("NFS 2015"));
    games();
    EXPECT_TRUE(window_.has("what the game reads; starts from its profile and your Mapping tab"));
    // Outdated: its folder stays listed when a game the user added is removed.
}

// The running game's line: force feedback alone, the pedals with a rotation; the session's force feedback not live
// for it; a kind it sends but does not ask for.
TEST_F(EditView, WhatIsInUseForTheRunningGame) {
    useRig(testRig());
    open();
    MappingEditView::GameNow now;
    now.session = true;
    now.id = "nfspayback";
    now.ffbGain = 3.0F;
    view_->setGameNow(now);
    view_->setGameNow(now);   // the same game again: nothing new
    games();
    EXPECT_TRUE(window_.has("now: force feedback x3"));
    now.id = "nfs2015";
    now.rotation = "540 deg";
    now.ffbGain = 1.0F;
    now.ffbLive = false;
    view_->setGameNow(now);
    games();
    EXPECT_TRUE(window_.has("now: steering 540 deg, pedals as below"));
    ASSERT_TRUE(click("Force feedback"));
    games();
    EXPECT_TRUE(window_.hasPart("times what the game asks"));
    now.ffbLive = true;
    now.sent[1] = 0.5F;   // sent but not asked: still shown
    view_->setGameNow(now);
    games();
    EXPECT_TRUE(window_.hasPart("0% -> 50%"));
    ASSERT_TRUE(click("Axes"));
    games();
    EXPECT_EQ(window_.count("profile"), 3U);   // its pedals come from its profile
}

// NFS 2015's pedals from its profile: each pedal's own axis and direction set apart, Default for each; an axis no
// pedal can take (x, from the file) starts < > from the first; the Mapping tab's Invert shows here.
TEST_F(EditView, PedalValuesOneByOne) {
    useOlderDefault();
    useRig(testRig());
    std::string text = read(dir_ / "config" / "g920.ini");
    text.replace(text.find("[throttle]\naxis = 2"), 0, "");
    text += "\n[game_tuning]\nnfs2015.throttle_axis = x\nnfs2015.brake_invert = true\n";
    write(userConfigFolder() / "my-g920.ini", text);
    open();
    ASSERT_TRUE(click("Invert", 3));   // the clutch, on the Mapping tab
    frame();
    games();
    EXPECT_TRUE(window_.has("Inverted"));
    ASSERT_TRUE(clickRight("Throttle", 2));   // its >: from the first axis (x is not one)
    games();
    EXPECT_NE(own().find("nfs2015.throttle_axis = z"), std::string::npos) << own();
    ASSERT_TRUE(clickRight("Throttle", 4));   // its Default (axis only)
    games();
    EXPECT_EQ(own().find("nfs2015.throttle_axis"), std::string::npos) << own();
    ASSERT_TRUE(clickRight("Brake", 4));   // its Default (direction only)
    games();
    EXPECT_EQ(own().find("nfs2015.brake_invert"), std::string::npos) << own();
    EXPECT_EQ(own().find("[game_tuning]\nnfs2015"), std::string::npos) << own();
}

// Two of a game's own values: Default drops one and keeps the other; NFS Payback's profile gain overridden by
// Vibration alone; its gain under a target its profile does not fit, with an older [game_ffb_gain] line.
TEST_F(EditView, OneValueOfSeveral) {
    useRig(testRig());
    std::string text = read(dir_ / "config" / "g920.ini");
    text += "\n[game_tuning]\nnfspayback.ffb_periodic = 2\nnfspayback.ffb_damper = 0.5\n";
    text += "[game_settings]\nnfsunbound.force_feedback = 0.7\nnfsunbound.spring = 0.6\n";
    text += "[game_ffb_gain]\nnfspayback = 2\n";
    write(userConfigFolder() / "my-g920.ini", text);
    write(userConfigFolder() / "my-g25.ini", read(dir_ / "config" / "g25.ini") + "\n[game_ffb_gain]\nnfspayback = 2\n");
    open();
    games();
    ASSERT_TRUE(click("NFS Payback"));
    ASSERT_TRUE(click("Force feedback"));
    games();
    EXPECT_TRUE(window_.hasPart("your own values below replace it"));
    ASSERT_TRUE(clickRight("Damper", 1));
    games();
    EXPECT_NE(own().find("nfspayback.ffb_periodic = 2"), std::string::npos) << own();
    EXPECT_EQ(own().find("nfspayback.ffb_damper"), std::string::npos) << own();
    ASSERT_TRUE(click("NFS Unbound"));
    ASSERT_TRUE(click("Game file"));
    ASSERT_TRUE(clickRight("force feedback", 1));
    games();
    EXPECT_NE(own().find("nfsunbound.spring = 0.6"), std::string::npos) << own();
    picker_->useTarget(ConfigPicker::Target::G25);
    games();
    ASSERT_TRUE(click("NFS Payback"));
    ASSERT_TRUE(click("Force feedback"));
    games();
    EXPECT_TRUE(window_.hasPart("Its game profile"));
}

// A game in games.ini with a built-in profile's program or id is that profile: listed once.
TEST_F(EditView, AnAddedGameThatIsABuiltInProfile) {
    useRig(testRig());
    write(userGamesFile(), "[game.copy]\nlabel = Copy\nprogram = nfs16.exe\n[game.nfspayback]\nlabel = Payback\n"
                           "program = other.exe\n");
    open();
    games();
    EXPECT_FALSE(window_.has("Copy"));
    EXPECT_FALSE(window_.has("Payback"));
    EXPECT_EQ(window_.count("built in"), 4U);   // not "yours": the built-in profile is the one used
    ASSERT_TRUE(click("NFS Payback"));
    games();
    const Window::Text* const remove = window_.find("Remove");
    ASSERT_NE(remove, nullptr);
    EXPECT_TRUE(sameColour(remove->colour, style::kFaint));   // a built-in profile is never removed
}

// Add game without the shim's folders (no Mode Shim view): only games.ini; ".exe" alone is no program.
TEST_F(EditView, AddingAGameWithoutFolders) {
    useRig(testRig());
    open();
    games();
    fileAnswers_.push_back(dir_ / ".exe");
    ASSERT_TRUE(click("Add game..."));
    games();
    EXPECT_TRUE(window_.has("Not added: pick the game's program, a .exe file."));
    fileAnswers_.push_back(dir_ / "Games" / "heat.exe");
    ASSERT_TRUE(click("Add game..."));
    games();
    EXPECT_TRUE(window_.hasPart("heat added"));
    fileAnswers_.push_back(dir_ / "Games" / "nfs16.exe");
    ASSERT_TRUE(click("Add game..."));
    games();
    EXPECT_TRUE(window_.hasPart("is in the list already"));
}

// A game the user added whose folder holds an older shim of ours: removed, its folder stays listed.
TEST_F(EditView, RemovingAGameWithAnOlderShim) {
    useRig(testRig());
    write(dir_ / "shim" / kShimDll, "our shim");
    write(userGamesFile(), "[game.shift]\nlabel = NFS Shift\nprogram = shift.exe\n");
    write(dir_ / "Games" / "Shift" / "shift.exe", "MZ");
    write(dir_ / "Games" / "Shift" / kShimDll, "an older shim");
    write(dir_ / "Games" / "Shift" / kShimSettings, "[shim]\n");
    open(true);
    std::string problem;
    ASSERT_TRUE(shims_->addFolder(dir_ / "Games" / "Shift", problem)) << problem;
    games();
    ASSERT_TRUE(click("NFS Shift"));
    ASSERT_TRUE(click("Remove"));
    games();
    EXPECT_TRUE(window_.hasPart("Its folder stays listed"));
}

// The game's button picked for one button keeps the others; a game's own value for a button that is only "nothing".
TEST_F(EditView, AGamesButtonsOneByOne) {
    useRig(testRig());
    std::string text = read(dir_ / "config" / "g920.ini");
    text += "\n[game_tuning]\nnfs2015.button_1 = 2\nnfs2015.button_2 = -1\n";
    write(userConfigFolder() / "my-g920.ini", text);
    open();
    games();
    ASSERT_TRUE(click("Buttons"));
    games();
    EXPECT_TRUE(window_.has("nothing"));
    ASSERT_TRUE(click("Add button..."));
    ASSERT_TRUE(click("4 Y"));
    games();
    EXPECT_TRUE(window_.hasPart("What drives button 4 Y in NFS 2015?"));
    ASSERT_TRUE(click("5 B5"));
    games();
    EXPECT_NE(own().find("nfs2015.button_3 = 5"), std::string::npos) << own();
    EXPECT_NE(own().find("nfs2015.button_1 = 2"), std::string::npos) << own();
}

// The file the user located is gone: locate it again; the seen file with no Documents folder; a located file the game
// was seen writing is its own.
TEST_F(EditView, TheGamesFileElsewhere) {
    useRig(testRig());
    const std::filesystem::path heat = dir_ / "Heat" / "settings" / "PROFILEOPTIONS";
    write(heat, unboundFile());
    ASSERT_TRUE(writeAppSetting(appStateFile(), "game_file.nfsunbound", pathToUtf8(heat)));
    ASSERT_TRUE(writeAppSetting(appStateFile(), "game_file_seen.nfsunbound", pathToUtf8(heat)));
    open();
    games();
    ASSERT_TRUE(click("NFS Unbound"));
    ASSERT_TRUE(click("Game file"));
    games();
    EXPECT_FALSE(window_.hasPart("not seen written by"));
    std::filesystem::remove(heat);
    nowMs_ += 2000;
    games();
    EXPECT_TRUE(window_.hasPart("no longer there: Locate it again"));
    ASSERT_TRUE(writeAppSetting(appStateFile(), "game_file.nfsunbound", ""));
    write(heat, unboundFile());
    documents_.clear();
    nowMs_ += 2000;
    games();
    EXPECT_TRUE(window_.hasPart("Heat"));
    fileAnswers_.push_back(std::nullopt);
    ASSERT_TRUE(click("Locate..."));   // no Documents folder: the dialog starts nowhere in particular
    EXPECT_EQ(pickTitles_.size(), 1U);
    view_->setLocked(true);
    games();
    const Window::Text* const save = window_.find("Save to file");
    ASSERT_NE(save, nullptr);
    EXPECT_TRUE(sameColour(save->colour, style::kFaint));
}

// Save to file while other programs run (not the game), and when the running programs cannot be listed.
TEST_F(EditView, SavingWithOtherProgramsRunning) {
    useRig(testRig());
    write(documents_ / kUnboundRelative, unboundFile());
    open();
    games();
    ASSERT_TRUE(click("NFS Unbound"));
    ASSERT_TRUE(click("Game file"));
    running_ = std::vector<RunningProgram>{{3, "bash"}};
    ASSERT_TRUE(click("Save to file"));
    games();
    EXPECT_TRUE(window_.has("Written to NFS Unbound's settings file."));
    running_.reset();
    write(documents_ / kUnboundRelative, unboundFile());
    nowMs_ += 2000;
    ASSERT_TRUE(click("Save to file"));
    games();
    EXPECT_TRUE(window_.has("Written to NFS Unbound's settings file."));
}

// A game that is not known any more (or has no settings file) ending teaches nothing.
TEST_F(EditView, AnUnknownGameEnding) {
    useRig(testRig());
    open();
    games();
    MappingEditView::GameNow now;
    now.session = true;
    now.id = "gone";
    view_->setGameNow(now);
    view_->setGameNow(MappingEditView::GameNow{});
    EXPECT_EQ(readAppSetting(appStateFile(), "game_file_seen.gone"), "");
}

// The user's own mappings at the older limits that cannot be written: left as they are (said in the log).
TEST_F(EditView, OlderLimitsThatCannotBeMoved) {
    useRig(testRig());
    std::string text = read(dir_ / "config" / "g920.ini");
    text += "\n[ffb]\nmaster_gain = 0.7\nmax_magnitude = 0.8\n";
    write(userConfigFolder() / "my-g25.ini", text);
    std::filesystem::create_directories(userConfigFolder() / "my-g25.ini.tmp" / "x");   // the write goes through it
    open();
    frame();
    EXPECT_NE(read(userConfigFolder() / "my-g25.ini").find("0.7"), std::string::npos);
    EXPECT_EQ(view_->takeToast(), "");
    // No folder for the user's mappings at all: nothing to move.
    std::filesystem::remove_all(userConfigFolder());
    picker_->useTarget(ConfigPicker::Target::G27);
    frame();
    EXPECT_TRUE(window_.has("AXES"));
}

// The pickers when the rig has no devices, or when the wheel the mapping names is not in it.
TEST_F(EditView, PickersWithoutTheWheel) {
    useOlderDefault();
    open();
    ASSERT_TRUE(click("the wheel: axis 0"));
    frame();
    EXPECT_TRUE(window_.hasPart("The rig has no devices yet"));
    view_->onKey(ui::kEscape);
    Rig rig = testRig();
    rig.devices[0].axes[3] = {};
    rig.devices[0].buttons[40] = {};
    useRig(rig);
    std::string text = read(dir_ / "config" / "g920.ini");
    text.replace(text.find("source_device ="), 15, "source_device = 9999:9999");
    text += "\n[buttons]\ntarget_0_or = 346e:0005/axis 3 > 50%, 346e:0005/40\n";
    write(userConfigFolder() / "my-g920.ini", text);
    picker_->refresh();
    nowMs_ += 2000;
    frame();
    const Window::Text* const alt = window_.find("+2");   // its chips: no room for them on this canvas but "+2"
    ASSERT_NE(alt, nullptr);
    view_->onClick(alt->x + 1, alt->y + 1);
    frame();
    EXPECT_TRUE(window_.hasPart("Moza R3  346e:0005  wheel"));   // not "(the wheel)": the mapping names another
    ASSERT_TRUE(click("0 B0"));
    ASSERT_TRUE(click("1 B1"));
    ASSERT_TRUE(click("0 B0"));   // taken off again, the others kept
    frame();
    EXPECT_TRUE(window_.hasPart("3 of 8."));
}

// The secondary picker on the smallest canvas lists as many devices as fit.
TEST_F(EditView, TheSecondaryPickerFitsTheCanvas) {
    Rig rig = testRig();
    for (std::size_t b = 0; b < MapperConfig::kMaxSourceButtons; ++b) {
        rig.devices[0].buttons[b] = {"B" + std::to_string(b), ""};
        rig.devices[1].buttons[b] = {"P", ""};
        rig.devices[3].buttons[b] = {"S", ""};
    }
    useRig(rig);
    open();
    ASSERT_TRUE(click("Show"));   // a group with nothing mapped is folded: open it for its rows
    ASSERT_TRUE(click("+"));
    frame();
    EXPECT_TRUE(window_.hasPart("What else presses"));
    EXPECT_FALSE(window_.hasPart("Shifter  1234:0003"));
}


// A device the rig gives no name: its id instead; a control named only by what it is: its number in the hover text;
// a source from a device the rig does not have: that device's id.
TEST_F(EditView, NamesFromWhatTheRigHas) {
    useOlderDefault();
    Rig rig = testRig();
    rig.devices[1].name.clear();
    rig.devices[0].axes[0] = {"", "Steering"};
    useRig(rig);
    std::string text = read(dir_ / "config" / "g920.ini");
    text.replace(text.find("[throttle]\naxis = 2"), 19, "[throttle]\naxis = 9999:0001/3");
    write(userConfigFolder() / "my-g920.ini", text);
    open();
    frame();
    EXPECT_TRUE(window_.has("Moza R3: Steering"));
    EXPECT_TRUE(window_.hasPart("9999:0001: axis 3"));
    const Window::Text* const steering = window_.find("Moza R3: Steering");
    ASSERT_NE(steering, nullptr);
    view_->setHover(steering->x, steering->y, true);
    frame();
    EXPECT_TRUE(window_.has("Moza R3: axis 0"));
    ASSERT_TRUE(click("Moza R3: Steering"));
    frame();
    EXPECT_TRUE(window_.hasPart("1234:0001  1234:0001  pedals"));   // the device without a name: its id
}

// The rig app.ini names is not there: no rig.
TEST_F(EditView, ARigThatIsNotThere) {
    ASSERT_TRUE(writeAppSetting(appStateFile(), "rig", "Gone"));
    open();
    frame();
    EXPECT_TRUE(window_.hasPart("The rig has no devices yet"));
}

// A config whose lines cannot be read at all (not KEY = VALUE): its problems say so.
TEST_F(EditView, AConfigThatCannotBeParsed) {
    useRig(testRig());
    write(userConfigFolder() / "my-g920.ini", read(dir_ / "config" / "g920.ini") + "\nthis is not a setting\n");
    open();
    frame();
    EXPECT_TRUE(window_.hasPart("The config has problems"));
}

#if !defined(_WIN32)
// A config folder whose name is not UTF-8 (Linux allows any bytes): read by its own name.
TEST_F(EditView, AConfigFolderNotInUtf8) {
    useRig(testRig());
    const std::filesystem::path odd = dir_ / std::string("conf\xff");
    std::filesystem::create_directories(odd);
    std::filesystem::copy_file(dir_ / "config" / "g920.ini", odd / "g920.ini");
    view_.reset();
    picker_ = std::make_unique<ConfigPicker>(window_, ConfigPicker::Settings{odd, userConfigFolder(), appStateFile()});
    view_ = std::make_unique<MappingEditView>(window_, rigsFolder(), appStateFile(), *picker_);
    frame();
    EXPECT_TRUE(window_.has("Moza R3: Wheel"));
}
#endif

// While a session runs the Mapping tab is shown greyed: nothing to save, no Default or Undo, fields and the slider
// not editable, the Target row and the slider not clickable; a session start that is already known changes nothing.
TEST_F(EditView, TheMappingTabWhileLocked) {
    useRig(testRig());
    open();
    ASSERT_TRUE(click("Invert"));
    frame();
    view_->setLocked(true);
    view_->setLocked(true);
    frame();
    const Window::Text* const field = window_.find("900");
    ASSERT_NE(field, nullptr);
    EXPECT_TRUE(sameColour(field->colour, style::kDim));
    view_->onClick(field->x + 1, field->y + 1);
    EXPECT_FALSE(view_->editingText());
    view_->onClick(80 + 2, 22 + 2);   // the Target's <
    EXPECT_EQ(picker_->target(), ConfigPicker::Target::G920);
    const Window::Text* const undo = window_.find("Undo");
    ASSERT_NE(undo, nullptr);
    EXPECT_TRUE(sameColour(undo->colour, style::kFaint));
}

// Typing in a game's degrees takes "%", spaces, "/" and the letters of "off" (an older "360/180" reads as 180).
TEST_F(EditView, TypingADegreesValue) {
    useRig(testRig());
    open();
    games();
    ASSERT_TRUE(clickRight("Steering", 1));
    type(std::string(4, ui::kBackspace) + "360/180" + std::string(1, ui::kEnter));
    games();
    EXPECT_NE(own().find("nfs2015 = 180"), std::string::npos) << own();
    ASSERT_TRUE(clickRight("Steering", 1));
    type(std::string(4, ui::kBackspace) + " oFf %" + std::string(1, ui::kEnter));
    games();
    EXPECT_TRUE(window_.hasPart("expected degrees (180) or off"));
    view_->onKey(ui::kEscape);
    ASSERT_TRUE(click("NFS Unbound"));
    ASSERT_TRUE(click("Game file"));
    ASSERT_TRUE(clickRight("force feedback"));
    type(std::string(4, ui::kBackspace) + "." + std::string(1, ui::kEnter));
    games();
    EXPECT_TRUE(window_.hasPart("expected a number from 0 to 1"));
}

// Under the steering rotation, one line says to set the wheel's own software to the same; why, and that a game can
// have its own rotation, is its hover text.
TEST_F(EditView, TheRotationReminderIsOneLine) {
    useRig(testRig());
    open();
    frame();
    const Window::Text* const reminder = window_.holding("deg in your wheel's own software too (Pit House for Moza).");
    ASSERT_NE(reminder, nullptr);
    EXPECT_TRUE(sameColour(reminder->colour, style::kCaution));
    EXPECT_FALSE(window_.hasPart("A game can steer with its own rotation"));
    view_->setHover(reminder->x + 8, reminder->y + 2, true);
    frame();
    EXPECT_TRUE(window_.hasPart("passes the wheel through 1:1"));
}

// The Target and Mode buttons say what they are in the tab's hover text.
TEST_F(EditView, TheTargetButtonsExplainThemselves) {
    useRig(testRig());
    open();
    frame();
    const Window::Text* const g25 = window_.find("G25");
    ASSERT_NE(g25, nullptr);
    view_->setHover(g25->x + 2, g25->y + 2, true);
    frame();
    EXPECT_TRUE(window_.hasPart("for older games"));
    EXPECT_TRUE(click("G25"));
    frame();
    EXPECT_EQ(picker_->target(), ConfigPicker::Target::G25);
}

// A drag of the slider ends when a session starts or a popup opens.
TEST_F(EditView, TheSliderDragEnds) {
    useRig(testRig());
    open();
    frame();
    const Window::Text* const left = window_.find("90");
    ASSERT_NE(left, nullptr);
    view_->onClick(left->x - 2, left->y - 14);
    view_->setLocked(true);
    view_->onPointer(left->x + 200, 0, true);   // locked: not moved
    view_->setLocked(false);
    frame();
    EXPECT_TRUE(window_.has("90"));
    EXPECT_FALSE(window_.hasPart("Set 1080 deg in your wheel's own software too"));
}

// A typing complaint answered by the same value while the change still cannot be saved: the status line is the
// unsaved state, in amber.
TEST_F(EditView, AnUnsavedChangeAfterATypingComplaint) {
    useRig(testRig());
    std::filesystem::remove_all(userConfigFolder());
    write(userConfigFolder(), "a file where the folder should be");
    open();
    ASSERT_TRUE(click("Invert"));
    frame();
    ASSERT_TRUE(click("900"));
    type(std::string(3, ui::kBackspace) + "1" + std::string(1, ui::kEnter));   // too small
    type(std::string(1, ui::kBackspace) + "900" + std::string(1, ui::kEnter));
    frame();
    const Window::Text* const status = window_.find("not saved", 1);
    ASSERT_NE(status, nullptr);
    EXPECT_TRUE(sameColour(status->colour, style::kCaution));
    // Another change: tried again (and refused again).
    ASSERT_TRUE(click("Invert", 1));
    frame();
    EXPECT_TRUE(window_.hasPart("Could not write"));
}

// A group with only a secondary mapped is shown (not folded); a folded group in a column (the paddles, nothing on
// them) is as wide as its column.
TEST_F(EditView, FoldedAndShownGroups) {
    useOlderDefault();
    useRig(testRig());
    std::string text = read(dir_ / "config" / "g920.ini");
    text.replace(text.find("target_4 = 13"), 13, "target_4 = -1");
    text.replace(text.find("target_5 = 12"), 13, "target_5 = -1");
    text += "\n[buttons]\ntarget_12_or = 0\n";
    write(userConfigFolder() / "my-g920.ini", text);
    open();
    frame();
    EXPECT_TRUE(window_.has("PADDLES"));
    EXPECT_TRUE(window_.hasPart("Gear 1"));
    EXPECT_EQ(window_.count("nothing mapped"), 1U);
}

// One control used by three: the tip names the two others; in a picker for a fourth, it says where it is used twice.
TEST_F(EditView, AControlUsedByThree) {
    useOlderDefault();
    useRig(testRig());
    std::string text = read(dir_ / "config" / "g920.ini");
    text.replace(text.find("axis = 5"), 8, "axis = 2");
    text.replace(text.find("axis = 6"), 8, "axis = 2");
    write(userConfigFolder() / "my-g920.ini", text);
    window_.setCanvas(1600, 900);
    open();
    frame();
    const Window::Text* const gas = window_.find("Moza R3: Gas");
    ASSERT_NE(gas, nullptr);
    view_->setHover(gas->x, gas->y, true);
    frame();
    EXPECT_TRUE(window_.hasPart("; "));
    ASSERT_TRUE(click("Moza R3: Wheel"));
    frame();
    const Window::Text* const entry = window_.find("2 Gas");
    ASSERT_NE(entry, nullptr);
    EXPECT_TRUE(window_.outlinedAt(entry->x, entry->y, style::kWarning));
    view_->setHover(entry->x, entry->y, true);
    frame();
    EXPECT_TRUE(window_.hasPart("Used twice: ") && !window_.hasPart("Used twice: here"));
    view_->onKey(ui::kEscape);
    ASSERT_TRUE(click("Moza R3: Gas"));   // the throttle's own picker: used twice, here among them
    frame();
    const Window::Text* const mine = window_.find("2 Gas");
    ASSERT_NE(mine, nullptr);
    view_->setHover(mine->x, mine->y, true);
    frame();
    EXPECT_TRUE(window_.hasPart("Used twice: here;"));
}

// The system's own dialog, Documents folder and clock when the tab is given none: no file window on this system
// (Linux), and on Windows the dialog is the system's.
TEST_F(EditView, TheSystemsOwnWhenNoneIsGiven) {
    useRig(testRig());
    open();
    MappingEditView::System system;
    system.canPickFile = [] { return true; };   // the button on; the dialog itself the system's
    view_->setSystem(system);
#if !defined(_WIN32)
    games();
    ASSERT_TRUE(click("Add game..."));
    games();
    EXPECT_FALSE(window_.hasPart("added"));
#endif
}

// A Game file sub-tab for the user's file the game was seen writing, with no Documents folder (not Windows).
TEST_F(EditView, TheSeenFileWithoutDocuments) {
    useRig(testRig());
    const std::filesystem::path seen = dir_ / "Seen" / "PROFILEOPTIONS";
    write(seen, unboundFile());
    ASSERT_TRUE(writeAppSetting(appStateFile(), "game_file_seen.nfsunbound", pathToUtf8(seen)));
    documents_.clear();
    open();
    games();
    ASSERT_TRUE(click("NFS Unbound"));
    ASSERT_TRUE(click("Game file"));
    games();
    EXPECT_TRUE(window_.hasPart("Seen"));
}

}  // namespace
}  // namespace wheelbri::service
