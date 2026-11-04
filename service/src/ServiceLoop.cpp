#include "ServiceLoop.hpp"

#include "AppLogLevel.hpp"

#include "wheelbri/Logging.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <utility>

namespace wheelbri::service {

namespace {

constexpr std::uint64_t kNsPerMs = 1'000'000;
constexpr std::uint64_t kNsPerSecond = 1'000'000'000;
constexpr std::uint64_t kStatsWindowNs = 10 * kNsPerSecond;
constexpr std::uint64_t kUsageLogNs = 10 * 60 * kNsPerSecond;   // force feedback usage table in the log
constexpr std::uint64_t kWakeLogNs = 60 * kNsPerSecond;         // the loop's wake-ups in the log
// A pass this long means the game gets no fresh input; it is logged at once, with where the time went.
constexpr std::uint64_t kStallNs = 50 * kNsPerMs;
constexpr std::uint64_t kPrintEveryNs = 100'000'000;   // --print: 10 lines a second

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service");
    return logger;
}

// Live console view for --print (user interface, deliberately not a log line).
void printLive(const Pipeline& pipeline) {
    const auto& raw = pipeline.lastRaw();
    const auto& state = pipeline.lastState();
    const auto& report = pipeline.lastReport();
    std::printf("\rraw %+.3f %+.3f %+.3f %+.3f | steer %+.3f thr %.2f brk %.2f clt %.2f btn %08x hat %2d"
                " | target steer %5d thr %5d brk %5d   ",
                raw.axes[0], raw.axes[1], raw.axes[2], raw.axes[3], state.steering, state.throttle, state.brake,
                state.clutch, state.buttons, state.hat, report.steering, report.throttle, report.brake);
    std::fflush(stdout);
}

// Brings `files` up to date. On the loop's thread: the paths are copied (which allocates) only when they changed (a
// new day's files), not every frame.
void updateTelemetryFiles(TelemetryFiles& files, const TelemetryRecorder* recorder) {
    files.on = recorder != nullptr;
    if (recorder == nullptr) {
        files.rows = 0;
        files.ffbRows = 0;
        return;
    }
    if (files.samplesPath != recorder->samplesPath()) files.samplesPath = recorder->samplesPath();
    if (files.ffbPath != recorder->ffbPath()) files.ffbPath = recorder->ffbPath();
    files.rows = recorder->rowsWritten();
    files.ffbRows = recorder->ffbRowsWritten();
}

// Where the time of one pass of the loop goes, to find the cause of a slow pass.
struct TickStages {
    enum Stage { Pipeline, Ffb, Telemetry, Ui, kCount };
    static constexpr std::array<const char*, kCount> kNames = {"wheel input and state to game", "force feedback",
                                                                "telemetry", "window copy"};
    std::array<std::uint64_t, kCount> ns{};

    // Adds the time from `mark` to `now` to `stage` and moves `mark` to `now`.
    void lap(const Stage stage, std::uint64_t& mark, const std::uint64_t now) {
        ns[stage] += now - mark;
        mark = now;
    }

    // "window 3.70 ms, force feedback 0.05 ms" (the biggest first, only the ones that matter).
    [[nodiscard]] std::string describe() const {
        std::array<int, kCount> order{};
        for (int i = 0; i < kCount; ++i) {
            order[i] = i;
        }
        std::sort(order.begin(), order.end(), [this](const int a, const int b) { return ns[a] > ns[b]; });
        std::string text;
        for (const int i : order) {
            if (ns[i] >= 20'000 || text.empty()) {   // 0.02 ms and up
                char part[64];
                std::snprintf(part, sizeof(part), "%s%s %.2f ms", text.empty() ? "" : ", ", kNames[i],
                              static_cast<double>(ns[i]) / 1e6);
                text += part;
            }
        }
        return text;
    }
};

}  // namespace

