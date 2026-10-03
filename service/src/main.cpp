// wheelemu-service: reads the physical wheel, maps it and publishes the emulated wheel's state
// to the in-game shim over UDP loopback, plays the game's force feedback on the wheel, and records
// telemetry of both directions for later tuning.

#include "App.hpp"
#include "FfbSelfTest.hpp"
#include "HideFilter.hpp"
#include "Housekeeping.hpp"
#include "MappingView.hpp"
#include "Platform.hpp"
#include "ServiceLogging.hpp"
#include "SessionUi.hpp"
#include "VirtualG920.hpp"
#include "VirtualXboxPad.hpp"
#include "Wheel.hpp"

#include "wheelemu/EmulatorConfig.hpp"
#include "wheelemu/FfbController.hpp"
#include "wheelemu/FfbPacket.hpp"
#include "wheelemu/Logging.hpp"
#include "wheelemu/LoopStats.hpp"
#include "wheelemu/Pipeline.hpp"
#include "wheelemu/Telemetry.hpp"
#include "wheelemu/TestClutch.hpp"
#include "wheelemu/UdpLoopback.hpp"
#include "wheelemu/HidXbox360.hpp"
#include "wheelemu/XboxPad.hpp"

#include <algorithm>
#include <utility>
#include <cmath>
#include <functional>
#include <cctype>
#include <array>
#include <chrono>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <sstream>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#ifndef WHEELEMU_VERSION
#define WHEELEMU_VERSION "dev"
#endif

namespace {

using wheelemu::logging::Logger;
namespace platform = wheelemu::service::platform;

constexpr std::uint64_t kStatsWindowNs = 10 * platform::kNsPerSecond;
constexpr std::uint64_t kUsageLogNs = 10 * 60 * platform::kNsPerSecond;   // force feedback usage table in the log

std::atomic<bool> gStop{false};
// A session is between its start and the end of its clean-up (the console close handler waits for it).
std::atomic<bool> gSessionActive{false};

void onSignal(int) { gStop = true; }
void requestStop() { gStop = true; }
bool sessionActive() { return gSessionActive; }

// runLoop's result when the physical wheel stopped answering (unplugged, USB reset): runSession waits for it to
// come back and carries on, keeping the virtual G920 and Xbox controller (games keep their controller).
constexpr int kWheelLost = 3;

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
    std::string stopFile;                       // --stop-file: stop when this file appears
    bool virtualG920 = false;                   // --virtual-g920: our own driver's G920 (driver/)
    bool hide = true;                           // --no-hide: leave the real wheel visible to games
    bool unhide = false;                        // --unhide: make the real wheel visible again and exit
    bool app = false;                           // --app: the window with Start / Stop (always in wheelemu.exe)
    bool noVirtualG920 = false;                 // --no-virtual-g920: the app without its virtual G920
    wheelemu::XboxPadMode xboxPad = wheelemu::XboxPadMode::Buttons;   // --xbox-pad: with --virtual-g920
};

// The wheel's USB vendor id: [emulator] source_device when it is one ("346e"), else Moza's.
std::string wheelVendorId(const std::string& sourceDevice) {
    std::string id = sourceDevice.substr(0, sourceDevice.find(':'));
    const bool hex = id.size() == 4 && id.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
    if (!hex) id = "346e";
    for (char& c : id) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return id;
}

// With --virtual-g920 (Start in the app): wheelemu's hide filter on the real wheel, installed now if it is not
// there yet, so Start alone leaves games only the G920. It installs from the package built in this source tree
// (driver/hide/x64/Release, scripts/make/drivers.sh -Only hide) with driver/hide/install-hide.ps1; the service
// runs as Administrator for --virtual-g920 anyway. Done before the wheel is opened: installing restarts it.
void ensureHideFilter(const std::string& vendor, wheelemu::service::WheelBackend& backend, const std::string& match) {
#ifdef _WIN32
    if (wheelemu::service::HideFilter::find(vendor) != nullptr) {
        return;
    }
    // The filter is found only on a wheel that is plugged in: without one, installing proves nothing (and would
    // run the installer on every Start).
    if (!wheelemu::service::wheelConnected(backend, match)) {
        WE_LOG_WARN(log(), "hide: the wheel is not connected, so wheelemu-hide cannot be checked or installed now");
        return;
    }
    const std::filesystem::path exe = platform::executablePath();
    std::filesystem::path script;
    for (std::filesystem::path base = exe.parent_path(); !base.empty() && script.empty(); base = base.parent_path()) {
        std::error_code ec;
        const std::filesystem::path candidate = base / "driver" / "hide" / "install-hide.ps1";
        const std::filesystem::path package =
            base / "driver" / "hide" / "x64" / "Release" / "wheelemu-hide" / "wheelemu-hide.inf";
        if (std::filesystem::is_regular_file(candidate, ec) && std::filesystem::is_regular_file(package, ec)) {
            script = candidate;
        }
        if (base == base.parent_path()) break;
    }
    if (script.empty()) {
        WE_LOG_ERROR(log(), "hide: wheelemu-hide is not installed and no built package was found next to the "
                            "program (build it once: scripts/make/drivers.sh -Only hide)");
        return;
    }
    const char* systemRoot = std::getenv("SystemRoot");
    const std::filesystem::path powershell =
        std::filesystem::path(systemRoot != nullptr ? systemRoot : "C:\\Windows") / "System32" / "WindowsPowerShell" /
        "v1.0" / "powershell.exe";
    WE_LOG_INFO(log(), "hide: wheelemu-hide is not on the wheel yet: installing it ({}); the wheel restarts once",
                script.string());
    std::string error;
    const auto child = platform::ChildProcess::start(
        {powershell.string(), "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script.string(), "-Configuration",
         "Release"},
        error);
    if (child == nullptr) {
        WE_LOG_ERROR(log(), "hide: cannot run the installer: {}", error);
        return;
    }
    // Never cut the installer off: stopped halfway, it can leave the wheel with a half-switched driver. Stop
    // waits for it (it is short), and after 2 minutes it is left to finish by itself.
    std::optional<int> exit;
    bool saidWaiting = false;
    for (int waited = 0; !(exit = child->poll()) && waited < 120'000; waited += 200) {
        if (gStop && !saidWaiting) {
            saidWaiting = true;
            WE_LOG_INFO(log(), "hide: stop requested; waiting for the installer to finish first (it must not be "
                               "cut off while it changes the wheel's driver)");
        }
        platform::sleepMs(200);
    }
    if (!exit) child->release();
    if (gStop) return;
    if (!exit || *exit != 0) {
        const char* why = !exit ? "took longer than 2 minutes (left to finish by itself)"
                          : *exit == 2 ? "installed the filter, but it is not on the wheel yet: unplug the wheel and "
                                         "plug it in again (or restart Windows)"
                                       : "failed";
        WE_LOG_ERROR(log(), "hide: the installer {}{} (its log: C:\\ProgramData\\wheelemu\\logs\\wheelemu-hide-install-"
                            "<date>.log; or run {} yourself as Administrator)",
                     why, exit ? " (exit code " + std::to_string(*exit) + ")" : std::string(), script.string());
        return;
    }
    // The wheel comes back with the filter a moment later.
    for (int waited = 0; waited < 15'000 && !gStop; waited += 250) {
        if (wheelemu::service::HideFilter::find(vendor) != nullptr) {
            WE_LOG_INFO(log(), "hide: wheelemu-hide installed and on the wheel");
            return;
        }
        platform::sleepMs(250);
    }
    WE_LOG_ERROR(log(), "hide: installed, but the filter is not on the wheel yet: unplug the R3 and plug it in again");
#else
    (void)vendor;
    (void)backend;
    (void)match;
#endif
}

