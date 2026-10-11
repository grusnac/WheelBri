#include "App.hpp"
#include "PrivilegedSteps.hpp"

#include "ConfigPicker.hpp"
#include "DeviceWatch.hpp"
#include "MappingEditView.hpp"
#include "Platform.hpp"
#include "ServiceLogging.hpp"
#include "ShimGamesView.hpp"
#include "UiWindow.hpp"
#include "Version.hpp"
#include "WheelSetupView.hpp"

#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/FileText.hpp"
#include "wheelbri/IniDocument.hpp"
#include "wheelbri/Rig.hpp"
#include "wheelbri/StartNeeds.hpp"
#include "wheelbri/WakePlanner.hpp"
#include "wheelbri/Logging.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <functional>
#include <optional>
#include <thread>

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.ui");
    return logger;
}

// The same colours as the live view (MappingView.cpp).
constexpr ui::Colour kBackground{22, 24, 28};
constexpr ui::Colour kPanel{44, 48, 56};
constexpr ui::Colour kText{222, 226, 232};
constexpr ui::Colour kDim{120, 126, 136};
constexpr ui::Colour kHeading{120, 190, 255};
constexpr ui::Colour kActive{64, 190, 100};
constexpr ui::Colour kWarning{214, 72, 64};
constexpr ui::Colour kPopup{36, 40, 48};
constexpr ui::Colour kCaution{206, 164, 58};
constexpr ui::Colour kDark{16, 18, 20};

// Window layout, in logical pixels: the panel (the tabs) fills the window. Every tab has the controls at the right end
// of the tab bar: the status (ready, what is missing, running...), then Start and Stop, as tall as the tab buttons. The
// shim's games row is on the Mapping tab only, in its top row.
constexpr float kWidth = MappingView::kWidth;
constexpr float kViewTop = 0;
constexpr float kHeight = MappingView::kHeight;
constexpr float kControlsY = 0;            // the top row: the shim's games
constexpr float kControlsHeight = 16;
// The tab bar at the bottom of the panel (MappingView's), in the panel's own coordinates.
constexpr float kTabBarTop = MappingView::kHeight - MappingView::kTabBarHeight;
constexpr float kButtonsY = kTabBarTop + 4;   // Start and Stop, level with the tab buttons
constexpr float kButtonWidth = 68;
constexpr float kStopX = kWidth - 4 - kButtonWidth;
constexpr float kStartX = kStopX - 4 - kButtonWidth;
constexpr float kGamesX = 220;              // the shim's games ("Game < name > Add"), left of the status: 360 wide
constexpr std::size_t kHeaderChars = 50;    // the live view's title on the Mapping tab...
constexpr std::size_t kHeaderCharsShim = 26;   // ...and with the games row next to it
// The app's popups (a question or a notice), over everything.
constexpr float kPopupX = 180;
constexpr float kPopupY = 200;
constexpr float kPopupW = 600;
constexpr std::size_t kPopupChars = 70;
constexpr int kWindowWidth = static_cast<int>(kWidth * 1.3F);  // 1.3x the canvas at 100 % (smaller to fit the screen)
constexpr int kWindowHeight = static_cast<int>(kHeight * 1.3F);

// The running session's wheel as the Wheel setup tab wants it.
WheelSetupView::SessionWheel sessionWheel(const SessionInfo& info, const UiFrame& frame) {
    WheelSetupView::SessionWheel wheel;
    wheel.info.name = info.sourceName;
    wheel.info.vendorId = info.vendorId;
    wheel.info.productId = info.productId;
    wheel.axes = info.axes;
    wheel.buttons = info.buttons;
    wheel.raw = frame.raw;
    return wheel;
}

void draw(const FfbDisplay& ffb, MappingView& view, const UiFrame& frame) {
    view.setTelemetry(frame.telemetry);
    view.setOldFiles(frame.oldFiles, frame.oldFilesFailed);
    view.draw(frame.inputs, frame.state, frame.report, ffb);
}

FfbDisplay ffbDisplay(const SessionInfo& info, const UiFrame& frame) {
    FfbDisplay ffb;
    ffb.controller = frame.ffb ? &*frame.ffb : nullptr;
    ffb.status = info.ffbStatus;
    ffb.invert = info.ffbInvert;
    ffb.nowNs = platform::nowNs();
    return ffb;
}

// --- the command line service's --ui window ------------------------------------------------------------

class SessionWindow final : public UiHost {
public:
    explicit SessionWindow(std::string configPath) : configPath_(std::move(configPath)) {}

    bool sessionStarted(const SessionInfo& info) override {
        // Called again when the session carries on after the wheel came back: keep the window, rebuild the view.
        if (window_ == nullptr) {
            std::string error;
            window_ = ui::openWindow("WheelBri: mapping", MappingView::kWidth, MappingView::kHeight,
                                     static_cast<int>(MappingView::kWidth * 1.3F),
                                     static_cast<int>(MappingView::kHeight * 1.3F), error);
            if (window_ == nullptr) {
                WE_LOG_ERROR(log(), "cannot open the mapping window: {}", error);
                return false;
            }
            WE_LOG_INFO(log(), "mapping window open ({})", window_->describe());
            // Logoff or shutdown: Windows ends the process soon after WM_ENDSESSION, and this window is pumped by the
            // session's own thread, which cannot clean up inside that message. So stop at the question before it:
            // frame() says stop and the session undoes everything before the window is pumped again.
            window_->onQueryEndSession([this] {
                if (!endingSession_) {
                    WE_LOG_WARN(log(), "Windows is ending the session (logoff or shutdown): stopping the service");
                }
                endingSession_ = true;
            });
            setup_ = std::make_unique<WheelSetupView>(*window_,
                                                      WheelSetupView::Settings{rigsFolder(), deviceLabelsFolder(),
                                                                               appStateFile()});
        }
        view_.reset();   // refers to the old profile
        info_ = info;
        profile_ = makeProfile(info.targetProfile);
        if (profile_ == nullptr) {
            return false;
        }
        view_ = std::make_unique<MappingView>(*window_, info.sourceName, info.axes, info.buttons, *profile_,
                                              info.mapper);
        view_->setFiles(info.files);
        view_->setDevices(info.devices, info.devicesWarning);
        view_->setAccessories(info.accessories);
        return true;
    }

