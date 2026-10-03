#include "App.hpp"

#include "Platform.hpp"
#include "UiWindow.hpp"

#include "wheelemu/Logging.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

namespace wheelemu::service {

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

// Window layout, in logical pixels: the toolbar, the live view, the log lines.
constexpr float kWidth = MappingView::kWidth;
constexpr float kToolbarHeight = 40;
constexpr float kViewTop = kToolbarHeight;
constexpr float kLogTop = kViewTop + MappingView::kHeight + 6;
constexpr int kLogLines = 6;
constexpr float kLogLineHeight = 11;
constexpr float kHeight = kLogTop + 12 + kLogLines * kLogLineHeight + 4;
constexpr int kWindowWidth = static_cast<int>(kWidth * 1.3F);  // 1.3x the canvas at 100 % (smaller to fit the screen)
constexpr int kWindowHeight = static_cast<int>(kHeight * 1.3F);

void draw(const FfbDisplay& ffb, MappingView& view, const UiFrame& frame) {
    view.setTelemetry(frame.telemetry);
    view.setOldFiles(frame.oldFiles, frame.oldFilesFailed);
    view.draw(frame.raw, frame.state, frame.report, ffb);
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
    bool sessionStarted(const SessionInfo& info) override {
        // Called again when the session carries on after the wheel came back: keep the window, rebuild the view.
        if (window_ == nullptr) {
            std::string error;
            window_ = ui::openWindow("wheel-emulator: mapping", MappingView::kWidth, MappingView::kHeight,
                                     static_cast<int>(MappingView::kWidth * 1.3F),
                                     static_cast<int>(MappingView::kHeight * 1.3F), error);
            if (window_ == nullptr) {
                WE_LOG_ERROR(log(), "cannot open the mapping window: {}", error);
                return false;
            }
            WE_LOG_INFO(log(), "mapping window open ({})", window_->describe());
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
        return true;
    }

    bool frame(const UiFrame& frame) override {
        const ui::WindowInput input = window_->pump();
        if (input.close) {
            return false;
        }
        for (const char key : input.keys) {
            if (key == ui::kEscape) {
                return false;
            }
            view_->onKey(key);
        }
        for (const ui::Click& click : input.clicks) {
            view_->onClick(click.x, click.y);
        }
        if (input.wheel != 0) {
            view_->onScroll(input.wheel);
        }
        // Nothing to show: drawing a hidden window can block (e.g. while a game switches display mode).
        if (frame.ready && window_->visible()) {
            window_->begin(kBackground);
            draw(ffbDisplay(info_, frame), *view_, frame);
            window_->present();
        }
        return true;
    }

private:
    std::unique_ptr<ui::UiWindow> window_;
    SessionInfo info_;
    std::unique_ptr<IDeviceProfile> profile_;
    std::unique_ptr<MappingView> view_;
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
        const std::lock_guard lock(mutex_);
        frame_.reset();
        frame_.emplace(frame);
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
    std::optional<UiFrame> frame_;
};

// The latest log lines, from every thread.
class LogLines {
public:
    struct Line {
        spdlog::level::level_enum level;
        std::string text;
    };

    void add(const spdlog::level::level_enum level, const std::string& component, const std::string& message) {
        if (level < spdlog::level::info) {
            return;
        }
        char stamp[16];
        const std::time_t now = std::time(nullptr);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &local);
        std::string text = std::string(stamp) + " " + component + ": " + message;
        std::replace(text.begin(), text.end(), '\n', ' ');
        const std::lock_guard lock(mutex_);
        if (level >= spdlog::level::err && lastProblem_.empty()) {
            lastProblem_ = message;   // the first error since Start: the cause; later ones are often consequences
        }
        lines_.push_back({level, std::move(text)});
        while (lines_.size() > kLogLines) {
            lines_.pop_front();
        }
    }

    std::deque<Line> copy() {
        const std::lock_guard lock(mutex_);
        return lines_;
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
    std::deque<Line> lines_;
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

// The parts of the devices line that are problems (written in capitals: "R3 NOT HIDDEN: ...", "NO XBOX PAD ..."),
// so they are what fits in the toolbar.
std::string problemsOf(const std::string& devices) {
    std::string problems;
    std::size_t start = 0;
    while (start < devices.size()) {
        std::size_t end = devices.find(", ", start);
        if (end == std::string::npos) end = devices.size();
        const std::string part = devices.substr(start, end - start);
        if (part.find("NOT ") != std::string::npos || part.find("NO ") != std::string::npos ||
            part.find("LOST") != std::string::npos) {
            problems += (problems.empty() ? "" : ", ") + part;
        }
        start = end + 2;
    }
    return problems.empty() ? devices : problems;
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
        window_ = ui::openWindow("wheel-emulator", static_cast<int>(kWidth), static_cast<int>(kHeight), kWindowWidth,
                                 kWindowHeight, error);
        if (window_ == nullptr) {
            WE_LOG_CRITICAL(log(), "cannot open the window: {}", error);
            return 1;
        }
        WE_LOG_INFO(log(), "app window open ({}); press Start", window_->describe());
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
            for (const ui::Click& click : input.clicks) {
                if (startButton().hit(click)) start();
                if (stopButton().hit(click)) stop();
                if (logsButton().hit(click)) openLogs();
                // Below the toolbar: the live view's tabs and lines (in its own coordinates).
                if (view_ != nullptr && click.y >= kViewTop) view_->onClick(click.x, click.y - kViewTop);
            }
            for (const char key : input.keys) {
                if (view_ != nullptr && key != ui::kEscape) view_->onKey(key);
            }
            if (view_ != nullptr && input.wheel != 0) view_->onScroll(input.wheel);
            collectSession();
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

    Button startButton() const { return {4, 8, 90, 24, "Start", state_ == State::Stopped}; }
    Button stopButton() const {
        return {100, 8, 90, 24, "Stop", state_ == State::Starting || state_ == State::Running};
    }
    Button logsButton() const { return {196, 8, 104, 24, "Open logs", true}; }

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
        WE_LOG_INFO(log(), "Start pressed: {}", settings_.mode);
        session_ = std::thread([this] {
            int result = 1;
            try {
                result = settings_.runSession(host_);
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

    void openLogs() {
        // The running session's folder (the config may have changed since the app started); made if missing,
        // since Explorer silently shows another folder for one that does not exist.
        const std::filesystem::path folder = info_ ? info_->files.logFolder : settings_.logFolder;
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        std::string error;
        if (ec) {
            const std::u8string name = folder.u8string();
            WE_LOG_WARN(log(), "cannot open the log folder {}: {}", std::string(name.begin(), name.end()), ec.message());
        } else if (!window_->openFolder(folder, error)) {
            WE_LOG_WARN(log(), "cannot open the log folder: {}", error);
        }
    }

    // A session that started showing itself, or one that ended.
    void collectSession() {
        if (const auto info = host_.newSession(seenGeneration_)) {
            info_ = info;
            view_.reset();   // before its profile goes
            profile_ = makeProfile(info->targetProfile);
            if (profile_ != nullptr) {
                view_ = std::make_unique<MappingView>(*window_, info->sourceName, info->axes, info->buttons, *profile_,
                                                      info->mapper);
                view_->setFiles(info->files);
                view_->setDevices(info->devices, info->devicesWarning);
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
        window_->setOrigin(0, 0);
        drawToolbar();
        window_->setOrigin(0, kViewTop);
        if (view_ != nullptr && info_) {
            if (const auto frame = host_.latest(); frame && frame->ready) {
                draw(ffbDisplay(*info_, *frame), *view_, *frame);
            }
        } else {
            drawIdle();
        }
        window_->setOrigin(0, 0);
        drawLog();
        window_->present();
    }

    void drawToolbar() {
        window_->fill(0, 0, kWidth, kToolbarHeight, kDark);
        startButton().draw(*window_, false);
        stopButton().draw(*window_, false);
        logsButton().draw(*window_, false);

        std::string status;
        ui::Colour colour = kText;
        const std::uint64_t now = platform::nowNs();
        switch (state_) {
            case State::Stopped:
                if (lastResult_ && *lastResult_ != 0) {
                    status = "Stopped: the last start failed (see the log below)";
                    colour = kWarning;
                } else {
                    status = "Stopped";
                    colour = kDim;
                }
                break;
            case State::Starting:
                status = "Starting... " + clock((now - startedAt_) / platform::kNsPerSecond);
                colour = kCaution;
                break;
            case State::Running:
                status = "Running " + clock((now - runningSince_) / platform::kNsPerSecond);
                colour = kActive;
                if (const auto frame = host_.latest(); frame && frame->wheelLostMs > 0) {
                    status = "WHEEL LOST " + clock(frame->wheelLostMs / 1000) + ": plug it in again";
                    colour = kWarning;
                }
                break;
            case State::Stopping: {
                const std::uint64_t seconds = (now - stoppingSince_) / platform::kNsPerSecond;
                // Long: something the session must not cut off (the hide installer changing the wheel's driver,
                // Windows removing a device). The log below says what.
                status = "Stopping... " + clock(seconds) + (seconds >= 10 ? " (finishing a step, see the log)" : "");
                colour = kCaution;
                break;
            }
        }
        window_->text(312, 10, fit(status, 50), colour);
        if (info_) {
            // A missing piece (the R3 not hidden, no Xbox pad) in red; otherwise which wheel is read.
            window_->text(312, 22, fit(info_->devicesWarning ? problemsOf(info_->devices) : info_->sourceName, 80),
                          info_->devicesWarning ? kWarning : kDim);
        }
        window_->fill(0, kToolbarHeight - 1, kWidth, 1, kPanel);
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
        y += 10;
        line("Config  " + settings_.configPath, kDim);
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

    void drawLog() {
        window_->fill(0, kLogTop - 4, kWidth, 1, kPanel);
        window_->text(4, kLogTop, "LOG", kHeading);
        window_->text(40, kLogTop, "the latest lines; Open logs for everything", kDim);
        float y = kLogTop + 12;
        for (const LogLines::Line& entry : lines_->copy()) {
            const ui::Colour colour = entry.level >= spdlog::level::err    ? kWarning
                                      : entry.level == spdlog::level::warn ? kCaution
                                                                           : kText;
            window_->text(4, y, fit(entry.text, 89), colour);
            y += kLogLineHeight;
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
};

}  // namespace

std::unique_ptr<UiHost> makeSessionWindow() { return std::make_unique<SessionWindow>(); }

int runApp(const AppSettings& settings) {
    App app(settings);
    return app.run();
}

}  // namespace wheelemu::service
