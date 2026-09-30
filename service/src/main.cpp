// wheelemu-service: reads the physical wheel, maps it and publishes the emulated wheel's state
// to the in-game shim over UDP loopback, plays the game's force feedback on the wheel, and records
// telemetry of both directions for later tuning.

#include "FfbSelfTest.hpp"
#include "Housekeeping.hpp"
#include "MappingView.hpp"
#include "SdlFfbSink.hpp"
#include "SdlInputSource.hpp"
#include "SdlRuntime.hpp"
#include "ServiceLogging.hpp"

#include "wheelemu/EmulatorConfig.hpp"
#include "wheelemu/FfbController.hpp"
#include "wheelemu/FfbPacket.hpp"
#include "wheelemu/Logging.hpp"
#include "wheelemu/LoopStats.hpp"
#include "wheelemu/Pipeline.hpp"
#include "wheelemu/Telemetry.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <array>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#ifndef WHEELEMU_VERSION
#define WHEELEMU_VERSION "dev"
#endif

namespace {

using wheelemu::logging::Logger;

constexpr Uint64 kStatsWindowNs = 10 * SDL_NS_PER_SECOND;

std::atomic<bool> gStop{false};

void onSignal(int) { gStop = true; }

const Logger& log() {
    static const Logger logger = wheelemu::logging::get("service");
    return logger;
}

struct Options {
    std::string configPath;
    bool list = false;
    bool print = false;
    bool ui = false;
    bool ffbTest = false;
    std::string oldFiles;                       // --old-files: overrides [retention] old_files
};

void printUsage() {
    std::puts(
        "usage: wheelemu-service [--config FILE] [--list] [--print] [--ui] [--ffb-test] [--old-files ACTION]\n"
        "  --config FILE  INI configuration (see config/moza-r3-as-g920.ini)\n"
        "  --list         list joysticks and exit\n"
        "  --print        show live raw and mapped values\n"
        "  --ui           open a window showing how every axis and button maps to the emulated wheel\n"
        "  --ffb-test     play a short, gentle force feedback sequence on the wheel and exit\n"
        "  --old-files delete|archive|keep\n"
        "                 logs and telemetry older than [retention] older_than_days (7): delete them (default),\n"
        "                 compress them (7-Zip, xz, zstd, zip or gzip, the best one found) or keep them\n"
        "logging: [logging] section of the config; per component with SPDLOG_LEVEL, e.g.\n"
        "  SPDLOG_LEVEL=\"info,core.pipeline=trace,service.ffb=debug\"");
}

bool parseOptions(const int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            options.configPath = argv[++i];
        } else if (arg == "--list") {
            options.list = true;
        } else if (arg == "--print") {
            options.print = true;
        } else if (arg == "--ui") {
            options.ui = true;
        } else if (arg == "--ffb-test") {
            options.ffbTest = true;
        } else if (arg == "--old-files" && i + 1 < argc) {
            options.oldFiles = argv[++i];
            if (!wheelemu::parseOldFilesAction(options.oldFiles)) {
                std::fprintf(stderr, "--old-files %s: expected delete, archive or keep\n", options.oldFiles.c_str());
                return false;
            }
        } else {
            printUsage();
            return false;
        }
    }
    return true;
}

struct LoadedConfig {
    wheelemu::EmulatorConfig config;
    std::vector<std::string> problems;
    bool fileMissing = false;
    std::string text;                           // the file as read, copied into telemetry headers
};

// Runs before logging exists (logging is configured by this file), so problems are collected, not logged.
LoadedConfig loadConfig(const std::string& path) {
    LoadedConfig result;
    if (path.empty()) {
        return result;
    }
    std::ifstream file(path);
    if (!file) {
        result.fileMissing = true;
        return result;
    }
    std::stringstream text;
    text << file.rdbuf();
    result.text = text.str();
    const auto parsed = wheelemu::IniDocument::parse(result.text);
    auto loaded = wheelemu::loadEmulatorConfig(parsed.document);
    result.config = std::move(loaded.config);
    result.problems = parsed.errors;
    result.problems.insert(result.problems.end(), loaded.errors.begin(), loaded.errors.end());
    return result;
}