ServiceLoop::ServiceLoop(Parts parts)
    : pipeline_(parts.pipeline),
      system_(parts.system),
      shared_(parts.shared),
      targetName_(std::move(parts.targetName)),
      accessoryNames_(std::move(parts.accessoryNames)),
      ffb_(parts.ffb),
      telemetry_(parts.telemetry),
      print_(parts.print),
      window_(parts.window),
      control_(parts.settings) {}

std::string ServiceLoop::pollingNote() const {
    return std::to_string(control_.settings().pollNs / kNsPerMs) + " ms";
}

void ServiceLoop::addDevice(const bool notifies, const std::string& name) {
    if (!notifies) {
        WE_LOG_WARN(log(), "loop: '{}' gives no notification of new input (DirectInput polls it): the loop polls it "
                           "every {}",
                    name, pollingNote());
        control_.pollBecause(PollReason::NoNotification);
        return;
    }
    wakeReasons_.push_back(WakeReason::Wheel);
}

void ServiceLoop::addGameFfbSocket(const bool waitable) {
    if (!waitable) {
        WE_LOG_WARN(log(), "loop: cannot wait for force feedback packets: the loop reads them every {}", pollingNote());
        control_.pollBecause(PollReason::SocketNotWaitable);
        return;
    }
    wakeReasons_.push_back(WakeReason::GameFfb);
}

void ServiceLoop::logStart() const {
    if (control_.polling()) {
        WE_LOG_INFO(log(), "running on events, and polling every {} ({}); press Ctrl+C to stop", pollingNote(),
                    describe(control_.pollReason()));
    } else {
        WE_LOG_INFO(log(), "running on events: it wakes when the wheel or the game has something new, else every "
                           "{} ms at most; press Ctrl+C to stop",
                    control_.settings().wake.idleNs / kNsPerMs);
    }
}

void ServiceLoop::watchTheGame() {
    if (ffb_ == nullptr) return;
    const WheelState& state = pipeline_.lastState();
    const bool pedal = state.throttle > 0.1F || state.brake > 0.1F;
    const FfbController& controller = ffb_->controller();
    const std::uint64_t nowMs = system_.nowNs() / kNsPerMs;
    if (const auto change = gameUse_.update(nowMs, pedal, controller.stats().packets)) {
        if (*change) {
            WE_LOG_WARN(log(), "the game is NOT USING the {}: pedals pressed for {} s and it has never "
                               "played force feedback on it, so it drives from another controller (a "
                               "gamepad or the keyboard). Close the game "
                               "with WheelBri running, start it again and pick the wheel in its controls "
                               "settings",
                        targetName_, GameUseWatch::kPedalMs / 1000);
        } else {
            WE_LOG_INFO(log(), "the game uses the {} now (its force feedback arrives)", targetName_);
        }
    }
    if (const auto change =
            ffbIdle_.update(nowMs, pedal, controller.lastingLoaded(), controller.lastingWanted())) {
        if (*change) {
            WE_LOG_WARN(log(), "the game loaded its force feedback ({} effects) but has started none of "
                               "them while you drove for {} s, so the wheel stays limp. Alt-tab out of "
                               "the game and back once: it then starts them (NFS 2015 does this at the "
                               "start of some races)",
                        controller.lastingLoaded(), FfbIdleWatch::kPedalMs / 1000);
        } else {
            WE_LOG_INFO(log(), "the game's force feedback plays again (or it unloaded it)");
        }
    }
}

void ServiceLoop::writeTelemetry() {
    if (telemetry_ == nullptr) return;
    const std::uint64_t now = system_.nowNs();
    if (!telemetry_->due(now)) return;
    TelemetrySample sample;
    sample.nowNs = now;
    sample.sequence = pipeline_.sequence();
    sample.raw = pipeline_.lastRaw();
    sample.state = pipeline_.lastState();
    sample.report = pipeline_.lastReport();
    sample.ffb = ffb_ != nullptr ? &ffb_->controller() : nullptr;
    sample.periodUs = static_cast<double>(lastPeriodNs_) / 1000.0;
    sample.workUs = static_cast<double>(lastWorkNs_) / 1000.0;
    telemetry_->writeSample(sample);
}

