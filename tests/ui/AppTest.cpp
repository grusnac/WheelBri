// SPDX-License-Identifier: GPL-3.0-or-later
// The app (service/src/App.cpp, runApp) and the command line service's --ui window (makeSessionWindow), run for real
// on a scripted window (UiFakes): its tabs, Start and Stop with a fake session on the app's own session thread, the
// status next to Start, the session bar, the popups (Before Start, first hide, a game run the wrong way, the shims at
// Stop), Mode Shim's game folders, and closing while a session runs.

#include "UiFakes.hpp"

#include "App.hpp"
#include "ConfigPicker.hpp"
#include "Platform.hpp"
#include "PrivilegedSteps.hpp"
#include "ServiceLogging.hpp"
#include "UiStyle.hpp"

#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/FfbPacket.hpp"
#include "wheelbri/Rig.hpp"
#include "wheelbri/ShimGames.hpp"
#include "wheelbri/StartNeeds.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace wheelbri::service {
namespace {

using uitest::FakeDevice;
using uitest::FakeDevices;
using uitest::ScriptedWindow;
using uitest::UserFolders;
using uitest::WindowFactory;

// A session as main.cpp's runSession runs one, without devices: it says what it does (`step`), shows itself
// (`sessionStarted`) and hands the window frames until it is asked to stop. How it behaves is set by the test.
class FakeSession {
public:
    enum class Mode {
        Normal,          // starts, runs until Stop
        FailsAtOnce,     // returns `exitCode` before showing itself
        Throws,          // throws a std::exception
        ThrowsUnknown,   // throws something else
        FirstHide,       // says kStepFirstHide until `hideDone`, then starts
        WaitsForWheel,   // says "waiting for the wheel" until Stop, never starts
        WheelBack,       // starts twice (the wheel came back)
        ProfileUnknown,  // starts with a target no profile has
        IgnoresStop,     // starts, and ends only once `release` is set
    };

    std::atomic<Mode> mode{Mode::Normal};
    std::atomic<int> exitCode{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> hideDone{false};
    std::atomic<bool> release{false};
    std::atomic<int> runs{0};
    std::atomic<bool> lastVirtual{false};
    std::atomic<ButtonOrder> lastOrder{ButtonOrder::Driver};
    std::atomic<bool> lastTrueForce{false};
    std::atomic<int> stopsRequested{0};
    std::atomic<int> frameDelayMs{5};

    // What the next frames carry (the test changes it while the session runs).
    void setFrame(const std::function<void(UiFrame&)>& edit) {
        const std::lock_guard lock(mutex_);
        edit(frame_);
    }
    void setInfo(const std::function<void(SessionInfo&)>& edit) {
        const std::lock_guard lock(mutex_);
        edit(info_);
    }

    int run(UiHost& host, const std::string&, const bool virtualDevice, const ButtonOrder order,
            const bool trueForce) {
        ++runs;
        lastVirtual = virtualDevice;
        lastOrder = order;
        lastTrueForce = trueForce;
        switch (mode.load()) {
            case Mode::FailsAtOnce: return exitCode;
            case Mode::Throws: throw std::runtime_error("broken on purpose");
            case Mode::ThrowsUnknown: throw 42;
            case Mode::FirstHide:
                host.step(kStepFirstHide);
                while (!hideDone && !stop) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                host.step("");
                break;
            case Mode::WaitsForWheel:
                host.step("waiting for the wheel");
                while (!stop) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                return 0;
            default: break;
        }
        SessionInfo info;
        {
            const std::lock_guard lock(mutex_);
            info = info_;
        }
        if (mode == Mode::ProfileUnknown) info.targetProfile = "g999";
        host.sessionStarted(info);
        int frames = 0;
        while (!stop || (mode == Mode::IgnoresStop && !release)) {
            std::unique_ptr<UiFrame> frame;
            {
                const std::lock_guard lock(mutex_);
                frame = std::make_unique<UiFrame>(frame_);
            }
            host.frame(*frame);
            if (mode == Mode::WheelBack && ++frames == 20) host.sessionStarted(info);
            std::this_thread::sleep_for(std::chrono::milliseconds(frameDelayMs.load()));
        }
        return exitCode;
    }

private:
    std::mutex mutex_;
    SessionInfo info_ = [] {
        SessionInfo info;
        info.sourceName = "Moza R3";
        info.vendorId = 0x346e;
        info.productId = 0x0005;
        info.axes = 8;
        info.buttons = 32;
        info.targetProfile = "g920";
        return info;
    }();
    UiFrame frame_ = [] {
        UiFrame frame;
        frame.ready = true;
        return frame;
    }();
};

// The sink the frames' FfbController plays on (it outlives every copy of them).
class FakeSinkHolder final : public IFfbSink {
public:
    static IFfbSink& sink() {
        static FakeSinkHolder holder;
        return holder;
    }
    static void reset() {}
    bool play(std::size_t, const FfbEffect&) override { return true; }
    void stop(std::size_t) override {}
    void stopAll() override {}
};

ui::WindowInput clickText(ScriptedWindow& window, const std::string& text, const bool part = false) {
    const auto at = window.clickOn(text, part);
    if (!at) return {};
    return uitest::click(at->x, at->y);
}

class AppRun : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        folders_ = std::make_unique<UserFolders>("app-" + name);
        root_ = folders_->root();
        FakeDevices::shared().clear();
        // The configs that come with the program.
        std::filesystem::create_directories(root_ / "builtin");
        for (const char* file : {"g25.ini", "g27.ini", "g920.ini", "g923.ini", "wheelbri-shim.ini"}) {
            std::filesystem::copy_file(std::filesystem::path(WHEELBRI_CONFIG_DIR) / file, root_ / "builtin" / file);
        }
        // WheelBri's shim, 64- and 32-bit.
        std::filesystem::create_directories(root_ / "shim" / "x86");
        std::ofstream(root_ / "shim" / kShimDll, std::ios::binary) << "our 64-bit shim";
        std::ofstream(root_ / "shim" / "x86" / kShimDll, std::ios::binary) << "our 32-bit shim";

        settings_.builtInConfigFolder = root_ / "builtin";
        settings_.userConfigFolder = userConfigFolder();
        settings_.stateFile = appStateFile();
        settings_.logFolder = root_ / "state" / "wheelbri";   // logsFolder(), XDG_STATE_HOME/wheelbri
        settings_.devicesFolder = deviceLabelsFolder();
        settings_.shimDlls = ShimSources{root_ / "shim" / kShimDll, root_ / "shim" / "x86" / kShimDll};
        settings_.shimSettings = root_ / "builtin" / kShimSettings;
        settings_.runSession = [this](UiHost& host, const std::string& config, const bool virtualDevice,
                                      const ButtonOrder order, const bool trueForce) {
            return session_.run(host, config, virtualDevice, order, trueForce);
        };
        settings_.requestStop = [this] {
            ++session_.stopsRequested;
            session_.stop = true;
        };
        settings_.clearStop = [this] { session_.stop = false; };
        std::filesystem::create_directories(settings_.logFolder);

        window_.closeAtEnd = true;
        WindowFactory::shared().make = uitest::proxyOf(window_);
    }
    void TearDown() override {
        WindowFactory::shared().make = nullptr;
        FakeDevices::shared().clear();
        folders_.reset();
    }

