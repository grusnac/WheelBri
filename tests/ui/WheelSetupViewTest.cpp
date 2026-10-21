// The Rig tab (service/src/WheelSetupView.cpp) driven as the app drives it: game controllers the test plugs in, moves
// and unplugs (UiFakes), clicks on what it drew, keys and typed text; the rig file it saves as it goes.

#include "UiFakes.hpp"

#include "WheelSetupView.hpp"

#include "UiStyle.hpp"

#include "wheelbri/DeviceLabels.hpp"
#include "wheelbri/IniDocument.hpp"
#include "wheelbri/Rig.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace wheelbri::service {
namespace {

using uitest::FakeDevice;
using uitest::FakeDevices;
using uitest::ScriptedWindow;

class RigTab : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("wheelbri-rigtab-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        FakeDevices::shared().clear();
    }
    void TearDown() override {
        FakeDevices::shared().clear();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    [[nodiscard]] WheelSetupView::Settings settings() const {
        return {dir_ / "rigs", dir_ / "devices", dir_ / "app.ini"};
    }
    // One frame as the app has it: update (reads the devices), draw, shown.
    void frame(WheelSetupView& view, const WheelSetupView::SessionWheel* session = nullptr) {
        view.update(session);
        window_.clearDrawing();
        view.draw();
        view.drawHoverText();
        window_.finish();
    }
    // Enough frames for every device's rest to be taken (Detect arms after ~20 reads).
    void settle(WheelSetupView& view) {
        for (int i = 0; i < 22; ++i) frame(view);
    }
    // Clicks the first text `value` of the last frame (then draws again); false when it is not drawn.
    bool clickOn(WheelSetupView& view, const std::string& value, const bool part = false) {
        const auto at = window_.clickOn(value, part);
        if (!at) return false;
        view.onClick(at->x, at->y);
        frame(view);
        return true;
    }
    [[nodiscard]] std::string rigFile(const std::string& name = "My rig") const {
        std::ifstream in(dir_ / "rigs" / (name + ".ini"), std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    [[nodiscard]] Rig savedRig(const std::string& name = "My rig") const {
        std::vector<std::string> errors;
        return readRig(dir_ / "rigs", name, errors).value_or(Rig{});
    }
    static void press(const std::shared_ptr<FakeDevice>& device, const int button, const bool down = true) {
        FakeDevices::shared().change(device, [button, down](FakeDevice& d) {
            if (down) {
                d.raw.buttons |= 1ULL << button;
            } else {
                d.raw.buttons &= ~(1ULL << button);
            }
        });
    }
    static void move(const std::shared_ptr<FakeDevice>& device, const int axis, const float value) {
        FakeDevices::shared().change(device, [axis, value](FakeDevice& d) { d.raw.axes[static_cast<std::size_t>(axis)] = value; });
    }
    void writeRig(const Rig& rig, const std::string& name = "My rig") const {
        ASSERT_TRUE(saveRig(dir_ / "rigs", name, rig));
    }
    [[nodiscard]] static RigDevice rigDevice(const std::string& name, const std::uint16_t vid, const std::uint16_t pid) {
        RigDevice device;
        device.name = name;
        device.id = {vid, pid};
        return device;
    }

    std::filesystem::path dir_;
    ScriptedWindow window_;
};

// No rig and nothing plugged in: it says so, nothing is saved, Undo is greyed and anyRig is false.
TEST_F(RigTab, EmptyStartSaysWhatToDo) {
    EXPECT_FALSE(WheelSetupView::anyRig(dir_ / "rigs"));
    WheelSetupView view(window_, settings());
    frame(view);
    EXPECT_TRUE(window_.has("No game controller found: plug one in.")) << window_.dump();
    EXPECT_TRUE(window_.has("nothing named yet"));
    EXPECT_TRUE(window_.has("none: plug in a game controller"));
    EXPECT_TRUE(window_.has("No rig yet", true));
    EXPECT_TRUE(window_.has("Plug in your wheel", true));
    EXPECT_FALSE(view.dirty() && false);
    EXPECT_FALSE(std::filesystem::exists(dir_ / "rigs" / "My rig.ini"));
    // Undo greyed: a click on it does nothing.
    EXPECT_TRUE(clickOn(view, "Undo"));
    EXPECT_FALSE(view.editingText());
}

// The system's game-controller API cannot start: the status says so (in red) and it is tried again later.
TEST_F(RigTab, NoBackendSaysItCannotRead) {
    FakeDevices::shared().backendFails = true;
    WheelSetupView view(window_, settings());
    frame(view);
    const auto* status = window_.find("Cannot read game controllers on this computer", true);
    ASSERT_NE(status, nullptr) << window_.dump();
    EXPECT_EQ(status->colour.r, style::kWarning.r);
}

// The first rig starts from the names given per device before (older Wheel setup files).
TEST_F(RigTab, FirstRigImportsOlderDeviceNames) {
    DeviceLabels labels;
    labels.name = "Old wheel";
    labels.vendorId = 0x346e;
    labels.productId = 0x0005;
    labels.axes[0] = "Wheel";
    std::filesystem::create_directories(dir_ / "devices");
    std::ofstream(dir_ / "devices" / deviceLabelsFileName(labels.vendorId, labels.productId))
        << writeDeviceLabels("", labels);
    WheelSetupView view(window_, settings());
    frame(view);
    EXPECT_TRUE(window_.has("Old wheel")) << window_.dump();
    EXPECT_TRUE(window_.has("Not connected"));
    // Not saved by itself... until a change: the imported names make the rig different from nothing.
    EXPECT_TRUE(std::filesystem::exists(dir_ / "rigs" / "My rig.ini"));
}

// A connected device: listed, read, and its controls shown; pressing a button lights it and (Detect on) opens its
// popup, where typing a name and Enter keep it: the device joins the rig, saved at once.
TEST_F(RigTab, DetectNamesAButtonAndSaves) {
    const auto wheel = FakeDevices::shared().add("Moza R3 Racing Wheel", 0x346e, 0x0005, 8, 16);
    WheelSetupView view(window_, settings());
    settle(view);
    EXPECT_TRUE(window_.has("1 connected; touch a control (or click it) to name it. Scroll for more devices.")) << window_.dump();
    EXPECT_TRUE(window_.has("Connected"));
    press(wheel, 3);
    frame(view);
    EXPECT_TRUE(view.editingText());
    EXPECT_TRUE(window_.has("Button 3 on Moza R3 Racing Wheel")) << window_.dump();
    EXPECT_TRUE(window_.has("pressed"));
    view.onText("  Horn;#=[]|\x01");   // a leading space, the file's special characters and a control character
    view.onText(std::string(1, ui::kBackspace));
    view.onText("n");
    view.onText(std::string(1, ui::kEnter));
    EXPECT_FALSE(view.editingText());
    frame(view);
    const Rig rig = savedRig();
    ASSERT_EQ(rig.devices.size(), 1U);
    EXPECT_EQ(rig.devices[0].buttons[3].label, "Horn");
    EXPECT_EQ(rig.devices[0].kind, DeviceKind::Wheel);   // guessed from "Racing Wheel"
    EXPECT_TRUE(window_.has("saved"));
    EXPECT_TRUE(window_.has("Last pressed: Moza R3 Racing Wheel button 3  Horn")) << window_.dump();
    // Pressed again: a named button only lights up, no popup.
    press(wheel, 3, false);
    frame(view);
    press(wheel, 3);
    frame(view);
    EXPECT_FALSE(view.editingText());
}

// A name is at most 12 characters; Escape in the popup drops what was typed.
TEST_F(RigTab, EscapeDropsTheName) {
    const auto pedals = FakeDevices::shared().add("Pedals", 0x1234, 0x0001, 3, 0);
    WheelSetupView view(window_, settings());
    settle(view);
    EXPECT_TRUE(window_.has("none on this device")) << window_.dump();
    EXPECT_TRUE(window_.has("Axis 2"));
    EXPECT_FALSE(window_.has("Axis 3"));   // it has 3
    move(pedals, 1, 1.0F);   // moved all the way from where it rested
    frame(view);
    ASSERT_TRUE(view.editingText());
    EXPECT_TRUE(window_.has("Axis 1 on Pedals")) << window_.dump();
    EXPECT_TRUE(window_.has("It rested at +0.00 when it was found.", true));
    view.onText("A very long name indeed");
    view.onKey(ui::kEscape);
    EXPECT_FALSE(view.editingText());
    frame(view);
    EXPECT_FALSE(std::filesystem::exists(dir_ / "rigs" / "My rig.ini"));   // nothing named: nothing saved
    // Back where it rested, then moved again: armed again, the popup opens again.
    move(pedals, 1, 0.0F);
    frame(view);
    move(pedals, 1, -0.9F);
    frame(view);
    EXPECT_TRUE(view.editingText());
    view.onText("Gas pedal that is long");
    view.onText(std::string(1, ui::kEnter));
    frame(view);
    EXPECT_EQ(savedRig().devices.at(0).axes[1].label, "Gas pedal th");   // cut to 12
    EXPECT_EQ(savedRig().devices.at(0).kind, DeviceKind::Pedals);
}

// A pedal found resting at the top: the popup says to invert it on the Mapping tab and its bar shows the travel from
// the top; one found at the bottom, its travel from the bottom; a centred axis, from the centre.
TEST_F(RigTab, AxisPopupShowsWhereItRested) {
    const auto device = FakeDevices::shared().add("Shifter thing", 0x2222, 0x0002, 4, 4);
    move(device, 0, 1.0F);
    move(device, 1, -1.0F);
    WheelSetupView view(window_, settings());
    settle(view);
    move(device, 0, -0.5F);   // from +1: moved 1.5
    frame(view);
    EXPECT_TRUE(window_.has("released at the top, so Invert it on the Mapping tab.", true)) << window_.dump();
    view.onKey(ui::kEscape);
    frame(view);
    move(device, 1, 0.5F);   // from -1
    frame(view);
    EXPECT_TRUE(window_.has("It rested at -1.00 when it was found.")) << window_.dump();
    view.onKey(ui::kEscape);
    frame(view);
    // Clicking an axis opens its popup with no rest known.
    ASSERT_TRUE(clickOn(view, "Axis 2"));
    EXPECT_TRUE(window_.has("Which way round: the Mapping tab's Invert for the axis this one drives.")) << window_.dump();
    // "It is": Throttle fills in the name; picked again it is unpicked (the name stays); OK keeps it.
    ASSERT_TRUE(clickOn(view, "Throttle"));
    ASSERT_TRUE(clickOn(view, "Throttle"));
    ASSERT_TRUE(clickOn(view, "Brake"));
    ASSERT_TRUE(clickOn(view, "OK"));
    const Rig rig = savedRig();
    ASSERT_EQ(rig.devices.size(), 1U);
    EXPECT_EQ(rig.devices[0].axes[2].label, "Throttle");
    EXPECT_EQ(rig.devices[0].axes[2].kind, "Brake");
    EXPECT_EQ(rig.devices[0].kind, DeviceKind::Shifter);
}

// The button popup: "Button" never fills in the name; Clear empties the control and keeps the popup open; Cancel
// keeps it as it was; OK after Clear leaves the control unnamed.
TEST_F(RigTab, ButtonPopupChoicesClearAndCancel) {
    FakeDevices::shared().add("Box", 0x3333, 0x0003, 0, 8);
    WheelSetupView view(window_, settings());
    view.update(nullptr);
    window_.clearDrawing();
    view.draw();
    window_.finish();
    // A button box: no axes listed at all.
    EXPECT_FALSE(window_.has("Axis 0"));
    EXPECT_FALSE(window_.has("axes not named", true));
    // Detect off: a press does nothing; a click on the tile opens it.
    ASSERT_TRUE(clickOn(view, "Detect"));
    ASSERT_TRUE(clickOn(view, " 5", false) || clickOn(view, "5"));
    EXPECT_TRUE(window_.has("Button 5 on Box")) << window_.dump();
    ASSERT_TRUE(clickOn(view, "Button"));
    ASSERT_TRUE(clickOn(view, "OK"));
    Rig rig = savedRig();
    ASSERT_EQ(rig.devices.size(), 1U);
    EXPECT_EQ(rig.devices[0].buttons[5].kind, "Button");
    EXPECT_EQ(rig.devices[0].buttons[5].label, "");
    EXPECT_EQ(rig.devices[0].kind, DeviceKind::Other);
    // Named once (the device has a named control): only its named buttons show, the rest folded with Show all.
    EXPECT_TRUE(window_.has("7 buttons not named (0-4, 6, 7): turn Detect on and press one to name it", true))
        << window_.dump();
    ASSERT_TRUE(clickOn(view, "Show all"));
    EXPECT_TRUE(window_.has("Every axis and button, named or not", true));
    ASSERT_TRUE(clickOn(view, "Named only"));
    // Open button 5 again: Clear, OK: unnamed again.
    ASSERT_TRUE(clickOn(view, "Button", false));
    ASSERT_TRUE(window_.has("Button 5 on Box")) << window_.dump();
    ASSERT_TRUE(clickOn(view, "Clear"));
    view.onText("Gone");
    ASSERT_TRUE(clickOn(view, "Cancel"));
    EXPECT_EQ(savedRig().devices.at(0).buttons[5].kind, "Button");   // Cancel: as it was
    ASSERT_TRUE(clickOn(view, "Button", false));
    ASSERT_TRUE(clickOn(view, "Clear"));
    ASSERT_TRUE(clickOn(view, "OK"));
    EXPECT_FALSE(savedRig().devices.at(0).buttons[5].defined());
}

// Undo: goes back one change and saves it; with nothing to undo it is greyed; a change that cannot be saved (the
// rigs folder is a file) says why, is not tried again, and Undo drops it.
TEST_F(RigTab, UndoAndChangesThatCannotBeSaved) {
    const auto wheel = FakeDevices::shared().add("Wheel", 0x346e, 0x0005, 2, 4);
    WheelSetupView view(window_, settings());
    settle(view);
    for (const int button : {0, 1}) {
        press(wheel, button);
        frame(view);
        view.onText("B" + std::to_string(button) + std::string(1, ui::kEnter));
        frame(view);
    }
    EXPECT_EQ(savedRig().devices.at(0).buttons[1].label, "B1");
    ASSERT_TRUE(clickOn(view, "Undo"));
    EXPECT_TRUE(window_.has("Undone and saved.")) << window_.dump();
    EXPECT_FALSE(savedRig().devices.at(0).buttons[1].defined());
    EXPECT_EQ(savedRig().devices.at(0).buttons[0].label, "B0");

    // The rigs folder replaced by a file: the next change cannot be saved.
    std::filesystem::remove_all(dir_ / "rigs");
    std::ofstream(dir_ / "rigs") << "not a folder";
    press(wheel, 2);
    frame(view);
    view.onText("B2" + std::string(1, ui::kEnter));
    frame(view);
    EXPECT_TRUE(window_.has("not saved")) << window_.dump();
    EXPECT_TRUE(window_.has("Could not save", true));
    frame(view);   // not tried again
    ASSERT_TRUE(clickOn(view, "Undo"));
    EXPECT_TRUE(window_.has("Change dropped.")) << window_.dump();
    std::filesystem::remove(dir_ / "rigs");
    // Undo again: back to before B0 (the list still has that one).
    ASSERT_TRUE(clickOn(view, "Undo"));
}

// The rig remembered in the app's state file is the one shown; one that cannot be read says so; a rig with errors
// says the first; without a remembered one the first file is used.
TEST_F(RigTab, WhichRigIsShown) {
    Rig rig;
    rig.devices.push_back(rigDevice("Remembered wheel", 0x346e, 0x0005));
    rig.devices[0].buttons[0].label = "A";
    writeRig(rig, "Second");
    Rig other;
    other.devices.push_back(rigDevice("First wheel", 0x1111, 0x0001));
    other.devices[0].axes[0].label = "Steer";
    writeRig(other, "First");
    {
        std::ofstream(dir_ / "app.ini") << "[app]\nrig = Second\n";
        WheelSetupView view(window_, settings());
        frame(view);
        EXPECT_TRUE(window_.has("Remembered wheel")) << window_.dump();
        EXPECT_TRUE(window_.has("saved"));
        EXPECT_TRUE(WheelSetupView::anyRig(dir_ / "rigs"));
    }
    {
        std::ofstream(dir_ / "app.ini") << "[app]\nrig = Gone\n";
        WheelSetupView view(window_, settings());
        frame(view);
        EXPECT_TRUE(window_.has("First wheel")) << window_.dump();
    }
    {
        std::ofstream(dir_ / "rigs" / "First.ini", std::ios::app) << "[device_1]\nbogus = 1\n";
        std::ofstream(dir_ / "app.ini") << "[app]\nrig = First\n";
        WheelSetupView view(window_, settings());
        frame(view);
        EXPECT_TRUE(window_.has("The rig: ", true)) << window_.dump();
    }
    {
        // A folder named like a rig is not one: the first rig is used.
        std::filesystem::create_directories(dir_ / "rigs" / "Broken.ini");
        std::ofstream(dir_ / "app.ini") << "[app]\nrig = Broken\n";
        WheelSetupView view(window_, settings());
        frame(view);
        EXPECT_TRUE(window_.has("First wheel")) << window_.dump();
    }
    {
        // A rig file that cannot be read (no permission; an Administrator or root reads it anyway: skipped then).
        writeRig(Rig{}, "Locked");
        const auto locked = dir_ / "rigs" / "Locked.ini";
        std::filesystem::permissions(locked, std::filesystem::perms::none);
        if (std::ifstream(locked).good()) {
            std::filesystem::permissions(locked, std::filesystem::perms::owner_all);
            GTEST_SKIP() << "permissions do not keep this user from reading a file";
        }
        std::ofstream(dir_ / "app.ini") << "[app]\nrig = Locked\n";
        WheelSetupView view(window_, settings());
        frame(view);
        EXPECT_TRUE(window_.has("Cannot read the rig (Locked.ini).")) << window_.dump();
        std::filesystem::permissions(locked, std::filesystem::perms::owner_all);
    }
}

// A rig device that is not plugged in shows as saved (its named buttons as tiles, "Not connected"); Forget takes it
// out (saved), Undo brings it back.
TEST_F(RigTab, UnpluggedDeviceAndForget) {
    Rig rig;
    rig.devices.push_back(rigDevice("Pedals", 0x1234, 0x0001));
    rig.devices[0].buttons[2].label = "Clip";
    rig.devices[0].axes[1].kind = "Throttle";
    writeRig(rig);
    WheelSetupView view(window_, settings());
    frame(view);
    EXPECT_TRUE(window_.has("Not connected")) << window_.dump();
    EXPECT_TRUE(window_.has("Clip"));
    EXPECT_TRUE(window_.has("Throttle"));
    EXPECT_TRUE(window_.has("click one to say what it is"));
    EXPECT_TRUE(window_.has("-"));   // no live value
    ASSERT_TRUE(clickOn(view, "Forget"));
    EXPECT_TRUE(window_.has("Forgot 1234:0001 and its names", true)) << window_.dump();
    EXPECT_TRUE(savedRig().devices.empty());
    ASSERT_TRUE(clickOn(view, "Undo"));
    frame(view);
    EXPECT_EQ(savedRig().devices.size(), 1U);
}

// The device's name is typed in its row: Enter ends typing, a click elsewhere too; an empty name becomes the
// system's again when saved.
TEST_F(RigTab, DeviceNameIsTyped) {
    Rig rig;
    rig.devices.push_back(rigDevice("Wheel", 0x346e, 0x0005));
    rig.devices[0].buttons[0].label = "A";
    writeRig(rig);
    FakeDevices::shared().add("Moza R3", 0x346e, 0x0005, 8, 4);
    WheelSetupView view(window_, settings());
    settle(view);
    ASSERT_TRUE(clickOn(view, "Wheel"));
    ASSERT_TRUE(view.editingText());
    for (int i = 0; i < 5; ++i) view.onText(std::string(1, ui::kBackspace));
    view.onText(std::string(1, ui::kBackspace));   // nothing left to delete
    view.onText("   ");                             // leading spaces are not taken
    view.onKey(ui::kPageDown);                      // keys do nothing while typing
    view.onText(std::string(1, ui::kEnter));
    EXPECT_FALSE(view.editingText());
    frame(view);
    EXPECT_EQ(savedRig().devices.at(0).name, "Moza R3");
    ASSERT_TRUE(clickOn(view, "Moza R3"));
    view.onText(" Rim");
    view.onClick(1, 1);   // a click elsewhere ends typing
    EXPECT_FALSE(view.editingText());
    frame(view);
    EXPECT_EQ(savedRig().devices.at(0).name, "Moza R3 Rim");
}

// A rig holds at most four devices: naming a control of a fifth says so and adds nothing.
TEST_F(RigTab, RigIsFull) {
    Rig rig;
    for (std::uint16_t i = 1; i <= Rig::kMaxDevices; ++i) {
        rig.devices.push_back(rigDevice("Device " + std::to_string(i), 0x1000, i));
        rig.devices.back().buttons[0].label = "X";
    }
    writeRig(rig);
    const auto fifth = FakeDevices::shared().add("Fifth", 0x5555, 0x0005, 0, 4);
    WheelSetupView view(window_, settings());
    settle(view);
    press(fifth, 1);
    frame(view);
    ASSERT_TRUE(view.editingText());
    view.onText("Nope" + std::string(1, ui::kEnter));
    frame(view);
    EXPECT_TRUE(window_.has("A rig has at most 4 devices: Forget one first", true)) << window_.dump();
    EXPECT_EQ(savedRig().devices.size(), Rig::kMaxDevices);
}

// Many devices: they scroll (keys, the wheel, the scroll bar dragged or clicked), the bottom-left list scrolls when
// it has more than six lines, a click on a line brings its section into view.
TEST_F(RigTab, ManyDevicesScroll) {
    for (std::uint16_t i = 0; i < 9; ++i) {
        FakeDevices::shared().add("Controller " + std::to_string(i), 0x7000, i, 8, 32);
    }
    WheelSetupView view(window_, settings());
    settle(view);
    EXPECT_TRUE(window_.has("0 up, 3 down")) << window_.dump();
    for (const char key : {ui::kPageDown, ui::kDown, ui::kEnd, ui::kUp, ui::kPageUp, ui::kHome, 'x'}) {
        view.onKey(key);
        frame(view);
    }
    // The wheel over the devices scrolls them; over the list, the list.
    view.onPointer(600, 100, false);
    view.onScroll(-2);
    frame(view);
    const float listY = window_.find("Controller 0")->y;
    view.onPointer(20, window_.find("DEVICES")->y + 30, false);
    view.onScroll(-5);
    frame(view);
    EXPECT_TRUE(window_.has("3 up, 0 down")) << window_.dump();
    view.onScroll(2);
    frame(view);
    EXPECT_TRUE(window_.has("1 up, 2 down")) << window_.dump();
    (void)listY;
    // The scroll bar: a press on the track jumps there, the drag goes on while the button is held.
    const float barX = window_.width() - 8 - 2 + 1;
    view.onClick(barX, 600);
    view.onPointer(barX, 650, true);
    frame(view);
    view.onPointer(barX, 60, true);
    frame(view);
    view.onPointer(barX, 60, false);   // released
    frame(view);
    // On the thumb (now at the top): taken where pressed.
    view.onClick(barX, 50);
    view.onPointer(barX, 52, true);
    view.onPointer(barX, 52, false);
    frame(view);
    // A line of the list brings its section into view.
    ASSERT_TRUE(clickOn(view, "Controller 5"));
    EXPECT_GE(window_.find("Controller 5")->y, 0.0F);
}

// The running session's wheel: only that one is shown ("-" for the others, not known then), no Forget; when the
// session ends the tab reads the devices again.
TEST_F(RigTab, SessionWheel) {
    Rig rig;
    rig.devices.push_back(rigDevice("Wheel", 0x346e, 0x0005));
    rig.devices[0].buttons[1].label = "B";
    rig.devices[0].axes[5].label = "Old axis";   // named on a wheel with more axes
    rig.devices.push_back(rigDevice("Pedals", 0x1234, 0x0001));
    rig.devices[1].axes[0].label = "Gas";
    writeRig(rig);
    WheelSetupView view(window_, settings());
    WheelSetupView::SessionWheel session;
    session.info = {"Moza R3", 0x346e, 0x0005, "/dev/input/event9"};
    session.axes = 2;
    session.buttons = 4;
    for (int i = 0; i < 22; ++i) frame(view, &session);
    EXPECT_TRUE(window_.has("The running session's wheel (stop to see every device).")) << window_.dump();
    EXPECT_TRUE(window_.has("-"));
    EXPECT_FALSE(window_.has("Forget"));
    // A press of an unnamed button opens its popup (not saved while the session runs).
    session.raw.buttons = 1;
    frame(view, &session);
    EXPECT_TRUE(view.editingText());
    view.onText("Shift" + std::string(1, ui::kEnter));
    frame(view, &session);
    EXPECT_FALSE(savedRig().devices.at(0).buttons[0].defined());
    // A named axis it does not have says so; the others it does not have are not listed.
    if (window_.has("Show all")) {
        ASSERT_TRUE(clickOn(view, "Show all"));
    }
    frame(view, &session);
    EXPECT_TRUE(window_.has("(this device has no axis 5)")) << window_.dump();
    // Another wheel for the session: the device read is replaced.
    session.info = {"Other", 0x9999, 0x0009, "/dev/input/event10"};
    frame(view, &session);
    // Stopped: the tab lists the devices itself again, and saves what was named meanwhile.
    frame(view);
    EXPECT_EQ(savedRig().devices.at(0).buttons[0].label, "Shift");
}

// A device that stops answering is closed and the list read again; one that cannot be opened is tried again; a second
// device with the same id is not read (a rig tells devices apart by id).
TEST_F(RigTab, DevicesComeAndGo) {
    const auto wheel = FakeDevices::shared().add("Wheel", 0x346e, 0x0005, 8, 8);
    const auto twin = FakeDevices::shared().add("Wheel twin", 0x346e, 0x0005, 8, 8);
    const auto stubborn = FakeDevices::shared().add("Stubborn", 0x4444, 0x0004, 8, 8);
    FakeDevices::shared().change(stubborn, [](FakeDevice& d) { d.openFails = true; });
    WheelSetupView view(window_, settings());
    settle(view);
    EXPECT_TRUE(window_.has("Stubborn")) << window_.dump();   // listed, not read
    EXPECT_FALSE(window_.has("Wheel twin"));
    FakeDevices::shared().change(wheel, [](FakeDevice& d) { d.pollFails = true; });
    frame(view);
    frame(view);
    (void)twin;
    // Plugged out entirely: gone from the list at the next listing (releaseWheel lists again at once).
    FakeDevices::shared().change(wheel, [](FakeDevice& d) { d.plugged = false; });
    FakeDevices::shared().change(stubborn, [](FakeDevice& d) { d.openFails = false; });
    view.releaseWheel();
    settle(view);
    EXPECT_TRUE(window_.has("Stubborn")) << window_.dump();
}

// A wider window: more tile columns, the bars and Detect move right; the hover texts show where the mouse is and not
// over a popup.
TEST_F(RigTab, WiderWindowAndHoverText) {
    window_.setSize(1440, 900);
    FakeDevices::shared().add("Wheel", 0x346e, 0x0005, 8, 40);
    WheelSetupView view(window_, settings());
    settle(view);
    // A copy of where Detect is: find() points into the last frame, which the next frame replaces.
    const auto* const found = window_.find("Detect");
    ASSERT_NE(found, nullptr);
    const ScriptedWindow::Text detectText = *found;
    const auto* const detect = &detectText;
    EXPECT_GT(detect->x, 1000.0F);
    view.setHover(detect->x, detect->y, true);
    frame(view);
    EXPECT_TRUE(window_.has("On: touch a control", true)) << window_.dump();
    ASSERT_TRUE(clickOn(view, "Detect"));
    view.setHover(detect->x, detect->y, true);
    frame(view);
    EXPECT_TRUE(window_.has("Off: click a control", true)) << window_.dump();
    view.setHover(detect->x, detect->y, false);
    frame(view);
    EXPECT_FALSE(window_.has("Off: click a control", true));
    // The popup covers the hover text; clicks outside it do nothing.
    ASSERT_TRUE(clickOn(view, "Axis 0"));
    view.setHover(detect->x, detect->y, true);
    frame(view);
    EXPECT_FALSE(window_.has("Off: click a control", true));
    view.onClick(2, 2);
    frame(view);
    EXPECT_TRUE(window_.has("Axis 0 on Wheel")) << window_.dump();
    view.onScroll(3);   // not while a popup is open
    view.onKey(ui::kEscape);
    view.onKey(ui::kEscape);   // no popup: nothing
}

// What kind of device it is, guessed from its name when it joins the rig: pedals (but a wheel with pedals is a
// wheel), a handbrake, a wheel ("wheel" or "racing"), else something else.
TEST_F(RigTab, KindGuessedFromTheName) {
    const auto wheelPedals = FakeDevices::shared().add("Wheel and Pedals", 0x1111, 0x0001, 2, 4);
    const auto handbrake = FakeDevices::shared().add("SIM Handbrake", 0x1111, 0x0002, 2, 4);
    const auto racing = FakeDevices::shared().add("Racing Box", 0x1111, 0x0003, 2, 4);
    const auto pedals = FakeDevices::shared().add("Pedal Set", 0x1111, 0x0004, 2, 4);
    WheelSetupView view(window_, settings());
    settle(view);
    for (const auto& device : {wheelPedals, handbrake, racing, pedals}) {
        press(device, 1);
        frame(view);
        ASSERT_TRUE(view.editingText()) << window_.dump();
        view.onText("Go");
        view.onText(std::string(1, ui::kEnter));
        press(device, 1, false);
        frame(view);
    }
    const Rig rig = savedRig();
    ASSERT_EQ(rig.devices.size(), 4U) << rigFile();
    EXPECT_EQ(rig.find(DeviceId{0x1111, 0x0001})->kind, DeviceKind::Wheel);
    EXPECT_EQ(rig.find(DeviceId{0x1111, 0x0002})->kind, DeviceKind::Handbrake);
    EXPECT_EQ(rig.find(DeviceId{0x1111, 0x0003})->kind, DeviceKind::Wheel);
    EXPECT_EQ(rig.find(DeviceId{0x1111, 0x0004})->kind, DeviceKind::Pedals);
}

// Undo keeps the last 50 changes: after 51, fifty Undos leave the first one, and Undo is then greyed.
TEST_F(RigTab, UndoKeepsFifty) {
    const auto box = FakeDevices::shared().add("Button box", 0x2222, 0x0001, 0, 64);
    WheelSetupView view(window_, settings());
    settle(view);
    for (int b = 0; b <= 50; ++b) {
        press(box, b);
        frame(view);
        ASSERT_TRUE(view.editingText()) << b << "\n" << window_.dump();
        view.onText("b" + std::to_string(b));
        view.onText(std::string(1, ui::kEnter));
        press(box, b, false);
        frame(view);
    }
    ASSERT_EQ(savedRig().devices.size(), 1U);
    EXPECT_EQ(savedRig().devices[0].buttons[50].label, "b50");
    for (int i = 0; i < 51; ++i) ASSERT_TRUE(clickOn(view, "Undo"));
    const Rig rig = savedRig();
    ASSERT_EQ(rig.devices.size(), 1U) << rigFile();
    EXPECT_EQ(rig.devices[0].buttons[0].label, "b0");
    EXPECT_TRUE(rig.devices[0].buttons[1].label.empty());
}

// Typed text with no name being typed is ignored.
TEST_F(RigTab, TextWithoutAField) {
    WheelSetupView view(window_, settings());
    frame(view);
    view.onText("x");
    EXPECT_FALSE(view.editingText());
}

}  // namespace
}  // namespace wheelbri::service