    bool frame(const UiFrame& frame) override {
        if (endingSession_) return false;   // no more pumping: WM_ENDSESSION waits until the clean-up is done
        const ui::WindowInput input = window_->pump();
        if (input.close || endingSession_) {
            return false;
        }
        const bool setup = view_->tab() == MappingView::Tab::WheelSetup;
        if (setup && setup_->editingText()) {
            setup_->onText(input.text);   // typing a name: letters are not shortcuts, Escape stops typing
            if (input.keys.find(ui::kEscape) != std::string::npos) setup_->onKey(ui::kEscape);
        } else {
            for (const char key : input.keys) {
                if (key == ui::kEscape) {
                    return false;
                }
                view_->onKey(key);
            }
        }
        for (const ui::Click& click : input.clicks) {
            if (setup && click.y < kTabBarTop) {
                setup_->onClick(click.x, click.y);
            } else {
                view_->onClick(click.x, click.y);
            }
        }
        if (input.wheel != 0) {
            view_->onScroll(input.wheel);
        }
        if (input.hwheel != 0) view_->onHScroll(input.hwheel);
        view_->onPointer(input.pointer.x, input.pointer.y, input.leftDown);   // the live log's scroll bars
        if (frame.ready && view_->tab() == MappingView::Tab::WheelSetup) {
            const auto wheel = sessionWheel(info_, frame);
            setup_->update(&wheel);
        }
        // Nothing to show: drawing a hidden window can block (e.g. while a game switches display mode).
        if (frame.ready && window_->visible()) {
            window_->begin(kBackground);
            if (view_->tab() == MappingView::Tab::WheelSetup) {
                setup_->draw();
                view_->drawTabBar();
            } else {
                draw(ffbDisplay(info_, frame), *view_, frame);
            }
            window_->present();
        }
        return true;
    }

private:
    std::string configPath_;
    bool endingSession_ = false;   // logoff or shutdown asked (Windows)
    std::unique_ptr<ui::UiWindow> window_;
    SessionInfo info_;
    std::unique_ptr<IDeviceProfile> profile_;
    std::unique_ptr<MappingView> view_;
    std::unique_ptr<WheelSetupView> setup_;
};

// --- the app ------------------------------------------------------------------------------------------

// The session's side: it hands over what to show; the window's thread picks it up.
class AppHost final : public UiHost {
public:
    bool sessionStarted(const SessionInfo& info) override {
        const std::lock_guard lock(mutex_);
        info_ = info;
        step_.clear();
        ++generation_;
        return true;
    }

    void step(const std::string& what) override {
        const std::lock_guard lock(mutex_);
        step_ = what;
    }

    [[nodiscard]] std::string currentStep() {
        const std::lock_guard lock(mutex_);
        return step_;
    }

    bool frame(const UiFrame& frame) override {
        // A new copy each time (it cannot be assigned), made outside the lock; a pointer rather than
        // optional::emplace, on which GCC 16 warns about a string "maybe used uninitialized" (a false positive).
        auto copy = std::make_unique<UiFrame>(frame);
        const std::lock_guard lock(mutex_);
        frame_ = std::move(copy);
        return true;
    }

    void clear() {
        const std::lock_guard lock(mutex_);
        info_.reset();
        frame_.reset();
        step_.clear();
    }

    // The session's info when it changed since `seen` (a new session started).
    std::optional<SessionInfo> newSession(std::uint64_t& seen) {
        const std::lock_guard lock(mutex_);
        if (generation_ == seen || !info_) {
            return std::nullopt;
        }
        seen = generation_;
        return info_;
    }

    std::optional<UiFrame> latest() {
        const std::lock_guard lock(mutex_);
        if (!frame_) {
            return std::nullopt;
        }
        return std::optional<UiFrame>(std::in_place, *frame_);
    }

private:
    std::mutex mutex_;
    std::optional<SessionInfo> info_;
    std::uint64_t generation_ = 0;
    std::unique_ptr<UiFrame> frame_;
    std::string step_;
};

struct Button {
    float x;
    float y;
    float w;
    float h;
    const char* label;
    bool enabled;

    [[nodiscard]] bool hit(const ui::Click& click) const {
        return enabled && click.x >= x && click.x < x + w && click.y >= y && click.y < y + h;
    }

    void draw(ui::UiWindow& window, const bool highlight) const {
        window.fill(x, y, w, h, enabled ? (highlight ? kActive : kPanel) : kDark);
        window.outline(x, y, w, h, enabled ? kHeading : kPanel);
        const float textWidth = static_cast<float>(std::string_view(label).size()) * ui::kCharWidth;
        window.text(x + (w - textWidth) / 2, y + (h - ui::kCharHeight) / 2, label,
                    enabled ? (highlight ? kDark : kText) : kDim);
    }
};

std::string clock(const std::uint64_t seconds) {
    char text[32];
    std::snprintf(text, sizeof(text), "%llu:%02llu:%02llu", static_cast<unsigned long long>(seconds / 3600),
                  static_cast<unsigned long long>(seconds / 60 % 60), static_cast<unsigned long long>(seconds % 60));
    return text;
}

std::string fit(std::string text, const std::size_t chars) {
    if (text.size() > chars) {
        text = text.substr(0, chars - 3) + "...";
    }
    return text;
}