    // A rig with the wheel (346e:0005), and the app set to `way` (shim / virtual) for the G920.
    void setUpRig(const std::string& way = "shim") {
        Rig rig;
        RigDevice wheel;
        wheel.name = "Moza R3";
        wheel.id = {0x346e, 0x0005};
        wheel.kind = DeviceKind::Wheel;
        wheel.axes[0].label = "Wheel";
        wheel.buttons[0].label = "A";
        rig.devices.push_back(wheel);
        ASSERT_TRUE(saveRig(rigsFolder(), "My rig", rig));
        std::filesystem::create_directories(settings_.stateFile.parent_path());
        std::ofstream(settings_.stateFile) << "[app]\nrig = My rig\nway = " << way << "\ntarget = g920\n";
    }
    // A game folder (with its .exe) listed for the shim.
    std::filesystem::path listGame(const std::string& name = "Need for Speed") {
        const auto folder = root_ / "games" / name;
        std::filesystem::create_directories(folder);
        std::ofstream(folder / "NFS16.exe", std::ios::binary) << "MZ";
        ShimGameList list = readShimGames(shimGamesFile());
        addShimGame(list, folder);
        list.selected = folder;
        EXPECT_TRUE(writeShimGames(shimGamesFile(), list));
        return folder;
    }
    std::shared_ptr<FakeDevice> plugWheel() { return FakeDevices::shared().add("Moza R3", 0x346e, 0x0005, 8, 32); }

    // Script helpers.
    void then(const std::string& what, std::function<bool(ScriptedWindow&)> until,
              std::function<ui::WindowInput(ScriptedWindow&)> input = nullptr, const int maxPumps = 400) {
        window_.script.push_back({std::move(until), std::move(input), maxPumps, what});
    }
    void once(std::function<ui::WindowInput(ScriptedWindow&)> input) {
        window_.script.push_back({nullptr, std::move(input), 1, "once"});
    }
    void waitFor(const std::string& text, const bool part = true, const int maxPumps = 400) {
        then("wait for " + text, [text, part](ScriptedWindow& w) { return w.has(text, part); }, nullptr, maxPumps);
    }
    void clickWhenShown(const std::string& text, const bool part = false) {
        waitFor(text, part);
        once([text, part](ScriptedWindow& w) { return clickText(w, text, part); });
    }
    void press(const std::string& keys) {
        once([keys](ScriptedWindow&) { return uitest::keys(keys); });
    }
    void pumps(const int count) {
        for (int i = 0; i < count; ++i) once([](ScriptedWindow&) { return uitest::nothing(); });
    }
    void check(std::function<void(ScriptedWindow&)> check) {
        once([check = std::move(check)](ScriptedWindow& w) {
            check(w);
            return uitest::nothing();
        });
    }
    int run() {
        const int result = runApp(settings_);
        EXPECT_FALSE(window_.timedOut) << "timed out at: " << window_.timedOutAt << "\n" << window_.dump();
        return result;
    }
    // Start and Stop are one button at the bar's right end: Start while stopped, Stop while a session runs.
    [[nodiscard]] float startX() const { return window_.width() - 4 - 68; }
    [[nodiscard]] float barY() const { return window_.height() - 28 + 4 + 8; }
    ui::WindowInput clickStart() const { return uitest::click(startX() + 10, barY()); }
    ui::WindowInput clickStop() const { return uitest::click(startX() + 10, barY()); }

    std::unique_ptr<UserFolders> folders_;
    std::filesystem::path root_;
    AppSettings settings_;
    FakeSession session_;
    ScriptedWindow window_;
};

// The window cannot be opened: runApp says so and returns 1.
TEST_F(AppRun, NoWindow) {
    WindowFactory::shared().make = nullptr;
    EXPECT_EQ(runApp(settings_), 1);
}

// The first start (no rig): the Rig tab, "NO RIG" in red, Start greyed, no notice above the tab bar; a click on Start
// shows Before Start with what is missing, in red; OK, and Escape, close it. The keys and the tab buttons switch the
// tabs; closing the window ends the app.
TEST_F(AppRun, FirstStartWithoutRig) {
    waitFor(kNoRig);
    check([](ScriptedWindow& w) {
        EXPECT_EQ(w.find(kNoRig)->colour.r, style::kWarning.r);
        EXPECT_FALSE(w.has("No rig yet: name your controls in the Rig tab (R).")) << w.dump();   // the old notice
        EXPECT_TRUE(w.has("RIG"));   // the Rig tab's heading
    });
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Before Start");
    check([](ScriptedWindow& w) {
        const auto* const toDo = w.find("TO DO Rig: name your controls in the Rig tab (R)", true);
        ASSERT_NE(toDo, nullptr) << w.dump();
        EXPECT_EQ(toDo->colour.r, style::kWarning.r);
    });
    once([this](ScriptedWindow&) { return clickStart(); });   // the popup takes the click: nothing
    clickWhenShown("OK");
    then("popup gone", [](ScriptedWindow& w) { return !w.has("Before Start"); });
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Before Start");
    press(std::string(1, ui::kEscape));
    then("popup gone", [](ScriptedWindow& w) { return !w.has("Before Start"); });
    for (const char key : {'m', 'g', 'l', 'r', 'x'}) press(std::string(1, key));
    press(std::string(1, ui::kPageDown));
    clickWhenShown("Logs (L)");
    waitFor("LIVE LOG");
    press(std::string(1, ui::kPageUp) + "w");
    once([](ScriptedWindow& w) {
        ui::WindowInput input;
        input.wheel = 1;
        input.hwheel = 1;
        input.pointer = {500, 500};
        input.pointerInside = true;
        (void)w;
        return input;
    });
    once([](ScriptedWindow& w) { return clickText(w, "Wrap (W)"); });
    clickWhenShown("Mapping (M)");
    clickWhenShown("Games (G)");
    clickWhenShown("Rig (R)");
    once([](ScriptedWindow&) {
        ui::WindowInput input;
        input.wheel = -1;
        input.pointer = {400, 200};
        input.pointerInside = true;
        input.leftDown = true;
        return input;
    });
    EXPECT_EQ(run(), 0);
}

