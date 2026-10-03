#include "App.hpp"

#include "ConfigPicker.hpp"
#include "MappingEditView.hpp"
#include "Platform.hpp"
#include "ServiceLogging.hpp"
#include "UiWindow.hpp"
#include "WheelSetupView.hpp"

#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/Logging.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <optional>
#include <tuple>
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
constexpr ui::Colour kCaution{206, 164, 58};
constexpr ui::Colour kDark{16, 18, 20};

// Window layout, in logical pixels: the panel (the tabs) fills the window. Only the Mapping tab has the controls, in
// its top right corner: the status, Start and Stop.
constexpr float kWidth = MappingView::kWidth;
constexpr float kViewTop = 0;
constexpr float kHeight = MappingView::kHeight;
constexpr float kControlsY = 0;
constexpr float kControlsHeight = 16;
constexpr float kButtonWidth = 68;
constexpr float kStopX = kWidth - 4 - kButtonWidth;
constexpr float kStartX = kStopX - 4 - kButtonWidth;
constexpr std::size_t kStatusChars = 20;   // between the tab's title and the toggle
constexpr int kWindowWidth = static_cast<int>(kWidth * 1.3F);  // 1.3x the canvas at 100 % (smaller to fit the screen)
constexpr int kWindowHeight = static_cast<int>(kHeight * 1.3F);
// The tab bar at the bottom of the panel (MappingView's), in the panel's own coordinates.
constexpr float kTabBarTop = MappingView::kHeight - 28;
constexpr float kTabWidth = 156;
constexpr float kSetupTabX = 4 + 4 * (kTabWidth + 6);   // where MappingView puts "Wheel setup (W)"
constexpr float kMappingTabX = 4;                         // ...and "Mapping (M)"
constexpr float kStatusTabX = 4 + kTabWidth + 6;          // the idle page's own, where the live view has Logs

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
        const ui::WindowInput input = window_->pump();
        if (input.close) {
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
        ++generation_;
        return true;
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
};

// The first error logged since Start, from every thread: the Status tab's "Why:" when a start failed.
class LogLines {
public:
    void add(const spdlog::level::level_enum level, const std::string& /*component*/, const std::string& message) {
        if (level < spdlog::level::err) {
            return;
        }
        std::string text = message;
        std::replace(text.begin(), text.end(), '\n', ' ');
        const std::lock_guard lock(mutex_);
        if (lastProblem_.empty()) {
            lastProblem_ = std::move(text);   // the first error since Start: the cause; later ones are often consequences
        }
    }

    // The latest error message (what made a start fail), empty if none since clearProblem().
    std::string lastProblem() {
        const std::lock_guard lock(mutex_);
        return lastProblem_;
    }
    void clearProblem() {
        const std::lock_guard lock(mutex_);
        lastProblem_.clear();
    }

private:
    std::mutex mutex_;
    std::string lastProblem_;
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
    ~App() { stopAndWait(std::chrono::seconds(30)); }

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
        stopAndWait(std::chrono::seconds(30));   // the session undoes everything before the app ends
        return 1;
    }