void logEffectiveConfig(const wheelemu::EmulatorConfig& config) {
    WE_LOG_INFO(log(), "source_device='{}' target_profile='{}' state_port={} ffb_port={} loop_hz={} game_effects={} "
                       "ffb_invert={}",
                config.sourceDeviceMatch, config.targetProfile, config.statePort, config.ffbPort, config.loopHz,
                config.gameFfb, config.ffbInvert);
    WE_LOG_DEBUG(log(), "ffb gain={:.2f} max_magnitude={:.2f} max_slew={:.1f}/s", config.ffb.masterGain,
                 config.ffb.maxMagnitude, config.ffb.maxSlewPerSecond);
}

void listDevices() {
    const auto joysticks = wheelemu::service::listJoysticks();
    if (joysticks.empty()) {
        std::puts("no joysticks found");
    }
    for (const auto& joystick : joysticks) {
        std::printf("%04x:%04x  %s\n", joystick.vendorId, joystick.productId, joystick.name.c_str());  // vid:pid  name
        WE_LOG_INFO(log(), "joystick {:04x}:{:04x} '{}'", joystick.vendorId, joystick.productId, joystick.name);
    }
}

// Live console view for --print (user interface, deliberately not a log line).
void printLive(const wheelemu::Pipeline& pipeline) {
    const auto& raw = pipeline.lastRaw();
    const auto& state = pipeline.lastState();
    const auto& report = pipeline.lastReport();
    std::printf("\rraw %+.3f %+.3f %+.3f %+.3f | steer %+.3f thr %.2f brk %.2f clt %.2f btn %08x hat %2d"
                " | target steer %5d thr %5d brk %5d   ",
                raw.axes[0], raw.axes[1], raw.axes[2], raw.axes[3], state.steering, state.throttle, state.brake,
                state.clutch, state.buttons, state.hat, report.steering, report.throttle, report.brake);
    std::fflush(stdout);
}

void logLoopStats(const char* what, const wheelemu::LoopStats& stats, const wheelemu::Pipeline& pipeline) {
    const auto snapshot = stats.snapshot();
    const double targetUs = static_cast<double>(stats.targetPeriodNs()) / 1000.0;
    WE_LOG_INFO(log(),
                "loop: {}: {} ticks, period avg {:.1f} us / max {:.1f} us (target {:.0f}), work avg {:.1f} us / max "
                "{:.1f} us, {} overruns, packet #{}, {} publish failures",
                what, snapshot.ticks, snapshot.averagePeriodUs, snapshot.maxPeriodUs, targetUs, snapshot.averageWorkUs,
                snapshot.maxWorkUs, snapshot.overruns, pipeline.sequence(), pipeline.publishFailures());
}

// Where the time of one loop tick goes, to find the cause of a slow tick.
struct TickStages {
    enum Stage { Input, Pipeline, Ffb, Telemetry, Housekeeping, Ui, kCount };
    static constexpr std::array<const char*, kCount> kNames = {"wheel input", "state to game", "force feedback",
                                                                "telemetry", "old files", "window"};
    std::array<Uint64, kCount> ns{};

    // Adds the time since `mark` to `stage` and moves `mark` to now.
    void lap(const Stage stage, Uint64& mark) {
        const Uint64 now = SDL_GetTicksNS();
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
            if (ns[i] >= 20'000 || text.empty()) {  // 0.02 ms and up
                char part[64];
                std::snprintf(part, sizeof(part), "%s%s %.2f ms", text.empty() ? "" : ", ", kNames[i],
                              static_cast<double>(ns[i]) / 1e6);
                text += part;
            }
        }
        return text;
    }
};

// Loop timing is logged only when its health changes (checked every 10 s), not every window: a line
// when ticks start running late, one when they are on time again, and a summary at exit.
class LoopHealth {
public:
    explicit LoopHealth(const Uint64 periodNs) : window_(periodNs), sinceReport_(periodNs) {}

    void record(const Uint64 periodNs, const Uint64 workNs, const TickStages& stages) {
        window_.record(periodNs, workNs);
        sinceReport_.record(periodNs, workNs);
        if (workNs > slowestWorkNs_) {
            slowestWorkNs_ = workNs;
            slowest_ = stages;
        }
    }