class App {
public:
    explicit App(const AppSettings& settings) : settings_(settings) {}

    // Never leave a session running (or its thread joinable, which would std::terminate) behind the app.
    ~App() { stopAndWaitForEver(); }

    // As stopAndWait, but for as long as it takes (logging every 30 s): a session thread still running when the App
    // goes would end the process with std::terminate, before the session gave the wheel back and removed the devices.
    void stopAndWaitForEver() {
        try {
            while (!stopAndWait(std::chrono::seconds(30))) {
                WE_LOG_WARN(log(), "still waiting for the service to stop");
            }
        } catch (...) {
            // Nothing can be reported from here; never throw out of the destructor.
        }
    }

    // Asks the session to stop and waits up to `limit` for its clean-up; true if it ended.
    bool stopAndWait(const std::chrono::milliseconds limit) {
        if (!running()) return true;
        settings_.requestStop();
        const auto until = std::chrono::steady_clock::now() + limit;
        while (!sessionDone_ && std::chrono::steady_clock::now() < until) {
            platform::sleepMs(20);
        }
        if (!sessionDone_) {
            WE_LOG_ERROR(log(), "the service did not stop within {} s", limit.count() / 1000);
            return false;
        }
        session_.join();
        return true;
    }

    int run() {
        try {
            return runWindow();
        } catch (const std::exception& e) {
            WE_LOG_CRITICAL(log(), "app window: stopped on an error: {}", e.what());
        } catch (...) {
            WE_LOG_CRITICAL(log(), "app window: stopped on an unknown error");
        }
        stopAndWaitForEver();   // the session undoes everything before the app ends
        return 1;
    }

private:
    int runWindow() {
        std::string error;
        window_ = ui::openWindow("WheelBri", static_cast<int>(kWidth), static_cast<int>(kHeight), kWindowWidth,
                                 kWindowHeight, error);
        if (window_ == nullptr) {
            WE_LOG_CRITICAL(log(), "cannot open the window: {}", error);
            return 1;
        }
        WE_LOG_INFO(log(), "app window open ({}); press Start", window_->describe());
        picker_ = std::make_unique<ConfigPicker>(
            *window_, ConfigPicker::Settings{settings_.builtInConfigFolder, settings_.userConfigFolder,
                                             settings_.stateFile});
        setup_ = std::make_unique<WheelSetupView>(
            *window_, WheelSetupView::Settings{rigsFolder(), settings_.devicesFolder, settings_.stateFile});
        // The first time (no rig yet) the devices are defined first; then the mapping is where to go.
        idleTab_ = WheelSetupView::anyRig(rigsFolder()) ? IdleTab::Mapping : IdleTab::Setup;
        mapping_ = std::make_unique<MappingEditView>(*window_, rigsFolder(), settings_.stateFile, *picker_);
        games_ = std::make_unique<ShimGamesView>(
            *window_, ShimGamesView::Settings{shimGamesFile(), settings_.shimDll, settings_.shimSettings});
        // The Logs tab while no session shows (stopped, starting): a view of its own, used for that page only.
        logsProfile_ = makeProfile("g920");
        logs_ = std::make_unique<MappingView>(*window_, std::string(), 0, 0, *logsProfile_, MapperConfig{});
        FilesDisplay files;
        files.logFolder = settings_.logFolder;
        logs_->setFiles(files);
        logs_->setTab(MappingView::Tab::Logs);   // it only ever shows its Logs page (scrolling and drags check the tab)
        // Logoff or shutdown: Windows ends the process right after this returns (about 5 s).
        window_->onEndSession([this] {
            if (!running()) return;
            WE_LOG_WARN(log(), "Windows is ending the session (logoff or shutdown): stopping the service first");
            state_ = State::Stopping;
            stoppingSince_ = platform::nowNs();
            if (stopAndWait(std::chrono::milliseconds(4500))) {
                WE_LOG_INFO(log(), "the service stopped before Windows ended the session");
            }
        });

        while (true) {
            const ui::WindowInput input = window_->pump();
            if (input.close && !quitting_) {
                quitting_ = true;
                if (running()) {
                    WE_LOG_INFO(log(), "window closed: stopping the service first");
                    stop();
                }
            }
            const bool setup = showingSetup();
            const bool editingMapping = showingMappingEditor();
            for (const ui::Click& click : input.clicks) {
                // A popup of the app's takes every click until it is answered.
                if (popup_) {
                    onPopupClick(click);
                    continue;
                }
                // So does the shim's games panel.
                if (games_->panelOpen()) {
                    games_->onPanelClick(click.x, click.y);
                    continue;
                }
                // Start and Stop, at the tab bar's right end on every tab; not over the editor's popups.
                if (!(editingMapping && mapping_->popupOpen()) &&
                    click.y >= kButtonsY + kViewTop && click.y < kButtonsY + kViewTop + MappingView::kTabHeight &&
                    click.x >= kStartX) {
                    if (startButton().hit(click)) start();
                    if (stopButton().hit(click)) stop();
                    continue;
                }
                // The shim's games, left of the status (only with the target G920 in-game shim).
                if (showingMapping() && shimTarget() && !(editingMapping && mapping_->popupOpen()) &&
                    click.y < kControlsY + kControlsHeight && click.x >= kGamesX) {
                    games_->onClick(click.x, click.y);
                    continue;
                }
                // The panel, in its own coordinates; the tab bar at its bottom.
                const float y = click.y - kViewTop;
                if (y >= kTabBarTop) {
                    if (view_ != nullptr) {
                        view_->onClick(click.x, y);
                    } else if (y >= kButtonsY && y < kButtonsY + MappingView::kTabHeight) {
                        for (const IdleTabButton& button : idleTabButtons()) {
                            if (click.x >= button.x && click.x < button.x + button.w) idleTab_ = button.tab;
                        }
                    }
                } else if (setup) {
                    setup_->onClick(click.x, y);
                } else if (editingMapping) {
                    mapping_->onClick(click.x, y);
                } else if (view_ != nullptr) {
                    view_->onClick(click.x, y);
                } else if (idleTab_ == IdleTab::Logs) {
                    logs_->onClick(click.x, y);
                }
            }
            if (popup_) {
                if (input.keys.find(ui::kEscape) != std::string::npos) answerPopup(false);   // Escape = the "no" answer
            } else if (games_->panelOpen()) {
                if (input.keys.find(ui::kEscape) != std::string::npos) games_->closePanel();
            } else if (setup && setup_->editingText()) {
                setup_->onText(input.text);   // typing a name: letters are not shortcuts, Escape stops typing
                if (input.keys.find(ui::kEscape) != std::string::npos) setup_->onKey(ui::kEscape);
            } else if (editingMapping && mapping_->editingText()) {
                mapping_->onText(input.text);   // typing a number: the same
                if (input.keys.find(ui::kEscape) != std::string::npos) mapping_->onKey(ui::kEscape);
            } else {
                for (const char key : input.keys) {
                    if (key == ui::kEscape) {
                        if (setup) setup_->onKey(key);
                        if (editingMapping) mapping_->onKey(key);
                    } else if (view_ != nullptr) {
                        view_->onKey(key);
                    } else if (key == 'r' || key == 'm' || key == 'l') {
                        idleTab_ = key == 'r' ? IdleTab::Setup : key == 'm' ? IdleTab::Mapping : IdleTab::Logs;
                    } else if (idleTab_ == IdleTab::Logs) {
                        logs_->onKey(key);   // scrolling, O / D open the folders
                    }
                }
            }
            if (editingMapping) mapping_->onPointer(input.pointer.x, input.pointer.y - kViewTop, input.leftDown);
            if (view_ != nullptr && input.wheel != 0) view_->onScroll(input.wheel);
            if (view_ == nullptr && idleTab_ == IdleTab::Logs && input.wheel != 0) logs_->onScroll(input.wheel);
            // The live log's scroll bars: dragged with the mouse, and the sideways wheel.
            if (MappingView* const logsView = view_ != nullptr ? view_.get()
                                              : idleTab_ == IdleTab::Logs ? logs_.get()
                                                                          : nullptr) {
                if (input.hwheel != 0) logsView->onHScroll(input.hwheel);
                logsView->onPointer(input.pointer.x, input.pointer.y - kViewTop, input.leftDown);
            }
            collectSession();
            showFirstHide();
            updateStartNeeds();
            logs_->setShimMode(shimTarget());   // the Logs tab while stopped follows the target
            updateSetup();
            mapping_->setLocked(state_ != State::Stopped);   // the way and the mapping stay as Start found them
            mapping_->setRigUnsaved(setup_->dirty());   // Auto-map reads the saved rig: it says so when that is behind
            if (showingMappingEditor()) mapping_->update();
            if (std::string problem; games_->takeProblem(problem)) notice("Games with the shim", problem);
            if (quitting_ && !running()) {
                break;
            }
            // The window is drawn kWindowFramesPerSecond (60) times a second, on a schedule that does not drift (a
            // sleep that wakes late is not added to the next one), and at once after input; only what changed reaches
            // the screen (UiWindow keeps the last frame). Input is answered within one frame (~17 ms).
            const std::uint64_t now = platform::nowNs();
            const bool touched = !input.keys.empty() || !input.text.empty() || !input.clicks.empty() ||
                                 input.wheel != 0 || input.hwheel != 0 || input.leftDown ||
                                 input.pointer.x != lastPointer_.x || input.pointer.y != lastPointer_.y;
            lastPointer_ = input.pointer;
            const bool visible = window_->visible();
            if (visible && (touched || now >= nextDrawNs_)) {
                drawWindow();
                // On schedule: the next frame one period after this one was due; held up (or after input): from now.
                nextDrawNs_ = !touched && now - nextDrawNs_ < kDrawEveryNs ? nextDrawNs_ + kDrawEveryNs
                                                                            : now + kDrawEveryNs;
            }
            const std::uint64_t after = platform::nowNs();
            platform::sleepNs(!visible ? kHiddenWaitNs
                                       : nextDrawNs_ > after ? std::min(nextDrawNs_ - after, kDrawEveryNs)
                                                             : platform::kNsPerMs);
        }
        return 0;
    }

private:
    enum class State { Stopped, Starting, Running, Stopping };
    enum class IdleTab { Mapping, Setup, Logs };