// Mode Shim with a rig and a listed game: the app opens on the Mapping tab, Ready; the session bar says what Start runs;
// Start puts the shim in the game's folder and runs the session, which shows itself on the Live tab; the status follows the frames; the
// Games tab opens when a profiled game starts; Stop ends it and, the shim having been heard, asks about the shims:
// "Delete them" with "Don't ask again" deletes them and is remembered.
TEST_F(AppRun, ShimStartRunStop) {
    setUpRig();
    const auto game = listGame();
    plugWheel();
    waitFor("Ready", false);
    check([](ScriptedWindow& w) {
        EXPECT_TRUE(w.has("G920 - Shim - 1 game", true)) << w.dump();
        EXPECT_TRUE(w.has("EDIT")) << w.dump();   // a rig already: the app opens on the Mapping tab
    });
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Running ");
    waitFor("Target G920  Mode Shim");   // Start ends on the Live tab once the session runs
    check([&game](ScriptedWindow& w) {
        EXPECT_FALSE(w.has("Start")) << w.dump();   // the same button is Stop now, in red
        ASSERT_NE(w.find("Stop"), nullptr);
        EXPECT_EQ(w.find("Stop")->colour.r, style::kWarning.r);
        EXPECT_TRUE(std::filesystem::exists(game / kShimDll));
        EXPECT_FALSE(w.has("LIVE LOG")) << w.dump();   // the logs only while it came up
    });
    // Running, Mapping is still the editor (each change in use at once); Live (S) next to it shows the session.
    press("m");
    waitFor("running: each change is in use at once", true);
    check([](ScriptedWindow& w) { EXPECT_TRUE(w.has("EDIT")) << w.dump(); });
    clickWhenShown("Live (S)");
    waitFor("Target G920  Mode Shim");
    // The session bar between the tabs and the status: at the smallest window (five tabs) the target and the mode;
    // a wider one has room for the game too.
    check([](ScriptedWindow& w) { EXPECT_TRUE(w.has("G920 - Shim", true)) << w.dump(); });
    // A profiled game starts: the Games tab opens on it; its name and rotation in the status.
    once([this](ScriptedWindow&) {
        session_.setFrame([](UiFrame& f) {
            f.gameId = "nfs2015";
            f.game = "NFS 2015";
            f.gameRotation = "900 deg";
            f.gameStarts = 1;
            f.gamePid = 1234;
            f.gameProfileApplies = true;
        });
        return uitest::nothing();
    });
    waitFor("Running 0:00:0", true);
    waitFor(", NFS 2015, 900 deg", true);
    // The Games tab opens on it (the switch may come a frame after the status: waited for, not checked once).
    waitFor("GAMES", false);
    // Force feedback from the shim: heard.
    FakeSinkHolder::reset();
    once([this](ScriptedWindow&) {
        session_.setFrame([](UiFrame& f) {
            FfbPacket packet;
            packet.kind = FfbPacket::Kind::SlotState;
            packet.slot = 0;
            packet.playing = true;
            packet.effect.type = FfbEffectType::Constant;
            packet.effect.constantLevel = 0.5F;
            f.ffb.emplace(FakeSinkHolder::sink(), FfbSafetyConfig{});
            f.ffb->onPacket(packet, 1'000'000);
            f.ffb->update(2'000'000);
        });
        return uitest::nothing();
    });
    pumps(10);
    // Each warning the status can show, one after the other.
    const auto status = [this](const std::string& expected, const std::function<void(UiFrame&)>& edit) {
        once([this, edit](ScriptedWindow&) {
            session_.setFrame(edit);
            return uitest::nothing();
        });
        waitFor(expected, false);
    };
    status("GAME NOT USING WHEEL", [](UiFrame& f) { f.gameIgnoresWheel = true; });
    status("GAME FFB OFF: ALT-TAB", [](UiFrame& f) {
        f.gameIgnoresWheel = false;
        f.gameFfbIdle = true;
    });
    status("NFS 2015: RESET ITS WHEEL CONTROLS", [](UiFrame& f) {
        f.gameFfbIdle = false;
        f.gameWarning = "NFS 2015: RESET ITS WHEEL CONTROLS";
    });
    status("WHEEL LOST 0:00:03", [](UiFrame& f) { f.wheelLostMs = 3000; });
    once([this](ScriptedWindow&) {
        session_.setFrame([](UiFrame& f) {
            f.wheelLostMs = 0;
            f.gameWarning.clear();
            f.game.clear();   // a short status: room for the session bar
            f.gameRotation.clear();
        });
        return uitest::nothing();
    });
    // The live view's tabs: keys and the session bar (to the Mapping tab).
    press("l");
    waitFor("LIVE LOG");
    press("r");
    waitFor("The running session's wheel (stop to see every device).");
    press("s");
    waitFor("LIVE", false);
    press("l");
    waitFor("LIVE LOG");
    waitFor("G920 - Shim - nfs2015", true);   // no name: the game's id
    once([](ScriptedWindow& w) { return clickText(w, "G920 - Shim", true); });
    waitFor("EDIT", false);   // the session bar: to the Mapping tab, where the target and the mode are
    once([this](ScriptedWindow&) { return clickStop(); });
    waitFor("Delete the shims from the games' folders?");
    once([](ScriptedWindow& w) { return clickText(w, "Don't ask again", true); });
    clickWhenShown("Delete them");
    then("stopped, shim deleted", [&game](ScriptedWindow& w) {
        return !w.has("Delete the shims from the games' folders?") && !std::filesystem::exists(game / kShimDll);
    });
    check([this](ScriptedWindow&) { EXPECT_EQ(readAppSetting(settings_.stateFile, "shim_at_stop"), "delete"); });
    // The next Start and Stop: deleted without asking.
    waitFor("Ready", false);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Running ");
    once([this](ScriptedWindow&) { return clickStop(); });
    waitFor("Ready", false);
    check([&game](ScriptedWindow& w) {
        EXPECT_FALSE(std::filesystem::exists(game / kShimDll));
        EXPECT_FALSE(w.has("Delete the shims from the games' folders?"));
    });
    EXPECT_EQ(run(), 0);
    EXPECT_EQ(session_.runs, 2);
    EXPECT_FALSE(session_.lastVirtual);
}

// The session fails at once (exit code 3): "Start failed (Logs)" in red, and Before Start says the last Start failed.
TEST_F(AppRun, SessionFailsAtOnce) {
    setUpRig();
    listGame();
    plugWheel();
    session_.mode = FakeSession::Mode::FailsAtOnce;
    session_.exitCode = 3;
    waitFor("Ready", false);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Start failed (Logs)", false);
    check([](ScriptedWindow& w) { EXPECT_EQ(w.find("Start failed (Logs)")->colour.r, style::kWarning.r); });
    EXPECT_EQ(run(), 0);
}

// A session that throws (a std::exception, or anything else) ends like a failed one; the app goes on.
TEST_F(AppRun, SessionThrows) {
    setUpRig();
    listGame();
    plugWheel();
    session_.mode = FakeSession::Mode::Throws;
    waitFor("Ready", false);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Start failed (Logs)", false);
    once([this](ScriptedWindow&) {
        session_.mode = FakeSession::Mode::ThrowsUnknown;
        return uitest::nothing();
    });
    pumps(70);   // Start is greyed for a moment after a failure? no: at once
    once([this](ScriptedWindow&) { return clickStart(); });
    pumps(30);
    waitFor("Start failed (Logs)", false);
    EXPECT_EQ(run(), 0);
    EXPECT_EQ(session_.runs, 2);
}

// Closing the window while a session runs stops it first, then the app ends.
TEST_F(AppRun, CloseWhileRunning) {
    setUpRig();
    listGame();
    plugWheel();
    waitFor("Ready", false);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Running ");
    EXPECT_EQ(run(), 0);
    EXPECT_GE(session_.stopsRequested, 1);
}

// Stop while the session is still starting (waiting for the wheel): "Starting ... : waiting for the wheel", then
// "Stopping", then stopped without the session ever showing itself; nothing is asked about the shims.
TEST_F(AppRun, StopWhileStarting) {
    setUpRig();
    listGame();
    plugWheel();
    session_.mode = FakeSession::Mode::WaitsForWheel;
    waitFor("Ready", false);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor(": waiting for the wheel");
    once([this](ScriptedWindow&) { return clickStop(); });
    waitFor("Ready", false);
    check([](ScriptedWindow& w) { EXPECT_FALSE(w.has("Delete the shims from the games' folders?")); });
    EXPECT_EQ(run(), 0);
}

// The hide filter put on the rig for the first time: a popup says so while it lasts, and goes by itself.
TEST_F(AppRun, FirstHidePopup) {
    setUpRig();
    std::ofstream(settings_.stateFile) << "[app]\nrig = My rig\nway = shim\ntarget = g25\n";
    listGame();
    plugWheel();
    session_.mode = FakeSession::Mode::FirstHide;
    waitFor("Ready", false);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Hiding your wheel from games: first time only");
    check([](ScriptedWindow& w) {
        // Information, not a warning: a blue title; it names the Target games will see.
        EXPECT_EQ(w.find("Hiding your wheel from games: first time only")->colour.r, style::kHeading.r);
        EXPECT_TRUE(w.has("G25. This happens once", true)) << w.dump();
        EXPECT_FALSE(w.has(" G25.", true)) << w.dump();   // a line never starts with the space it broke at
    });
    once([this](ScriptedWindow&) {
        session_.hideDone = true;
        return uitest::nothing();
    });
    then("popup gone", [](ScriptedWindow& w) { return !w.has("Hiding your wheel from games: first time only"); });
    waitFor("Running ");
    EXPECT_EQ(run(), 0);
}

// A game run the wrong way: told once per start, by Windows (none here: the app's popup says it), OK closes it.
TEST_F(AppRun, WayNotice) {
    setUpRig();
    listGame();
    plugWheel();
    waitFor("Ready", false);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Running ");
    once([this](ScriptedWindow&) {
        session_.setFrame([](UiFrame& f) {
            f.gameWayTitle = "NFS Heat needs Mode Virtual";
            f.gameWayText = "Stop, switch the Mode on the Mapping tab, Start again.";
            f.gameWayNotices = 1;
        });
        return uitest::nothing();
    });
    waitFor("NFS Heat needs Mode Virtual");
    clickWhenShown("OK");
    then("notice gone", [](ScriptedWindow& w) { return !w.has("NFS Heat needs Mode Virtual"); });
    pumps(20);
    check([](ScriptedWindow& w) { EXPECT_FALSE(w.has("NFS Heat needs Mode Virtual")); });
    EXPECT_EQ(run(), 0);
    EXPECT_EQ(window_.notified.size(), 1U);
}