    // End of a 10-second window.
    void endWindow(const wheelemu::Pipeline& pipeline) {
        const auto snapshot = window_.snapshot();
        const double targetUs = static_cast<double>(window_.targetPeriodNs()) / 1000.0;
        const bool late = snapshot.overruns > 0 || snapshot.maxWorkUs > targetUs;
        if (!reported_ || late != late_) {
            if (late) {
                WE_LOG_WARN(log(), "loop: ticks running late: {} overruns in 10 s, longest period {:.0f} us, longest "
                                   "work {:.0f} us (target {:.0f} us; that tick: {}); next report when this changes",
                            snapshot.overruns, snapshot.maxPeriodUs, snapshot.maxWorkUs, targetUs, slowest_.describe());
            } else {
                logLoopStats(reported_ ? "on time again, since the last report" : "on time", sinceReport_, pipeline);
            }
            reported_ = true;
            late_ = late;
            sinceReport_.reset();
        }
        window_.reset();
        slowestWorkNs_ = 0;
    }

    void logSummary(const wheelemu::Pipeline& pipeline) const {
        logLoopStats("since the last report", sinceReport_, pipeline);
    }

private:
    wheelemu::LoopStats window_;
    wheelemu::LoopStats sinceReport_;
    bool reported_ = false;
    bool late_ = false;
    Uint64 slowestWorkNs_ = 0;
    TickStages slowest_;
};

// A tick this long means the game gets no fresh input; it is logged at once, with where the time went.
constexpr Uint64 kStallNs = 50 * SDL_NS_PER_MS;

// Phase 3: the game's force feedback, sent by the in-game shim, limited and played on the physical wheel.
class GameFfb {
public:
    // Returns nullptr (and says why) when game force feedback is off or cannot run; input still works.
    static std::unique_ptr<GameFfb> open(const wheelemu::EmulatorConfig& config, SDL_Joystick* joystick,
                                         std::string& status) {
        if (!config.gameFfb) {
            status = "off ([ffb] game_effects = false)";
            WE_LOG_INFO(log(), "game force feedback off ([ffb] game_effects = false)");
            return nullptr;
        }
        auto ffb = std::unique_ptr<GameFfb>(new GameFfb());
        ffb->sink_ = wheelemu::service::SdlFfbSink::open(joystick, config.ffbInvert);
        if (ffb->sink_ == nullptr) {
            status = "unavailable: the wheel's force feedback could not be opened (see the log)";
            WE_LOG_ERROR(log(), "game force feedback unavailable: the wheel's force feedback could not be opened");
            return nullptr;
        }
        ffb->listener_ = wheelemu::UdpLoopbackListener::create(config.ffbPort);
        if (ffb->listener_ == nullptr) {
            status = "unavailable: port " + std::to_string(config.ffbPort) + " is in use";
            WE_LOG_ERROR(log(), "game force feedback unavailable: cannot listen on port {}", config.ffbPort);
            return nullptr;
        }
        ffb->controller_ = std::make_unique<wheelemu::FfbController>(*ffb->sink_, config.ffb);
        WE_LOG_INFO(log(), "game force feedback on: max {:.0f}% of the wheel's torque, gain {:.2f}",
                    config.ffb.maxMagnitude * 100.0F, config.ffb.masterGain);
        return ffb;
    }

    ~GameFfb() {
        if (controller_ != nullptr) {
            controller_->stopAll();
        }
    }

    GameFfb(const GameFfb&) = delete;
    GameFfb& operator=(const GameFfb&) = delete;

    // Every packet from the game is also written here (may be null).
    void setTelemetry(wheelemu::TelemetryRecorder* telemetry) { telemetry_ = telemetry; }

    void pump(const Uint64 nowNs) {
        std::array<std::uint8_t, 256> buffer{};
        while (const auto size = listener_->receive(buffer)) {
            if (const auto packet = wheelemu::decodeFfbPacket(std::span<const std::uint8_t>(buffer.data(), *size))) {
                controller_->onPacket(*packet, nowNs);
                if (telemetry_ != nullptr) {
                    telemetry_->writeFfbPacket(nowNs, *packet);
                }
            } else {
                if (telemetry_ != nullptr) {
                    telemetry_->noteMalformedFfb(nowNs, *size);
                }
                WE_LOG_WARN_LIMITED(log(), malformedLimiter_, "ffb: dropped malformed datagram of {} bytes", *size);
            }
        }
        controller_->update(nowNs);
    }