private:
    int runWindow() {
        // The listener outlives the app (logging goes on until the process ends): it shares the lines.
        logging::addListener([lines = lines_](const spdlog::level::level_enum level, const std::string& component,
                                              const std::string& message) { lines->add(level, component, message); });
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
                                             settings_.stateFile, settings_.configPath});
        setup_ = std::make_unique<WheelSetupView>(
            *window_, WheelSetupView::Settings{rigsFolder(), settings_.devicesFolder, settings_.stateFile});
        // The first time (no rig yet) the devices are defined first; then the mapping is where to go.
        idleTab_ = WheelSetupView::anyRig(rigsFolder()) ? IdleTab::Mapping : IdleTab::Setup;
        mapping_ = std::make_unique<MappingEditView>(*window_, rigsFolder(), settings_.stateFile, *picker_);
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

        bool quit = false;
        while (true) {
            const ui::WindowInput input = window_->pump();
            if (input.close && !quit) {
                quit = true;
                if (running()) {
                    WE_LOG_INFO(log(), "window closed: stopping the service first");
                    stop();
                }
            }
            const bool setup = showingSetup();
            const bool editingMapping = showingMappingEditor();
            for (const ui::Click& click : input.clicks) {
                // The Mapping tab's controls (top right); not over the editor's popups.
                if (showingMapping() && !(editingMapping && mapping_->popupOpen()) &&
                    click.y < kControlsY + kControlsHeight && click.x >= kStartX) {
                    if (startButton().hit(click)) start();
                    if (stopButton().hit(click)) stop();
                    continue;
                }
                // The panel, in its own coordinates; the tab bar at its bottom.
                const float y = click.y - kViewTop;
                if (y >= kTabBarTop) {
                    if (view_ != nullptr) {
                        view_->onClick(click.x, y);
                    } else if (y >= kTabBarTop + 4 && y < kTabBarTop + 24) {
                        if (click.x >= kMappingTabX && click.x < kMappingTabX + kTabWidth) idleTab_ = IdleTab::Mapping;
                        if (click.x >= kStatusTabX && click.x < kStatusTabX + kTabWidth) idleTab_ = IdleTab::Status;
                        if (click.x >= kSetupTabX && click.x < kSetupTabX + kTabWidth) idleTab_ = IdleTab::Setup;
                    }
                } else if (setup) {
                    setup_->onClick(click.x, y);
                } else if (editingMapping) {
                    mapping_->onClick(click.x, y);
                } else if (view_ != nullptr) {
                    view_->onClick(click.x, y);
                } else {
                    picker_->onClick(click.x, y);
                }
            }
            const bool picker = view_ == nullptr && idleTab_ == IdleTab::Status;   // where the config is picked
            if (editingMapping && mapping_->editingText()) {
                mapping_->onText(input.text);   // the name of a copy of a built-in config
                if (input.keys.find(ui::kEscape) != std::string::npos) mapping_->onKey(ui::kEscape);
            } else if (setup && setup_->editingText()) {
                setup_->onText(input.text);   // typing a name: letters are not shortcuts, Escape stops typing
                if (input.keys.find(ui::kEscape) != std::string::npos) setup_->onKey(ui::kEscape);
            } else if (picker && picker_->editingText()) {
                picker_->onText(input.text);   // a new config's name
                if (input.keys.find(ui::kEscape) != std::string::npos) picker_->onKey(ui::kEscape);
            } else {
                for (const char key : input.keys) {
                    if (key == ui::kEscape) {
                        if (setup) setup_->onKey(key);
                        if (picker) picker_->onKey(key);
                        if (editingMapping) mapping_->onKey(key);
                    } else if (view_ != nullptr) {
                        view_->onKey(key);
                    } else if (key == 'w' || key == 's' || key == 'm') {
                        idleTab_ = key == 'w' ? IdleTab::Setup : key == 'm' ? IdleTab::Mapping : IdleTab::Status;
                    }
                }
            }
            if (view_ != nullptr && input.wheel != 0) view_->onScroll(input.wheel);
            collectSession();
            updateSetup();
            if (showingMappingEditor()) mapping_->update();
            if (quit && !running()) {
                break;
            }
            if (window_->visible()) {
                drawWindow();
            }
            platform::sleepMs(16);
        }
        return 0;
    }