    static constexpr std::uint64_t kDrawEveryNs = platform::kNsPerSecond / kWindowFramesPerSecond;
    static constexpr std::uint64_t kHiddenWaitNs = 16 * platform::kNsPerMs;   // the window minimised: input only
    std::uint64_t nextDrawNs_ = 0;
    ui::Click lastPointer_{};

    [[nodiscard]] bool running() const { return session_.joinable(); }

    // Whether the Wheel setup tab shows: the live view's tab while a session runs, the idle page's otherwise.
    [[nodiscard]] bool showingSetup() const {
        return view_ != nullptr ? view_->tab() == MappingView::Tab::WheelSetup : idleTab_ == IdleTab::Setup;
    }
    // The Mapping tab's editor: on the idle page (no session shows; while running it is the live view).
    [[nodiscard]] bool showingMappingEditor() const { return view_ == nullptr && idleTab_ == IdleTab::Mapping; }
    // Either Mapping tab (the editor while stopped, the live view's while running): the only place with the shim's games
    // row (Start, Stop and the status are on every tab).
    [[nodiscard]] bool showingMapping() const {
        return view_ != nullptr ? view_->tab() == MappingView::Tab::Mapping : idleTab_ == IdleTab::Mapping;
    }

    // The tab reads the running session's wheel, or (stopped) the wheel picked in it; nothing while starting or
    // stopping, when the session may be opening or closing the wheel.
    void updateSetup() {
        if (!showingSetup()) {
            if (state_ == State::Stopped) setup_->releaseWheel();   // not kept open while nobody looks
            return;
        }
        if (state_ == State::Stopped) {
            setup_->update(nullptr);
        } else if (info_) {
            if (const auto frame = host_.latest(); frame && frame->ready) {
                const auto wheel = sessionWheel(*info_, *frame);
                setup_->update(&wheel);
            }
        }
    }