// The rig's wheel not plugged in: "NOT CONNECTED: Moza R3" and Start greyed; Before Start says to plug it in, in red
// (no notice above the tab bar: the status and the list say it). Plugged in: Ready.
TEST_F(AppRun, RigNotConnected) {
    setUpRig();
    listGame();
    waitFor("NOT CONNECTED: Moza R3", false);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Devices: switch on or plug in Moza R3", true);
    check([](ScriptedWindow& w) {
        const auto* const toDo = w.find("TO DO Devices: switch on or plug in Moza R3");
        ASSERT_NE(toDo, nullptr) << w.dump();
        EXPECT_EQ(toDo->colour.r, style::kWarning.r);
        const auto* const done = w.find("ok    Rig: ", true);
        ASSERT_NE(done, nullptr) << w.dump();
        EXPECT_EQ(done->colour.r, style::kText.r);
        EXPECT_FALSE(w.has("not connected: switch it on", true)) << w.dump();
    });
    clickWhenShown("OK");
    once([this](ScriptedWindow&) {
        plugWheel();
        return uitest::nothing();
    });
    waitFor("Ready", false, 600);
    check([](ScriptedWindow& w) {
        const auto* const start = w.find("Start");
        ASSERT_NE(start, nullptr);
        EXPECT_EQ(start->colour.r, style::kDark.r);   // on the green of a Start that can run
    });
    EXPECT_EQ(run(), 0);
}

// Mode Shim with no game listed: Start greyed; the session bar says "no game" in amber; Before Start says to add one.
TEST_F(AppRun, ShimWithoutGames) {
    setUpRig();
    plugWheel();
    waitFor("G920 - Shim - no game", true);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Games: add at least one on the Games tab (G)", true);
    EXPECT_EQ(run(), 0);
    EXPECT_EQ(session_.runs, 0);
}

// Mode Virtual: the WheelBri Helper (or Administrator) decides whether Start can run; the session gets the virtual
// device. The wheel coming back mid-session keeps the tab that was showing.
TEST_F(AppRun, VirtualMode) {
    setUpRig("virtual");
    plugWheel();
    session_.mode = FakeSession::Mode::WheelBack;
    if (!ConfigPicker::virtualDeviceSupported(platform::windowsBuild())) {
        // Windows before 11 (Wine says Windows 10): no virtual device, so Mode stays Shim whatever app.ini says.
        waitFor("G920 - Shim", true);
        EXPECT_EQ(run(), 0);
        return;
    }
    if (!PrivilegedSteps::available()) {
        waitFor("HELPER NOT INSTALLED", false);
        EXPECT_EQ(run(), 0);
        return;
    }
    waitFor("Ready", false);
    check([](ScriptedWindow& w) { EXPECT_TRUE(w.has("G920 - Virtual - every game", true)) << w.dump(); });
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Running ");
    press("s");
    pumps(40);
    check([](ScriptedWindow& w) { EXPECT_TRUE(w.has("LIVE")) << w.dump(); });
    EXPECT_EQ(run(), 0);
    EXPECT_TRUE(session_.lastVirtual);
}

// Windows ending the session (logoff, shutdown): stopped, there is nothing to stop; running, the app stops the
// session and waits for it before Windows ends the process (the window is closed from then on).
TEST_F(AppRun, WindowsEndsTheSession) {
    setUpRig();
    listGame();
    plugWheel();
    waitFor("Ready", false);
    once([this](ScriptedWindow&) {
        uitest::lastProxy()->endSession();   // nothing runs: nothing to do... and the window is closed now
        return uitest::nothing();
    });
    EXPECT_EQ(run(), 0);
}

TEST_F(AppRun, WindowsEndsTheSessionWhileRunning) {
    setUpRig();
    listGame();
    plugWheel();
    waitFor("Ready", false);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Running ");
    once([](ScriptedWindow&) {
        uitest::lastProxy()->endSession();   // stops it and waits: it stops
        return uitest::nothing();
    });
    EXPECT_EQ(run(), 0);
    EXPECT_EQ(session_.stopsRequested, 1);
}

// A session that does not stop within the 4.5 s Windows leaves: logged; the app waits for it when it closes.
TEST_F(AppRun, WindowsEndsTheSessionSessionTooSlow) {
    setUpRig();
    listGame();
    plugWheel();
    session_.mode = FakeSession::Mode::IgnoresStop;
    waitFor("Ready", false);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Running ");
    once([this](ScriptedWindow&) {
        uitest::lastProxy()->endSession();
        session_.release = true;
        return uitest::nothing();
    });
    EXPECT_EQ(run(), 0);
}

// Something thrown on the window's thread: the app logs it, stops the session first and returns 1.
TEST_F(AppRun, WindowThreadThrows) {
    setUpRig();
    listGame();
    plugWheel();
    waitFor("Ready", false);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Running ");
    once([](ScriptedWindow&) -> ui::WindowInput { throw std::runtime_error("window broke"); });
    EXPECT_EQ(run(), 1);
    EXPECT_GE(session_.stopsRequested, 1);
    window_.script.clear();
    window_.timedOut = false;
    once([](ScriptedWindow&) -> ui::WindowInput { throw 7; });
    EXPECT_EQ(runApp(settings_), 1);
}

// A hidden (minimised) window draws nothing and waits for input only; shown again it draws.
TEST_F(AppRun, HiddenWindow) {
    waitFor(kNoRig);
    once([](ScriptedWindow& w) {
        w.shown = false;
        return uitest::nothing();
    });
    check([](ScriptedWindow& w) { w.frames = 0; });
    pumps(5);
    check([](ScriptedWindow& w) {
        EXPECT_EQ(w.frames, 0);
        w.shown = true;
    });
    pumps(3);
    check([](ScriptedWindow& w) { EXPECT_GT(w.frames, 0); });
    EXPECT_EQ(run(), 0);
}

// Clicks and keys reach the tab that shows: the Rig tab's (typing a name: letters are text, Escape stops it), the
// Mapping editor's (typing the rotation), the Logs tab's, the live view's; the wheel over the Rig tab.
TEST_F(AppRun, InputReachesEachTab) {
    setUpRig();
    listGame();
    const auto wheel = plugWheel();
    waitFor("Ready", false);
    press("r");
    waitFor("Moza R3", true);
    once([](ScriptedWindow& w) { return clickText(w, "Moza R3"); });   // its name field
    once([](ScriptedWindow&) {
        ui::WindowInput input = uitest::typed("X");
        input.keys = "x";
        return input;
    });
    once([](ScriptedWindow&) {
        ui::WindowInput input;
        input.keys = std::string(1, ui::kEscape);
        return input;
    });
    once([](ScriptedWindow&) {
        ui::WindowInput input;
        input.wheel = -2;
        input.pointer = {500, 200};
        input.pointerInside = true;
        return input;
    });
    press(std::string(1, ui::kEscape) + std::string(1, ui::kPageDown));
    // The Mapping editor: its rotation field typed, Escape.
    press("m");
    waitFor("STEERING ROTATION");
    once([](ScriptedWindow& w) {
        const auto* deg = w.find("deg");
        return deg != nullptr ? uitest::click(deg->x - 40, deg->y + 2) : uitest::nothing();
    });
    once([](ScriptedWindow&) { return uitest::typed("5"); });
    press(std::string(1, ui::kEscape));
    press(std::string(1, ui::kEscape));
    once([](ScriptedWindow& w) { return clickText(w, "STEERING ROTATION"); });
    // The Logs tab while stopped: a click on its page.
    press("l");
    waitFor("LIVE LOG");
    once([](ScriptedWindow& w) { return clickText(w, "WheelBri"); });
    // Running: the live view's own clicks (its page, its tab bar), the Rig tab's.
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Running ");
    once([](ScriptedWindow& w) { return clickText(w, "Live (S)"); });
    waitFor("LIVE", false);
    once([](ScriptedWindow&) { return uitest::click(100, 100); });
    press("r");
    waitFor("The running session's wheel", true);
    once([](ScriptedWindow&) { return uitest::click(100, 100); });
    once([](ScriptedWindow&) {
        ui::WindowInput input;
        input.wheel = 1;
        input.hwheel = 1;
        return input;
    });
    (void)wheel;
    EXPECT_EQ(run(), 0);
}