    [[nodiscard]] const wheelemu::FfbController& controller() const { return *controller_; }

    // Called every 10 s; logs only when something changed: effects playing, failures, watchdog stops,
    // or the game starting / stopping to send.
    void logStatsIfChanged() {
        const auto& stats = controller_->stats();
        const bool receiving = stats.packets != lastPackets_;
        lastPackets_ = stats.packets;
        const auto key = std::make_tuple(stats.activeSlots, stats.playFailures, stats.watchdogStops, receiving);
        if (lastLogged_ && *lastLogged_ == key) {
            return;
        }
        lastLogged_ = key;
        WE_LOG_INFO(log(), "ffb: game {}, {} effects playing, {} packets and {} updates sent so far, {} failed, {} "
                           "watchdog stops",
                    receiving ? "sending" : "quiet", stats.activeSlots, stats.packets, stats.plays, stats.playFailures,
                    stats.watchdogStops);
    }

private:
    GameFfb() = default;

    std::unique_ptr<wheelemu::service::SdlFfbSink> sink_;
    std::unique_ptr<wheelemu::UdpLoopbackListener> listener_;
    std::unique_ptr<wheelemu::FfbController> controller_;
    wheelemu::TelemetryRecorder* telemetry_ = nullptr;
    std::uint64_t lastPackets_ = 0;
    std::optional<std::tuple<std::size_t, std::uint64_t, std::uint64_t, bool>> lastLogged_;
    wheelemu::logging::RateLimiter malformedLimiter_{std::chrono::seconds(5)};
};