private:
    enum class State { Stopped, Starting, Running, Stopping };

    [[nodiscard]] bool running() const { return session_.joinable(); }

    // Whether the Wheel setup tab shows: the live view's tab while a session runs, the idle page's otherwise.
    [[nodiscard]] bool showingSetup() const {
        return view_ != nullptr ? view_->tab() == MappingView::Tab::WheelSetup : idleTab_ == IdleTab::Setup;
    }
    // The Mapping tab's editor: on the idle page (no session shows; while running it is the live view).
    [[nodiscard]] bool showingMappingEditor() const { return view_ == nullptr && idleTab_ == IdleTab::Mapping; }
    // Either Mapping tab (the editor while stopped, the live view's while running): the only place with Start / Stop.
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

    // The idle page's tab bar: Status and Wheel setup (where the live view has its Wheel setup tab).
    void drawIdleTabs() {
        window_->fill(0, kTabBarTop, kWidth, 1, kPanel);
        const std::tuple<float, const char*, IdleTab> tabs[] = {{kMappingTabX, "Mapping (M)", IdleTab::Mapping},
                                                                {kStatusTabX, "Status (S)", IdleTab::Status},
                                                                {kSetupTabX, "Wheel setup (W)", IdleTab::Setup}};
        for (const auto& [x, label, tab] : tabs) {
            const bool active = tab == idleTab_;
            window_->fill(x, kTabBarTop + 4, kTabWidth, 20, active ? kHeading : kPanel);
            const float labelWidth = static_cast<float>(std::string_view(label).size()) * ui::kCharWidth;
            window_->text(x + (kTabWidth - labelWidth) / 2, kTabBarTop + 10, label, active ? kDark : kText);
        }
    }

    Button startButton() const {
        return {kStartX, kControlsY, kButtonWidth, kControlsHeight, "Start", state_ == State::Stopped};
    }
    Button stopButton() const {
        return {kStopX, kControlsY, kButtonWidth, kControlsHeight, "Stop",
                state_ == State::Starting || state_ == State::Running};
    }

    void start() {
        if (running()) return;
        everRan_ = false;
        settings_.clearStop();
        host_.clear();
        view_.reset();
        info_.reset();
        sessionDone_ = false;
        lines_->clearProblem();
        state_ = State::Starting;
        startedAt_ = platform::nowNs();
        const std::string config = picker_->selected();
        if (mapping_->dirty()) {
            WE_LOG_WARN(log(), "the Mapping tab has unsaved changes: Start uses the config as saved");
        }
        WE_LOG_INFO(log(), "Start pressed: {}; config {}", settings_.mode,
                    config.empty() ? std::string("built-in defaults") : config);
        setup_->releaseWheel();   // the session opens the wheel now
        session_ = std::thread([this, config] {
            int result = 1;
            try {
                result = settings_.runSession(host_, config);
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
            const std::optional<MappingView::Tab> previousTab =
                view_ != nullptr ? std::optional(view_->tab()) : std::nullopt;
            view_.reset();   // before its profile goes
            profile_ = makeProfile(info->targetProfile);
            if (profile_ != nullptr) {
                view_ = std::make_unique<MappingView>(*window_, info->sourceName, info->axes, info->buttons, *profile_,
                                                      info->mapper);
                view_->setFiles(info->files);
                view_->setDevices(info->devices, info->devicesWarning);
                view_->setAccessories(info->accessories);
                // Start: the logs, to see it come up; the wheel back after a loss: the tab that was showing.
                view_->setTab(previousTab ? *previousTab : MappingView::Tab::Logs);
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
            lastRunStarted_ = everRan_;
            const bool wasStopping = state_ == State::Stopping;
            state_ = State::Stopped;
            idleTab_ = showingSetup()                                     ? IdleTab::Setup
                       : view_ != nullptr && view_->tab() == MappingView::Tab::Mapping ? IdleTab::Mapping
                                                                                     : IdleTab::Status;
            view_.reset();
            info_.reset();
            if (lastResult_ == 0 || wasStopping) {
                WE_LOG_INFO(log(), "the service stopped");
            } else {
                WE_LOG_WARN(log(), "the service could not run (exit code {}); the lines above say why", *lastResult_);
            }
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
        } else {
            drawIdle();
            drawIdleTabs();
        }
        if (showingMapping()) drawControls();
        window_->present();
    }

    // The Mapping tab's controls, top right: the status, Start, Stop. Nowhere else.
    void drawControls() {
        std::string status;
        ui::Colour colour = kText;
        const std::uint64_t now = platform::nowNs();
        switch (state_) {
            case State::Stopped:
                if (lastResult_ && *lastResult_ != 0) {
                    status = "Start failed";   // the Status tab says why
                    colour = kWarning;
                } else {
                    status = "Stopped";
                    colour = kDim;
                }
                break;
            case State::Starting:
                status = "Starting " + clock((now - startedAt_) / platform::kNsPerSecond);
                colour = kCaution;
                break;
            case State::Running:
                status = "Running " + clock((now - runningSince_) / platform::kNsPerSecond);
                colour = kActive;
                if (const auto frame = host_.latest(); frame && frame->wheelLostMs > 0) {
                    status = "WHEEL LOST " + clock(frame->wheelLostMs / 1000);
                    colour = kWarning;
                } else if (frame && frame->gameIgnoresWheel) {
                    status = "GAME NOT USING WHEEL";
                    colour = kWarning;
                }
                break;
            case State::Stopping:
                status = "Stopping " + clock((now - stoppingSince_) / platform::kNsPerSecond);
                colour = kCaution;
                break;
        }
        status = fit(status, kStatusChars);
        window_->text(kStartX - 6 - static_cast<float>(status.size()) * ui::kCharWidth, kControlsY + 4, status, colour);
        startButton().draw(*window_, false);
        stopButton().draw(*window_, false);
    }

    void drawIdle() {
        float y = 24;
        // Wrapped at spaces to the panel's width.
        const auto line = [&](const std::string& text, const ui::Colour colour) {
            constexpr std::size_t kChars = 86;
            std::size_t start = 0;
            do {
                std::size_t end = text.size();
                if (end - start > kChars) {
                    end = text.rfind(' ', start + kChars);
                    if (end == std::string::npos || end <= start) end = start + kChars;
                }
                window_->text(16, y, text.substr(start, end - start), colour);
                y += 14;
                start = end < text.size() && text[end] == ' ' ? end + 1 : end;
            } while (start < text.size());
        };
        line(state_ == State::Starting ? "STARTING" : state_ == State::Stopping ? "STOPPING" : "STOPPED", kHeading);
        y += 6;
        if (state_ == State::Stopped) {
            line("Start: reads the wheel, then " + settings_.mode + ".", kText);
            line("Then start the game. Stop (or closing this window) undoes all of it.", kText);
        } else {
            line("Opening the wheel and creating the virtual devices; this takes a few seconds.", kText);
        }
        y += 14;
        picker_->draw(16, y, state_ == State::Stopped);
        y += ConfigPicker::kHeight;
        line("Rigs    " + rigsFolder().string() + " (the Wheel setup tab's)", kDim);
        line("Logs    " + settings_.logFolder.string(), kDim);
        if (lastResult_) {
            y += 10;
            line(*lastResult_ == 0 ? "Last run: ended normally."
                 : lastRunStarted_ ? "Last run: ended with an error (exit code " + std::to_string(*lastResult_) + ")."
                                   : "Last run: did not start (exit code " + std::to_string(*lastResult_) + ").",
                 *lastResult_ == 0 ? kDim : kWarning);
            if (*lastResult_ != 0) {
                if (const std::string problem = lines_->lastProblem(); !problem.empty()) {
                    line("Why: " + problem, kText);
                }
            }
        }
    }

    const AppSettings& settings_;
    std::unique_ptr<ui::UiWindow> window_;
    AppHost host_;
    std::shared_ptr<LogLines> lines_ = std::make_shared<LogLines>();
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
    bool lastRunStarted_ = false;   // the last session that ended had been running
    std::optional<SessionInfo> info_;
    std::unique_ptr<IDeviceProfile> profile_;
    std::unique_ptr<MappingView> view_;
    std::unique_ptr<WheelSetupView> setup_;
    std::unique_ptr<ConfigPicker> picker_;
    std::unique_ptr<MappingEditView> mapping_;
    enum class IdleTab { Mapping, Status, Setup };
    IdleTab idleTab_ = IdleTab::Status;   // what shows while no session does
};

}  // namespace

std::unique_ptr<UiHost> makeSessionWindow(std::string configPath) {
    return std::make_unique<SessionWindow>(std::move(configPath));
}

int runApp(const AppSettings& settings) {
    App app(settings);
    return app.run();
}

}  // namespace wheelbri::service