// Side by side (B): the window lays out two pages wide, the editor left and Live right (stopped: what it will be),
// kept in app.ini; the live view's own button turns it off again.
TEST_F(AppRun, SideBySide) {
    window_.setSize(1920, 748);   // the canvas a two-page design gets (the fake window does not fit it itself)
    setUpRig();
    listGame();
    plugWheel();
    waitFor("Ready", false);
    press("m");
    press("b");
    waitFor("Press Start: your mapping at work shows here", true);
    check([this](ScriptedWindow& w) {
        EXPECT_EQ(readAppSetting(settings_.stateFile, "side_by_side"), "true");
        ASSERT_NE(w.find("EDIT"), nullptr);
        EXPECT_LT(w.find("EDIT")->x, 960.0F);   // the editor, left
        EXPECT_GE(w.find("LIVE")->x, 960.0F);   // Live's place, right
    });
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Running ");
    press("m");   // Start shows the logs first
    waitFor("Target G920  Mode", true);
    check([](ScriptedWindow& w) {
        ASSERT_NE(w.find("EDIT"), nullptr);
        EXPECT_LT(w.find("EDIT")->x, 960.0F);
        ASSERT_NE(w.find("LIVE"), nullptr);
        EXPECT_GE(w.find("LIVE")->x, 960.0F);   // the live view, right
        EXPECT_TRUE(w.has("running: each change is in use at once", true)) << w.dump();
    });
    // The Logs tab takes the whole window; Mapping again: side by side.
    press("l");
    waitFor("LIVE LOG");
    press("m");
    waitFor("Target G920  Mode", true);
    clickWhenShown("Side by side (B)");
    waitFor("Target G920  Mode", true);
    check([this](ScriptedWindow&) { EXPECT_EQ(readAppSetting(settings_.stateFile, "side_by_side"), "false"); });
    once([this](ScriptedWindow&) { return clickStop(); });
    waitFor("Ready", false);
    EXPECT_EQ(run(), 0);
}

// Stopped, the Live tab is greyed: a click (or S) only says it needs Start, and the tab stays as it was.
TEST_F(AppRun, LiveTabNeedsStart) {
    setUpRig();
    plugWheel();
    waitFor("Ready", false);
    press("m");
    clickWhenShown("Live (S)");
    waitFor("Live shows a running session: press Start", true);
    check([](ScriptedWindow& w) {
        EXPECT_TRUE(w.has("EDIT")) << w.dump();
        EXPECT_FALSE(w.has("LIVE")) << w.dump();
        ASSERT_NE(w.find("Live (S)"), nullptr);
        EXPECT_EQ(w.find("Live (S)")->colour.r, style::kFaint.r);
    });
    EXPECT_EQ(run(), 0);
}

// Mode Shim on the Mapping tab: the games row's Games button goes to the Games tab (greyed while a session runs).
TEST_F(AppRun, GamesRowOpensTheGamesTab) {
    setUpRig();
    listGame();
    plugWheel();
    waitFor("Ready", false);
    press("m");
    clickWhenShown("Games");
    waitFor("GAMES");
    EXPECT_EQ(run(), 0);
}

// Hot swap while stopped: a folder with our shim follows the target (a toast says so, gone after 3 s); one whose
// shim settings cannot be written is told in a popup.
TEST_F(AppRun, HotSwap) {
    setUpRig();
    plugWheel();
    const auto good = listGame("Good");
    const auto bad = listGame("Bad");
    for (const auto& folder : {good, bad}) {
        std::filesystem::copy_file(root_ / "shim" / kShimDll, folder / kShimDll);
    }
    std::ofstream(good / kShimSettings) << "[shim]\ntarget_profile = g25\n";
    std::filesystem::create_directories(bad / kShimSettings);   // a folder where the settings go
    waitFor("Hot swap");
    // "Bad:" alone: the popup wraps after it when the folder's path is long (Windows' temp folder).
    check([](ScriptedWindow& w) { EXPECT_TRUE(w.has("Bad:", true)) << w.dump(); });
    clickWhenShown("OK");
    waitFor("Hot swap: 1 game presents the G920");
    then("toast gone", [](ScriptedWindow& w) { return !w.has("Hot swap: 1 game presents the G920"); }, nullptr, 400);
    EXPECT_EQ(run(), 0);
}

// Start refused: every listed game holds another mod's dinput8.dll that cannot be chained.
TEST_F(AppRun, StartRefusedForAModThatCannotBeChained) {
    setUpRig();
    plugWheel();
    const auto game = listGame();
    std::ofstream(game / kShimDll, std::ios::binary) << "someone else's dll";
    std::filesystem::create_directories(game / kShimChainDll);   // a folder where the chain would go
    waitFor("Ready", false);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Cannot start");
    check([](ScriptedWindow& w) { EXPECT_TRUE(w.has("another mod", true)) << w.dump(); });
    EXPECT_EQ(run(), 0);
    EXPECT_EQ(session_.runs, 0);
}

// What Start needs, each in turn: still looking for the devices (the listing takes its time), a mapping for another
// target, a rig the mapping takes nothing from, the system that cannot list game controllers (Start then tries).
TEST_F(AppRun, WhatStartNeeds) {
    setUpRig();
    listGame();
    plugWheel();
    FakeDevices::shared().listerHolds = true;
    waitFor("Looking for the rig...", false);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Devices: still looking for them (a moment)", true);
    clickWhenShown("OK");
    once([](ScriptedWindow&) {
        FakeDevices::shared().listerHolds = false;
        return uitest::nothing();
    });
    waitFor("Ready", false);
    once([](ScriptedWindow&) {
        FakeDevices::shared().clear();   // unplugged...
        return uitest::nothing();
    });
    waitFor("NOT CONNECTED: Moza R3", false, 600);
    once([](ScriptedWindow&) {
        FakeDevices::shared().listerUnavailable = true;   // ...and then the listing unavailable: Start tries
        return uitest::nothing();
    });
    waitFor("Ready", false, 600);
    EXPECT_EQ(run(), 0);
}

// The user's own mapping is for another target (the G25) than the one picked (G920): Start is greyed and says why.
TEST_F(AppRun, MappingForAnotherTarget) {
    setUpRig();
    listGame();
    plugWheel();
    std::filesystem::create_directories(settings_.userConfigFolder);
    std::filesystem::copy_file(root_ / "builtin" / "g25.ini", settings_.userConfigFolder / "my-g920.ini");
    waitFor("G920", true);
    pumps(30);
    check([](ScriptedWindow& w) { EXPECT_FALSE(w.has("Ready")) << w.dump(); });
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("TO DO Mapping: ", true);
    EXPECT_EQ(run(), 0);
}

// The tab a session opens on is the one showing when it showed itself: switched while it was still starting.
TEST_F(AppRun, TabChosenWhileStarting) {
    setUpRig();
    listGame();
    plugWheel();
    session_.mode = FakeSession::Mode::FirstHide;
    waitFor("Ready", false);
    for (const char* key : {"r", "g", "m"}) {
        once([this](ScriptedWindow&) {
            session_.hideDone = false;
            return clickStart();
        });
        waitFor("Hiding your wheel", true);
        clickWhenShown("OK");
        press(key);
        once([this](ScriptedWindow&) {
            session_.hideDone = true;
            return uitest::nothing();
        });
        waitFor("Running ");
        once([this](ScriptedWindow&) { return clickStop(); });
        waitFor("Ready", false);
    }
    EXPECT_EQ(run(), 0);
}