// Local recording of the wheel, the mapping, what the game sees and the game's force feedback.
// Returns nullptr when turned off or when the files cannot be created (the emulator runs anyway).
std::unique_ptr<wheelemu::TelemetryRecorder> openTelemetry(const wheelemu::EmulatorConfig& config,
                                                           const std::string& configPath,
                                                           const std::string& configText,
                                                           wheelemu::service::SdlInputSource& source) {
    const wheelemu::TelemetryConfig& settings = config.telemetry;
    if (!settings.enabled) {
        WE_LOG_INFO(log(), "telemetry off ([telemetry] enabled = false)");
        return nullptr;
    }
    const std::filesystem::path directory =
        settings.directory.empty() ? std::filesystem::path(wheelemu::service::defaultLogFile()).parent_path()
                                   : std::filesystem::path(settings.directory);

    SDL_Joystick* const joystick = source.joystick();
    char device[64];
    std::snprintf(device, sizeof(device), "%04x:%04x, %d axes, %d buttons, %d hats", SDL_GetJoystickVendor(joystick),
                  SDL_GetJoystickProduct(joystick), SDL_GetNumJoystickAxes(joystick),
                  SDL_GetNumJoystickButtons(joystick), SDL_GetNumJoystickHats(joystick));
    std::vector<std::string> header = {
        std::string("wheelemu-service ") + WHEELEMU_VERSION + " on " + SDL_GetPlatform(),
        "source: " + source.name() + " (" + device + ")",
        "target_profile: " + config.targetProfile + ", loop_hz: " + std::to_string(config.loopHz) +
            ", rate_hz: " + std::to_string(settings.rateHz),
        "config: " + (configPath.empty() ? std::string("(built-in defaults)") : configPath),
    };
    std::istringstream lines(configText);
    for (std::string line; std::getline(lines, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        header.push_back("ini| " + line);
    }

    std::string error;
    auto recorder = wheelemu::TelemetryRecorder::open(settings, directory, wheelemu::WallClock::now(), header, error);
    if (recorder == nullptr) {
        WE_LOG_ERROR(log(), "telemetry off: {}", error);
        return nullptr;
    }
    WE_LOG_INFO(log(), "telemetry: recording {} rows/s to {} (game force feedback: {}), a new file every day",
                settings.rateHz, recorder->samplesPath().string(), recorder->ffbPath().filename().string());
    return recorder;
}

// The folders [retention] looks after: the log folder and, if elsewhere, the telemetry folder.
std::vector<std::filesystem::path> managedFolders(const wheelemu::EmulatorConfig& config,
                                                  const std::filesystem::path& logFolder) {
    std::vector<std::filesystem::path> folders = {logFolder};
    if (!config.telemetry.directory.empty()) {
        std::error_code ec;
        const std::filesystem::path telemetry(config.telemetry.directory);
        if (!std::filesystem::equivalent(telemetry, logFolder, ec)) {
            folders.push_back(telemetry);
        }
    }
    return folders;
}

int runLoop(const wheelemu::EmulatorConfig& config, const std::string& configPath, const std::string& configText,
            const std::filesystem::path& logFolder, const bool logToFile, wheelemu::service::SdlInputSource& source,
            const bool print, const bool ui) {
    const auto profile = wheelemu::makeProfile(config.targetProfile);
    if (profile == nullptr) {
        WE_LOG_CRITICAL(log(), "unknown target_profile '{}' (known: {})", config.targetProfile,
                        wheelemu::kKnownProfiles);
        return 2;
    }
    const auto publisher = wheelemu::UdpLoopbackPublisher::create(config.statePort);
    if (publisher == nullptr) {
        WE_LOG_CRITICAL(log(), "cannot publish state, the game shim would receive nothing");
        return 1;
    }

    const wheelemu::Mapper mapper(config.mapper);
    wheelemu::Pipeline pipeline(source, mapper, *profile, *publisher);

    std::unique_ptr<wheelemu::service::MappingView> view;
    if (ui) {
        SDL_Joystick* const joystick = source.joystick();
        view = wheelemu::service::MappingView::open(source.name(), SDL_GetNumJoystickAxes(joystick),
                                                    SDL_GetNumJoystickButtons(joystick), *profile, config.mapper);
        if (view == nullptr) {
            return 1;
        }
    }

    std::string ffbStatus;
    const auto gameFfb = GameFfb::open(config, source.joystick(), ffbStatus);
    const auto telemetry = openTelemetry(config, configPath, configText, source);
    if (gameFfb != nullptr) {
        gameFfb->setTelemetry(telemetry.get());
    }

    // Old logs and telemetry: now, and again after every midnight.
    wheelemu::service::Housekeeping housekeeping(config.retention, managedFolders(config, logFolder));
    WE_LOG_INFO(log(), "old files: {}", housekeeping.status());
    housekeeping.run(wheelemu::WallClock::now());
    auto nextMidnight = wheelemu::startOfDay(
        wheelemu::addDays(wheelemu::localDate(wheelemu::WallClock::now()), 1).value_or(""));
    Uint64 lastHousekeepingNs = 0;

    if (view != nullptr) {
        wheelemu::service::FilesDisplay files;
        files.logFolder = logFolder;
        files.logToFile = logToFile;
        files.telemetry = telemetry.get();
        files.telemetryStatus = config.telemetry.enabled ? "off: the files could not be created (see the log)"
                                                         : "off ([telemetry] enabled = false)";
        files.housekeeping = &housekeeping;
        view->setFiles(std::move(files));
    }

    const Uint64 periodNs = SDL_NS_PER_SECOND / static_cast<Uint64>(config.loopHz);
    const Uint64 printEveryTicks = std::max<Uint64>(1, static_cast<Uint64>(config.loopHz) / 10);
    const Uint64 frameEveryTicks = std::max<Uint64>(1, static_cast<Uint64>(config.loopHz) / 60);  // ~60 fps
    LoopHealth loopHealth(periodNs);

    // Above normal, so a game that is loading (all cores busy) does not starve the input loop.
    if (!SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH)) {
        WE_LOG_WARN(log(), "cannot raise the loop's thread priority: {}", SDL_GetError());
    }
    WE_LOG_INFO(log(), "running at {} Hz; press Ctrl+C to stop", config.loopHz);
    Uint64 nextTick = SDL_GetTicksNS();
    Uint64 lastTickStart = nextTick;
    Uint64 windowStart = nextTick;
    Uint64 ticks = 0;
    Uint64 lastPeriodNs = 0;
    Uint64 lastWorkNs = 0;

    while (!gStop) {
        const Uint64 tickStart = SDL_GetTicksNS();
        TickStages stages;
        Uint64 mark = tickStart;
        SDL_UpdateJoysticks();
        stages.lap(TickStages::Input, mark);
        if (!pipeline.tick()) {
            WE_LOG_ERROR(log(), "stopping: physical wheel lost after {} ticks", ticks);
            return 1;
        }
        stages.lap(TickStages::Pipeline, mark);
        ++ticks;
        if (gameFfb != nullptr) {
            gameFfb->pump(SDL_GetTicksNS());
        }
        stages.lap(TickStages::Ffb, mark);
        if (telemetry != nullptr) {
            const Uint64 now = SDL_GetTicksNS();
            if (telemetry->due(now)) {
                wheelemu::TelemetrySample sample;
                sample.nowNs = now;
                sample.sequence = pipeline.sequence();
                sample.raw = pipeline.lastRaw();
                sample.state = pipeline.lastState();
                sample.report = pipeline.lastReport();
                sample.ffb = gameFfb != nullptr ? &gameFfb->controller() : nullptr;
                sample.periodUs = static_cast<double>(lastPeriodNs) / 1000.0;
                sample.workUs = static_cast<double>(lastWorkNs) / 1000.0;
                telemetry->writeSample(sample);
            }
        }
        stages.lap(TickStages::Telemetry, mark);
        if (tickStart - lastHousekeepingNs >= 250 * SDL_NS_PER_MS) {
            lastHousekeepingNs = tickStart;
            housekeeping.pump();
            if (nextMidnight && wheelemu::WallClock::now() >= *nextMidnight) {
                const auto now = wheelemu::WallClock::now();
                WE_LOG_INFO(log(), "new day {}: new log and telemetry files, checking old ones", wheelemu::localDate(now));
                housekeeping.run(now);
                nextMidnight = wheelemu::startOfDay(wheelemu::addDays(wheelemu::localDate(now), 1).value_or(""));
            }
        }
        stages.lap(TickStages::Housekeeping, mark);
        if (print && ticks % printEveryTicks == 0) {
            printLive(pipeline);
        }
        if (view != nullptr && ticks % frameEveryTicks == 0) {
            // Drawing lands inside the tick, so the loop statistics include it; the game path is unaffected.
            if (!view->pumpEvents()) {
                WE_LOG_INFO(log(), "mapping window closed after {} ticks", ticks);
                break;
            }
            wheelemu::service::FfbDisplay ffbDisplay;
            ffbDisplay.controller = gameFfb != nullptr ? &gameFfb->controller() : nullptr;
            ffbDisplay.status = ffbStatus;
            ffbDisplay.invert = config.ffbInvert;
            ffbDisplay.nowNs = SDL_GetTicksNS();
            view->render(pipeline.lastRaw(), pipeline.lastState(), pipeline.lastReport(), ffbDisplay);
        }
        stages.lap(TickStages::Ui, mark);

        const Uint64 tickEnd = SDL_GetTicksNS();
        const Uint64 period = tickStart - lastTickStart;
        lastTickStart = tickStart;
        lastPeriodNs = period;
        lastWorkNs = tickEnd - tickStart;
        if (ticks > 1) {
            loopHealth.record(period, tickEnd - tickStart, stages);
        }
        if (tickEnd - tickStart >= kStallNs) {
            WE_LOG_WARN(log(), "loop: one tick took {:.0f} ms, the game got no fresh input meanwhile: {}",
                        static_cast<double>(tickEnd - tickStart) / 1e6, stages.describe());
        }
        if (tickEnd - windowStart >= kStatsWindowNs) {
            loopHealth.endWindow(pipeline);
            if (gameFfb != nullptr) {
                gameFfb->logStatsIfChanged();
            }
            windowStart = tickEnd;
        }

        // Fixed rate without drift; if we fell behind (e.g. system sleep), resynchronise instead of bursting.
        nextTick += periodNs;
        if (nextTick > tickEnd) {
            SDL_DelayPrecise(nextTick - tickEnd);
            const Uint64 woke = SDL_GetTicksNS();
            if (woke - nextTick >= kStallNs) {
                // Not our work: Windows let the service sleep far longer than asked (the PC was busy).
                WE_LOG_WARN(log(), "loop: woke {:.0f} ms late from a {:.2f} ms sleep: Windows did not schedule the "
                                   "service (PC busy?)",
                            static_cast<double>(woke - nextTick) / 1e6, static_cast<double>(nextTick - tickEnd) / 1e6);
            }
        } else {
            if (tickEnd - nextTick > 100 * SDL_NS_PER_MS) {
                WE_LOG_WARN(log(), "loop: {:.0f} ms behind schedule, resynchronising",
                            static_cast<double>(tickEnd - nextTick) / 1e6);
            }
            nextTick = tickEnd;
        }
    }
    if (print) {
        std::puts("");
    }
    loopHealth.logSummary(pipeline);
    if (telemetry != nullptr) {
        WE_LOG_INFO(log(), "telemetry: {} rows written ({} unchanged samples skipped), {} force feedback changes in {}",
                    telemetry->rowsWritten(), telemetry->rowsSkipped(), telemetry->ffbRowsWritten(),
                    telemetry->samplesPath().parent_path().string());
    }
    if (gStop) {
        WE_LOG_INFO(log(), "stopped by signal after {} ticks", ticks);
    }
    return 0;
}