    // The app's own tab buttons while no session shows (Mapping, the editor; Rig; Logs), placed and sized as the live
    // view places its first three (MappingView::tabWidth), so nothing moves when a session starts.
    struct IdleTabButton {
        IdleTab tab;
        const char* label;
        float x;
        float w;
    };
    [[nodiscard]] static std::array<IdleTabButton, 3> idleTabButtons() {
        std::array<IdleTabButton, 3> buttons{{{IdleTab::Mapping, "Mapping (M)", 0, 0},
                                              {IdleTab::Setup, "Rig (R)", 0, 0},
                                              {IdleTab::Logs, "Logs (L)", 0, 0}}};
        float x = MappingView::kTabLeft;
        for (IdleTabButton& button : buttons) {
            button.x = x;
            button.w = MappingView::tabWidth(button.label);
            x += button.w + MappingView::kTabGap;
        }
        return buttons;
    }

    // The tab bar while no session shows, where the live view has its own.
    void drawIdleTabs() {
        window_->fill(0, kTabBarTop, kWidth, 1, kPanel);
        for (const IdleTabButton& button : idleTabButtons()) {
            const bool active = button.tab == idleTab_;
            window_->fill(button.x, kButtonsY, button.w, MappingView::kTabHeight, active ? kHeading : kPanel);
            window_->text(button.x + MappingView::kTabPadding, kTabBarTop + 10, button.label, active ? kDark : kText);
        }
    }

    // The target is G920 in-game shim: the games row shows, and Start needs a game.
    [[nodiscard]] bool shimTarget() const { return picker_ != nullptr && !picker_->virtualDevice(); }

    Button startButton() const {
        return {kStartX, kButtonsY, kButtonWidth, MappingView::kTabHeight, "Start",
                state_ == State::Stopped && !popup_ && !games_->panelOpen() && (!shimTarget() || games_->hasGame()) &&
                    rigReady()};
    }

    // Every device of the rig the mapping uses is connected (or game controllers cannot be listed here at all: then
    // Start tries and says what is wrong).
    [[nodiscard]] bool rigReady() const {
        return needsKnown_ && needsProblem_.empty() && missing_.empty() && !helperMissing_;
    }

    // What Start needs (StartNeeds.hpp): the config Start would use, as saved, and the active rig, checked against
    // the devices connected now (DeviceWatch, every 2 s) while stopped. The status next to Start shows the result.
    void updateStartNeeds() {
        devices_.setActive(state_ == State::Stopped && !quitting_);
        if (state_ != State::Stopped) {
            needsKnown_ = false;
            return;
        }
        const auto snapshot = devices_.latest();
        const std::uint64_t now = platform::nowMs();
        const std::uint64_t generation = snapshot ? snapshot->generation : 0;
        // A new listing, or once a second (a saved mapping, another rig or target).
        if (generation == needsGeneration_ && needsCheckedMs_ != 0 && now - needsCheckedMs_ < 1000) return;
        needsGeneration_ = generation;
        needsCheckedMs_ = now;

        EmulatorConfig config;
        if (const std::string path = picker_->selected(); !path.empty()) {
            if (const auto text = readFileText(pathFromUtf8(path))) {
                config = loadEmulatorConfig(IniDocument::parse(*text).document).config;
            }
        }
        std::optional<Rig> rig;
        if (const std::string name = readAppSetting(settings_.stateFile, "rig"); !name.empty()) {
            std::vector<std::string> errors;
            rig = readRig(rigsFolder(), name, errors);
        }
        const StartNeeds needs = startNeeds(config, rig ? &*rig : nullptr);
        needsProblem_ = needs.problem;
        // The virtual device needs Administrator or the WheelBri Helper; the shim needs neither.
        helperMissing_ = !shimTarget() && !PrivilegedSteps::available();
        if (devices_.unavailable()) {
            needsKnown_ = true;   // nothing to wait for: Start says what is wrong
            missing_.clear();
        } else if (!snapshot) {
            needsKnown_ = false;  // the first listing is under way
            missing_.clear();
        } else {
            std::vector<PresentDevice> present;
            for (const WheelInfo& info : snapshot->devices) present.push_back({info.name, info.vendorId, info.productId});
            needsKnown_ = true;
            missing_ = missingDevices(needs, present);
        }
        // Said in the log when it changes, not every second.
        std::string said = needsProblem_;
        if (said.empty() && helperMissing_) said = "the WheelBri Helper is not running (the virtual device needs it)";
        if (said.empty() && needsKnown_) {
            for (const std::string& label : missing_) said += (said.empty() ? "not connected: " : ", ") + label;
            if (said.empty()) said = "every device of the rig the mapping uses is connected";
        }
        if (!said.empty() && said != needsLogged_) {
            needsLogged_ = said;
            if (needsProblem_.empty() && missing_.empty() && !helperMissing_) {
                WE_LOG_INFO(log(), "Start: {}", said);
            } else {
                WE_LOG_WARN(log(), "Start is greyed out: {}{}", said,
                            needsProblem_.empty() && !helperMissing_ ? " (switch it on or plug it in)" : "");
            }
        }
    }
    Button stopButton() const {
        return {kStopX, kButtonsY, kButtonWidth, MappingView::kTabHeight, "Stop",
                state_ == State::Starting || state_ == State::Running};
    }