// A session whose target no profile has: it runs, the app shows the logs page (no live view).
TEST_F(AppRun, SessionWithUnknownTarget) {
    setUpRig();
    listGame();
    plugWheel();
    session_.mode = FakeSession::Mode::ProfileUnknown;
    waitFor("Ready", false);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Running ");
    check([](ScriptedWindow& w) { EXPECT_TRUE(w.has("LIVE LOG")) << w.dump(); });
    EXPECT_EQ(run(), 0);
}

// The session bar while a game runs that does not support the target (NFS Unbound, not for the G923): amber; read
// again every 2 s. Mode Virtual names the game that runs.
TEST_F(AppRun, SessionBarGameAdvice) {
    setUpRig();
    listGame();
    plugWheel();
    std::ofstream(settings_.stateFile, std::ios::app) << "target = g923\n";
    waitFor("Ready", false);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Running ");
    once([this](ScriptedWindow&) {
        session_.setFrame([](UiFrame& f) {
            f.gameId = "nfsunbound";
            f.gameStarts = 1;
        });
        return uitest::nothing();
    });
    waitFor("G923 - Shim - nfsunbound", true);
    check([](ScriptedWindow& w) {
        EXPECT_EQ(w.find("G923 - Shim - nfsunbound", true)->colour.r, style::kCaution.r) << w.dump();
    });
    pumps(140);   // more than 2 s: read again
    EXPECT_EQ(run(), 0);
}

// The shim heard from through its log (no force feedback yet): at Stop the app asks; "Keep them" with "Don't
// ask again" keeps them and is remembered (the next Stop keeps them without asking). A long folder name wraps; of 8
// folders 6 are listed and "and 2 more"; Keep (Escape's answer) is at the left of Delete.
TEST_F(AppRun, ShimsKeptAtStop) {
    setUpRig();
    for (int i = 1; i <= 7; ++i) listGame("Game " + std::to_string(i));
    const auto game = listGame(std::string(90, 'N'));   // a folder name longer than a popup line
    plugWheel();
    waitFor("Ready", false);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Running ");
    once([this](ScriptedWindow&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto shimLog = settings_.logFolder / "wheelbri-shim-2026-10-20-01.log";
        std::ofstream(shimLog) << "[2026-10-20 23:59:59.999] shim up\n";
        // Its write time cut to the whole second, as Windows' file times read through MinGW are: still "since Start".
        std::filesystem::last_write_time(
            shimLog, std::chrono::floor<std::chrono::seconds>(std::filesystem::last_write_time(shimLog)));
        return uitest::nothing();
    });
    pumps(150);   // the log is looked for every 2 s
    once([this](ScriptedWindow&) { return clickStop(); });
    waitFor("Delete the shims from the games' folders?");
    check([](ScriptedWindow& w) {
        EXPECT_TRUE(w.has("  and 2 more")) << w.dump();
        ASSERT_NE(w.find("Keep them"), nullptr);
        ASSERT_NE(w.find("Delete them"), nullptr);
        EXPECT_LT(w.find("Keep them")->x, w.find("Delete them")->x);
    });
    once([](ScriptedWindow& w) { return clickText(w, "Don't ask again", true); });
    once([](ScriptedWindow& w) { return clickText(w, "Don't ask again", true); });   // off again
    once([](ScriptedWindow& w) { return clickText(w, "Don't ask again", true); });   // and on
    once([](ScriptedWindow&) { return uitest::click(1, 1); });   // outside the buttons: nothing
    clickWhenShown("Keep them");
    then("asked", [](ScriptedWindow& w) { return !w.has("Delete the shims from the games' folders?"); });
    check([this, &game](ScriptedWindow&) {
        EXPECT_EQ(readAppSetting(settings_.stateFile, "shim_at_stop"), "keep");
        EXPECT_TRUE(std::filesystem::exists(game / kShimDll));
    });
    waitFor("Ready", false);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Running ");
    once([this](ScriptedWindow&) {
        std::ofstream(settings_.logFolder / "wheelbri-shim-2026-10-20-01.log", std::ios::app) << "more\n";
        return uitest::nothing();
    });
    pumps(150);
    once([this](ScriptedWindow&) { return clickStop(); });
    once([this](ScriptedWindow&) { return clickStop(); });   // already stopping: nothing more
    waitFor("Ready", false);
    check([&game](ScriptedWindow& w) {
        EXPECT_FALSE(w.has("Delete the shims from the games' folders?"));
        EXPECT_TRUE(std::filesystem::exists(game / kShimDll));
    });
    EXPECT_EQ(run(), 0);
}

// Deleting the shims fails (the DLL there is no longer ours): the popup says so and Yes tries again; Escape is No.
// Remembered as "delete", a failing delete is a notice.
TEST_F(AppRun, ShimsNotDeleted) {
    setUpRig();
    const auto game = listGame();
    plugWheel();
    session_.setFrame([](UiFrame& f) {
        FfbPacket packet;
        packet.kind = FfbPacket::Kind::KeepAlive;
        f.ffb.emplace(FakeSinkHolder::sink(), FfbSafetyConfig{});
        f.ffb->onPacket(packet, 1'000'000);
    });
    waitFor("Ready", false);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Running ");
    once([this](ScriptedWindow&) { return clickStop(); });
    waitFor("Delete the shims from the games' folders?");
    once([&game](ScriptedWindow&) {
        std::ofstream(game / kShimDll, std::ios::binary) << "replaced by another mod";
        std::filesystem::remove(game / kShimSettings);   // no longer ours: left alone
        return uitest::nothing();
    });
    once([](ScriptedWindow& w) { return clickText(w, "Don't ask again", true); });   // remembered: delete
    clickWhenShown("Delete them");
    waitFor("Not deleted:", true);
    clickWhenShown("Try again");   // the same answer, said as what it does now
    waitFor("Not deleted:", true);
    once([](ScriptedWindow& w) { return clickText(w, "Don't ask again", true); });   // unticked: No changes nothing
    press(std::string(1, ui::kEscape));
    then("closed", [](ScriptedWindow& w) { return !w.has("Not deleted:", true); });
    check([this](ScriptedWindow&) { EXPECT_EQ(readAppSetting(settings_.stateFile, "shim_at_stop"), "delete"); });
    // Remembered "delete", and it fails: a notice.
    once([&game](ScriptedWindow&) {
        std::filesystem::remove(game / kShimDll);   // Start puts ours in again
        return uitest::nothing();
    });
    waitFor("Ready", false);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Running ");
    once([this, &game](ScriptedWindow&) {
        std::ofstream(game / kShimDll, std::ios::binary) << "replaced by another mod";
        std::filesystem::remove(game / kShimSettings);
        return clickStop();
    });
    waitFor("Not deleted");
    clickWhenShown("OK");
    EXPECT_EQ(run(), 0);
}

// Two notices for one popup's place: the second waits until the first is answered. A notice with no title is not
// shown; one Windows shows itself needs no log line.
TEST_F(AppRun, NoticesWaitTheirTurn) {
    setUpRig();
    listGame();
    plugWheel();
    window_.notifies = true;
    waitFor("Ready", false);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Running ");
    once([this](ScriptedWindow&) {
        session_.setFrame([](UiFrame& f) {
            f.gameWayNotices = 1;   // no title: nothing
        });
        return uitest::nothing();
    });
    pumps(10);
    once([this](ScriptedWindow&) {
        session_.setFrame([](UiFrame& f) {
            f.gameWayTitle = "First";
            f.gameWayText = "one";
            f.gameWayNotices = 2;
        });
        return uitest::nothing();
    });
    waitFor("First");
    once([this](ScriptedWindow&) {
        session_.setFrame([](UiFrame& f) {
            f.gameWayTitle = "Second";
            f.gameWayNotices = 3;
        });
        return uitest::nothing();
    });
    pumps(20);
    check([](ScriptedWindow& w) { EXPECT_TRUE(w.has("First")) << w.dump(); });
    clickWhenShown("OK");
    waitFor("Second");
    clickWhenShown("OK");
    EXPECT_EQ(run(), 0);
}