// --stop-file: another program (scripts/wheelemu.sh) asks the service to stop by creating this file; the
// same clean stop as Ctrl+C or closing the window (force feedback off, logs closed).
std::filesystem::path gStopFile;

bool stopRequested() {
    if (gStopFile.empty()) {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(gStopFile, ec)) {
        return false;
    }
    std::filesystem::remove(gStopFile, ec);
    return true;
}

void printUsage() {
    std::puts(
        "usage: wheelemu-service [--config FILE] [--list] [--print] [--ui] [--ffb-test] [--old-files ACTION]\n"
        "                        [--stop-file FILE] [--virtual-g920 [--no-hide] [--xbox-pad MODE]] [--unhide]\n"
        "  --config FILE  INI configuration (see config/moza-r3-as-g920.ini)\n"
        "  --list         list joysticks and exit\n"
        "  --print        show live raw and mapped values\n"
        "  --ui           open a window showing how every axis and button maps to the emulated wheel\n"
        "  --ffb-test     play a short, gentle force feedback sequence on the wheel and exit\n"
        "  --stop-file FILE  stop cleanly when FILE appears (it is deleted then; also at start)\n"
        "  --virtual-g920 while running, a virtual G920 for every game through wheelemu's own driver\n"
        "                 (driver\\install-driver.ps1 once; Administrator; not with the shim or the bridge);\n"
        "                 the real wheel is hidden from games meanwhile by wheelemu's own filter\n"
        "                 (driver\\hide\\install-hide.ps1 once), unless --no-hide\n"
        "  --xbox-pad buttons|idle|off\n"
        "                 with --virtual-g920: a virtual Xbox 360 controller too, so games show Xbox button\n"
        "                 prompts (driver\\xusb\\install-xusb.ps1 once). buttons (default): the wheel's A/B/X/Y/\n"
        "                 Menu/View/D-pad press it in menus; idle: there, never pressed; off: none\n"
        "  --unhide       after a crash: make the real wheel visible to games again and exit\n"
        "  --app          a window with Start / Stop / Open logs instead of running at once (what wheelemu.exe\n"
        "                 does); on Windows it starts with --virtual-g920 unless --no-virtual-g920\n"
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
        } else if (arg == "--virtual-g920") {
            options.virtualG920 = true;
        } else if (arg == "--xbox-pad" && i + 1 < argc) {
            const auto mode = wheelemu::parseXboxPadMode(argv[++i]);
            if (!mode) {
                std::fprintf(stderr, "--xbox-pad %s: expected buttons, idle or off\n", argv[i]);
                return false;
            }
            options.xboxPad = *mode;
        } else if (arg == "--app") {
            options.app = true;
        } else if (arg == "--no-virtual-g920") {
            options.noVirtualG920 = true;
        } else if (arg == "--no-hide") {
            options.hide = false;
        } else if (arg == "--unhide") {
            options.unhide = true;
        } else if (arg == "--stop-file" && i + 1 < argc) {
            options.stopFile = argv[++i];
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

void listDevices(wheelemu::service::WheelBackend& backend) {
    const auto joysticks = backend.list();
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
    enum Stage { Input, Pipeline, Ffb, Telemetry, Ui, kCount };
    static constexpr std::array<const char*, kCount> kNames = {"wheel input", "state to game", "force feedback",
                                                                "telemetry", "window copy"};
    std::array<std::uint64_t, kCount> ns{};

    // Adds the time since `mark` to `stage` and moves `mark` to now.
    void lap(const Stage stage, std::uint64_t& mark) {
        const std::uint64_t now = platform::nowNs();
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
    explicit LoopHealth(const std::uint64_t periodNs) : window_(periodNs), sinceReport_(periodNs) {}

    void record(const std::uint64_t periodNs, const std::uint64_t workNs, const TickStages& stages) {
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
    std::uint64_t slowestWorkNs_ = 0;
    TickStages slowest_;
};

// A tick this long means the game gets no fresh input; it is logged at once, with where the time went.
constexpr std::uint64_t kStallNs = 50 * platform::kNsPerMs;

// Phase 3: the game's force feedback, sent by the in-game shim, limited and played on the physical wheel.
class GameFfb {
public:
    // Returns nullptr (and says why) when game force feedback is off or cannot run; input still works.
    static std::unique_ptr<GameFfb> open(const wheelemu::EmulatorConfig& config, wheelemu::service::Wheel& wheel,
                                         std::string& status) {
        if (!config.gameFfb) {
            status = "off ([ffb] game_effects = false)";
            WE_LOG_INFO(log(), "game force feedback off ([ffb] game_effects = false)");
            return nullptr;
        }
        auto ffb = std::unique_ptr<GameFfb>(new GameFfb());
        ffb->sink_ = wheel.openForceFeedback(config.ffbInvert);
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

    void pump(const std::uint64_t nowNs) {
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

    // Which effects the game used and how hard (FfbUsage), since the start: a table in the log, only
    // when forces played since the last one.
    void logUsage() {
        const wheelemu::FfbUsage& usage = controller_->usage();
        if (usage.anyForceNs() == loggedUsageNs_) {
            return;
        }
        loggedUsageNs_ = usage.anyForceNs();
        WE_LOG_INFO(log(), "ffb usage since the start: game active {:.1f} min, forces on {:.1f} min "
                           "(share: of the game's time; capped / slew: of that effect's time)",
                    static_cast<double>(usage.gameNs()) / 60e9, static_cast<double>(usage.anyForceNs()) / 60e9);
        WE_LOG_INFO(log(), "ffb usage:   {}", wheelemu::FfbUsage::tableHeader());
        for (const auto type : usage.usedTypes()) {
            WE_LOG_INFO(log(), "ffb usage:   {}", usage.tableRow(type));
        }
        WE_LOG_INFO(log(), "ffb usage: {}", usage.histogramLine());
    }

private:
    GameFfb() = default;

    std::unique_ptr<wheelemu::IFfbSink> sink_;
    std::unique_ptr<wheelemu::UdpLoopbackListener> listener_;
    std::unique_ptr<wheelemu::FfbController> controller_;
    wheelemu::TelemetryRecorder* telemetry_ = nullptr;
    std::uint64_t lastPackets_ = 0;
    std::uint64_t loggedUsageNs_ = 0;
    std::optional<std::tuple<std::size_t, std::uint64_t, std::uint64_t, bool>> lastLogged_;
    wheelemu::logging::RateLimiter malformedLimiter_{std::chrono::seconds(5)};
};

// Local recording of the wheel, the mapping, what the game sees and the game's force feedback.
// Returns nullptr when turned off or when the files cannot be created (the emulator runs anyway).
std::unique_ptr<wheelemu::TelemetryRecorder> openTelemetry(const wheelemu::EmulatorConfig& config,
                                                           const std::string& configPath,
                                                           const std::string& configText,
                                                           wheelemu::service::Wheel& source) {
    const wheelemu::TelemetryConfig& settings = config.telemetry;
    if (!settings.enabled) {
        WE_LOG_INFO(log(), "telemetry off ([telemetry] enabled = false)");
        return nullptr;
    }
    const std::filesystem::path directory =
        settings.directory.empty() ? std::filesystem::path(wheelemu::service::defaultLogFile()).parent_path()
                                   : std::filesystem::path(settings.directory);

    char device[64];
    std::snprintf(device, sizeof(device), "%04x:%04x, %d axes, %d buttons, %d hats", source.info().vendorId,
                  source.info().productId, source.axes(), source.buttons(), source.hats());
    std::vector<std::string> header = {
        std::string("wheelemu-service ") + WHEELEMU_VERSION + " on " + platform::name(),
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

using wheelemu::service::UiFrame;

wheelemu::service::TelemetryFiles telemetryFiles(const wheelemu::TelemetryRecorder* recorder) {
    wheelemu::service::TelemetryFiles files;
    if (recorder != nullptr) {
        files.on = true;
        files.samplesPath = recorder->samplesPath();
        files.ffbPath = recorder->ffbPath();
        files.rows = recorder->rowsWritten();
        files.ffbRows = recorder->ffbRowsWritten();
    }
    return files;
}

// Between the pipeline and the state publisher while the virtual Xbox controller runs: every published wheel state
// goes through XboxMirror (core/XboxPad.hpp), the G920 gets its version (D-pad and steering possibly held back in
// menus) and the Xbox controller's driver gets the pad (UDP kXusbPort; at least every 100 ms, so it knows the
// service is there). Runs on the loop's thread.
class XboxPadPublisher final : public wheelemu::IStatePublisher {
public:
    // The strongest force the game plays now (0..1) and whether it has used force feedback at all.
    using ForceQuery = std::function<std::pair<float, bool>()>;

    XboxPadPublisher(wheelemu::IStatePublisher& inner, const wheelemu::XboxPadMode mode)
        : inner_(inner),
          mirror_(mode),
          sender_(wheelemu::UdpLoopbackSender::create(wheelemu::kXusbPort)),
          hidSender_(wheelemu::UdpLoopbackSender::create(wheelemu::kXboxHidPort)) {}

    void setForceQuery(ForceQuery query) { force_ = std::move(query); }

    bool publish(const wheelemu::StatePacket& packet) override {
        const auto now = static_cast<std::int64_t>(platform::nowMs());
        const auto [force, usesForce] = force_ ? force_() : std::pair<float, bool>{0.0F, false};
        const auto result = mirror_.update(packet.report, now, force, usesForce);
        if (result.menuChanged) {
            WE_LOG_INFO(log(), *result.menuChanged
                                   ? "xbox: in a menu (no pedal; a pad button pressed or no force): the G920's steering is held until a pedal"
                                   : "xbox: driving again: the G920's steering follows the wheel");
        }
        if (result.firstPress) {
            WE_LOG_INFO(log(), "xbox: first button press passed on to the Xbox controller (0x{:04x})", result.pad.buttons);
        }
        if (sender_ != nullptr && (result.pad != lastPad_ || now - lastSentMs_ >= 100)) {
            const auto bytes = wheelemu::encodeXusbPacket(result.pad);
            sender_->send(bytes);
            // The HID side (joy.cpl) is only there to be listed: it never presses anything. DirectInput games
            // would read it as one more controller and act on its buttons too.
            if (hidSender_ != nullptr) hidSender_->send(wheelemu::encodeXusbPacket(wheelemu::XusbPad{}));
            lastPad_ = result.pad;
            lastSentMs_ = now;
        }
        wheelemu::StatePacket out = packet;
        out.report = result.g920;
        return inner_.publish(out);
    }

    [[nodiscard]] std::uint64_t presses() const { return mirror_.presses(); }

private:
    wheelemu::IStatePublisher& inner_;
    wheelemu::XboxMirror mirror_;
    std::unique_ptr<wheelemu::UdpLoopbackSender> sender_;
    std::unique_ptr<wheelemu::UdpLoopbackSender> hidSender_;
    ForceQuery force_;
    wheelemu::XusbPad lastPad_;
    std::int64_t lastSentMs_ = 0;
};

// What XboxPadPublisher needs from the game's force feedback.
std::pair<float, bool> gameForce(const wheelemu::FfbController& controller) {
    float strongest = 0.0F;
    for (std::size_t i = 0; i < controller.slotCount(); ++i) {
        const auto slot = controller.slot(i);
        if (slot.playing) strongest = std::max(strongest, std::abs(wheelemu::ffbStrength(slot.requested)));
    }
    return {strongest, controller.stats().packets > 0};
}

// What the window says about the virtual devices (--virtual-g920), see SessionInfo::devices.
struct DeviceSummary {
    std::string text;
    bool warning = false;
};

int runLoop(const wheelemu::EmulatorConfig& config, const std::string& configPath, const std::string& configText,
            const std::filesystem::path& logFolder, const bool logToFile, wheelemu::service::Wheel& source,
            const bool print, wheelemu::service::UiHost* const host,
            const std::optional<wheelemu::XboxPadMode> xboxPad, const DeviceSummary& devices,
            std::optional<UiFrame>& lastFrame) {
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
    std::unique_ptr<XboxPadPublisher> xboxPublisher;
    if (xboxPad) {
        xboxPublisher = std::make_unique<XboxPadPublisher>(*publisher, *xboxPad);
    }
    wheelemu::Pipeline pipeline(source, mapper, *profile,
                                xboxPublisher != nullptr ? static_cast<wheelemu::IStatePublisher&>(*xboxPublisher)
                                                         : *publisher);

    // Test clutch (tools/vbridge/ClutchProbe) while no clutch pedal is connected.
    const auto clutchListener = wheelemu::UdpLoopbackListener::create(wheelemu::kTestClutchPort);
    if (clutchListener != nullptr) {
        WE_LOG_INFO(log(), "test clutch: listening on 127.0.0.1:{} (ClutchProbe)", wheelemu::kTestClutchPort);
    }
    wheelemu::TestClutch testClutch;
    bool testClutchOn = false;

    std::string ffbStatus;
    const auto gameFfb = GameFfb::open(config, source, ffbStatus);
    if (xboxPublisher != nullptr && gameFfb != nullptr) {
        const GameFfb* ffb = gameFfb.get();
        xboxPublisher->setForceQuery([ffb] { return gameForce(ffb->controller()); });
    }
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

    if (host != nullptr) {
        wheelemu::service::SessionInfo info;
        info.sourceName = source.name();
        info.axes = source.axes();
        info.buttons = source.buttons();
        info.targetProfile = config.targetProfile;
        info.mapper = config.mapper;
        info.files.logFolder = logFolder;
        info.files.logToFile = logToFile;
        info.files.telemetryStatus = config.telemetry.enabled ? "off: the files could not be created (see the log)"
                                                              : "off ([telemetry] enabled = false)";
        info.ffbStatus = ffbStatus;
        info.ffbInvert = config.ffbInvert;
        info.devices = devices.text;
        info.devicesWarning = devices.warning;
        if (!host->sessionStarted(info)) {
            return 1;
        }
    }

    const std::uint64_t periodNs = platform::kNsPerSecond / static_cast<std::uint64_t>(config.loopHz);
    const std::uint64_t printEveryTicks = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(config.loopHz) / 10);
    const std::uint64_t frameEveryTicks = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(config.loopHz) / 60);  // ~60 fps

    // The 1 kHz loop (wheel input, state to the game, force feedback, telemetry) runs on its own thread; the
    // window and old-file checks stay on this one. A window that Windows holds up (a game going full screen,
    // dragging it) then only freezes the window, never the wheel or its force feedback.
    std::mutex frameMutex;
    UiFrame sharedFrame;
    std::atomic<bool> stopLoop{false};
    std::atomic<bool> loopDone{false};
    int loopResult = 0;

    const auto tickLoop = [&]() -> int {
        LoopHealth loopHealth(periodNs);
        // Above normal, so a game that is loading (all cores busy) does not starve the input loop.
        if (std::string error; !platform::raiseThreadPriority(error)) {
            WE_LOG_WARN(log(), "cannot raise the loop's thread priority: {}", error);
        }
        WE_LOG_INFO(log(), "running at {} Hz; press Ctrl+C to stop", config.loopHz);
        std::uint64_t nextTick = platform::nowNs();
        std::uint64_t lastTickStart = nextTick;
        std::uint64_t windowStart = nextTick;
        std::uint64_t ticks = 0;
        std::uint64_t lastPeriodNs = 0;
        std::uint64_t lastWorkNs = 0;
        std::uint64_t lastUsageLogNs = nextTick;
        int result = 0;

        while (!gStop && !stopLoop) {
            const std::uint64_t tickStart = platform::nowNs();
            TickStages stages;
            std::uint64_t mark = tickStart;
            std::optional<float> clutchOverride;
            if (clutchListener != nullptr) {
                std::array<std::uint8_t, 64> datagram{};
                for (int i = 0; i < 64; ++i) {   // everything queued since the last tick; the newest wins
                    const auto size = clutchListener->receive(datagram);
                    if (!size) {
                        break;
                    }
                    if (const auto pressed = wheelemu::ClutchPacket::decode(std::span(datagram).first(*size))) {
                        testClutch.offer(*pressed, tickStart);
                    }
                }
                clutchOverride = testClutch.active(tickStart);
                if (clutchOverride.has_value() != testClutchOn) {
                    testClutchOn = clutchOverride.has_value();
                    WE_LOG_INFO(log(), testClutchOn
                                           ? "test clutch: controlled by ClutchProbe, the mapped clutch is ignored"
                                           : "test clutch: ended, the mapped clutch is used again");
                }
            }
            stages.lap(TickStages::Input, mark);
            if (!pipeline.tick(clutchOverride)) {
                WE_LOG_ERROR(log(), "physical wheel lost after {} ticks (unplugged, or a USB reset?)", ticks);
                result = kWheelLost;
                break;
            }
            stages.lap(TickStages::Pipeline, mark);
            ++ticks;
            if (gameFfb != nullptr) {
                gameFfb->pump(platform::nowNs());
            }
            stages.lap(TickStages::Ffb, mark);
            if (telemetry != nullptr) {
                const std::uint64_t now = platform::nowNs();
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
            if (print && ticks % printEveryTicks == 0) {
                printLive(pipeline);
            }
            if (host != nullptr && ticks % frameEveryTicks == 0) {
                // Hand the window a copy. Never wait for it: if it is copying right now, skip this frame.
                const std::unique_lock lock(frameMutex, std::try_to_lock);
                if (lock.owns_lock()) {
                    sharedFrame.raw = pipeline.lastRaw();
                    sharedFrame.state = pipeline.lastState();
                    sharedFrame.report = pipeline.lastReport();
                    sharedFrame.ffb.reset();
                    if (gameFfb != nullptr) {
                        sharedFrame.ffb.emplace(gameFfb->controller());
                    }
                    sharedFrame.telemetry = telemetryFiles(telemetry.get());
                    sharedFrame.ready = true;
                }
            }
            stages.lap(TickStages::Ui, mark);

            const std::uint64_t tickEnd = platform::nowNs();
            const std::uint64_t period = tickStart - lastTickStart;
            lastTickStart = tickStart;
            lastPeriodNs = period;
            lastWorkNs = tickEnd - tickStart;
            if (ticks > 1) {
                loopHealth.record(period, tickEnd - tickStart, stages);
            }
            if (tickEnd - tickStart >= kStallNs) {
                static wheelemu::logging::RateLimiter limiter(std::chrono::seconds(5));
                WE_LOG_WARN_LIMITED(log(), limiter,
                                    "loop: one tick took {:.0f} ms, the game got no fresh input meanwhile: {}",
                                    static_cast<double>(tickEnd - tickStart) / 1e6, stages.describe());
            }
            if (tickEnd - windowStart >= kStatsWindowNs) {
                loopHealth.endWindow(pipeline);
                if (gameFfb != nullptr) {
                    gameFfb->logStatsIfChanged();
                    if (tickEnd - lastUsageLogNs >= kUsageLogNs) {
                        gameFfb->logUsage();
                        lastUsageLogNs = tickEnd;
                    }
                }
                windowStart = tickEnd;
            }

            // Fixed rate without drift; if we fell behind (e.g. system sleep), resynchronise instead of bursting.
            nextTick += periodNs;
            if (nextTick > tickEnd) {
                platform::sleepPrecise(nextTick - tickEnd);
                const std::uint64_t woke = platform::nowNs();
                if (woke - nextTick >= kStallNs) {
                    // Not our work: Windows let the service sleep far longer than asked (the PC was busy).
                    static wheelemu::logging::RateLimiter limiter(std::chrono::seconds(5));
                    WE_LOG_WARN_LIMITED(log(), limiter,
                                        "loop: woke {:.0f} ms late from a {:.2f} ms sleep: Windows did not schedule "
                                        "the service (PC busy?)",
                                        static_cast<double>(woke - nextTick) / 1e6,
                                        static_cast<double>(nextTick - tickEnd) / 1e6);
                }
            } else {
                if (tickEnd - nextTick > 100 * platform::kNsPerMs) {
                    static wheelemu::logging::RateLimiter limiter(std::chrono::seconds(5));
                    WE_LOG_WARN_LIMITED(log(), limiter, "loop: {:.0f} ms behind schedule, resynchronising",
                                        static_cast<double>(tickEnd - nextTick) / 1e6);
                }
                nextTick = tickEnd;
            }
        }
        if (print) {
            std::puts("");
        }
        loopHealth.logSummary(pipeline);
        if (gameFfb != nullptr) {
            gameFfb->logUsage();
        }
        if (gStop) {
            WE_LOG_INFO(log(), "stopped by signal after {} ticks", ticks);
        }
        return result;
    };

    std::thread loopThread([&] {
        // An exception must not escape this thread (std::terminate: no clean-up, the wheel left hidden and its
        // forces playing). It ends the session instead; runSession then undoes everything.
        try {
            loopResult = tickLoop();
        } catch (const std::exception& e) {
            WE_LOG_CRITICAL(log(), "loop: stopped on an error: {}", e.what());
            loopResult = 1;
        } catch (...) {
            WE_LOG_CRITICAL(log(), "loop: stopped on an unknown error");
            loopResult = 1;
        }
        loopDone = true;
    });

    // This thread: the window (~60 fps) and old files (every 250 ms, and again after every midnight).
    try {
        constexpr std::uint64_t kFrameNs = platform::kNsPerSecond / 60;
        std::uint64_t lastHousekeepingNs = 0;
        std::uint64_t nextFrame = platform::nowNs();
        while (!loopDone) {
            const std::uint64_t now = platform::nowNs();
            if (now - lastHousekeepingNs >= 250 * platform::kNsPerMs) {
                lastHousekeepingNs = now;
                if (stopRequested()) {
                    WE_LOG_INFO(log(), "stop requested ({} appeared)", gStopFile.string());
                    gStop = true;
                }
                housekeeping.pump();
                if (nextMidnight && wheelemu::WallClock::now() >= *nextMidnight) {
                    const auto wall = wheelemu::WallClock::now();
                    WE_LOG_INFO(log(), "new day {}: new log and telemetry files, checking old ones",
                                wheelemu::localDate(wall));
                    housekeeping.run(wall);
                    nextMidnight = wheelemu::startOfDay(wheelemu::addDays(wheelemu::localDate(wall), 1).value_or(""));
                }
            }
            if (host == nullptr) {
                platform::sleepMs(50);
                continue;
            }
            std::optional<UiFrame> frame;   // copy-constructed: its force feedback copy cannot be assigned
            {
                const std::lock_guard lock(frameMutex);
                if (sharedFrame.ready) {
                    frame.emplace(sharedFrame);
                }
            }
            if (!frame) {
                frame.emplace();
            }
            frame->oldFiles = housekeeping.status();
            frame->oldFilesFailed = housekeeping.failed();
            if (!host->frame(*frame)) {
                WE_LOG_INFO(log(), "window closed: stopping");
                break;
            }
            nextFrame += kFrameNs;
            const std::uint64_t after = platform::nowNs();
            if (nextFrame > after) {
                platform::sleepNs(nextFrame - after);
            } else {
                nextFrame = after;   // the window was held up: carry on from now, no catching up
            }
        }
    } catch (...) {
        stopLoop = true;   // never leave the loop thread running behind an error here
        loopThread.join();
        throw;
    }
    stopLoop = true;
    loopThread.join();
    if (sharedFrame.ready) {
        lastFrame.reset();
        lastFrame.emplace(sharedFrame);   // what the window keeps showing if the wheel was lost
    }

    if (telemetry != nullptr) {
        WE_LOG_INFO(log(), "telemetry: {} rows written ({} unchanged samples skipped), {} force feedback changes in {}",
                    telemetry->rowsWritten(), telemetry->rowsSkipped(), telemetry->ffbRowsWritten(),
                    telemetry->samplesPath().parent_path().string());
    }
    return loopResult;
}

// The config, with --old-files applied.
LoadedConfig loadForRun(const Options& options) {
    LoadedConfig loaded = loadConfig(options.configPath);
    if (!options.oldFiles.empty()) {
        loaded.config.retention.action = *wheelemu::parseOldFilesAction(options.oldFiles);
    }
    return loaded;
}

// Logging (once per process), the timer resolution and the start-up banner. Returns whether the log file opened.
bool startProcess(const Options& options, wheelemu::logging::LogConfig logConfig, const bool console) {
    logConfig.console = logConfig.console && console;
    const bool logFileOk = wheelemu::logging::init(logConfig, wheelemu::service::defaultLogFile());
    platform::init();
    wheelemu::service::logStartupBanner(options.configPath);
    return logFileOk;
}

std::filesystem::path logFolderOf(const wheelemu::EmulatorConfig& config) {
    const std::string logFile =
        config.logging.filePath.empty() ? wheelemu::service::defaultLogFile() : config.logging.filePath;
    std::filesystem::path folder = std::filesystem::path(logFile).parent_path();
    return folder.empty() ? std::filesystem::current_path() : folder;
}

// One session: the wheel opened, the virtual devices created, the loop run until stopped (gStop, the stop
// file, or the host's window), and everything undone again. Logging must be up.
int runSession(const Options& options, const LoadedConfig& loaded, const bool logFileOk,
               wheelemu::service::UiHost* const host) {
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

    const auto backend = wheelemu::service::startWheelBackend();
    if (backend == nullptr) {
        return 1;
    }
    WE_LOG_INFO(log(), "reading the wheel through {}", backend->describe());
    if (options.list) {
        listDevices(*backend);
        return 0;
    }
    if (options.unhide) {
        wheelemu::service::HideFilter::clear();
        return 0;
    }
    // One session at a time on this computer: a second would send the game a second, interleaved wheel.
    std::string instanceError;
    const auto instance = platform::SingleInstance::acquire("wheelemu-service", instanceError);
    if (instance == nullptr) {
        WE_LOG_CRITICAL(log(), "{}", instanceError);
        return 1;
    }
    // Until the end of this function (its clean-up included), the console close handler waits.
    gSessionActive = true;
    struct SessionActive {
        ~SessionActive() { gSessionActive = false; }
    } sessionActiveGuard;
    platform::installConsoleStopHandler(requestStop, sessionActive);

    if (options.virtualG920 && !platform::isElevated()) {
        WE_LOG_CRITICAL(log(), "--virtual-g920 needs Administrator (it creates devices and hides the real wheel): "
                               "start it from an Administrator prompt, or use wheelemu.exe, which asks for it");
        return 1;
    }
    // A wheel still hidden for a service that crashed would be hidden from this one too.
    wheelemu::service::HideFilter::clearStale();
    if (!options.stopFile.empty()) {
        gStopFile = options.stopFile;
        std::error_code ec;
        std::filesystem::remove(gStopFile, ec);   // left over from an earlier run
        WE_LOG_INFO(log(), "stops when {} appears", gStopFile.string());
    }
    // Stop pressed, Ctrl+C, or the stop file: the waits below give up (and the stop file counts from now on).
    const auto cancelled = [] {
        if (!gStop && stopRequested()) {
            WE_LOG_INFO(log(), "stop requested ({} appeared)", gStopFile.string());
            gStop = true;
        }
        return gStop.load();
    };
    if (options.virtualG920 && options.hide) {
        ensureHideFilter(wheelVendorId(config.sourceDeviceMatch), *backend, config.sourceDeviceMatch);
    }
    if (cancelled()) {
        WE_LOG_INFO(log(), "stopped before the session started");
        return 0;
    }

    auto source = wheelemu::service::openWheel(*backend, config.sourceDeviceMatch);
    if (source == nullptr) {
        std::fprintf(stderr, "no joystick matching '%s'. Connected devices:\n", config.sourceDeviceMatch.c_str());
        listDevices(*backend);
        return 1;
    }

    if (options.ffbTest) {
        const auto sink = source->openForceFeedback(config.ffbInvert);
        if (sink == nullptr) {
            return 1;
        }
        return wheelemu::service::runFfbSelfTest(*sink, config.ffb) ? 0 : 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    const std::filesystem::path logFolder = logFolderOf(config);
    // Our own driver's G920 (driver/): there while the loop runs, removed when it returns or the process ends.
    std::unique_ptr<wheelemu::service::VirtualG920> g920;
    if (options.virtualG920) {
        std::string error;
        g920 = wheelemu::service::VirtualG920::create(error, cancelled);
        if (g920 == nullptr) {
            if (cancelled()) {
                WE_LOG_INFO(log(), "stopped while the virtual G920 was being created");
                return 0;
            }
            WE_LOG_CRITICAL(log(), "--virtual-g920: {}", error);
            return 1;
        }
    }
    // The real wheel hidden from games meanwhile, or a game may take it instead of the G920 (NFS does): with
    // wheelemu's own filter (driver/hide), which lets only this process open the wheel.
    std::unique_ptr<wheelemu::service::HideFilter> hideFilter;
    if (g920 != nullptr && options.hide) {
        hideFilter = wheelemu::service::HideFilter::find(wheelVendorId(config.sourceDeviceMatch));
        if (hideFilter == nullptr) {
            WE_LOG_ERROR(log(), "the real wheel is NOT hidden: wheelemu-hide is not on it (install it once, as "
                                "Administrator: driver\\hide\\install-hide.ps1, or scripts/make/drivers.sh -Only "
                                "hide -Install); games see it next to the G920 and may take it");
        } else if (!hideFilter->hide()) {
            hideFilter.reset();
        }
    }
    // The Xbox controller (driver/xusb), for games' Xbox button prompts. Without its driver the G920 still works.
    std::unique_ptr<wheelemu::service::VirtualXboxPad> xboxDevice;
    std::optional<wheelemu::XboxPadMode> xboxPadMode;
    if (g920 != nullptr && options.xboxPad != wheelemu::XboxPadMode::Off && !cancelled()) {
        std::string error;
        xboxDevice = wheelemu::service::VirtualXboxPad::create(error, cancelled);
        if (xboxDevice == nullptr) {
            if (!cancelled()) WE_LOG_WARN(log(), "no Xbox controller (games show keyboard prompts): {}", error);
        } else {
            xboxPadMode = options.xboxPad;
            WE_LOG_INFO(log(), "Xbox controller: {}", options.xboxPad == wheelemu::XboxPadMode::Buttons
                                                           ? "the wheel's A/B/X/Y/Menu/View/D-pad press it instead of the G920 when no pedal is pressed"
                                                           : "idle (there, never pressed)");
        }
    }
    // One line for the window: what games will see besides the G920, and what is missing.
    DeviceSummary devices;
    if (g920 != nullptr) {
        devices.text = "virtual G920 on";
        if (hideFilter != nullptr) {
            devices.text += ", R3 hidden (wheelemu-hide)";
        } else if (options.hide) {
            devices.text += ", R3 NOT HIDDEN: games see it (install driver\\hide\\install-hide.ps1)";
            devices.warning = true;
        } else {
            devices.text += ", R3 left visible (--no-hide)";
        }
        if (xboxDevice != nullptr) {
            devices.text += xboxDevice->hidSide() ? ", Xbox pad on (XInput and joy.cpl)"
                                                  : ", Xbox pad on for XInput (not in joy.cpl: reinstall driver\\install-driver.ps1)";
        } else if (options.xboxPad == wheelemu::XboxPadMode::Off) {
            devices.text += ", no Xbox pad (--xbox-pad off)";
        } else {
            devices.text += ", NO XBOX PAD (install driver\\xusb)";
            devices.warning = true;
        }
    }
    int result = 0;
    std::optional<UiFrame> lastFrame;
    std::uint64_t sessionStartMs = platform::nowMs();
    int quickLosses = 0;   // lost again soon after it came back: wait longer before the next try
    while (!gStop) {
        sessionStartMs = platform::nowMs();
        result = runLoop(config, options.configPath, loaded.text, logFolder, logFileOk, *source, options.print, host,
                         xboxPadMode, devices, lastFrame);
        if (result != kWheelLost || gStop) {
            break;
        }
        // The wheel is gone (unplugged, USB reset, power). Keep the virtual devices: the game keeps its G920, which
        // reads centred with the pedals released until the wheel is back. Then the same session carries on.
        source.reset();
        quickLosses = platform::nowMs() - sessionStartMs < 10'000 ? quickLosses + 1 : 0;
        const std::uint64_t retryEveryMs = quickLosses == 0 ? 1000 : std::min<std::uint64_t>(30'000, 5000ULL * static_cast<std::uint64_t>(quickLosses));
        WE_LOG_WARN(log(), "waiting for the wheel to come back (plug it in again; Stop to give up); the virtual "
                           "devices stay, centred and released{}",
                    quickLosses > 0 ? "; it was lost again within 10 s of coming back (" + std::to_string(quickLosses) +
                                          " times in a row), so it is tried every " +
                                          std::to_string(retryEveryMs / 1000) + " s"
                                    : std::string());
        std::uint64_t waitedMs = 0;
        while (!gStop && source == nullptr) {
            if (stopRequested()) {
                WE_LOG_INFO(log(), "stop requested ({} appeared)", gStopFile.string());
                gStop = true;
                break;
            }
            // Keeps the window answering (the last picture of the wheel stays, marked lost); false: it was closed.
            if (lastFrame) lastFrame->wheelLostMs = std::max<std::uint64_t>(waitedMs, 1);
            if (host != nullptr && !host->frame(lastFrame ? *lastFrame : UiFrame{})) {
                WE_LOG_INFO(log(), "window closed: stopping");
                gStop = true;
                break;
            }
            if (waitedMs % retryEveryMs == 0 && waitedMs >= (quickLosses > 0 ? retryEveryMs : 0) &&
                wheelemu::service::wheelConnected(*backend, config.sourceDeviceMatch)) {
                source = wheelemu::service::openWheel(*backend, config.sourceDeviceMatch);
                if (source == nullptr) {
                    static wheelemu::logging::RateLimiter limiter(std::chrono::seconds(30));
                    WE_LOG_WARN_LIMITED(log(), limiter, "the wheel is back but cannot be opened yet; trying again");
                }
            }
            if (source == nullptr) {
                platform::sleepMs(50);
                waitedMs += 50;
                if (waitedMs % 60'000 == 0) {
                    WE_LOG_WARN(log(), "still waiting for the wheel ({} min)", waitedMs / 60'000);
                }
            }
        }
        if (source == nullptr) {
            result = 0;   // stopped while waiting: a normal stop
            break;
        }
        WE_LOG_INFO(log(), "the wheel is back after {:.1f} s: carrying on", static_cast<double>(waitedMs) / 1000.0);
    }
    if (hideFilter != nullptr) {
        hideFilter->restore();
    }
    xboxDevice.reset();
    if (g920 != nullptr) {
        WE_LOG_INFO(log(), "removing the virtual G920 {}", g920->instanceId());
        g920.reset();
    }
    return result;
}

// The command line service: one session, with the --ui window when asked for.
int run(const Options& options) {
    const LoadedConfig loaded = loadForRun(options);
    const bool logFileOk = startProcess(options, loaded.config.logging, true);
    std::unique_ptr<wheelemu::service::UiHost> window;
    if (options.ui) {
        window = wheelemu::service::makeSessionWindow();
    }
    return runSession(options, loaded, logFileOk, window.get());
}

// The config the app uses when none is given: config/moza-r3-as-g920.ini next to the program, or one or two
// folders up (build/service/ in the source tree), or under the current folder.
std::string defaultConfigPath() {
    const std::filesystem::path exe = platform::executablePath();
    std::vector<std::filesystem::path> bases;
    if (!exe.empty()) {
        bases = {exe.parent_path(), exe.parent_path().parent_path(), exe.parent_path().parent_path().parent_path()};
    }
    bases.push_back(std::filesystem::current_path());
    for (const auto& base : bases) {
        const std::filesystem::path candidate = base / "config" / "moza-r3-as-g920.ini";
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate.lexically_normal().string();
        }
    }
    return {};
}

// The app: Start / Stop instead of a command line. Each Start reads the config again, so edits apply.
int runAppMode(Options options, const bool consoleLogging) {
    if (options.configPath.empty()) {
        options.configPath = defaultConfigPath();
    }
#ifdef _WIN32
    options.virtualG920 = !options.noVirtualG920;
#endif
    options.print = false;
    const LoadedConfig loaded = loadForRun(options);
    const bool logFileOk = startProcess(options, loaded.config.logging, consoleLogging);

    wheelemu::service::AppSettings settings;
    settings.configPath = options.configPath.empty() ? std::string("(none found: built-in defaults)") : options.configPath;
    settings.mode = options.virtualG920
                        ? std::string("creates the virtual G920") +
                              (options.xboxPad != wheelemu::XboxPadMode::Off ? " and Xbox controller" : "") +
                              (options.hide ? " and hides the real wheel from games" : "")
                        : std::string("sends it to the in-game shim (dinput8.dll)");
    settings.logFolder = logFolderOf(loaded.config);
    settings.runSession = [options, logFileOk](wheelemu::service::UiHost& host) {
        const LoadedConfig fresh = loadForRun(options);
        return runSession(options, fresh, logFileOk, &host);
    };
    settings.requestStop = [] { gStop = true; };
    settings.clearStop = [] { gStop = false; };
    return wheelemu::service::runApp(settings);
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        return 2;
    }
#ifdef WHEELEMU_APP
    constexpr bool kAppBuild = true;   // wheelemu.exe: a window program, no console
#else
    constexpr bool kAppBuild = false;
#endif

    int exitCode = 1;
    try {
        exitCode = kAppBuild || options.app ? runAppMode(options, !kAppBuild) : run(options);
    } catch (const std::exception& e) {
        WE_LOG_CRITICAL(log(), "unhandled exception: {}", e.what());
    } catch (...) {
        WE_LOG_CRITICAL(log(), "unhandled non-standard exception");
    }
    WE_LOG_INFO(log(), "exit code {}", exitCode);
    wheelemu::logging::shutdown();
    return exitCode;
}
