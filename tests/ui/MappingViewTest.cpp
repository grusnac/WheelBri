// The live view and the Logs tab (service/src/MappingView.cpp): every tab drawn from a session's state, the live log
// followed from real files, its scrolling, the scroll bars, word wrap and the level button; the Telemetry and Force
// feedback tabs with a real FfbController.

#include "UiFakes.hpp"

#include "AppLogLevel.hpp"
#include "MappingView.hpp"
#include "UiStyle.hpp"

#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/FfbPacket.hpp"
#include "wheelbri/IniDocument.hpp"
#include "wheelbri/LocalDate.hpp"
#include "wheelbri/Logging.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace wheelbri::service {
namespace {

using uitest::ScriptedWindow;

// A sink that plays everything (or refuses, `refuse`).
class FakeSink final : public IFfbSink {
public:
    bool refuse = false;
    bool play(std::size_t, const FfbEffect&) override { return !refuse; }
    void stop(std::size_t) override {}
    void stopAll() override {}
};

class LiveView : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("wheelbri-liveview-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_ / "logs");
        profile_ = makeProfile("g920");
        MappingView::setOptionalTabs(false, false);
        MappingView::setGamesTab(false);
    }
    void TearDown() override {
        MappingView::setOptionalTabs(false, false);
        MappingView::setGamesTab(false);
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    // A mapping with every kind of source: the wheel's axes, an accessory's, none; buttons from the wheel, from an
    // accessory, none, with a second source; a D-pad from buttons.
    [[nodiscard]] static MapperConfig mapping() {
        MapperConfig mapper;
        mapper.steering.sourceAxis = 0;
        mapper.steering.sourceDevice = 0;
        mapper.throttle.sourceAxis = 1;
        mapper.throttle.sourceDevice = 1;   // an accessory
        mapper.brake.sourceAxis = -1;       // none
        mapper.clutch.sourceAxis = 3;
        mapper.clutch.sourceDevice = 0;
        mapper.sourceAxisLabels[0] = "Wheel";
        for (std::size_t t = 0; t < mapper.buttonMap.size(); ++t) {
            mapper.buttonMap[t] = static_cast<int>(t);
            mapper.buttonDevice[t] = 0;
        }
        mapper.buttonMap[1] = -1;          // B from nothing
        mapper.buttonMap[2] = 7;           // X from the accessory's button 7
        mapper.buttonDevice[2] = 1;
        mapper.buttonMap[3] = 0;           // Y from the same button as A: button 0 reaches two
        mapper.buttonSecondary[0][0].index = 9;   // A also from button 9
        mapper.sourceButtonLabels[0] = "Cross";
        mapper.hatButtons.up = 12;
        mapper.buttonMap[12] = -1;   // button 12 drives only the D-pad
        mapper.hatButtons.devices = {0, 0, 0, 0};
        mapper.hatButtons.right = 4;   // a D-pad direction and a button (target 4) at once
        mapper.hatButtons.down = 70;   // out of range: ignored
        mapper.hatButtons.left = -1;
        return mapper;
    }
    [[nodiscard]] std::unique_ptr<MappingView> makeView(const int axes = 6, const int buttons = 20,
                                                        const MapperConfig& mapper = mapping()) {
        auto view = std::make_unique<MappingView>(window_, "Moza R3 Racing Wheel and Wheelbase", axes, buttons,
                                                  *profile_, mapper);
        FilesDisplay files;
        files.logFolder = dir_ / "logs";
        view->setFiles(files);
        return view;
    }
    void draw(MappingView& view, const RawInputs& inputs = {}, const WheelState& state = {},
              const TargetReport& report = {}, const FfbDisplay& ffb = {}) {
        window_.clearDrawing();
        view.draw(inputs, state, report, ffb);
        window_.finish();
    }
    bool clickOn(MappingView& view, const std::string& value, const bool part = false) {
        const auto at = window_.clickOn(value, part);
        if (!at) return false;
        view.onClick(at->x, at->y);
        return true;
    }
    void writeLog(const std::string& stem, const std::vector<std::string>& lines) const {
        std::ofstream out(dir_ / "logs" / (stem + "-" + localDate(WallClock::now()) + "-01.log"), std::ios::app | std::ios::binary);
        for (const std::string& line : lines) out << line << "\n";
    }
    static std::string spdlogLine(const std::string& level, const std::string& logger, const std::string& message) {
        return "[2026-10-20 23:59:59.999] [" + level + "] [" + logger + "] " + message;
    }

    std::filesystem::path dir_;
    ScriptedWindow window_;
    std::unique_ptr<IDeviceProfile> profile_;
};

// The tabs that exist: Rig, Mapping, Logs; Games in the app; Telemetry and Force feedback only when asked for. Keys
// and clicks switch them; a key for a tab that is not there does nothing.
TEST_F(LiveView, TabsAndKeys) {
    EXPECT_EQ(MappingView::visibleTabs().size(), 3U);
    const float three = MappingView::tabsEnd();
    MappingView::setGamesTab(true);
    MappingView::setOptionalTabs(true, true);
    EXPECT_EQ(MappingView::visibleTabs().size(), 6U);
    EXPECT_GT(MappingView::tabsEnd(), three);
    MappingView::setOptionalTabs(false, false);
    MappingView::setGamesTab(false);
    auto view = makeView();
    for (const char key : {'g', 't', 'f'}) {
        view->onKey(key);
        EXPECT_EQ(view->tab(), MappingView::Tab::Mapping);
    }
    view->onKey('l');
    EXPECT_EQ(view->tab(), MappingView::Tab::Logs);
    view->onKey('r');
    EXPECT_EQ(view->tab(), MappingView::Tab::WheelSetup);
    draw(*view);   // the window draws the Rig tab itself: only the tab bar here
    EXPECT_TRUE(window_.has("Rig (R)"));
    EXPECT_FALSE(window_.has("AXES"));
    view->onKey('m');
    MappingView::setGamesTab(true);
    MappingView::setOptionalTabs(true, true);
    for (const auto& [key, tab] : {std::pair{'g', MappingView::Tab::Games}, std::pair{'t', MappingView::Tab::Telemetry},
                                   std::pair{'f', MappingView::Tab::ForceFeedback}}) {
        view->onKey(key);
        EXPECT_EQ(view->tab(), tab);
    }
    // Clicks on the tab buttons; between them and outside the bar nothing changes.
    draw(*view);
    ASSERT_TRUE(clickOn(*view, "Logs (L)"));
    EXPECT_EQ(view->tab(), MappingView::Tab::Logs);
    view->onClick(9000, window_.height() - 14);   // right of every tab
    EXPECT_EQ(view->tab(), MappingView::Tab::Logs);
    view->drawTabBar();
}

// The Mapping tab: every axis with its source (the wheel's, an accessory's, none), the rig's axes it has and not, the
// D-pad, the target's buttons (from nothing, an accessory, two sources) and the rig's (pressed: mapped green, unmapped
// red); the last pressed button and where it went.
// The target's buttons by the numbers the game sees: with Logitech driver order from 1 (DiRT Rally 2.0 asks for
// "Button 7", the G25's R Wheel, 6 as named); with the native order the wheel's own HID number, from 0.
TEST_F(LiveView, TargetButtonsShowTheGamesNumbers) {
    profile_ = makeProfile("g25");
    MapperConfig mapper;
    for (std::size_t t = 0; t < mapper.buttonMap.size(); ++t) mapper.buttonMap[t] = -1;
    mapper.buttonMap[6] = 34;   // R Wheel <- the rim's R
    mapper.sourceButtonLabels[34] = "R";
    auto view = makeView(8, 40, mapper);
    view->setTab(MappingView::Tab::Mapping);
    view->showShadow(false);   // the List view (the G25 starts with its Shadow view)
    RawInputs inputs;
    draw(*view, inputs);
    inputs.devices[0].buttons = 1ULL << 34;
    draw(*view, inputs);
    EXPECT_TRUE(window_.has("Last pressed: G25 7 R Wheel <- Rig 34 R")) << window_.dump();
    EXPECT_TRUE(window_.has("G25  7 R Wheel  <- Rig 34 R", true)) << window_.dump();
    EXPECT_TRUE(window_.has("G25 button (from 1, as games show it)", true)) << window_.dump();

    auto native = makeView(8, 40, mapper);
    native->setTab(MappingView::Tab::Mapping);
    native->setButtonOrder(ButtonOrder::Native);
    inputs.devices[0].buttons = 0;
    draw(*native, inputs);
    inputs.devices[0].buttons = 1ULL << 34;
    draw(*native, inputs);
    EXPECT_TRUE(window_.has("Last pressed: G25 6 R Wheel <- Rig 34 R")) << window_.dump();
    EXPECT_TRUE(window_.has("G25 11 Red 1    <- --", true)) << window_.dump();   // Red 1 is native 11
    EXPECT_TRUE(window_.has("G25 button (native, from 0)", true)) << window_.dump();
}

// --- the Shadow view (ShadowView.cpp) -------------------------------------------------------------------------------

// The shipped G25 config's mapping (config/g25.ini): the Moza rim's labels on the G25's buttons, no shifter mapped.
MapperConfig g25Mapping() {
    std::ifstream in(std::filesystem::path(WHEELBRI_CONFIG_DIR) / "g25.ini", std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return loadEmulatorConfig(IniDocument::parse(text.str()).document).config.mapper;
}

bool sameColour(const ui::Colour a, const ui::Colour b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

// A text drawn exactly `value` (not part of a longer one) in `colour`.
bool hasIn(const ScriptedWindow& window, const std::string& value, const ui::Colour colour) {
    for (const auto& t : window.last.texts) {
        if (t.value == value && sameColour(t.colour, colour)) return true;
    }
    return false;
}

// The G25 starts on its Shadow view (the picture), with the Shadow / List switch in the header; the List view is the
// rig's axes and buttons as before. Clicks and S switch, and the choice holds for the next session's view. A target
// with no picture (the G920) has no switch and always shows the list.
TEST_F(LiveView, ShadowViewIsTheG25sFirstView) {
    profile_ = makeProfile("g25");
    auto view = makeView(8, 40, g25Mapping());
    view->showShadow(true);
    draw(*view);
    EXPECT_TRUE(view->showingShadow());
    EXPECT_TRUE(window_.has("SHADOW")) << window_.dump();
    EXPECT_TRUE(window_.has("PRESSED NOW"));
    EXPECT_TRUE(window_.has("Shadow"));
    EXPECT_TRUE(window_.has("List"));
    EXPECT_FALSE(window_.has("AXES"));
    EXPECT_GT(window_.last.rects.size(), 500U);   // the shapes, drawn row by row

    ASSERT_TRUE(clickOn(*view, "List"));
    draw(*view);
    EXPECT_FALSE(view->showingShadow());
    EXPECT_TRUE(window_.has("AXES"));
    EXPECT_FALSE(window_.has("PRESSED NOW"));
    auto next = makeView(8, 40, g25Mapping());   // the next session's view: still the list
    EXPECT_FALSE(next->showingShadow());
    view->onKey('s');
    EXPECT_TRUE(view->showingShadow());
    view->onKey('l');   // S only on the Mapping tab
    view->onKey('s');
    EXPECT_TRUE(view->showingShadow());
    view->onKey('m');
    ASSERT_TRUE(clickOn(*view, "Shadow"));
    EXPECT_TRUE(view->showingShadow());

    profile_ = makeProfile("g920");
    auto g920 = makeView();
    g920->onKey('s');
    draw(*g920);
    EXPECT_FALSE(g920->showingShadow());
    EXPECT_TRUE(window_.has("AXES"));
    EXPECT_FALSE(window_.has("Shadow"));
}

// Every one of the G25's 19 buttons has its place on the picture: each number the game knows drawn once on a
// button, in either order (driver from 1, native from 0). Beside them the rig's labels (g25.ini's Moza rim), and the
// G25's own names where a control has none: Red 2 (rig 37 has no label), the gears (nothing drives them: faint).
TEST_F(LiveView, ShadowViewHasEveryG25ButtonByTheGamesNumber) {
    profile_ = makeProfile("g25");
    for (const ButtonOrder order : {ButtonOrder::Driver, ButtonOrder::Native}) {
        auto view = makeView(8, 40, g25Mapping());
        view->showShadow(true);
        view->setButtonOrder(order);
        draw(*view);
        for (std::size_t b = 0; b < kG25Buttons; ++b) {
            const std::string number = std::to_string(gameButtonNumber(*profile_, order, b));
            int drawn = 0;
            for (const auto& t : window_.last.texts) {
                if (t.value == number && t.y > 60) ++drawn;   // on the picture (not the header's)
            }
            EXPECT_GE(drawn, 1) << "button " << b << " (" << number << ")\n" << window_.dump();
        }
    }
    // The labels beside them.
    for (const char* label : {"Radio", "Home", "Start", "Cam", "R", "L paddle", "R paddle", "Y", "X", "A", "B"}) {
        EXPECT_TRUE(hasIn(window_, label, style::kText)) << label << "\n" << window_.dump();
    }
    EXPECT_TRUE(hasIn(window_, "Red 2", style::kText));   // driven by rig 37, which has no label
    for (const char* gear : {"1", "2", "3", "4", "5", "6", "R"}) {
        EXPECT_TRUE(hasIn(window_, gear, style::kFaint)) << gear;   // nothing drives the gears by default
    }
    EXPECT_TRUE(window_.has("Gear 1, Gear 2", true)) << window_.dump();   // named in "Nothing drives"
    EXPECT_TRUE(window_.has("Gas pedal") && window_.has("Brake pedal") && window_.has("Clutch pedal"));
}

// What the game sees pressed lights up: the button (its number dark on green), the D-pad's arm, the pedals' fill by
// how far, the steering arc and degrees; the list on the right says what drives each, yours first.
TEST_F(LiveView, ShadowViewLightsWhatTheGameSees) {
    profile_ = makeProfile("g25");
    MapperConfig mapper = g25Mapping();
    mapper.buttonMap[11] = 4;                        // Gear 4 <- a shifter's button 4 (no label)
    mapper.buttonDevice[11] = 1;
    mapper.steering.targetRotationDeg = 900.0F;
    auto view = makeView(8, 40, mapper);
    std::array<DeviceLabels, MapperConfig::kMaxAccessories> accessories{};
    accessories[0].name = "Shifter";
    accessories[0].vendorId = 0x1234;
    accessories[0].productId = 2;
    view->setAccessories(accessories);
    view->showShadow(true);
    WheelState state;
    state.steering = 120.0F / 450.0F;
    state.brake = 0.85F;
    state.clutch = 1.0F;
    TargetReport report;
    report.buttons = (1U << 5) | (1U << 17) | (1U << 11);   // L Paddle, Blk bottom, Gear 4
    report.hat = 2;                                          // right
    draw(*view, {}, state, report);

    EXPECT_TRUE(hasIn(window_, "6", style::kDark)) << window_.dump();    // L Paddle (driver 6) lit
    EXPECT_TRUE(hasIn(window_, "18", style::kDark));                     // Blk bottom (driver 18) lit
    EXPECT_TRUE(hasIn(window_, "12", style::kActive));                   // Gear 4's number on the knob
    EXPECT_TRUE(hasIn(window_, "L paddle", style::kActive));
    EXPECT_TRUE(hasIn(window_, "A", style::kActive));
    EXPECT_TRUE(hasIn(window_, "Right", style::kActive));                // the D-pad's right arm
    EXPECT_TRUE(hasIn(window_, "85%", style::kActive));
    EXPECT_TRUE(hasIn(window_, "100%", style::kActive));
    EXPECT_TRUE(hasIn(window_, "0%", style::kDim));                      // the throttle at rest
    EXPECT_TRUE(window_.has("Wheel +120 deg")) << window_.dump();
    EXPECT_TRUE(window_.has("Steering +120 deg", true));
    EXPECT_TRUE(window_.has("L paddle     -> 6 L Paddle")) << window_.dump();
    EXPECT_TRUE(window_.has("A            -> 18 Blk bottom"));
    EXPECT_TRUE(window_.has("Shifter 4    -> 12 Gear 4"));   // an unlabelled source: its device and number
    EXPECT_TRUE(window_.has("Right        -> Hat right"));
    EXPECT_TRUE(window_.has("Brake pedal  -> Brake 85%"));
    bool green = false;
    for (const auto& r : window_.last.rects) green = green || (r.filled && sameColour(r.colour, style::kActive));
    EXPECT_TRUE(green);

    // Nothing pressed: the list says so, and nothing is green.
    draw(*view);
    EXPECT_TRUE(window_.has("nothing (press a button, a pedal, turn)"));
    for (const auto& r : window_.last.rects) {
        if (r.y > 40) {
            EXPECT_FALSE(sameColour(r.colour, style::kActive)) << r.x << "," << r.y;
        }
    }
}

// The amber mark on the rim follows the steering arc: at full lock left it sits at the arc's left end (150 degrees
// left of the top), not wherever 450 degrees would put it; centred, at the top.
TEST_F(LiveView, ShadowViewRimMarkFollowsTheArc) {
    profile_ = makeProfile("g25");
    MapperConfig mapper = g25Mapping();
    mapper.steering.targetRotationDeg = 900.0F;
    auto view = makeView(8, 40, mapper);
    view->showShadow(true);
    const auto markCentre = [&](const float steering) {
        WheelState state;
        state.steering = steering;
        draw(*view, {}, state);
        float sx = 0;
        float sy = 0;
        float n = 0;
        for (const auto& r : window_.last.rects) {
            if (r.filled && sameColour(r.colour, style::kCaution)) {
                sx += (r.x + r.w / 2) * r.w;
                sy += r.y * r.w;
                n += r.w;
            }
        }
        EXPECT_GT(n, 0.0F);
        return std::pair{sx / n, sy / n};
    };
    const auto [cx0, cy0] = markCentre(0.0F);       // the top: straight above the hub
    const auto [lx, ly] = markCentre(-1.0F);        // full lock left: lower left, 150 degrees round
    const auto [hx, hy] = markCentre(-0.5F);        // half way: 75 degrees left
    EXPECT_NEAR(cx0, 260.0F, 2.0F);
    EXPECT_LT(lx, 260.0F - 40.0F);
    EXPECT_GT(ly, cy0 + 150.0F);                    // below the hub's height
    EXPECT_LT(hx, 260.0F - 80.0F);
    EXPECT_LT(hy, ly);                              // half way is higher up than full lock
}

TEST_F(LiveView, MappingTabShowsEverySource) {
    auto view = makeView();
    std::array<DeviceLabels, MapperConfig::kMaxAccessories> accessories{};
    accessories[0].name = "Pedals";
    accessories[0].vendorId = 0x1234;
    accessories[0].productId = 1;
    accessories[0].buttons[7] = "Clip";
    accessories[1].vendorId = 0x2222;   // no name: its id
    accessories[1].productId = 2;
    view->setAccessories(accessories);
    view->setDevices("virtual G920 on, rig hidden (1 of 1 devices)", false);
    view->setSessionLine("Target G920   Mode Virtual   fixed while running");
    RawInputs inputs;
    inputs.devices[0].axes[0] = 0.5F;
    inputs.devices[0].axes[3] = -0.25F;
    inputs.devices[1].axes[1] = -1.0F;
    WheelState state;
    state.steering = -0.5F;
    state.throttle = 0.4F;
    TargetReport report;
    report.hat = 2;
    report.buttons = 1U;   // A pressed
    draw(*view, inputs, state, report);
    for (const std::string text : {"ax0", "d1.1", "--", "d1 = Pedals, d2 = 2222:0002", "(not on this device)",
                                   "-> steering", "-> clutch", "not used", "-> G920 hat  2", "LIVE",
                                   "Target G920   Mode Virtual   fixed while running",
                                   "Press any button on the wheel to see where it goes."}) {
        EXPECT_TRUE(window_.has(text, true)) << text << "\n" << window_.dump();
    }
    EXPECT_TRUE(window_.has("<- Pedals 7 Clip", true)) << window_.dump();
    EXPECT_TRUE(window_.has("+", true));
    // Buttons pressed: 0 (two targets), 4 (D-pad right and a target), 12 (D-pad up), 15 (nothing... 15 is target 15).
    inputs.devices[0].buttons = 1ULL;
    draw(*view, inputs, state, report);
    EXPECT_TRUE(window_.has("Last pressed: G920 1 A <- Rig 0 Cross  (and others)")) << window_.dump();
    inputs.devices[0].buttons = 1ULL | (1ULL << 4);
    draw(*view, inputs, state, report);
    EXPECT_TRUE(window_.has("Last pressed: G920 D-pad right <- Rig 4  (and a button: check [buttons])")) << window_.dump();
    inputs.devices[0].buttons = 1ULL << 12;
    draw(*view, inputs, state, report);
    EXPECT_TRUE(window_.has("Last pressed: G920 D-pad up <- Rig 12")) << window_.dump();
    inputs.devices[0].buttons = 1ULL << 19;   // target 19 has no name on the G920: not mapped
    draw(*view, inputs, state, report);
    EXPECT_TRUE(window_.has("Last pressed: Rig 19  (not mapped to any G920 button)")) << window_.dump();
    // A warning about the devices, red; no devices: the hint.
    view->setDevices("NOT HIDDEN: Moza R3", true);
    view->setHeaderChars(10);
    view->setShimMode(true);
    draw(*view, inputs, state, report);
    EXPECT_EQ(window_.find("NOT HIDDEN: Moza R3", true)->colour.r, style::kWarning.r);
    view->setDevices("", false);
    view->setSessionLine("");
    draw(*view, inputs, state, report);
    EXPECT_TRUE(window_.has("Move every control and check where it lands.", true));
}

// A device with no buttons says so; out-of-range counts are clamped; a wider window has room for longer lines.
TEST_F(LiveView, NoButtonsAndAWideWindow) {
    MapperConfig plain;
    auto view = makeView(99, -3, plain);
    window_.setSize(1600, 1000);
    draw(*view);
    EXPECT_TRUE(window_.has("(the device reports no buttons)")) << window_.dump();
}

// The Logs tab: one row per component (the drivers' Windows only here, left out with the shim), "no log yet",
// a file's size and age; the live log merged from the files, coloured by level; clicking a row opens its folder.
TEST_F(LiveView, LogsTabRowsAndLiveLog) {
    writeLog("wheelbri-shim", {spdlogLine("error", "shim.device", "cannot open the wheel"),
                               spdlogLine("warning", "shim", "a warning"), spdlogLine("debug", "shim.audit", "detail"),
                               spdlogLine("info", "service.averyveryverylongname", "hello"), "a plain line"});
    auto view = makeView();
    view->setTab(MappingView::Tab::Logs);
    FilesDisplay files;
    files.logFolder = dir_ / "logs";
    files.logToFile = false;   // the app's own log: console only
    files.oldFiles = "3 files older than 7 days deleted";
    files.oldFilesFailed = true;
    view->setFiles(files);
    draw(*view);
    // The drivers' rows: in the same folder on Windows (none written yet), not on Linux.
#if defined(_WIN32)
    const std::string driverRow = "no log yet";
    constexpr std::size_t kDriverOpens = 1;   // a click on its row opens the folder
#else
    const std::string driverRow = "Windows only";
    constexpr std::size_t kDriverOpens = 0;   // no folder: nothing to open
#endif
    for (const std::string& text : {std::string("console only (no log file)"), driverRow, std::string("KB, written"),
                                   std::string("cannot open the wheel"), std::string("a plain line"),
                                   std::string("averyveryver."), std::string("old files: 3")}) {
        EXPECT_TRUE(window_.has(text, true)) << text << "\n" << window_.dump();
    }
    EXPECT_EQ(window_.find("cannot open the wheel")->colour.r, style::kWarning.r);
    EXPECT_EQ(window_.find("detail")->colour.r, style::kDim.r);
    EXPECT_EQ(window_.find("old files: ", true)->colour.r, style::kWarning.r);
    // A row opens its folder; one with no folder (a driver's on Linux) logs that there is none; a failure is logged.
    ASSERT_TRUE(clickOn(*view, "Game shim"));
    ASSERT_EQ(window_.opened.size(), 1U);
    ASSERT_TRUE(clickOn(*view, "Wheel driver"));
    EXPECT_EQ(window_.opened.size(), 1U + kDriverOpens);
    window_.openFolderWorks = false;
    view->onKey('o');   // O: the logs folder
    EXPECT_EQ(window_.opened.size(), 2U + kDriverOpens);
    // The shim's session: no driver rows.
    view->setShimMode(true);
    draw(*view);
    EXPECT_FALSE(window_.has("Wheel driver"));
    view->setShimMode(true);   // unchanged: nothing listed again
}

// A folder named like a log file is not one: its row says it cannot be read (it said "0 KB" before the fix, the
// write time's lookup clearing the size's error), and the live log skips it.
TEST_F(LiveView, FolderNamedLikeALog) {
    std::filesystem::create_directories(dir_ / "logs" / ("wheelbri-shim-" + localDate(WallClock::now()) + "-02.log"));
    auto view = makeView();
    view->setTab(MappingView::Tab::Logs);
    FilesDisplay files;
    files.logFolder = dir_ / "logs";
    files.logToFile = false;   // the app's own log is not followed either: nothing to show
    view->setFiles(files);
    draw(*view);
    EXPECT_TRUE(window_.has("Is a directory", true)) << window_.dump();
    EXPECT_FALSE(window_.has("0 KB", true)) << window_.dump();
    EXPECT_TRUE(window_.has("no log lines yet", true)) << window_.dump();
}

// A file's age in seconds, minutes or hours.
TEST_F(LiveView, FileAges) {
    writeLog("wheelbri-shim", {spdlogLine("info", "shim", "old")});
    const auto file = dir_ / "logs" / ("wheelbri-shim-" + localDate(WallClock::now()) + "-01.log");
    for (const auto& [ago, said] : {std::pair<std::chrono::seconds, std::string>{std::chrono::seconds(5 * 60), "5 min ago"},
                                    {std::chrono::seconds(3 * 3600 + 5), "3 h ago"}}) {
        std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now() - ago);
        auto view = makeView();
        view->setTab(MappingView::Tab::Logs);
        draw(*view);
        EXPECT_TRUE(window_.has(said, true)) << said << "\n" << window_.dump();
    }
}

// The live log scrolls (keys, wheel), follows new lines at the bottom or holds still while scrolled up, wraps long
// lines (W, or the Wrap button) or scrolls them sideways (Left / Right, Shift + wheel, the sideways bar), and its bars
// are dragged.
TEST_F(LiveView, LiveLogScrollsWrapsAndDrags) {
    std::vector<std::string> lines;
    for (int i = 0; i < 120; ++i) lines.push_back(spdlogLine("info", "service.devices", "line " + std::to_string(i)));
    lines.push_back(spdlogLine("info", "service", "long " + std::string(200, 'x') + " end of the long line"));
    lines.push_back(spdlogLine("info", "service", "words " + [] {
                                   std::string s;
                                   for (int i = 0; i < 40; ++i) s += "word" + std::to_string(i) + " ";
                                   return s;
                               }()));
    writeLog("wheelbri-shim", lines);
    auto view = makeView();
    view->setTab(MappingView::Tab::Logs);
    draw(*view);
    EXPECT_TRUE(window_.has("newest last; wheel or PageUp goes back", true)) << window_.dump();
    for (const char key : {ui::kPageUp, ui::kUp, ui::kHome}) {
        view->onKey(key);
        draw(*view);
    }
    EXPECT_TRUE(window_.has("lines up: End follows the newest", true)) << window_.dump();
    view->onKey(ui::kPageDown);
    view->onKey(ui::kDown);
    view->onScroll(2);
    draw(*view);
    // New lines while scrolled up: the view holds still.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    writeLog("wheelbri-shim", {spdlogLine("info", "service", "newer line")});
    draw(*view);
    EXPECT_FALSE(window_.has("newer line"));
    view->onKey(ui::kEnd);
    draw(*view);
    EXPECT_TRUE(window_.has("newer line")) << window_.dump();
    // Sideways: Right, Left, the sideways wheel; not on another tab.
    view->onKey(ui::kRight);
    view->onHScroll(3);
    draw(*view);
    view->onKey(ui::kLeft);
    draw(*view);
    // Drag the vertical bar: press on its track, move, release; then on its thumb.
    const float vx = window_.width() - 2 - 8 + 2;
    view->onClick(vx, 400);
    view->onPointer(vx, 300, true);
    draw(*view);
    view->onPointer(vx, 300, false);
    draw(*view);
    view->onClick(vx, 300);
    view->onPointer(vx, 320, true);
    view->onPointer(vx, 320, false);
    // The sideways bar, under the messages.
    const float hy = window_.height() - 34 - 8 - 2 + 2 + 4;
    view->onClick(4 + 35 * 8 + 300, hy);
    view->onPointer(4 + 35 * 8 + 500, hy, true);
    draw(*view);
    view->onClick(4 + 35 * 8 + 2, hy);
    view->onPointer(4 + 35 * 8 + 2, hy, false);
    // Wrap: the long lines on several rows; W again, and the Wrap button.
    view->onKey('w');
    view->onKey(ui::kEnd);
    draw(*view);
    EXPECT_TRUE(window_.has("end of the long line", true)) << window_.dump();
    view->onHScroll(3);   // nothing sideways with wrap
    ASSERT_TRUE(clickOn(*view, "Wrap (W)"));
    draw(*view);
    // The wheel and the bars only on the Logs tab.
    view->setTab(MappingView::Tab::Mapping);
    view->onScroll(1);
    view->onHScroll(1);
    view->onPointer(vx, 300, true);
}

// The level button (V, the app's) steps warn -> info -> debug -> trace -> warn; lit while it writes more than warnings.
TEST_F(LiveView, LevelButton) {
    AppLogLevel::shared().start(dir_ / "app.ini");
    auto view = makeView();
    view->setTab(MappingView::Tab::Logs);
    draw(*view);
    const auto* level = window_.find("Level: ", true);
    ASSERT_NE(level, nullptr) << window_.dump();
    const std::string before = level->value;
    view->onKey('v');
    draw(*view);
    EXPECT_NE(window_.find("Level: ", true)->value, before);
    ASSERT_TRUE(clickOn(*view, "Level: ", true));
    draw(*view);
    // Round to warn again for the other tests.
    while (AppLogLevel::shared().current() != LogLevel::Warn) static_cast<void>(AppLogLevel::shared().next());
}

// The Telemetry tab: the files (recording or not, and why), what is held, and the latest events: buttons pressed
// (mapped, D-pad, nothing) and released, the D-pad, pedals pressed and released with their deepest, effects starting
// and stopping; the first frame only takes what is held then.
TEST_F(LiveView, TelemetryTabEvents) {
    MappingView::setOptionalTabs(true, true);
    auto view = makeView();
    view->setTab(MappingView::Tab::Telemetry);
    FilesDisplay files;
    files.logFolder = dir_ / "logs";
    files.telemetryStatus = "off in the config";
    view->setFiles(files);
    RawInputs inputs;
    inputs.devices[0].buttons = 1ULL << 5;   // held when the window opens: not an event
    WheelState state;
    TargetReport report;
    draw(*view, inputs, state, report);
    EXPECT_TRUE(window_.has("not recording   O opens")) << window_.dump();
    EXPECT_TRUE(window_.has("off in the config"));
    EXPECT_TRUE(window_.has("nothing yet: press a button or a pedal"));
    FakeSink sink;
    FfbController controller(sink, FfbSafetyConfig{});
    FfbPacket packet;
    packet.kind = FfbPacket::Kind::SlotState;
    packet.slot = 0;
    packet.playing = true;
    packet.effect.type = FfbEffectType::Constant;
    packet.effect.constantLevel = 0.5F;
    controller.onPacket(packet, 1'000'000);
    controller.update(2'000'000);
    FfbDisplay ffb{&controller, "", false, 3'000'000};
    inputs.devices[0].buttons = (1ULL << 0) | (1ULL << 12) | (1ULL << 19);
    report.hat = 0;
    state.throttle = 0.6F;
    draw(*view, inputs, state, report, ffb);
    state.throttle = 0.9F;
    inputs.devices[0].buttons = 1ULL << 0;
    report.hat = -1;
    draw(*view, inputs, state, report, ffb);
    state.throttle = 0.0F;
    packet.playing = false;
    controller.onPacket(packet, 4'000'000);
    controller.update(5'000'000);
    draw(*view, inputs, state, report, ffb);
    for (const std::string text : {"Rig 0 Cross pressed -> G920 1 A", "Rig 12 pressed -> G920 D-pad up",
                                   "Rig 19 pressed -> nothing (not mapped)", "Rig 19 released", "G920 D-pad up",
                                   "G920 D-pad centred", "Throttle pressed", "Throttle released (deepest 90%)",
                                   "slot 0 constant starts", "slot 0 constant stops", "Rig 0 Cross"}) {
        EXPECT_TRUE(window_.has(text, true)) << text << "\n" << window_.dump();
    }
    // Recording: the files with their rows; one not there yet has no size; O opens their folder, as does a click.
    TelemetryFiles telemetry;
    telemetry.on = true;
    telemetry.samplesPath = dir_ / "logs" / "telemetry-x.log";
    telemetry.ffbPath = dir_ / "logs" / "ffb-x.log";
    telemetry.rows = 12;
    std::ofstream(telemetry.samplesPath) << "rows";
    view->setTelemetry(telemetry);
    draw(*view, inputs, state, report, ffb);
    EXPECT_TRUE(window_.has("12 rows,", true)) << window_.dump();
    EXPECT_TRUE(window_.has("0 changes"));
    ASSERT_TRUE(clickOn(*view, "Samples"));
    view->onKey('o');
    EXPECT_EQ(window_.opened.size(), 2U);
    EXPECT_EQ(window_.opened.back(), dir_ / "logs");
    // Recording with no ffb file: "-".
    telemetry.ffbPath.clear();
    view->setTelemetry(telemetry);
    draw(*view, inputs, state, report, ffb);
    EXPECT_TRUE(window_.has("-"));
    // More events than fit: the newest shown.
    for (int i = 0; i < 260; ++i) {
        inputs.devices[0].buttons ^= 1ULL << 2;
        draw(*view, inputs, state, report, ffb);
    }
}

// The Force feedback tab: no controller (off, or why); a controller with effects playing, stopped, refused, its
// usage and histogram; the watchdog stopping everything.
TEST_F(LiveView, ForceFeedbackTab) {
    MappingView::setOptionalTabs(false, true);
    auto view = makeView();
    view->setTab(MappingView::Tab::ForceFeedback);
    draw(*view);
    EXPECT_TRUE(window_.has("off")) << window_.dump();
    EXPECT_TRUE(window_.has("none yet: start the game and drive"));
    FfbDisplay none;
    none.status = "the wheel has no force feedback";
    draw(*view, {}, {}, {}, none);
    EXPECT_TRUE(window_.has("the wheel has no force feedback"));

    FakeSink sink;
    FfbController controller(sink, FfbSafetyConfig{});
    FfbDisplay ffb{&controller, "", true, 0};
    draw(*view, {}, {}, {}, ffb);
    EXPECT_TRUE(window_.has("no effects from the game yet", true)) << window_.dump();
    EXPECT_TRUE(window_.has("no force feedback played yet", true));
    EXPECT_TRUE(window_.has("last packet from the game never", true));
    std::uint64_t now = 1'000'000;
    const auto send = [&](const std::uint8_t slot, const FfbEffectType type, const bool playing) {
        FfbPacket packet;
        packet.kind = FfbPacket::Kind::SlotState;
        packet.slot = slot;
        packet.playing = playing;
        packet.effect.type = type;
        packet.effect.constantLevel = 0.9F;
        packet.effect.periodic.magnitude = 0.4F;
        packet.effect.periodic.periodMs = 50;
        packet.effect.condition.positiveCoefficient = 0.5F;
        packet.effect.condition.negativeCoefficient = 0.5F;
        packet.effect.condition.positiveSaturation = 1.0F;
        packet.effect.condition.negativeSaturation = 1.0F;
        controller.onPacket(packet, now);
    };
    send(0, FfbEffectType::Constant, true);
    send(1, FfbEffectType::Spring, true);
    send(2, FfbEffectType::Sine, true);
    send(3, FfbEffectType::Damper, false);
    for (int i = 0; i < 50; ++i) {
        now += 5'000'000;
        FfbPacket keep;
        keep.kind = FfbPacket::Kind::KeepAlive;
        controller.onPacket(keep, now);
        controller.update(now);
    }
    ffb.nowNs = now + 2'000'000;
    draw(*view, {}, {}, {}, ffb);
    for (const std::string text : {"level +0.90", "k +0.50/+0.50", "mag 0.40", "3 playing", "ms ago",
                                   "direction inverted", "constant asked", "top "}) {
        EXPECT_TRUE(window_.has(text, true)) << text << "\n" << window_.dump();
    }
    // The game goes silent: the watchdog stops everything.
    now += 2'000'000'000;
    controller.update(now);
    ffb.nowNs = now;
    draw(*view, {}, {}, {}, ffb);
    EXPECT_TRUE(window_.has("game silent for 500 ms", true)) << window_.dump();
    // An effect the wheel refuses: wanted, not playing ("stopped").
    FakeSink refusing;
    refusing.refuse = true;
    FfbController refused(refusing, FfbSafetyConfig{});
    FfbPacket packet;
    packet.kind = FfbPacket::Kind::SlotState;
    packet.playing = true;
    packet.effect.type = FfbEffectType::Constant;
    packet.effect.constantLevel = 0.3F;
    refused.onPacket(packet, 1'000'000);   // wanted, not played yet (no update)
    draw(*view, {}, {}, {}, FfbDisplay{&refused, "", false, 7'000'000});
    EXPECT_TRUE(window_.has("stopped")) << window_.dump();
    // A packet stamped after "now" (clocks apart): no age.
    ffb.nowNs = 0;
    draw(*view, {}, {}, {}, ffb);
}

}  // namespace
}  // namespace wheelbri::service