// A profiled game starting while a name is typed in the Rig tab, or a popup is open: the Games tab does not open
// (the game is picked there anyway); with no rotation the status names the game only; frames not ready yet: nothing
// drawn for the session.
TEST_F(AppRun, GamesTabWaitsForTheUser) {
    setUpRig();
    listGame();
    plugWheel();
    session_.setFrame([](UiFrame& f) { f.ready = false; });
    waitFor("Ready", false);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Running ");
    press("m");
    pumps(5);
    once([this](ScriptedWindow&) {
        session_.setFrame([](UiFrame& f) { f.ready = true; });
        return uitest::nothing();
    });
    press("r");
    waitFor("The running session's wheel", true);
    once([](ScriptedWindow& w) { return clickText(w, "Moza R3"); });   // its name field: typing
    once([this](ScriptedWindow&) {
        session_.setFrame([](UiFrame& f) {
            f.gameId = "nfs2015";
            f.game = "NFS 2015";
            f.gameStarts = 1;
        });
        return uitest::nothing();
    });
    waitFor(", NFS 2015", true);
    check([](ScriptedWindow& w) { EXPECT_FALSE(w.has("GAMES")) << w.dump(); });
    press(std::string(1, ui::kEscape));
    EXPECT_EQ(run(), 0);
}

// The games the user added (games.ini) count as games (a folder of theirs is named after it); a game's own pedal axes
// from the mapping go into the shim's settings at Start; two pedals on one axis are not used (logged).
TEST_F(AppRun, UserGamesAndGameAxes) {
    setUpRig();
    plugWheel();
    std::filesystem::create_directories(userGamesFile().parent_path());
    std::ofstream(userGamesFile()) << "[game.mygame]\nlabel = My Game\nprogram = MyGame.exe\n";
    const auto mine = root_ / "games" / "Mine";
    std::filesystem::create_directories(mine);
    std::ofstream(mine / "MyGame.exe") << "MZ";
    {
        ShimGameList list;
        addShimGame(list, mine);
        list.selected = mine;
        ASSERT_TRUE(writeShimGames(shimGamesFile(), list));
    }
    const auto nfs = listGame();
    std::filesystem::create_directories(settings_.userConfigFolder);
    {
        std::ifstream in(root_ / "builtin" / "g920.ini");
        std::ofstream out(settings_.userConfigFolder / "my-g920.ini");
        out << in.rdbuf() << "\n[game_tuning]\nnfs2015.throttle_axis = y\nmygame.throttle_axis = rz\n"
                             "mygame.brake_axis = rz\n";
    }
    waitFor("Ready", false);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Running ");
    check([&nfs](ScriptedWindow&) {
        std::ifstream in(nfs / kShimSettings);
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        EXPECT_NE(text.find("throttle_axis = y"), std::string::npos) << text;
    });
    EXPECT_EQ(run(), 0);
}

// The Mapping tab's changes that cannot be saved (its folder is a file): Start uses the config as saved (logged).
// A click on Start while one of the tab's pickers is open is the picker's.
TEST_F(AppRun, UnsavedMappingAndOpenPicker) {
    setUpRig();
    listGame();
    plugWheel();
    std::filesystem::create_directories(settings_.userConfigFolder.parent_path());
    std::ofstream(settings_.userConfigFolder) << "a file where the folder goes";
    waitFor("Ready", false);
    press("m");
    clickWhenShown("Invert");
    pumps(5);
    // A source picker open: Start and the session bar are not clicked through it.
    once([](ScriptedWindow& w) {
        const auto* steering = w.find("Steering");
        return steering != nullptr ? uitest::click(steering->x + 150, steering->y + 2) : uitest::nothing();
    });
    pumps(3);
    once([this](ScriptedWindow&) { return clickStart(); });
    once([this](ScriptedWindow&) { return uitest::click(window_.width() / 2, barY()); });
    press(std::string(1, ui::kEscape));
    pumps(3);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Running ");
    EXPECT_EQ(run(), 0);
    std::filesystem::remove(settings_.userConfigFolder);
}

// The Mapping tab's secondary popup open, stopped: a button pressed on the rig is added to it (the app reads the
// devices as the Rig tab does meanwhile); typing goes to its find box, not to the tabs' keys.
TEST_F(AppRun, SecondaryPopupTakesTheRigsTouches) {
    setUpRig();
    {
        std::vector<std::string> errors;
        Rig rig = readRig(rigsFolder(), "My rig", errors).value_or(Rig{});
        rig.devices[0].buttons[1].label = "Horn";
        ASSERT_TRUE(saveRig(rigsFolder(), "My rig", rig));
    }
    listGame();
    const auto wheel = plugWheel();
    waitFor("Ready", false);
    press("m");
    then("the Mapping tab", [](ScriptedWindow& w) { return w.has("+") || w.has("Show"); });
    once([](ScriptedWindow& w) { return w.has("+") ? uitest::nothing() : clickText(w, "Show"); });
    clickWhenShown("+");
    waitFor("What else presses", true);
    once([](ScriptedWindow&) {   // a key arrives as a key and as typed text: the find box takes it, not the Games tab
        ui::WindowInput input = uitest::keys("g");
        input.text = "g";
        return input;
    });
    waitFor("g_", false);
    check([](ScriptedWindow& w) { EXPECT_TRUE(w.has("What else presses", true)) << w.dump(); });
    press(std::string(1, ui::kEscape));   // clears it
    // Pressed and let go every few frames until it is added (the devices are listed, opened and settled first).
    int frames = 0;
    then(
        "added", [](ScriptedWindow& w) { return w.has("Added 1 Horn.", false); },
        [&wheel, &frames](ScriptedWindow&) {
            const bool down = (++frames / 4) % 2 == 1;
            FakeDevices::shared().change(wheel, [down](FakeDevice& d) {
                d.raw.buttons.set(1, down);
            });
            return uitest::nothing();
        },
        1200);
    check([](ScriptedWindow& w) { EXPECT_TRUE(w.has("Has 1 of 8.", true)) << w.dump(); });
    EXPECT_EQ(run(), 0);
}

// The session ends by itself with an error code after Stop was pressed: just "stopped"; a rig of two devices is
// counted in Before Start; clicks between Start and Stop, below the tab buttons, on the session bar while a session
// starts.
TEST_F(AppRun, OddClicksAndEndings) {
    setUpRig();
    listGame();
    plugWheel();
    {
        std::vector<std::string> errors;
        Rig rig = readRig(rigsFolder(), "My rig", errors).value_or(Rig{});
        RigDevice pedals;
        pedals.name = "Pedals";
        pedals.id = {0x1234, 0x0001};
        pedals.axes[0].label = "Gas";
        rig.devices.push_back(pedals);
        ASSERT_TRUE(saveRig(rigsFolder(), "My rig", rig));
    }
    session_.exitCode = 2;
    waitFor("Ready", false);
    once([this](ScriptedWindow&) { return uitest::click(startX() + 68 + 2, barY()); });   // the gap: nothing
    once([this](ScriptedWindow&) { return uitest::click(20, window_.height() - 2); });     // below the tabs
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("Running ");
    once([this](ScriptedWindow&) { return clickStop(); });
    waitFor("Ready", false);
    check([](ScriptedWindow& w) { EXPECT_FALSE(w.has("Start failed (Logs)")) << w.dump(); });
    // Before Start with nothing missing is not shown (Start is not greyed); greyed by unplugging: two devices.
    once([](ScriptedWindow&) {
        FakeDevices::shared().clear();
        return uitest::nothing();
    });
    waitFor("NOT CONNECTED: ", true, 600);
    once([this](ScriptedWindow&) { return clickStart(); });
    waitFor("(2 devices)", true);
    EXPECT_EQ(run(), 0);
}

