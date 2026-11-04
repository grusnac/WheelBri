#include "App.hpp"
#include "UiStyle.hpp"
#include "GameWatch.hpp"
#include "PrivilegedSteps.hpp"

#include "ConfigPicker.hpp"
#include "DeviceWatch.hpp"
#include "MappingEditView.hpp"
#include "PaneWindow.hpp"
#include "Platform.hpp"
#include "ServiceLogging.hpp"
#include "ShimGamesView.hpp"
#include "UiWindow.hpp"
#include "Version.hpp"
#include "WheelSetupView.hpp"

#include "wheelbri/AutoMap.hpp"
#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/FileText.hpp"
#include "wheelbri/GameTab.hpp"
#include "wheelbri/IniDocument.hpp"
#include "wheelbri/Rig.hpp"
#include "wheelbri/ShimWatch.hpp"
#include "wheelbri/LogFiles.hpp"
#include "wheelbri/StartNeeds.hpp"
#include "wheelbri/WakePlanner.hpp"
#include "wheelbri/WayNotices.hpp"
#include "wheelbri/Logging.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <functional>
#include <optional>
#include <span>
#include <thread>

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.ui");
    return logger;
}

// The same colours as the live view (MappingView.cpp).
constexpr ui::Colour kBackground = style::kBackground;
constexpr ui::Colour kPanel = style::kPanel;
constexpr ui::Colour kText = style::kText;
constexpr ui::Colour kDim = style::kDim;
constexpr ui::Colour kHeading = style::kHeading;
constexpr ui::Colour kActive = style::kActive;
constexpr ui::Colour kWarning = style::kWarning;
constexpr ui::Colour kPopup = style::kPopup;
constexpr ui::Colour kCaution = style::kCaution;
constexpr ui::Colour kDark = style::kDark;

// Window layout, in logical pixels: the panel (the tabs) fills the window's canvas, which is at least the design size
// (kWidth x kHeight) and grows with the window (UiWindow.hpp). Every tab has the controls at the right end of the tab
// bar, which is at the bottom: the status (ready, what is missing, running...), then Start (Stop while a session runs),
// as tall as the tab buttons, kept at the right edge (App::startX, stopX, tabBarTop). Above the bar, across the window,
// a notice says in plain words what blocks Start or what is wrong while running (App::alert). The shim's games row
// is on the Mapping tab only, in its top row.
constexpr float kWidth = MappingView::kWidth;
constexpr float kViewTop = 0;
constexpr float kHeight = MappingView::kHeight;
constexpr float kControlsY = 0;            // the top row: the shim's games
constexpr float kControlsHeight = 16;
constexpr float kButtonWidth = 68;         // Start (Stop while a session runs): one button, at the right end
constexpr float kAlertHeight = 26;         // the notice above the tab bar (what blocks Start, what is wrong): 22 + 4
constexpr float kGamesX = 220;              // the live view: the shim's games ("Game < name > Games (N)"), 370 wide
constexpr float kGamesWidth = 370;
constexpr float kGamesEditorY = 18;         // the Mapping tab's editor: its second row, at the right edge
constexpr std::size_t kHeaderChars = 50;    // the live view's title on the Mapping tab...
constexpr std::size_t kHeaderCharsShim = 26;   // ...and with the games row next to it
// The app's popups (a question or a notice), over everything: centred (App::popupX, popupY).
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