int run(const Options& options) {
    LoadedConfig loaded = loadConfig(options.configPath);
    if (!options.oldFiles.empty()) {
        loaded.config.retention.action = *wheelemu::parseOldFilesAction(options.oldFiles);
    }
    const std::string logFile =
        loaded.config.logging.filePath.empty() ? wheelemu::service::defaultLogFile() : loaded.config.logging.filePath;
    const bool logFileOk = wheelemu::logging::init(loaded.config.logging, wheelemu::service::defaultLogFile());
    wheelemu::service::routeSdlLogging();
    wheelemu::service::logStartupBanner(options.configPath);

    if (loaded.fileMissing) {
        WE_LOG_CRITICAL(log(), "cannot open config file '{}'", options.configPath);
        return 2;
    }
    for (const std::string& problem : loaded.problems) {
        WE_LOG_ERROR(log(), "config {}: {}", options.configPath, problem);
    }
    if (!loaded.problems.empty()) {
        WE_LOG_CRITICAL(log(), "{} problem(s) in the config, fix them and restart", loaded.problems.size());
        return 2;
    }
    const wheelemu::EmulatorConfig& config = loaded.config;
    logEffectiveConfig(config);

    const wheelemu::service::SdlRuntime sdl;
    if (!sdl.ok()) {
        return 1;
    }
    if (options.list) {
        listDevices();
        return 0;
    }

    const auto source = wheelemu::service::SdlInputSource::open(config.sourceDeviceMatch);
    if (source == nullptr) {
        std::fprintf(stderr, "no joystick matching '%s'. Connected devices:\n", config.sourceDeviceMatch.c_str());
        listDevices();
        return 1;
    }

    if (options.ffbTest) {
        const auto sink = wheelemu::service::SdlFfbSink::open(source->joystick(), config.ffbInvert);
        if (sink == nullptr) {
            return 1;
        }
        return wheelemu::service::runFfbSelfTest(*sink, config.ffb) ? 0 : 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::filesystem::path logFolder = std::filesystem::path(logFile).parent_path();
    if (logFolder.empty()) {
        logFolder = std::filesystem::current_path();
    }
    return runLoop(config, options.configPath, loaded.text, logFolder, logFileOk, *source, options.print, options.ui);
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        return 2;
    }

    int exitCode = 1;
    try {
        exitCode = run(options);
    } catch (const std::exception& e) {
        WE_LOG_CRITICAL(log(), "unhandled exception: {}", e.what());
    } catch (...) {
        WE_LOG_CRITICAL(log(), "unhandled non-standard exception");
    }
    WE_LOG_INFO(log(), "exit code {}", exitCode);
    wheelemu::logging::shutdown();
    return exitCode;
}