    void start() {
        if (running() || popup_ || games_->panelOpen() || !rigReady()) return;
        // The in-game shim: the picked game must have it (put in, or replaced by a newer one, now).
        sessionGame_.clear();
        if (shimTarget()) {
            std::string error;
            if (!games_->prepareStart(error)) {
                WE_LOG_WARN(log(), "Start: {}", error);
                notice("Cannot start", error);
                return;
            }
            sessionGame_ = games_->selectedFolder();
            sessionGameName_ = games_->selectedName();
        }
        everRan_ = false;
        settings_.clearStop();
        host_.clear();
        view_.reset();
        info_.reset();
        sessionDone_ = false;
        state_ = State::Starting;
        startedAt_ = platform::nowNs();
        const std::string config = picker_->selected();
        const bool virtualDevice = picker_->virtualDevice();
        if (mapping_->dirty()) {
            WE_LOG_WARN(log(), "the Mapping tab has unsaved changes: Start uses the config as saved");
        }
        WE_LOG_INFO(log(), "Start pressed: {}; config {} ({})", ConfigPicker::name(picker_->way()),
                    config.empty() ? std::string("built-in defaults") : config,
                    picker_->builtIn() ? "the default mapping" : "the user's own mapping");
        if (!sessionGame_.empty()) {
            WE_LOG_INFO(log(), "the shim's game: {} ({}); start the game now", sessionGameName_,
                        pathToUtf8(sessionGame_));
        }
        setup_->releaseWheel();   // the session opens the wheel now
        idleTab_ = IdleTab::Logs;  // the logs at once: the session writes them while it comes up
        session_ = std::thread([this, config, virtualDevice] {
            platform::nameThisThread("wheelbri session");
            int result = 1;
            try {
                result = settings_.runSession(host_, config, virtualDevice);
            } catch (const std::exception& e) {
                WE_LOG_CRITICAL(log(), "the service stopped on an error: {}", e.what());
            } catch (...) {
                WE_LOG_CRITICAL(log(), "the service stopped on an unknown error");
            }
            exitCode_ = result;
            sessionDone_ = true;
        });
    }

    void stop() {
        if (!running() || state_ == State::Stopping) return;
        WE_LOG_INFO(log(), "Stop pressed: stopping the service");
        state_ = State::Stopping;
        stoppingSince_ = platform::nowNs();
        settings_.requestStop();
    }

    // A session that started showing itself, or one that ended.
    void collectSession() {
        if (const auto info = host_.newSession(seenGeneration_)) {
            info_ = info;
            // A flag and a value rather than std::optional: GCC 12 warns that an optional enum "may be used
            // uninitialized" here (a false alarm; the Steam Deck build may use any GCC).
            const bool hadView = view_ != nullptr;
            const MappingView::Tab previousTab = hadView ? view_->tab() : MappingView::Tab::Logs;
            view_.reset();   // before its profile goes
            profile_ = makeProfile(info->targetProfile);
            if (profile_ != nullptr) {
                view_ = std::make_unique<MappingView>(*window_, info->sourceName, info->axes, info->buttons, *profile_,
                                                      info->mapper);
                view_->setFiles(info->files);
                view_->setDevices(info->devices, info->devicesWarning);
                view_->setAccessories(info->accessories);
                view_->setHeaderChars(sessionGame_.empty() ? kHeaderChars : kHeaderCharsShim);
                view_->setShimMode(!sessionGame_.empty());
                // Start: the tab showing while it came up (the logs, unless changed); the wheel back after a loss:
                // the tab that was showing.
                view_->setTab(hadView                           ? previousTab
                              : idleTab_ == IdleTab::Mapping   ? MappingView::Tab::Mapping
                              : idleTab_ == IdleTab::Setup     ? MappingView::Tab::WheelSetup
                                                               : MappingView::Tab::Logs);
            }
            if (state_ == State::Starting) {   // not again when the session carries on after the wheel came back
                state_ = State::Running;
                runningSince_ = platform::nowNs();
                everRan_ = true;
            }
        }
        if (running() && sessionDone_) {
            session_.join();
            lastResult_ = exitCode_.load();
            const bool wasStopping = state_ == State::Stopping;
            state_ = State::Stopped;
            idleTab_ = showingSetup()                                                 ? IdleTab::Setup
                       : view_ != nullptr && view_->tab() == MappingView::Tab::Mapping ? IdleTab::Mapping
                                                                                       : IdleTab::Logs;
            view_.reset();
            info_.reset();
            if (lastResult_ == 0 || wasStopping) {
                WE_LOG_INFO(log(), "the service stopped");
            } else {
                WE_LOG_WARN(log(), "the service could not run (exit code {}); the lines above say why", *lastResult_);
            }
            // A shim session: the shim stays in the game's folder unless the user says to delete it (not asked when
            // the window is closing: it stays then).
            if (!sessionGame_.empty() && !quitting_) askDeleteShim(sessionGame_, sessionGameName_);
            sessionGame_.clear();
        }
    }

    void drawWindow() {
        window_->begin(kBackground);
        window_->setOrigin(0, kViewTop);
        if (showingSetup()) {
            setup_->draw();
            if (view_ != nullptr) {
                view_->drawTabBar();
            } else {
                drawIdleTabs();
            }
        } else if (showingMappingEditor()) {
            mapping_->draw();
            drawIdleTabs();
        } else if (view_ != nullptr && info_) {
            if (const auto frame = host_.latest(); frame && frame->ready) {
                draw(ffbDisplay(*info_, *frame), *view_, *frame);
            }
        } else if (view_ == nullptr) {
            logs_->drawLogsPage();   // stopped or starting: the same live log, before the session shows itself
            drawIdleTabs();
        }
        drawControls();   // every tab
        drawVersion();
        if (games_->panelOpen()) games_->drawPanel();
        if (popup_) drawPopup();
        window_->present();
    }