// The mapping the session uses now (changed while it runs: LiveMapping.hpp), into the view when it moved.
void followMapping(MappingView& view, std::uint64_t& shownVersion, const UiFrame& frame) {
    if (frame.mapping != nullptr && frame.mappingVersion != shownVersion) {
        shownVersion = frame.mappingVersion;
        view.setMapping(*frame.mapping);
    }
    view.setButtonOrder(frame.buttonOrder);
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
            MappingView::setEditorTabs(false);   // no editor here: Mapping is the live page, no Live or Games tab
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
        view_->setButtonOrder(info.buttonOrder);
        mappingShown_ = 0;   // the session's count starts again with it
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
                if (setup) setup_->onKey(key);   // scrolling the devices
                view_->onKey(key);
            }
        }
        for (const ui::Click& click : input.clicks) {
            if (setup && click.y < window_->height() - MappingView::kTabBarHeight) {
                setup_->onClick(click.x, click.y);
            } else {
                view_->onClick(click.x, click.y);
            }
        }
        if (setup) setup_->onPointer(input.pointer.x, input.pointer.y, input.leftDown);
        if (input.wheel != 0) {
            if (setup) setup_->onScroll(input.wheel);
            view_->onScroll(input.wheel);
        }
        if (input.hwheel != 0) view_->onHScroll(input.hwheel);
        view_->onPointer(input.pointer.x, input.pointer.y, input.leftDown);   // the live log's scroll bars
        if (frame.ready && view_->tab() == MappingView::Tab::WheelSetup) {
            const auto wheel = sessionWheel(info_, frame);
            setup_->update(&wheel);
        }
        if (frame.ready) followMapping(*view_, mappingShown_, frame);
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
    std::uint64_t mappingShown_ = 0;   // UiFrame::mappingVersion the view shows
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
        if (enabled && highlight) {
            window.fill(x, y, w, h, kActive);
            const float textWidth = static_cast<float>(std::string_view(label).size()) * ui::kCharWidth;
            window.text(x + (w - textWidth) / 2, y + (h - ui::kCharHeight) / 2, label, kDark);
            return;
        }
        style::button(window, x, y, w, h, label, false, enabled);
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
        // The Mapping tab's editor and the live view each draw into a pane of the window: the whole of it, or with
        // Side by side its left and right halves (placePanes).
        editorPane_ = std::make_unique<ui::PaneWindow>(*window_);
        livePane_ = std::make_unique<ui::PaneWindow>(*window_);
        sideBySide_ = readAppSetting(settings_.stateFile, "side_by_side") == "true";
        window_->setDesignColumns(sideBySide_ ? 2 : 1);
        const std::uint32_t windowsBuild = platform::windowsBuild();
        if (windowsBuild != 0) WE_LOG_INFO(log(), "Windows build {}", windowsBuild);
        picker_ = std::make_unique<ConfigPicker>(
            *editorPane_, ConfigPicker::Settings{settings_.builtInConfigFolder, settings_.userConfigFolder,
                                             settings_.stateFile,
                                             ConfigPicker::virtualDeviceSupported(windowsBuild),
                                             ConfigPicker::windowsName(windowsBuild), settings_.trueForceSdk});
        setup_ = std::make_unique<WheelSetupView>(
            *window_, WheelSetupView::Settings{rigsFolder(), settings_.devicesFolder, settings_.stateFile});
        MappingView::setEditorTabs(true);   // the live view's tab bar has Mapping (the editor), Live and Games
        // The first time (no rig yet) the devices are defined first; then the mapping is where to go.
        idleTab_ = WheelSetupView::anyRig(rigsFolder()) ? IdleTab::Mapping : IdleTab::Setup;
        mapping_ = std::make_unique<MappingEditView>(*editorPane_, rigsFolder(), settings_.stateFile, *picker_);
        games_ = std::make_unique<ShimGamesView>(
            *window_, ShimGamesView::Settings{shimGamesFile(), settings_.shimDlls, settings_.shimSettings,
                                                settings_.stateFile, [](const std::filesystem::path& folder) {
                                                    std::vector<GameProfile> games(gameProfiles().begin(),
                                                                                   gameProfiles().end());
                                                    for (GameProfile& game : loadUserGames()) {
                                                        games.push_back(std::move(game));
                                                    }
                                                    const GameProfile* const known = gameInFolder(folder, games);
                                                    return known != nullptr ? known->label : std::string();
                                                },
                                                [this] { openGamesTab(); }});
        mapping_->setShimGames(games_.get());
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
            placePanes();   // the canvas may have changed with the window
            if (input.close && !quitting_) {
                quitting_ = true;
                if (running()) {
                    WE_LOG_INFO(log(), "window closed: stopping the service first");
                    stop();
                }
            }
            const bool setup = showingSetup();
            const bool editingMapping = showingEditor();   // the Mapping tab's editor, or the Games tab
            for (const ui::Click& click : input.clicks) {
                // A popup of the app's takes every click until it is answered.
                if (popup_) {
                    onPopupClick(click);
                    continue;
                }
                // Start / Stop, at the tab bar's right end on every tab; not over the editor's popups.
                if (!(editingMapping && mapping_->popupOpen()) &&
                    click.y >= buttonsY() + kViewTop && click.y < buttonsY() + kViewTop + MappingView::kTabHeight &&
                    click.x >= startX()) {
                    if (state_ == State::Stopped) {
                        if (startButton().hit(click)) {
                            start();
                        } else if (click.x < startX() + kButtonWidth) {
                            showChecklist();   // the greyed Start: what it still needs
                        }
                    } else if (stopButton().hit(click)) {
                        stop();
                    }
                    continue;
                }
                // The notice's "What's missing": the same list as the greyed Start.
                if (alertButtonW_ > 0 && !(editingMapping && mapping_->popupOpen()) &&
                    click.y >= alertButtonY_ + kViewTop && click.y < alertButtonY_ + kViewTop + 16 &&
                    click.x >= alertButtonX_ && click.x < alertButtonX_ + alertButtonW_) {
                    showChecklist();
                    continue;
                }
                // The session bar: to the tab where the target, the mode and the game are set.
                if (chipW_ > 0 && !(editingMapping && mapping_->popupOpen()) &&
                    click.y >= buttonsY() + kViewTop && click.y < buttonsY() + kViewTop + MappingView::kTabHeight &&
                    click.x >= chipX_ && click.x < chipX_ + chipW_) {
                    if (view_ != nullptr) {
                        view_->setTab(MappingView::Tab::Mapping);
                    } else {
                        idleTab_ = IdleTab::Mapping;
                    }
                    continue;
                }
                // The shim's game (only with Mode Shim), where gamesRowX / gamesRowY put it.
                if (showingMapping() && shimTarget() && !(editingMapping && mapping_->popupOpen()) &&
                    click.y >= gamesRowY() && click.y < gamesRowY() + kControlsHeight && click.x >= gamesRowX() &&
                    click.x < gamesRowX() + kGamesWidth) {
                    games_->onClick(click.x, click.y);
                    continue;
                }
                // The panel, in its own coordinates; the tab bar at its bottom.
                const float y = click.y - kViewTop;
                if (y >= tabBarTop()) {
                    if (view_ != nullptr) {
                        livePane_->place(0, window_->width());   // the tab bar is the whole window's
                        view_->onClick(click.x, y);
                        placePanes();
                    } else if (y >= buttonsY() && y < buttonsY() + MappingView::kTabHeight) {
                        for (const IdleTabButton& button : idleTabButtons()) {
                            if (click.x >= button.x && click.x < button.x + button.w) idleTabClicked(button.tab);
                        }
                    }
                } else if (setup) {
                    setup_->onClick(click.x, y);
                } else if (splitShown() && click.x >= livePane_->left() && !mapping_->popupOpen()) {
                    // Side by side: the right half is the live view's (in its own coordinates), or Start's place.
                    if (view_ != nullptr) {
                        view_->onClick(click.x - livePane_->left(), y);
                    } else {
                        onPlaceholderClick(click.x, y);
                    }
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
                    } else if (key == 'b' && !setup) {
                        toggleSideBySide();
                    } else if (view_ != nullptr) {
                        if (setup) setup_->onKey(key);   // scrolling the devices (letters are the tabs' keys)
                        view_->onKey(key);
                    } else if (key == 's') {
                        idleTabClicked(IdleTab::Live);
                    } else if (key == 'r' || key == 'm' || key == 'l' || key == 'g') {
                        idleTab_ = key == 'r'   ? IdleTab::Setup
                                   : key == 'm' ? IdleTab::Mapping
                                   : key == 'g' ? IdleTab::Games
                                                : IdleTab::Logs;
                    } else if (idleTab_ == IdleTab::Logs) {
                        logs_->onKey(key);   // scrolling, O / D open the folders
                    } else if (setup) {
                        setup_->onKey(key);  // scrolling the devices
                    }
                }
            }
            if (editingMapping) mapping_->onPointer(input.pointer.x, input.pointer.y - kViewTop, input.leftDown);
            // Hover tooltips (the pickers' used controls): where the mouse is, or nowhere once it left the window.
            mapping_->setHover(input.pointer.x, input.pointer.y - kViewTop, editingMapping && input.pointerInside);
            // The Rig tab's scroll bar is dragged with the mouse.
            if (setup) setup_->onPointer(input.pointer.x, input.pointer.y - kViewTop, input.leftDown);
            setup_->setHover(input.pointer.x, input.pointer.y - kViewTop, setup && input.pointerInside);
            if (setup && input.wheel != 0 && !popup_) setup_->onScroll(input.wheel);   // the Rig tab's devices
            if (editingMapping && input.wheel != 0 && !popup_) mapping_->onScroll(input.wheel);   // a popup's list
            if (view_ != nullptr && input.wheel != 0) view_->onScroll(input.wheel);
            if (view_ == nullptr && idleTab_ == IdleTab::Logs && input.wheel != 0) logs_->onScroll(input.wheel);
            // The live log's scroll bars: dragged with the mouse, and the sideways wheel.
            if (MappingView* const logsView = view_ != nullptr ? view_.get()
                                              : idleTab_ == IdleTab::Logs ? logs_.get()
                                                                          : nullptr) {
                if (input.hwheel != 0) logsView->onHScroll(input.hwheel);
                const float left = logsView == view_.get() ? livePane_->left() : 0.0F;   // its pane's coordinates
                logsView->onPointer(input.pointer.x - left, input.pointer.y - kViewTop, input.leftDown);
            }
            if (view_ != nullptr && view_->takeSideBySideRequest()) toggleSideBySide(/*fromLive=*/true);
            collectSession();
            showFirstHide();
            updateStartNeeds();
            logs_->setShimMode(shimTarget());   // the Logs tab while stopped follows the target
            updateSetup();
            picker_->setLocked(state_ != State::Stopped);    // Target and Mode: from Start until Stop, fixed
            mapping_->setLocked(state_ != State::Stopped);   // the way and the mapping stay as Start found them
            updateGames();
            hotSwapShims();
            mapping_->setPage(showingGames() ? MappingEditView::Page::Games : MappingEditView::Page::Mapping);
            // Every frame, not only while it shows: the live views change the rotation through it while running.
            mapping_->update();
            if (std::string problem; games_->takeProblem(problem)) notice("Games", problem);
            for (std::string text : {mapping_->takeToast(), games_->takeToast()}) {
                if (!text.empty()) toast(std::move(text));
            }
            if (pendingWayNotice_ && !popup_) {   // never over another popup (a question would be lost)
                notice(pendingWayNotice_->title, pendingWayNotice_->text);
                popup_->warning = true;
                pendingWayNotice_.reset();
            }
            if (quitting_ && !running()) {
                break;
            }
            // The window is drawn kWindowFramesPerSecond (60) times a second, on a schedule that does not drift (a
            // sleep that wakes late is not added to the next one), and at once after input; only what changed reaches
            // the screen (UiWindow keeps the last frame). Input is answered within one frame (~17 ms).
            const std::uint64_t now = platform::nowNs();
            const bool touched = !input.keys.empty() || !input.text.empty() || !input.clicks.empty() ||
                                 input.wheel != 0 || input.hwheel != 0 || input.leftDown ||
                                 input.pointer.x != lastPointer_.x || input.pointer.y != lastPointer_.y ||
                                 input.pointerInside != lastInside_;
            lastPointer_ = input.pointer;
            lastInside_ = input.pointerInside;
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
    // Live never shows while stopped (greyed: idleTabClicked), it only has a button.
    enum class IdleTab { Mapping, Games, Setup, Logs, Live };

    static constexpr std::uint64_t kDrawEveryNs = platform::kNsPerSecond / kWindowFramesPerSecond;
    static constexpr std::uint64_t kHiddenWaitNs = 16 * platform::kNsPerMs;   // the window minimised: input only
    std::uint64_t nextDrawNs_ = 0;
    ui::Click lastPointer_{};
    bool lastInside_ = false;

    [[nodiscard]] bool running() const { return session_.joinable(); }

    // Where things are on the canvas now (it grows with the window): the tab bar at the bottom, Start and Stop at its
    // right end, popups centred.
    [[nodiscard]] float tabBarTop() const { return window_->height() - MappingView::kTabBarHeight; }
    // The shim's game row: in the Mapping tab's editor its second row at the right edge (where Default, Save and Revert
    // were: they are in the top row now); in the live view the top row, next to the title.
    [[nodiscard]] float gamesRowX() const {
        return showingMappingEditor() ? editorPane_->width() - 4 - kGamesWidth : kGamesX;
    }
    // The editor's place also while it shows during a session (the live view's Edit).
    [[nodiscard]] float gamesRowY() const { return showingMappingEditor() ? kGamesEditorY : kControlsY; }
    [[nodiscard]] float buttonsY() const { return tabBarTop() + 4; }   // Start / Stop, level with the tab buttons
    // Start and Stop are one button (Start while stopped, Stop from Starting to Stopped), at the bar's right end.
    [[nodiscard]] float stopX() const { return window_->width() - 4 - kButtonWidth; }
    [[nodiscard]] float startX() const { return stopX(); }
    [[nodiscard]] float popupX() const { return std::floor((window_->width() - kPopupW) / 2); }
    // A popup's top: 200 down (more in a taller window), higher when that would take it past the tab bar.
    [[nodiscard]] float popupY() const {
        const float wanted = 200 + (window_->height() - kHeight) / 2;
        const float lowest = tabBarTop() - (popup_ ? popupHeight() : 0.0F) - 8;
        return std::floor(std::max(8.0F, std::min(wanted, lowest)));
    }

    // Whether the Wheel setup tab shows: the live view's tab while a session runs, the idle page's otherwise.
    [[nodiscard]] bool showingSetup() const {
        return view_ != nullptr ? view_->tab() == MappingView::Tab::WheelSetup : idleTab_ == IdleTab::Setup;
    }
    // The Mapping tab: always the editor, stopped or running (the Live tab next to it shows the session at work).
    [[nodiscard]] bool showingMappingEditor() const {
        return view_ != nullptr ? view_->tab() == MappingView::Tab::Mapping || splitShown()
                                : idleTab_ == IdleTab::Mapping;
    }
    // Side by side now: on, and the Mapping or Live tab showing (the editor left, the live view or Start's place
    // right). Other tabs keep the whole window.
    [[nodiscard]] bool splitShown() const {
        return sideBySide_ && (view_ != nullptr ? MappingView::pairedTab(view_->tab()) : idleTab_ == IdleTab::Mapping);
    }
    // The panes for what shows now: halves side by side, else both the whole window.
    void placePanes() {
        const float width = window_->width();
        if (splitShown()) {
            const float half = std::floor(width / 2);
            editorPane_->place(0, half);
            livePane_->place(half, width - half);
        } else {
            editorPane_->place(0, width);
            livePane_->place(0, width);
        }
        if (view_ != nullptr) view_->setSideBySide(sideBySide_);
    }
    // Side by side on or off (B, or the live view's button): the window lays out two pages wide, kept in app.ini.
    // `fromLive`: asked on the live view (its button): turned off, the Live tab stays.
    void toggleSideBySide(const bool fromLive = false) {
        sideBySide_ = !sideBySide_;
        window_->setDesignColumns(sideBySide_ ? 2 : 1);
        writeAppSetting(settings_.stateFile, "side_by_side", sideBySide_ ? "true" : "false");
        WE_LOG_INFO(log(), "window: side by side {}", sideBySide_ ? "on" : "off");
        // On: the Mapping tab (both show). Off from the live view's button: Live; else the tab as it is.
        if (!sideBySide_ && fromLive && view_ != nullptr) view_->setTab(MappingView::Tab::Live);
        if (sideBySide_ && view_ == nullptr && idleTab_ != IdleTab::Mapping) idleTab_ = IdleTab::Mapping;
        if (sideBySide_ && view_ != nullptr && !MappingView::pairedTab(view_->tab())) {
            view_->setTab(MappingView::Tab::Mapping);
        }
        placePanes();
    }
    // Side by side while stopped: the right half says what Live is, and has the way back.
    void drawPlaceholder() {
        const float x = livePane_->left();
        window_->fill(x, 0, 1, tabBarTop(), style::kRule);
        window_->text(x + 16, 16, "LIVE", kDim);
        window_->text(x + 16, 40, "Press Start: your mapping at work shows here (Shadow or List),", kDim);
        window_->text(x + 16, 54, "and each change on the left is in use at once.", kDim);
        style::button(*window_, x + 16, 76, kPlaceholderW, 16, "Side by side (B)", true, true);
    }
    void onPlaceholderClick(const float x, const float y) {
        const float left = livePane_->left() + 16;
        if (x >= left && x < left + kPlaceholderW && y >= 76 && y < 92) toggleSideBySide();
    }
    static constexpr float kPlaceholderW = 16 * ui::kCharWidth + 12;
    // The Games tab: the editor's Games page, stopped or running (then locked, with the game that runs).
    [[nodiscard]] bool showingGames() const {
        return view_ != nullptr ? view_->tab() == MappingView::Tab::Games : idleTab_ == IdleTab::Games;
    }
    // What the editor (MappingEditView) draws: either page.
    [[nodiscard]] bool showingEditor() const { return showingMappingEditor() || showingGames(); }

    // The Games tab's view of the session's games, every frame; and when a profiled game starts, the tab opens
    // on it (once per start: the user may go to another tab while it runs).
    void updateGames() {
        MappingEditView::GameNow now;
        if (state_ == State::Running) {
            if (const auto frame = host_.latest()) {
                now.session = true;
                now.id = frame->gameId;
                now.profileApplies = frame->gameProfileApplies;
                now.pid = frame->gamePid;
                now.forMs = frame->gameForMs;
                now.rotation = frame->gameRotation;
                now.warning = frame->gameWarning;
                // A game that does not work the way this session runs: told once per start, by Windows (seen over the
                // game) and in the app (when the user comes back to it).
                if (wayFollower_.onFrame(frame->gameWayNotices) && !frame->gameWayTitle.empty()) {
                    if (!window_->notify(frame->gameWayTitle, frame->gameWayText)) {
                        WE_LOG_INFO(log(), "no system notification here ({}): the app's popup says it",
                                    window_->describe());
                    }
                    pendingWayNotice_ = Notice{frame->gameWayTitle, frame->gameWayText};
                }
                if (sessionShim_) watchShim(*frame);
                const std::optional<std::string> started = gameFollower_.onFrame(frame->gameStarts, frame->gameId);
                if (started) {
                    mapping_->selectGame(*started);
                    // Not while a name is being typed in the Rig tab, nor over a popup: the game is picked anyway.
                    if (view_ != nullptr && !popup_ &&
                        !(showingSetup() && setup_->editingText())) {
                        view_->setTab(MappingView::Tab::Games);
                        WE_LOG_INFO(log(), "Games tab: {} started, its profile shown", frame->game);
                    }
                }
            }
        }
        mapping_->setGameNow(std::move(now));
    }
    // Mode Shim: has the shim been heard from (its force feedback reached WheelBri, or it wrote its log since Start)?
    // A known game running two minutes without it: the emulation is stalled or the shim is not in that game's folder.
    // Told once per start, by Windows (seen over the game) and in the app.
    void watchShim(const UiFrame& frame) {
        const std::uint64_t now = platform::nowMs();
        bool heard = frame.ffb && frame.ffb->stats().packets > 0;
        if (!heard && !shimLogSeen_ && now - shimLogCheckedMs_ >= 2000) {
            shimLogCheckedMs_ = now;
            const std::filesystem::path shimLog = logfiles::newestFile(settings_.logFolder, "wheelbri-shim");
            std::error_code ec;
            if (!shimLog.empty()) {
                const auto written = std::filesystem::last_write_time(shimLog, ec);
                shimLogSeen_ = !ec && written >= shimStartedFileTime_;
            }
        }
        heard = heard || shimLogSeen_;
        const bool before = shimWatch_.heard();
        const std::optional<std::string> due = shimWatch_.update(now, frame.gameId, frame.gameStarts, heard);
        if (!before && shimWatch_.heard()) WE_LOG_INFO(log(), "Mode Shim: the shim is working (heard from it)");
        if (!due) return;
        const std::string game = frame.game.empty() ? *due : frame.game;
        shimWarned_ = game;
        const std::string title = "No shim activity: " + game;
        const std::string text =
            game + " has been running for 2 minutes and WheelBri has heard nothing from the shim: the emulation is "
                   "stalled, or the shim is not in the game's folder (or the game loads another dinput8.dll). On the "
                   "Games tab (G), check that " + game + " has its folder with \"shim in place\" (Choose folder..., "
                   "Put shim), then close the game and start it again while WheelBri runs.";
        WE_LOG_WARN(log(), "Mode Shim: {} runs for {} s and the shim was never heard from: stalled or not installed",
                    game, ShimWatch::kQuietMs / 1000);
        if (!window_->notify(title, text)) {
            WE_LOG_INFO(log(), "no system notification here ({}): the app's popup says it", window_->describe());
        }
        pendingWayNotice_ = Notice{title, text};
    }

    // The Mapping tab (the editor) or the Live tab (running): the only places with the shim's games row (Start, Stop
    // and the status are on every tab).
    [[nodiscard]] bool showingMapping() const {
        return view_ != nullptr ? view_->tab() == MappingView::Tab::Mapping || view_->tab() == MappingView::Tab::Live
                                : idleTab_ == IdleTab::Mapping;
    }

    // The tab reads the running session's wheel, or (stopped) the wheel picked in it; nothing while starting or
    // stopping, when the session may be opening or closing the wheel.
    void updateSetup() {
        if (!showingSetup()) {
            // The Mapping tab's secondary popup: the Rig tab's reading of the devices, for the controls touched on the
            // rig (stopped only: running, the session has them).
            if (state_ == State::Stopped && showingEditor() && mapping_->wantsRigTouches()) {
                setup_->setListening(true);
                setup_->update(nullptr);
                for (const WheelSetupView::Touch& touch : setup_->takeTouches()) {
                    mapping_->onRigTouch(touch.device, touch.axis, touch.index);
                }
                return;
            }
            setup_->setListening(false);
            if (state_ == State::Stopped) setup_->releaseWheel();   // not kept open while nobody looks
            return;
        }
        setup_->setListening(false);
        if (state_ == State::Stopped) {
            setup_->update(nullptr);
        } else if (info_) {
            if (const auto frame = host_.latest(); frame && frame->ready) {
                const auto wheel = sessionWheel(*info_, *frame);
                setup_->update(&wheel);
            }
        }
    }

    // The app's own tab buttons while no session shows (Rig; Mapping, the editor; Live, greyed: nothing runs; Games;
    // Logs), placed and sized as the live view places its first five (MappingView::tabWidth), so nothing moves when a
    // session starts.
    struct IdleTabButton {
        IdleTab tab;
        const char* label;
        float x;
        float w;
    };
    [[nodiscard]] static std::array<IdleTabButton, 5> idleTabButtons() {
        std::array<IdleTabButton, 5> buttons{{{IdleTab::Setup, "Rig (R)", 0, 0},
                                              {IdleTab::Mapping, "Mapping (M)", 0, 0},
                                              {IdleTab::Live, "Live (S)", 0, 0},
                                              {IdleTab::Games, "Games (G)", 0, 0},
                                              {IdleTab::Logs, "Logs (L)", 0, 0}}};
        float x = MappingView::kTabLeft;
        for (IdleTabButton& button : buttons) {
            button.x = x;
            button.w = MappingView::tabWidth(button.label);
            x += button.w + MappingView::kTabGap;
        }
        return buttons;
    }

    // A tab button while no session shows: Live has nothing to show until Start, and says so.
    void idleTabClicked(const IdleTab tab) {
        if (tab == IdleTab::Live) {
            toast("Live shows a running session: press Start");
            return;
        }
        idleTab_ = tab;
    }

    // The tab bar while no session shows, where the live view has its own.
    void drawIdleTabs() {
        window_->fill(0, tabBarTop(), window_->width(), 1, style::kRule);
        for (const IdleTabButton& button : idleTabButtons()) {
            const bool active = button.tab == idleTab_;   // Live (greyed) never: side by side shows its place
            const bool greyed = button.tab == IdleTab::Live;   // there is nothing live until Start
            if (greyed) {
                window_->outline(button.x, buttonsY(), button.w, MappingView::kTabHeight, style::kRule);
            } else {
                window_->fill(button.x, buttonsY(), button.w, MappingView::kTabHeight, active ? kHeading : kPanel);
            }
            window_->text(button.x + MappingView::kTabPadding, tabBarTop() + 10, button.label,
                          greyed ? style::kFaint : active ? kDark : kText);
        }
    }

    // The target is G920 in-game shim: the games row shows, and Start needs a game.
    [[nodiscard]] bool shimTarget() const { return picker_ != nullptr && !picker_->virtualDevice(); }

    Button startButton() const {
        return {startX(), buttonsY(), kButtonWidth, MappingView::kTabHeight, "Start",
                state_ == State::Stopped && !popup_ && (!shimTarget() || games_->hasGame()) &&
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
        bool configRead = false;
        if (const std::string path = picker_->selected(); !path.empty()) {
            if (const auto text = readFileText(pathFromUtf8(path))) {
                config = loadEmulatorConfig(IniDocument::parse(*text).document, loadUserGames()).config;
                configRead = true;
            }
        }
        std::optional<Rig> rig;
        if (const std::string name = readAppSetting(settings_.stateFile, "rig"); !name.empty()) {
            std::vector<std::string> errors;
            rig = readRig(rigsFolder(), name, errors);
        }
        if (rig) (void)applyRig(config, *rig);   // from_rig: what the rig maps by itself counts (AutoMap.hpp)
        rigName_ = rig ? readAppSetting(settings_.stateFile, "rig") : std::string();
        rigDevices_ = rig ? rig->devices.size() : 0;
        const StartNeeds needs = startNeeds(config, rig ? &*rig : nullptr);
        needsProblem_ = needs.problem;
        // The mapping must be for the picked target (a user's own one edited by hand may not be).
        if (needsProblem_.empty() && configRead) {
            needsProblem_ = targetProblem(config, picker_->profileName());
        }
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
        if (said.empty() && helperMissing_) said = "the WheelBri Helper is not running (Mode Virtual needs it)";
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
        return {stopX(), buttonsY(), kButtonWidth, MappingView::kTabHeight, "Stop",
                state_ == State::Starting || state_ == State::Running};
    }

    // The in-game shim: the pedal axes of the game in `folder` as the user set them for it (the Games tab),
    // over its profile's for `target`; nullopt when the user set none (WheelBri's lines come out, the shim lays the
    // profile itself).
    [[nodiscard]] static std::optional<JoystickLayout> shimAxesFor(const std::filesystem::path& folder,
                                                                  const EmulatorConfig& config,
                                                                  std::span<const GameProfile> games,
                                                                  const std::string& target) {
        const GameProfile* const game = gameInFolder(folder, games);
        if (game == nullptr) return std::nullopt;
        const auto tuning = config.gameTunings.find(game->id);
        if (tuning == config.gameTunings.end() ||
            std::none_of(tuning->second.pedalAxes.begin(), tuning->second.pedalAxes.end(),
                         [](const auto& axis) { return axis.has_value(); })) {
            return std::nullopt;
        }
        JoystickLayout layout = layoutFor(target, 0);
        if (profileFitsTarget(*game, target)) layout = overlay(layout, *game);
        const JoystickLayout axes = tunedLayout(layout, tuning->second);
        if (!isValid(axes)) {
            // Two pedals on one axis (the Games tab says so): the shim's own layout until fixed.
            WE_LOG_WARN(log(), "shim: {}'s pedal axes put two pedals on one axis: not used (Games tab)",
                        game->label);
            return std::nullopt;
        }
        return axes;
    }

    // The mapping's per-game values (the config Start uses) and every game WheelBri knows (built in and the user's).
    struct ShimInputs {
        EmulatorConfig config;
        std::vector<GameProfile> games;
    };
    [[nodiscard]] ShimInputs shimInputs() const {
        ShimInputs inputs;
        std::vector<GameProfile> userGames = loadUserGames();
        if (const std::string path = picker_->selected(); !path.empty()) {
            if (const auto text = readFileText(pathFromUtf8(path))) {
                inputs.config = loadEmulatorConfig(IniDocument::parse(*text).document, userGames).config;
            }
        }
        inputs.games.assign(gameProfiles().begin(), gameProfiles().end());
        for (GameProfile& game : userGames) inputs.games.push_back(std::move(game));
        return inputs;
    }

    // Hot swap: while stopped in Mode Shim, the shims in the games' folders follow the target as soon as it (or the
    // list, or the switch) changes. Called every frame; does something only on a change.
    void hotSwapShims() {
        if (!shimTarget() || state_ != State::Stopped || !games_->hotSwap()) return;
        const std::string target = picker_->profileName();
        const std::string key = target + "|" + std::to_string(games_->changes());
        if (key == hotSwapKey_) return;
        hotSwapKey_ = key;
        const ShimInputs inputs = shimInputs();
        const std::vector<std::string> problems =
            games_->swapShims(target, [&inputs, &target](const std::filesystem::path& folder) {
                return shimAxesFor(folder, inputs.config, inputs.games, target);
            });
        if (problems.empty() || popup_) return;
        std::string text = "These games' shims could not be set to the " +
                           std::string(ConfigPicker::targetName(picker_->target())) + ":\n";
        for (const std::string& line : problems) text += "  " + line + "\n";
        notice("Hot swap", text);
    }

    void start() {
        if (running() || popup_ || !rigReady()) return;
        // Mode Shim: every listed game's folder gets the shim, current and presenting the target, with the game's own
        // pedal axes; whichever game starts finds it ready (the app sees which one runs).
        sessionShim_ = false;
        sessionFolders_.clear();
        shimWatch_.reset();
        shimWarned_.clear();
        shimLogSeen_ = false;
        shimLogCheckedMs_ = 0;
        if (shimTarget()) {
            std::string error;
            const ShimInputs inputs = shimInputs();
            const std::string target = picker_->profileName();
            if (!games_->prepareStart(target,
                                      [&inputs, &target](const std::filesystem::path& folder) {
                                          return shimAxesFor(folder, inputs.config, inputs.games, target);
                                      },
                                      sessionFolders_, error)) {
                WE_LOG_WARN(log(), "Start: {}", error);
                notice("Cannot start", error);
                return;
            }
            sessionShim_ = true;
            // Whole seconds: on Windows (MinGW's libstdc++) a file's write time comes in whole seconds, cut down,
            // so a shim log written in Start's own second would read as older than Start and not count.
            shimStartedFileTime_ =
                std::chrono::floor<std::chrono::seconds>(std::filesystem::file_time_type::clock::now());
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
        const ButtonOrder order = picker_->buttonOrder();
        const bool trueForce = picker_->trueForceAtStart();
        if (mapping_->dirty()) {
            WE_LOG_WARN(log(), "the Mapping tab has unsaved changes: Start uses the config as saved");
        }
        WE_LOG_INFO(log(), "Start pressed: {}{}; config {} ({})", picker_->name(),
                    ConfigPicker::hasTrueForce(picker_->target()) ? (trueForce ? ", TrueForce on" : ", TrueForce off")
                                                                  : "",
                    config.empty() ? std::string("built-in defaults") : config,
                    picker_->builtIn() ? "the default mapping" : "the user's own mapping");
        if (sessionShim_) {
            WE_LOG_INFO(log(), "Mode Shim: the shim is ready in {} game folder(s); start the game now",
                        sessionFolders_.size());
        }
        setup_->releaseWheel();   // the session opens the wheel now
        gameFollower_.reset();    // the new session counts its games' starts from 0
        wayFollower_.reset();
        pendingWayNotice_.reset();
        idleTab_ = IdleTab::Logs;  // the logs while the session comes up (it writes them); Live once it runs
        session_ = std::thread([this, config, virtualDevice, order, trueForce] {
            platform::nameThisThread("wheelbri session");
            int result = 1;
            try {
                result = settings_.runSession(host_, config, virtualDevice, order, trueForce);
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
                view_ = std::make_unique<MappingView>(*livePane_, info->sourceName, info->axes, info->buttons, *profile_,
                                                      info->mapper);
                view_->setFiles(info->files);
                view_->setDevices(info->devices, info->devicesWarning);
                view_->setAccessories(info->accessories);
                view_->setButtonOrder(info->buttonOrder);
                mappingShown_ = 0;   // the session's count starts again with it
                view_->setHeaderChars(sessionShim_ ? kHeaderCharsShim : kHeaderChars);
                view_->setShimMode(sessionShim_);
                view_->setSessionLine(std::string("Target ") + ConfigPicker::targetName(picker_->target()) +
                                      "  Mode " + ConfigPicker::wayName(picker_->way()));
                // Start: the Live tab once the session runs (the logs showed while it came up), unless the user
                // went to another tab meanwhile; the wheel back after a loss: the tab that was showing.
                view_->setTab(hadView                           ? previousTab
                              : idleTab_ == IdleTab::Mapping   ? MappingView::Tab::Mapping
                              : idleTab_ == IdleTab::Games     ? MappingView::Tab::Games
                              : idleTab_ == IdleTab::Setup     ? MappingView::Tab::WheelSetup
                                                               : MappingView::Tab::Live);
            }
            if (state_ == State::Starting) {   // not again when the session carries on after the wheel came back
                state_ = State::Running;
                runningSince_ = platform::nowNs();
                everRan_ = true;
            }
        }
        if (running() && sessionDone_) {
            session_.join();
            const bool wasStopping = state_ == State::Stopping;
            // A session the user stopped is not a failed Start, whatever its code (the log says "stopped" too).
            lastResult_ = wasStopping ? 0 : exitCode_.load();
            state_ = State::Stopped;
            idleTab_ = showingSetup()                                                 ? IdleTab::Setup
                       : showingGames()                                                ? IdleTab::Games
                       : showingMapping()                                              ? IdleTab::Mapping   // Live too
                                                                                       : IdleTab::Logs;
            view_.reset();
            info_.reset();
            if (lastResult_ == 0) {
                WE_LOG_INFO(log(), "the service stopped");
            } else {
                WE_LOG_WARN(log(), "the service could not run (exit code {}); the lines above say why", *lastResult_);
            }
            // A shim session that worked (the shim was heard): the shims stay in the games' folders unless the user
            // says to delete them, asked unless they said not to ask again (not when the window is closing: they stay).
            if (sessionShim_ && shimWatch_.heard() && !quitting_) atStopShims();
            sessionShim_ = false;
            sessionFolders_.clear();
        }
    }

    void drawWindow() {
        window_->begin(kBackground);
        window_->setOrigin(0, kViewTop);
        // The notice above the tab bar: the views end their content above it (UiWindow::bottomReserve).
        alert_ = alert();
        window_->setBottomReserve(alert_.text.empty() ? 0.0F : kAlertHeight);
        placePanes();   // after the reserve: the panes carry it
        if (splitShown()) {
            // Side by side: the editor left, the live view (or, stopped, what it will be) right, one tab bar.
            mapping_->draw();
            const float half = livePane_->left();
            window_->fill(half, 0, window_->width() - half, tabBarTop(), kBackground);   // over the editor's overflow
            if (view_ != nullptr && info_) {
                if (const auto frame = host_.latest(); frame && frame->ready) {
                    followMapping(*view_, mappingShown_, *frame);
                    view_->setTelemetry(frame->telemetry);
                    view_->setOldFiles(frame->oldFiles, frame->oldFilesFailed);
                    view_->drawLivePage(frame->inputs, frame->state, frame->report, ffbDisplay(*info_, *frame));
                }
                window_->fill(half, 0, 1, tabBarTop(), style::kRule);
                livePane_->place(0, window_->width());   // the tab bar is the whole window's
                view_->drawTabBar();
                placePanes();
            } else {
                drawPlaceholder();
                drawIdleTabs();
            }
        } else if (showingSetup()) {
            setup_->draw();
            if (view_ != nullptr) {
                view_->drawTabBar();
            } else {
                drawIdleTabs();
            }
        } else if (showingEditor()) {
            mapping_->draw();   // the Mapping tab's editor, or the Games tab (running too: locked)
            if (view_ != nullptr) {
                view_->drawTabBar();
            } else {
                drawIdleTabs();
            }
        } else if (view_ != nullptr && info_) {
            if (const auto frame = host_.latest(); frame && frame->ready) {
                followMapping(*view_, mappingShown_, *frame);
                draw(ffbDisplay(*info_, *frame), *view_, *frame);
            }
        } else if (view_ == nullptr) {
            logs_->drawLogsPage();   // stopped or starting: the same live log, before the session shows itself
            drawIdleTabs();
        }
        drawControls();   // every tab
        drawVersion();
        // The editor's hover text over everything it shares the window with (the shim's game row), under the panels.
        if (showingEditor() && !popup_) mapping_->drawTooltip();
        if (showingSetup() && !popup_) setup_->drawHoverText();
        if (popup_) drawPopup();
        window_->present();
    }

    // The Games tab, from the Mapping tab's game row (its Games button), stopped or running.
    void openGamesTab() {
        if (view_ != nullptr) {
            view_->setTab(MappingView::Tab::Games);
        } else {
            idleTab_ = IdleTab::Games;
        }
    }

    // What the session bar shows: the target, the mode, the game (the shim's, or what runs), and whether that game does
    // not support the target ([game] not_for).
    struct SessionBar {
        std::string target;
        std::string mode;
        std::string game;
        bool gameWarning = false;
        std::string advice;   // targetAdvice for that game, when gameWarning
    };
    [[nodiscard]] SessionBar sessionBar() {
        SessionBar bar;
        bar.target = ConfigPicker::targetName(picker_->target());
        bar.mode = ConfigPicker::wayName(picker_->way());
        const std::uint64_t now = platform::nowMs();
        if (shimTarget()) {
            // Mode Shim: no game is picked. Running: the known game WheelBri sees (and whether it supports the
            // target); stopped, or no game yet: how many are ready for it.
            const auto frame = running() ? host_.latest() : std::nullopt;
            if (frame && !frame->gameId.empty()) {
                if (frame->gameId != barKey_ || now - barReadMs_ >= 2000) {
                    barKey_ = frame->gameId;
                    barReadMs_ = now;
                    barAdvice_.clear();
                    std::vector<GameProfile> games(gameProfiles().begin(), gameProfiles().end());
                    for (GameProfile& game : loadUserGames()) games.push_back(std::move(game));
                    for (const GameProfile& game : games) {
                        if (game.id == frame->gameId) {
                            barAdvice_ = targetAdvice(game, picker_->profileName()).value_or("");
                        }
                    }
                }
                bar.game = frame->game.empty() ? frame->gameId : frame->game;
                bar.gameWarning = !barAdvice_.empty() || frame->game == shimWarned_;
                bar.advice = barAdvice_;
            } else {
                barKey_.clear();
                barAdvice_.clear();
                const std::size_t n = games_->folders().size();
                bar.game = n == 0       ? std::string("no game")
                           : running() ? std::string("waiting for a game")
                                       : std::to_string(n) + (n == 1 ? " game" : " games");
                bar.gameWarning = n == 0;
            }
        } else {
            bar.game = "every game";
            if (const auto frame = host_.latest(); running() && frame && !frame->game.empty()) bar.game = frame->game;
        }
        return bar;
    }

    // The greyed Start, clicked: what it still needs, each with where to do it.
    void showChecklist() {
        std::string text;
        const auto line = [&text](const bool done, const std::string& what) {
            text += std::string(done ? "ok    " : "TO DO ") + what + "\n";
        };
        const bool noRig = needsProblem_ == kNoRig || rigDevices_ == 0;
        line(!noRig, noRig ? "Rig: name your controls in the Rig tab (R)"
                           : "Rig: " + rigName_ + " (" + std::to_string(rigDevices_) + " device" +
                                 (rigDevices_ == 1 ? "" : "s") + ")");
        const bool mappingProblem = !needsProblem_.empty() && needsProblem_ != kNoRig;
        line(!mappingProblem && !noRig, mappingProblem ? "Mapping: " + needsProblem_
                                        : "Mapping: " + picker_->name() +
                                              (picker_->builtIn() ? " (WheelBri's default)" : " (yours)"));
        if (!needsKnown_) {
            line(false, "Devices: still looking for them (a moment)");
        } else if (!missing_.empty()) {
            std::string names;
            for (const std::string& label : missing_) names += (names.empty() ? "" : ", ") + label;
            line(false, "Devices: switch on or plug in " + names);
        } else {
            line(true, "Devices: every one the mapping uses is connected");
        }
        if (shimTarget()) {
            const std::size_t n = games_->folders().size();
            line(n > 0, n > 0 ? "Games: " + std::to_string(n) + " listed; Start puts the shim in each one's folder"
                              : "Games: add at least one on the Games tab (G): its folder gets the shim");
        } else {
            line(!helperMissing_, helperMissing_ ? "WheelBri Helper: not running (run the setup again)"
                                                 : "WheelBri Helper: running");
        }
        if (lastResult_ && *lastResult_ != 0) text += "\nThe last Start failed: the Logs tab (L) says why.\n";
        popup_ = Popup{"Before Start", wrap(text), "OK", "", [] { return true; }, nullptr, false, {}, false};
    }

    // The version and the commit it was built from, in the top-right corner of every tab (Version.hpp).
    void drawVersion() {
        static const std::string version = buildVersion();
        // Side by side: at the editor's right edge, where it keeps room for it (kVersionReserve).
        const float right = splitShown() ? editorPane_->width() : window_->width();
        window_->text(right - 4 - static_cast<float>(version.size()) * ui::kCharWidth, 6, version, kDim);
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
            case State::Stopping: {
                // What Stop still does (the hide filter out of the wheel's stack), so it does not look stuck.
                const std::string step = host_.currentStep();
                status = "Stopping " + clock((now - stoppingSince_) / platform::kNsPerSecond) +
                         (step.empty() ? std::string() : ": " + step);
                colour = kCaution;
                break;
            }
        }
        // Between the last tab button and Start: what is left of the bar (3 tabs while stopped, up to 5 while running).
        const float tabsEnd = view_ != nullptr ? MappingView::tabsEnd()
                                               : idleTabButtons().back().x + idleTabButtons().back().w;
        const float room = startX() - 12 - tabsEnd - 12;
        // The session bar: what Start runs (or runs now), left of the status, when there is room for it next to the
        // status; the game is dropped first, then the bar.
        const SessionBar bar = sessionBar();
        const std::array<std::string, 2> chips{bar.target + " - " + bar.mode + " - " + bar.game,
                                               bar.target + " - " + bar.mode};
        std::string chip;
        for (const std::string& candidate : chips) {
            if (static_cast<float>(candidate.size() + status.size() + 3) * ui::kCharWidth + 12 <= room) {
                chip = candidate;
                break;
            }
        }
        status = fit(status, static_cast<std::size_t>(std::max(4.0F, room / ui::kCharWidth)));
        if (shimTarget() && showingMapping()) {
            games_->draw(gamesRowX(), gamesRowY(), state_ == State::Stopped && !popup_);
        }
        const float statusX = startX() - 12 - static_cast<float>(status.size()) * ui::kCharWidth;
        window_->text(statusX, tabBarTop() + 10, status, colour);
        chipW_ = 0;
        if (!chip.empty()) {
            const float w = static_cast<float>(chip.size()) * ui::kCharWidth + 12;
            chipX_ = statusX - 12 - w;
            chipW_ = w;
            const bool live = state_ == State::Running || state_ == State::Starting;
            const ui::Colour edge = bar.gameWarning ? kCaution : live ? kActive : style::kFieldEdge;
            window_->fill(chipX_, buttonsY(), w, MappingView::kTabHeight, style::kField);
            window_->outline(chipX_, buttonsY(), w, MappingView::kTabHeight, edge);
            window_->text(chipX_ + 6, tabBarTop() + 10, chip,
                          bar.gameWarning ? kCaution : live ? kActive : kHeading);
        }
        if (state_ == State::Stopped) {
            const Button start = startButton();
            start.draw(*window_, start.enabled);   // filled green when it can run
        } else {
            const Button stop = stopButton();
            if (stop.enabled) {
                window_->outline(stop.x, stop.y, stop.w, stop.h, kWarning);
                const float textW = static_cast<float>(std::string_view(stop.label).size()) * ui::kCharWidth;
                window_->text(stop.x + (stop.w - textW) / 2, stop.y + (stop.h - ui::kCharHeight) / 2, stop.label,
                              kWarning);
            } else {
                stop.draw(*window_, false);   // Stopping: greyed
            }
        }
        drawAlert();
        drawToast();
    }

    // A short message after something was done ("Mapping saved", "Hot swap: 2 games present the G920"): above the tab
    // bar's right end for 3 s, then gone; a new one replaces it.
    void toast(std::string text) {
        WE_LOG_DEBUG(log(), "toast: {}", text);
        toast_ = std::move(text);
        toastUntilMs_ = platform::nowMs() + 3000;
    }
    // The notice above the tab bar, on every tab: what blocks Start (stopped) or what is wrong (running), said as what
    // to do; the status in the bar keeps its short form. Empty text: no notice. `checklist`: it has a "What's missing"
    // button (the greyed Start's list); `red`: broken, else amber (check this).
    struct Alert {
        std::string text;
        bool red = true;
        bool checklist = false;
    };
    [[nodiscard]] Alert alert() {
        Alert a;
        if (state_ == State::Stopped) {
            a.checklist = true;
            if (needsProblem_ == kNoRig) {
                a.text = "No rig yet: name your controls in the Rig tab (R).";
            } else if (needsProblem_ == kNoMapping) {
                a.text = "Nothing is mapped yet: map the rig in the Mapping tab (M).";
            } else if (!needsProblem_.empty()) {
                a.text = needsProblem_;
            } else if (helperMissing_) {
                a.text = "The WheelBri Helper is not running (Mode Virtual needs it): run the setup again.";
            } else if (needsKnown_ && !missing_.empty()) {
                std::string names;
                for (std::size_t i = 0; i < missing_.size(); ++i) names += (i == 0 ? "" : ", ") + missing_[i];
                a.text = names + (missing_.size() == 1 ? " is" : " are") +
                         " not connected: switch it on or plug it in.";
            } else if (needsKnown_ && shimTarget() && !games_->hasGame()) {
                a.text = "Mode Shim needs a game: add one on the Games tab (G).";
            } else if (needsKnown_ && lastResult_ && *lastResult_ != 0) {
                a.text = "The last Start failed: the Logs tab (L) says why.";
            }
            if (a.text.empty()) a.checklist = false;
            return a;
        }
        if (state_ != State::Running) return a;
        const auto frame = host_.latest();
        if (!frame) return a;
        if (frame->wheelLostMs > 0) {
            a.text = "A device of the rig was unplugged or switched off: plug it back in.";
        } else if (frame->gameIgnoresWheel) {
            a.text = "The game drives from a gamepad or the keyboard: close it, unplug the pad, start it again and "
                     "pick the Logitech wheel.";
        } else if (frame->gameFfbIdle) {
            a.text = "The game loaded its force feedback but never started it: Alt+Tab out of the game and back.";
            a.red = false;
        } else if (!frame->gameWarning.empty()) {
            a.text = frame->gameWarning;
        }
        return a;
    }

    // The notice: a strip across the window just above the tab bar (the room the views left, bottomReserve), red or
    // amber at its left edge, the text, and "What's missing" at its right when Start is blocked.
    void drawAlert() {
        alertButtonW_ = 0;
        if (alert_.text.empty()) return;
        const float x = 4;
        const float y = tabBarTop() - kAlertHeight + 2;
        const float w = window_->width() - 8;
        const ui::Colour edge = alert_.red ? kWarning : kCaution;
        window_->fill(x, y, w, 20, alert_.red ? style::kWarningBack : style::kCautionBack);
        window_->fill(x, y, 3, 20, edge);
        float textRoom = w - 20;
        if (alert_.checklist) {
            const std::string label = "What's missing";
            alertButtonW_ = static_cast<float>(label.size()) * ui::kCharWidth + 16;
            alertButtonX_ = x + w - 4 - alertButtonW_;
            alertButtonY_ = y + 2;
            style::button(*window_, alertButtonX_, alertButtonY_, alertButtonW_, 16, label, false, !popup_);
            textRoom -= alertButtonW_ + 8;
        }
        const auto chars = static_cast<std::size_t>(std::max(4.0F, textRoom / ui::kCharWidth));
        window_->text(x + 12, y + 6, fit(alert_.text, chars), kText);
    }

    void drawToast() {
        if (toast_.empty() || platform::nowMs() >= toastUntilMs_) return;
        const std::string text = fit(toast_, 60);
        const float w = static_cast<float>(text.size()) * ui::kCharWidth + 20;
        const float x = window_->width() - 4 - w;
        const float y = tabBarTop() - 26 - window_->bottomReserve();   // above the notice when there is one
        window_->fill(x, y, w, 20, kPopup);
        window_->outline(x, y, w, 20, kActive);
        window_->text(x + 10, y + 6, text, kActive);
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
        std::string check;                // a checkbox above the buttons ("Don't ask again"), empty for none
        bool checked = false;
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
                const bool atSpace = cut != std::string::npos && cut != 0;   // else a word longer than the line
                if (!atSpace) cut = kPopupChars;
                lines.push_back(paragraph.substr(0, cut));
                paragraph = paragraph.substr(atSpace ? cut + 1 : cut);   // the space it broke at goes
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
                   "WheelBri is putting its filter on your rig, so that games see only the " +
                       std::string(ConfigPicker::targetName(picker_->target())) + ". This happens once per "
                   "installation and can take up to a minute; meanwhile Windows reconnects the wheel, so it may "
                   "disappear for a moment. Please do not unplug it or press Stop.\nStart goes on by itself when it is "
                   "done; the next Starts are quick.", false);
            firstHidePopup_ = true;
        }
        if (firstHidePopup_ && (!popup_ || !first)) {
            if (popup_ && !first) popup_.reset();   // done: the popup goes by itself
            firstHidePopup_ = false;
        }
    }

    // A popup with OK: `warning` (the title in red) for what went wrong or needs doing, else (blue) for information.
    void notice(const std::string& title, const std::string& text, const bool warning = true) {
        popup_ = Popup{title, wrap(text), "OK", "", [] { return true; }, nullptr, warning, {}, false};
    }

    // The end of a shim session that worked: what to do with the shims in the games' folders. Asked, with "Don't ask
    // again" (the answer kept in app.ini: ShimGamesView::setAtStop), or done as the user said before.
    void atStopShims() {
        const std::vector<std::filesystem::path> folders = sessionFolders_;
        const auto removeAll = [this, folders](std::string& problems) {
            for (const std::filesystem::path& folder : folders) {
                std::string error;
                if (!games_->removeFrom(folder, error)) problems += pathToUtf8(folder) + ": " + error + "\n";
            }
            return problems.empty();
        };
        switch (games_->atStop()) {
            case ShimGamesView::AtStop::Keep:
                WE_LOG_INFO(log(), "the shims stay in {} game folder(s) (as asked before)", folders.size());
                return;
            case ShimGamesView::AtStop::Delete: {
                std::string problems;
                if (!removeAll(problems)) notice("Not deleted", "These shims could not be deleted:\n" + problems);
                WE_LOG_INFO(log(), "the shims were deleted from {} game folder(s) (as asked before)", folders.size());
                return;
            }
            case ShimGamesView::AtStop::Ask: break;
        }
        // The folders, at most kListed of them (a long list would push the answers out of the window).
        constexpr std::size_t kListed = 6;
        std::string where;
        for (std::size_t i = 0; i < folders.size() && i < kListed; ++i) where += "  " + pathToUtf8(folders[i]) + "\n";
        if (folders.size() > kListed) where += "  and " + std::to_string(folders.size() - kListed) + " more\n";
        popup_ = Popup{"Delete the shims from the games' folders?",
                       wrap("The session stopped. Kept, the shims are ready for the next Start; a game started "
                            "without WheelBri running then sees a wheel that does not move.\n" +
                            where + "(" + std::string(kShimDll) + " and " + kShimSettings + " in each)"),
                       "Delete them",
                       "Keep them",
                       [this, removeAll] {
                           if (popup_->checked) games_->setAtStop(ShimGamesView::AtStop::Delete);
                           std::string problems;
                           if (removeAll(problems)) return true;
                           popup_->lines = wrap("Not deleted:\n" + problems);
                           popup_->warning = true;
                           popup_->yes = "Try again";
                           return false;
                       },
                       [this] {
                           if (popup_->checked) games_->setAtStop(ShimGamesView::AtStop::Keep);
                           WE_LOG_INFO(log(), "the shims stay in the games' folders");
                       },
                       false,
                       "Don't ask again (the Games tab's Ask again brings this back)",
                       false};
    }

    [[nodiscard]] float popupHeight() const {
        return 44 + static_cast<float>(popup_->lines.size()) * 14 + 30 + (popup_->check.empty() ? 0 : 22);
    }
    // The checkbox's line (its box and label), above the buttons.
    [[nodiscard]] Button popupCheck() const {
        return {popupX() + 16, popupY() + popupHeight() - 50, kPopupW - 32, 16, popup_->check.c_str(),
                !popup_->check.empty()};
    }
    [[nodiscard]] Button popupYes() const {
        // With a second answer, the first (yes: it acts) at the right; the second (no: Escape) at the left.
        const float x = popupX() + 16 + (popup_->no.empty() ? 0.0F : 158.0F);
        return {x, popupY() + popupHeight() - 26, 150, 18, popup_->yes.c_str(), true};
    }
    [[nodiscard]] Button popupNo() const {
        return {popupX() + 16, popupY() + popupHeight() - 26, 150, 18, popup_->no.c_str(), !popup_->no.empty()};
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
        if (popupCheck().hit(click)) {
            popup_->checked = !popup_->checked;
        } else if (popupYes().hit(click)) {
            answerPopup(true);
        } else if (popupNo().hit(click)) {
            answerPopup(false);
        }
    }

    void drawPopup() {
        const float h = popupHeight();
        window_->fill(popupX(), popupY(), kPopupW, h, style::kBox);
        window_->outline(popupX(), popupY(), kPopupW, h, kHeading);
        window_->text(popupX() + 16, popupY() + 12, popup_->title, popup_->warning ? kWarning : kHeading);
        float y = popupY() + 34;
        for (const std::string& line : popup_->lines) {
            window_->text(popupX() + 16, y, line, kText);
            y += 14;
        }
        if (!popup_->check.empty()) {
            const Button check = popupCheck();
            window_->outline(check.x, check.y + 2, 12, 12, kHeading);
            if (popup_->checked) window_->fill(check.x + 3, check.y + 5, 6, 6, kHeading);
            window_->text(check.x + 20, check.y + 4, popup_->check, kText);
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
    std::uint64_t mappingShown_ = 0;   // UiFrame::mappingVersion the live view shows
    // Where the editor (and its Target row) and the live view draw: the whole window, or its halves (placePanes).
    std::unique_ptr<ui::PaneWindow> editorPane_;
    std::unique_ptr<ui::PaneWindow> livePane_;
    bool sideBySide_ = false;   // app.ini side_by_side
    std::unique_ptr<WheelSetupView> setup_;
    std::unique_ptr<ConfigPicker> picker_;
    std::unique_ptr<MappingEditView> mapping_;
    std::unique_ptr<ShimGamesView> games_;   // the shim's games (target G920 in-game shim)
    GameTabFollower gameFollower_;            // the Games tab opens when a profiled game starts
    WayNoticeFollower wayFollower_;           // a game that does not work this way: told once per start
    struct Notice {
        std::string title;
        std::string text;
    };
    std::optional<Notice> pendingWayNotice_;  // shown as a popup once no other popup is open
    bool sessionShim_ = false;                          // the session runs Mode Shim
    std::vector<std::filesystem::path> sessionFolders_; // the game folders Start readied the shim in
    ShimWatch shimWatch_;                               // the shim heard from, or a game running without it
    std::string shimWarned_;                            // the game the last "no shim activity" warning was about
    std::filesystem::file_time_type shimStartedFileTime_{};   // Start: a shim log written after it is the shim heard
    std::uint64_t shimLogCheckedMs_ = 0;
    bool shimLogSeen_ = false;
    std::string hotSwapKey_;
    // The session bar (sessionBar, drawControls) and the checklist (showChecklist).
    std::string toast_;
    std::uint64_t toastUntilMs_ = 0;
    std::string barKey_;
    std::uint64_t barReadMs_ = 0;
    std::string barAdvice_;
    float chipX_ = 0;
    float chipW_ = 0;
    Alert alert_;               // the notice above the tab bar this frame (App::alert)
    float alertButtonX_ = 0;    // its "What's missing" button, when drawn (width 0: none)
    float alertButtonY_ = 0;
    float alertButtonW_ = 0;
    std::string rigName_;
    std::size_t rigDevices_ = 0;   // the target and the games list (changes) the shims were last swapped for
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