void ServiceLoop::handFrame() {
    // Never wait for the window: if it is copying right now, skip this frame.
    const std::unique_lock lock(shared_.frameMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    UiFrame& frame = shared_.frame;
    frame.raw = pipeline_.lastRaw();
    frame.inputs = pipeline_.lastInputs();
    frame.state = pipeline_.lastState();
    frame.report = pipeline_.lastReport();
    frame.ffb.reset();
    if (ffb_ != nullptr) {
        frame.ffb.emplace(ffb_->controller());
    }
    updateTelemetryFiles(frame.telemetry, telemetry_);
    frame.gameIgnoresWheel = gameUse_.warning();
    frame.gameFfbIdle = ffbIdle_.warning();
    frame.ready = true;
}

void ServiceLoop::logStats(const std::uint64_t tickEnd, std::uint64_t& windowStart) {
    if (tickEnd - windowStart < kStatsWindowNs) return;
    // Once a minute: how often it woke and why.
    if (tickEnd - lastWakeLogNs_ >= kWakeLogNs) {
        WE_LOG_INFO(log(), "loop: {}; {}{}; since start, notifications missed {}, late {}; packet #{}, {} "
                           "publish failures",
                    wakeStats_.describe(tickEnd - lastWakeLogNs_), control_.polling() ? "polling: " : "on events",
                    control_.polling() ? describe(control_.pollReason()) : "", control_.missedNotifications(),
                    control_.lateNotifications(), pipeline_.sequence(), pipeline_.publishFailures());
        wakeStats_.reset();
        lastWakeLogNs_ = tickEnd;
    }
    if (ffb_ != nullptr) {
        ffb_->logStatsIfChanged();
        if (tickEnd - lastUsageLogNs_ >= kUsageLogNs) {
            ffb_->logUsage();
            lastUsageLogNs_ = tickEnd;
        }
    }
    windowStart = tickEnd;
}

void ServiceLoop::waitForNews(const std::uint64_t tickStart) {
    // Until something new arrives, or what must run on time is due (core EventLoopControl, WakePlanner; every
    // millisecond while the loop polls); then at least minGapNs after this pass started (a source that signals
    // without pause cannot spin the loop). It polls as well, for the rest of the session, when the timer keeps
    // finding new input (the devices' notifications miss it), and only sleeps once the wait itself failed.
    EventLoopControl::Pass pass;
    pass.startNs = tickStart;
    pass.inputChanged = pipeline_.inputChanged();
    pass.windowOpen = window_;
    pass.forcesPlaying = ffb_ != nullptr && ffb_->controller().stats().activeSlots > 0;
    control_.afterPass(pass);
    const std::uint64_t deadline = control_.deadlineNs();
    int ready = kWaitTimedOut;
    if (control_.canWait()) {
        ready = system_.waitUntil(deadline);
    } else if (const std::uint64_t now = system_.nowNs(); deadline > now) {
        system_.sleepNs(deadline - now);
    }
    const std::uint64_t woke = system_.nowNs();
    if (woke > deadline && woke - deadline >= kStallNs) {
        // Not our work: the system let the loop sleep far longer than asked (busy, or asleep: a Steam Deck
        // with its screen off sleeps, and a few seconds late is that).
        static logging::RateLimiter limiter(std::chrono::seconds(5));
        WE_LOG_WARN_LIMITED(log(), limiter,
                            "loop: woke {:.0f} ms after its deadline: the system did not run the service for "
                            "that long ({})",
                            static_cast<double>(woke - deadline) / 1e6,
                            woke - deadline >= 2'000'000'000ULL ? "asleep?" : "busy?");
    }
    const EventLoopControl::Woke next = control_.afterWait(ready, wakeReasons_, woke);
    if (next.startedPolling) {
        if (!pollingSinceNs_) pollingSinceNs_ = woke;
        if (control_.pollReason() == PollReason::WaitFailed) {
            WE_LOG_WARN(log(), "loop: cannot wait for the devices ({}); the loop polls every {} for the rest "
                               "of this session",
                        system_.waitFailureText(), pollingNote());
        } else {
            WE_LOG_WARN(log(), "loop: the timer found new input {} times within {} s that no notification "
                               "announced: the devices' notifications cannot be trusted; the loop also polls "
                               "every {} for the rest of this session",
                        control_.settings().missThreshold, control_.settings().missWindowNs / kNsPerSecond,
                        pollingNote());
        }
    }
    if (next.sleepNs > 0) system_.sleepNs(next.sleepNs);
}

int ServiceLoop::run() {
    system_.threadStarted();
    logStart();
    // The window's copy of the state, kWindowFramesPerSecond (60) times a second, as often as the window draws.
    Periodic frameDue(control_.settings().wake.windowNs);
    Periodic printDue(kPrintEveryNs);
    const std::uint64_t loopStart = system_.nowNs();
    std::uint64_t lastTickStart = loopStart;
    std::uint64_t windowStart = loopStart;
    lastUsageLogNs_ = loopStart;
    lastWakeLogNs_ = loopStart;
    std::uint64_t ticks = 0;
    int result = 0;

    while (!system_.stopRequested() && !shared_.stop) {
        const std::uint64_t tickStart = system_.nowNs();
        TickStages stages;
        std::uint64_t mark = tickStart;
        if (!pipeline_.tick(tickStart)) {
            const int lost = pipeline_.lostDevice();
            const auto index = static_cast<std::size_t>(lost - 1);
            WE_LOG_ERROR(log(), "{} lost after {} ticks (unplugged, or a USB reset?)",
                         lost <= 0 ? std::string("physical wheel")
                                   : "accessory '" +
                                         (index < accessoryNames_.size() ? accessoryNames_[index] : std::string("?")) +
                                         "'",
                         ticks);
            result = kWheelLost;
            break;
        }
        system_.passDone(tickStart, control_.wokeBy());
        stages.lap(TickStages::Pipeline, mark, system_.nowNs());
        ++ticks;
        if (ffb_ != nullptr) ffb_->pump(system_.nowNs());
        watchTheGame();
        if (const LogLevel level = AppLogLevel::shared().current();
            levelBeacon_.due(system_.nowNs() / kNsPerMs, level)) {
            system_.publishLogLevel(level);
        }
        stages.lap(TickStages::Ffb, mark, system_.nowNs());
        writeTelemetry();
        stages.lap(TickStages::Telemetry, mark, system_.nowNs());
        if (print_ && printDue.due(tickStart)) {
            printLive(pipeline_);
        }
        if (window_ && frameDue.due(tickStart)) handFrame();
        const std::uint64_t tickEnd = system_.nowNs();
        stages.lap(TickStages::Ui, mark, tickEnd);

        lastPeriodNs_ = tickStart - lastTickStart;
        lastTickStart = tickStart;
        lastWorkNs_ = tickEnd - tickStart;
        wakeStats_.record(control_.wokeBy(), tickEnd - tickStart);
        if (tickEnd - tickStart >= kStallNs) {
            static logging::RateLimiter limiter(std::chrono::seconds(5));
            WE_LOG_WARN_LIMITED(log(), limiter,
                                "loop: one tick took {:.0f} ms, the game got no fresh input meanwhile: {}",
                                static_cast<double>(tickEnd - tickStart) / 1e6, stages.describe());
        }
        logStats(tickEnd, windowStart);
        waitForNews(tickStart);
    }
    if (print_) {
        std::puts("");
    }
    WE_LOG_INFO(log(), "loop, since the last report: {}; packet #{}",
                wakeStats_.describe(system_.nowNs() - lastWakeLogNs_), pipeline_.sequence());
    if (ffb_ != nullptr) {
        ffb_->logUsage();
    }
    if (system_.stopRequested()) {
        WE_LOG_INFO(log(), "stopped by signal after {} ticks", ticks);
    }
    return result;
}

}  // namespace wheelbri::service