    // The version and the commit it was built from, in the top-right corner of every tab (Version.hpp).
    void drawVersion() {
        static const std::string version = buildVersion();
        window_->text(kWidth - 4 - static_cast<float>(version.size()) * ui::kCharWidth, 6, version, kDim);
    }

    // The controls at the tab bar's right end, on every tab: the status, Start, Stop; the games row on the Mapping tab.
    void drawControls() {
        std::string status;
        ui::Colour colour = kText;
        const std::uint64_t now = platform::nowNs();
        switch (state_) {
            case State::Stopped:
                // What is missing comes first, the most basic first (no rig, no mapping): it is why Start is greyed out.
                if (!needsProblem_.empty()) {
                    status = needsProblem_;
                    colour = kWarning;
                } else if (helperMissing_) {
                    status = "HELPER NOT INSTALLED";
                    colour = kWarning;
                } else if (needsKnown_ && !missing_.empty()) {
                    status = "NOT CONNECTED: ";
                    for (std::size_t i = 0; i < missing_.size(); ++i) status += (i == 0 ? "" : ", ") + missing_[i];
                    colour = kWarning;
                } else if (!needsKnown_) {
                    status = "Looking for the rig...";
                    colour = kDim;

                } else if (lastResult_ && *lastResult_ != 0) {
                    status = "Start failed (Logs)";   // the Logs tab says why, in red
                    colour = kWarning;
                } else {
                    status = "Ready";
                    colour = kActive;
                }
                break;
            case State::Starting: {
                const std::string step = host_.currentStep();
                status = "Starting " + clock((now - startedAt_) / platform::kNsPerSecond) +
                         (step.empty() ? std::string() : ": " + step);
                colour = kCaution;
                break;
            }
            case State::Running:
                status = "Running " + clock((now - runningSince_) / platform::kNsPerSecond);
                colour = kActive;
                if (const auto frame = host_.latest(); frame && !frame->game.empty()) {
                    status += ", " + frame->game;   // the known game followed, "NFS 2015 pedal profile" (GameWatch)
                    if (!frame->gameRotation.empty()) {
                        status += ", " + frame->gameRotation;   // its rotation (set the wheel's software to it too)
                    }
                }
                if (const auto frame = host_.latest(); frame && frame->wheelLostMs > 0) {
                    status = "WHEEL LOST " + clock(frame->wheelLostMs / 1000);
                    colour = kWarning;
                } else if (frame && frame->gameIgnoresWheel) {
                    status = "GAME NOT USING WHEEL";
                    colour = kWarning;
                } else if (frame && frame->gameFfbIdle) {
                    status = "GAME FFB OFF: ALT-TAB";   // the Logs tab says more (main.cpp, FfbIdleWatch)
                    colour = kWarning;
                } else if (frame && !frame->gameWarning.empty()) {
                    status = frame->gameWarning;   // e.g. NFS 2015's own wheel controls undo its pedal profile
                    colour = kWarning;
                }
                break;
            case State::Stopping:
                status = "Stopping " + clock((now - stoppingSince_) / platform::kNsPerSecond);
                colour = kCaution;
                break;
        }
        // Between the last tab button and Start: what is left of the bar (3 tabs while stopped, up to 5 while running).
        const float tabsEnd = view_ != nullptr ? MappingView::tabsEnd()
                                               : idleTabButtons().back().x + idleTabButtons().back().w;
        const float room = kStartX - 12 - tabsEnd - 12;
        status = fit(status, static_cast<std::size_t>(std::max(4.0F, room / ui::kCharWidth)));
        if (shimTarget() && showingMapping()) games_->draw(kGamesX, kControlsY, state_ == State::Stopped && !popup_);
        window_->text(kStartX - 12 - static_cast<float>(status.size()) * ui::kCharWidth, kTabBarTop + 10, status, colour);
        startButton().draw(*window_, false);
        stopButton().draw(*window_, false);
    }

    // --- popups: a question (two answers) or a notice (OK) -----------------------------------------------------
    struct Popup {
        std::string title;
        std::vector<std::string> lines;
        std::string yes;                  // the first button ("OK" for a notice)
        std::string no;                   // the second, empty for a notice
        std::function<bool()> onYes;      // true: the popup closes
        std::function<void()> onNo;
        bool warning = false;             // the title in red
    };

    // Wraps at spaces (and at '\n') to the popup's width.
    static std::vector<std::string> wrap(const std::string& text) {
        std::vector<std::string> lines;
        std::size_t start = 0;
        while (start <= text.size()) {
            std::size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            std::string paragraph = text.substr(start, end - start);
            while (paragraph.size() > kPopupChars) {
                std::size_t cut = paragraph.rfind(' ', kPopupChars);
                if (cut == std::string::npos || cut == 0) cut = kPopupChars;
                lines.push_back(paragraph.substr(0, cut));
                paragraph = paragraph.substr(cut == kPopupChars ? cut : cut + 1);
            }
            lines.push_back(paragraph);
            start = end + 1;
        }
        return lines;
    }

    // While Start puts the hide filter on the rig for the first time (once per installation; ~10-40 s, during which
    // Windows reconnects the wheel), a popup says so, so it does not look stuck (2026-10-06: Stop was pressed just as it
    // finished). It closes by itself when that is done, or with OK.
    void showFirstHide() {
        const bool first = state_ == State::Starting && host_.currentStep() == kStepFirstHide;
        if (state_ != State::Starting) firstHideNoticed_ = false;
        if (first && !firstHideNoticed_ && !popup_) {
            firstHideNoticed_ = true;
            notice("Hiding your wheel from games: first time only",
                   "WheelBri is putting its filter on your rig, so that games see only the G920. This happens once per "
                   "installation and can take up to a minute; meanwhile Windows reconnects the wheel, so it may "
                   "disappear for a moment. Please do not unplug it or press Stop.\nStart goes on by itself when it is "
                   "done; the next Starts are quick.");
            firstHidePopup_ = true;
        }
        if (firstHidePopup_ && (!popup_ || !first)) {
            if (popup_ && !first) popup_.reset();   // done: the popup goes by itself
            firstHidePopup_ = false;
        }
    }