// Moving the mouse alone (in and out), holding the button: the window is drawn again at once.
TEST_F(AppRun, PointerAloneRedraws) {
    waitFor(kNoRig);
    once([](ScriptedWindow&) {
        ui::WindowInput input;
        input.pointer = {10, 10};
        return input;
    });
    once([](ScriptedWindow&) {
        ui::WindowInput input;
        input.pointer = {10, 10};
        input.pointerInside = true;
        return input;
    });
    once([](ScriptedWindow&) {
        ui::WindowInput input;
        input.pointer = {10, 10};
        input.pointerInside = true;
        input.leftDown = true;
        return input;
    });
    once([](ScriptedWindow&) {
        ui::WindowInput input;
        input.pointer = {10, 12};
        input.pointerInside = true;
        return input;
    });
    once([](ScriptedWindow&) { return uitest::typed("x"); });
    pumps(3);
    EXPECT_EQ(run(), 0);
}

// A Games tab problem (forgetting a folder that still holds WheelBri's shim) is a notice.
TEST_F(AppRun, GamesTabProblemIsANotice) {
    setUpRig();
    plugWheel();
    const auto game = listGame();
    std::filesystem::copy_file(root_ / "shim" / kShimDll, game / kShimDll);
    std::ofstream(game / kShimSettings) << "[shim]\ntarget_profile = g920\n";
    waitFor("Ready", false);
    press("g");
    clickWhenShown("Forget folder");
    waitFor("Games");
    check([](ScriptedWindow& w) { EXPECT_TRUE(w.has("is off the list", true)) << w.dump(); });
    EXPECT_EQ(run(), 0);
}

// --- the command line service's --ui window -----------------------------------------------------------------------

class SessionWindowRun : public ::testing::Test {
protected:
    void SetUp() override {
        folders_ = std::make_unique<UserFolders>(
            "sessionwindow-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        FakeDevices::shared().clear();
        WindowFactory::shared().make = uitest::proxyOf(window_);
        info_.sourceName = "Moza R3";
        info_.vendorId = 0x346e;
        info_.productId = 0x0005;
        info_.axes = 8;
        info_.buttons = 16;
        info_.targetProfile = "g920";
        frame_.ready = true;
        window_.closeAtEnd = false;
    }
    void TearDown() override {
        WindowFactory::shared().make = nullptr;
        folders_.reset();
    }
    // One frame with `input` as what the window was given.
    bool frame(UiHost& host, ui::WindowInput input) {
        window_.script.push_back({nullptr, [input](ScriptedWindow&) { return input; }, 1, "frame"});
        return host.frame(frame_);
    }

    std::unique_ptr<UserFolders> folders_;
    ScriptedWindow window_;
    SessionInfo info_;
    UiFrame frame_;
};

// The window opens when the session starts and draws its frames; keys switch tabs; Escape or the close button end
// it (frame says stop); a window that cannot open ends the session.
TEST_F(SessionWindowRun, OpensDrawsAndCloses) {
    {
        WindowFactory::shared().make = nullptr;
        auto host = makeSessionWindow("config.ini");
        EXPECT_FALSE(host->sessionStarted(info_));
        WindowFactory::shared().make = uitest::proxyOf(window_);
    }
    auto host = makeSessionWindow("config.ini");
    ASSERT_TRUE(host->sessionStarted(info_));
    EXPECT_TRUE(frame(*host, uitest::nothing()));
    EXPECT_TRUE(window_.has("Moza R3  ->  ", true)) << window_.dump();
    EXPECT_TRUE(frame(*host, uitest::keys("l")));
    EXPECT_TRUE(window_.has("LIVE LOG")) << window_.dump();
    ui::WindowInput scroll;
    scroll.wheel = 1;
    scroll.hwheel = -1;
    EXPECT_TRUE(frame(*host, scroll));
    // Not ready yet, or hidden: nothing drawn.
    const int drawn = window_.frames;
    frame_.ready = false;
    EXPECT_TRUE(frame(*host, uitest::nothing()));
    frame_.ready = true;
    window_.shown = false;
    EXPECT_TRUE(frame(*host, uitest::nothing()));
    EXPECT_EQ(window_.frames, drawn);
    window_.shown = true;
    EXPECT_FALSE(frame(*host, uitest::keys(std::string(1, ui::kEscape))));
    ui::WindowInput close;
    close.close = true;
    EXPECT_FALSE(frame(*host, close));
}

// The Rig tab in the --ui window: it reads the session's wheel; a click above the tab bar is the tab's, on it the
// bar's; while a name is typed letters are text, Escape stops typing; the wheel scrolls it.
TEST_F(SessionWindowRun, RigTab) {
    auto host = makeSessionWindow("config.ini");
    ASSERT_TRUE(host->sessionStarted(info_));
    EXPECT_TRUE(frame(*host, uitest::keys("r")));
    for (int i = 0; i < 22; ++i) EXPECT_TRUE(frame(*host, uitest::nothing()));
    EXPECT_TRUE(window_.has("The running session's wheel (stop to see every device).")) << window_.dump();
    frame_.raw.buttons = SourceButtons::fromBits(1U << 2);   // pressed: its popup opens, ready to type
    EXPECT_TRUE(frame(*host, uitest::nothing()));
    EXPECT_TRUE(window_.has("Button 2 on Moza R3")) << window_.dump();
    ui::WindowInput typing = uitest::typed("Hornm");
    typing.keys = "m";   // the same key press: not a tab switch while typing
    EXPECT_TRUE(frame(*host, typing));
    EXPECT_TRUE(window_.has("Button 2 on Moza R3"));
    EXPECT_TRUE(frame(*host, uitest::keys(std::string(1, ui::kEscape))));   // stops typing, closes the popup
    EXPECT_FALSE(window_.has("Button 2 on Moza R3"));
    const auto at = window_.clickOn("Detect");
    ASSERT_TRUE(at.has_value());
    EXPECT_TRUE(frame(*host, uitest::click(at->x, at->y)));
    ui::WindowInput wheel;
    wheel.wheel = -1;
    EXPECT_TRUE(frame(*host, wheel));
    const auto tab = window_.clickOn("Mapping (M)");
    ASSERT_TRUE(tab.has_value());
    EXPECT_TRUE(frame(*host, uitest::click(tab->x, tab->y)));
    EXPECT_TRUE(window_.has("LIVE")) << window_.dump();
}

// The session carries on after the wheel came back: the same window, a new view; a target no profile has ends it.
// Windows ending the session (logoff): frame says stop, and the window is not pumped again.
TEST_F(SessionWindowRun, WheelBackUnknownTargetAndLogoff) {
    auto host = makeSessionWindow("config.ini");
    ASSERT_TRUE(host->sessionStarted(info_));
    EXPECT_TRUE(frame(*host, uitest::nothing()));
    const std::size_t opened = WindowFactory::shared().titles.size();
    ASSERT_TRUE(host->sessionStarted(info_));
    EXPECT_EQ(WindowFactory::shared().titles.size(), opened);   // the same window
    EXPECT_TRUE(frame(*host, uitest::nothing()));
    SessionInfo unknown = info_;
    unknown.targetProfile = "g999";
    EXPECT_FALSE(host->sessionStarted(unknown));
    ASSERT_TRUE(host->sessionStarted(info_));
    ASSERT_NE(uitest::lastProxy(), nullptr);
    uitest::lastProxy()->queryEndSession();
    uitest::lastProxy()->queryEndSession();   // asked again: said once
    const int pumpsBefore = window_.pumps;
    EXPECT_FALSE(host->frame(frame_));
    EXPECT_EQ(window_.pumps, pumpsBefore);
}

}  // namespace
}  // namespace wheelbri::service