    void notice(const std::string& title, const std::string& text) {
        popup_ = Popup{title, wrap(text), "OK", "", [] { return true; }, nullptr, true};
    }

    void askDeleteShim(const std::filesystem::path& folder, const std::string& name) {
        const std::string where = pathToUtf8(folder);
        popup_ = Popup{"The session stopped (" + name + ").",
                       wrap("Would you like to delete the shim from the target folder?\n" + where + "\n(" +
                            std::string(kShimDll) + ", " + kShimSettings + ")"),
                       "Yes, delete it",
                       "No, keep it",
                       [this, folder, where] {
                           std::string error;
                           if (games_->removeFrom(folder, error)) return true;
                           popup_->lines = wrap("Not deleted: " + error + "\n" + where);   // Yes again to retry
                           popup_->warning = true;
                           return false;
                       },
                       [] { WE_LOG_INFO(log(), "the shim stays in the game's folder"); },
                       false};
    }

    [[nodiscard]] float popupHeight() const {
        return 44 + static_cast<float>(popup_->lines.size()) * 14 + 30;
    }
    [[nodiscard]] Button popupYes() const {
        return {kPopupX + 16, kPopupY + popupHeight() - 26, 150, 18, popup_->yes.c_str(), true};
    }
    [[nodiscard]] Button popupNo() const {
        return {kPopupX + 16 + 158, kPopupY + popupHeight() - 26, 150, 18, popup_->no.c_str(), !popup_->no.empty()};
    }

    void answerPopup(const bool yes) {
        if (!popup_) return;
        if (yes) {
            if (popup_->onYes && !popup_->onYes()) return;
        } else if (popup_->onNo) {
            popup_->onNo();
        }
        popup_.reset();
    }

    void onPopupClick(const ui::Click& click) {
        if (popupYes().hit(click)) {
            answerPopup(true);
        } else if (popupNo().hit(click)) {
            answerPopup(false);
        }
    }

    void drawPopup() {
        const float h = popupHeight();
        window_->fill(kPopupX, kPopupY, kPopupW, h, kPopup);
        window_->outline(kPopupX, kPopupY, kPopupW, h, kHeading);
        window_->text(kPopupX + 16, kPopupY + 12, popup_->title, popup_->warning ? kWarning : kHeading);
        float y = kPopupY + 34;
        for (const std::string& line : popup_->lines) {
            window_->text(kPopupX + 16, y, line, kText);
            y += 14;
        }
        popupYes().draw(*window_, false);
        if (!popup_->no.empty()) popupNo().draw(*window_, false);
    }

    const AppSettings& settings_;
    std::unique_ptr<ui::UiWindow> window_;
    AppHost host_;
    std::thread session_;
    std::atomic<bool> sessionDone_{false};
    std::atomic<int> exitCode_{0};
    State state_ = State::Stopped;
    std::optional<int> lastResult_;
    std::uint64_t startedAt_ = 0;
    std::uint64_t runningSince_ = 0;
    std::uint64_t seenGeneration_ = 0;
    bool everRan_ = false;          // this session got as far as running
    std::uint64_t stoppingSince_ = 0;
    std::optional<SessionInfo> info_;
    std::unique_ptr<IDeviceProfile> profile_;
    std::unique_ptr<MappingView> view_;
    std::unique_ptr<WheelSetupView> setup_;
    std::unique_ptr<ConfigPicker> picker_;
    std::unique_ptr<MappingEditView> mapping_;
    std::unique_ptr<ShimGamesView> games_;   // the shim's games (target G920 in-game shim)
    std::filesystem::path sessionGame_;       // the game a shim session was started for
    std::string sessionGameName_;
    std::optional<Popup> popup_;
    bool quitting_ = false;                   // the window was closed: stop, then end
    std::unique_ptr<IDeviceProfile> logsProfile_;   // logs_ needs one; it shows no mapping
    std::unique_ptr<MappingView> logs_;             // the Logs tab while no session shows
    IdleTab idleTab_ = IdleTab::Mapping;   // what shows while no session does
    // What Start needs from the rig (updateStartNeeds).
    DeviceWatch devices_{DeviceWatch::systemLister(), platform::DeviceChanges::shared().counter(),
                         DeviceWatch::systemOptions(platform::DeviceChanges::shared().available())};
    bool needsKnown_ = false;              // the connected devices are known (first listing done)
    std::string needsProblem_;             // no rig, no wheel in it
    bool helperMissing_ = false;           // target virtual device, not elevated, no WheelBri Helper running
    std::vector<std::string> missing_;     // the needed devices not connected
    std::uint64_t needsGeneration_ = 0;
    std::uint64_t needsCheckedMs_ = 0;
    std::string needsLogged_;
    bool firstHideNoticed_ = false;   // this Start's "hiding for the first time" popup was shown (showFirstHide)
    bool firstHidePopup_ = false;     // ...and is the popup open now
};

}  // namespace

std::unique_ptr<UiHost> makeSessionWindow(std::string configPath) {
    return std::make_unique<SessionWindow>(std::move(configPath));
}

int runApp(const AppSettings& settings) {
    platform::nameThisThread("wheelbri window");   // the app's own thread: the window and its tabs
    App app(settings);
    return app.run();
}

}  // namespace wheelbri::service
