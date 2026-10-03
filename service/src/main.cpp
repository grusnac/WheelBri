// wheelbri-service: reads the physical wheel, maps it and publishes the emulated wheel's state
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
#include "Wheel.hpp"

#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/DeviceLabels.hpp"
#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/FfbController.hpp"
#include "wheelbri/FfbPacket.hpp"
#include "wheelbri/GameUseWatch.hpp"
#include "wheelbri/HidHideCli.hpp"
#include "wheelbri/Logging.hpp"
#include "wheelbri/LoopStats.hpp"
#include "wheelbri/Pipeline.hpp"
#include "wheelbri/Rig.hpp"
#include "wheelbri/Telemetry.hpp"
#include "wheelbri/TestClutch.hpp"
#include "wheelbri/UdpLoopback.hpp"

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

#ifndef WHEELBRI_VERSION
#define WHEELBRI_VERSION "dev"
#endif

namespace {

using wheelbri::logging::Logger;
namespace platform = wheelbri::service::platform;

constexpr std::uint64_t kStatsWindowNs = 10 * platform::kNsPerSecond;
constexpr std::uint64_t kUsageLogNs = 10 * 60 * platform::kNsPerSecond;   // force feedback usage table in the log

std::atomic<bool> gStop{false};
// A session is between its start and the end of its clean-up (the console close handler waits for it).
std::atomic<bool> gSessionActive{false};

void onSignal(int) { gStop = true; }
void requestStop() { gStop = true; }
bool sessionActive() { return gSessionActive; }

// runLoop's result when the physical wheel stopped answering (unplugged, USB reset): runSession waits for it to
// come back and carries on, keeping the virtual G920 (games keep their controller).
constexpr int kWheelLost = 3;

const Logger& log() {
    static const Logger logger = wheelbri::logging::get("service");
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
    bool app = false;                           // --app: the window with Start / Stop (always in wheelbri.exe)
    bool noVirtualG920 = false;                 // --no-virtual-g920: the app without its virtual G920
};

// The active rig's devices (the Wheel setup tab's, [app] rig in the app's state file): everything the hide filter
// hides from games while a session runs. Empty when there is no rig yet. `rigName` gets its name.
std::vector<wheelbri::service::HideFilter::Target> rigHideTargets(std::string& rigName) {
    std::vector<wheelbri::service::HideFilter::Target> targets;
    rigName = wheelbri::readAppSetting(wheelbri::service::appStateFile(), "rig");
    if (rigName.empty()) return targets;
    std::vector<std::string> errors;
    const auto rig = wheelbri::readRig(wheelbri::service::rigsFolder(), rigName, errors);
    if (!rig) return targets;
    for (const wheelbri::RigDevice& device : rig->devices) {
        if (!wheelbri::hidhide::mayHide(device.id)) {
            WE_LOG_WARN(log(), "hide: '{}' ({}) is in the rig but is never hidden (the virtual G920, or no id)",
                        device.name, wheelbri::hidhide::deviceIdText(device.id));
            continue;
        }
        targets.push_back({device.id, device.name.empty() ? wheelbri::hidhide::deviceIdText(device.id) : device.name});
    }
    return targets;
}

// With --virtual-g920 (Start in the app): wheelbri's hide filter on every connected device of the rig, put there now
// if it is not yet, so Start alone leaves games only the G920. Which devices: the rig's ids, nothing else
// (install-hide.ps1 -Devices with the HID instance ids of their game controllers, as DirectInput lists them). It installs from the package built in this source tree or installed
// with the program (driver/hide/x64/Release); the service runs as Administrator for --virtual-g920 anyway. Done
// before the wheel is opened: putting the filter on a device restarts it.
void ensureHideFilter(const std::vector<wheelbri::service::HideFilter::Target>& targets) {
#ifdef _WIN32
    using wheelbri::service::HideFilter;
    std::string devices;
    for (const HideFilter::Check& c : HideFilter::check(targets)) {
        if (c.state == HideFilter::State::NotInstalled) {
            // The exact interfaces DirectInput lists for it (the script skips those that have the filter already).
            for (const std::string& id : c.interfaces) devices += (devices.empty() ? "" : ",") + id;
        }
    }
    if (devices.empty()) return;   // everything connected already has it (or waits for a restart, said above)
    const std::filesystem::path exe = platform::executablePath();
    std::filesystem::path script;
    for (std::filesystem::path base = exe.parent_path(); !base.empty() && script.empty(); base = base.parent_path()) {
        std::error_code ec;
        const std::filesystem::path candidate = base / "driver" / "hide" / "install-hide.ps1";
        const std::filesystem::path package =
            base / "driver" / "hide" / "x64" / "Release" / "wheelbri-hide" / "wheelbri-hide.inf";
        if (std::filesystem::is_regular_file(candidate, ec) && std::filesystem::is_regular_file(package, ec)) {
            script = candidate;
        }
        if (base == base.parent_path()) break;
    }
    if (script.empty()) {
        WE_LOG_ERROR(log(), "hide: wheelbri-hide is not on {} and no built package was found next to the program "
                            "(build it once: scripts/make/drivers.sh -Only hide)",
                     devices);
        return;
    }
    const char* systemRoot = std::getenv("SystemRoot");
    const std::filesystem::path powershell =
        std::filesystem::path(systemRoot != nullptr ? systemRoot : "C:\\Windows") / "System32" / "WindowsPowerShell" /
        "v1.0" / "powershell.exe";
    WE_LOG_INFO(log(), "hide: putting wheelbri-hide on the rig's devices {} ({}); each restarts once", devices,
                script.string());
    std::string error;
    const auto child = platform::ChildProcess::start(
        {powershell.string(), "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script.string(), "-Configuration",
         "Release", "-Devices", devices},
        error);
    if (child == nullptr) {
        WE_LOG_ERROR(log(), "hide: cannot run the installer: {}", error);
        return;
    }
    // Never cut the installer off: stopped halfway, it can leave a device with a half-switched driver. Stop
    // waits for it (it is short), and after 2 minutes it is left to finish by itself.
    std::optional<int> exit;
    bool saidWaiting = false;
    for (int waited = 0; !(exit = child->poll()) && waited < 120'000; waited += 200) {
        if (gStop && !saidWaiting) {
            saidWaiting = true;
            WE_LOG_INFO(log(), "hide: stop requested; waiting for the installer to finish first (it must not be "
                               "cut off while it changes a device's driver)");
        }
        platform::sleepMs(200);
    }
    if (!exit) child->release();
    if (gStop) return;
    if (!exit || *exit != 0) {
        const char* why = !exit ? "took longer than 2 minutes (left to finish by itself)"
                          : *exit == 2 ? "put the filter on, but it is not running on every device yet: restart "
                                         "Windows (when Windows is not waiting for a restart, unplugging the device "
                                         "and plugging it in again is enough; the installer's log says which)"
                                       : "failed";
        WE_LOG_ERROR(log(), "hide: the installer {}{} (its log: C:\\ProgramData\\wheelbri\\logs\\wheelbri-hide-install-"
                            "<date>.log; or run {} -Devices {} yourself as Administrator)",
                     why, exit ? " (exit code " + std::to_string(*exit) + ")" : std::string(), script.string(), devices);
        return;
    }
    // The devices come back with the filter a moment later.
    for (int waited = 0; waited < 15'000 && !gStop; waited += 250) {
        bool pending = false;
        for (const HideFilter::Check& c : HideFilter::check(targets, /*quiet=*/true)) {
            pending = pending || c.state == HideFilter::State::NotInstalled || c.state == HideFilter::State::NeedsRestart;
        }
        if (!pending) {
            WE_LOG_INFO(log(), "hide: wheelbri-hide is on every connected device of the rig");
            return;
        }
        platform::sleepMs(250);
    }
    WE_LOG_ERROR(log(), "hide: the filter is on, but not running on every device of the rig yet (the next hide lines "
                        "say which): restart Windows");
#else
    (void)targets;
#endif
}

// The device in the active rig (the Wheel setup tab's, [app] rig in the app's state file), if it is in it.
std::optional<wheelbri::RigDevice> rigDevice(const wheelbri::DeviceId& id) {
    const std::string name = wheelbri::readAppSetting(wheelbri::service::appStateFile(), "rig");
    if (name.empty()) return std::nullopt;
    std::vector<std::string> errors;
    const auto rig = wheelbri::readRig(wheelbri::service::rigsFolder(), name, errors);
    if (!rig) return std::nullopt;
    if (const wheelbri::RigDevice* const device = rig->find(id)) return *device;
    return std::nullopt;
}

// The names the Wheel setup tab gave this device (the active rig; else the older deviceLabelsFolder()/<vid>-<pid>.ini),
// in place of the config's [button_labels] / [axis_labels]: what the live view shows next to the numbers.
void applySavedDeviceLabels(const wheelbri::service::WheelInfo& wheel, wheelbri::MapperConfig& mapper) {
    if (const auto device = rigDevice({wheel.vendorId, wheel.productId})) {
        wheelbri::applyDeviceLabels(wheelbri::labelsOf(*device), mapper);
        WE_LOG_INFO(log(), "names of '{}' ({}) from the rig", device->name, wheel.name);
        return;
    }
    const std::filesystem::path path =
        wheelbri::service::deviceLabelsFolder() / wheelbri::deviceLabelsFileName(wheel.vendorId, wheel.productId);
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return;
    }
    std::stringstream text;
    text << file.rdbuf();
    const auto parsed = wheelbri::IniDocument::parse(text.str());
    const auto loaded = wheelbri::loadDeviceLabels(parsed.document);
    for (const std::string& problem : parsed.errors) WE_LOG_WARN(log(), "{}: {}", path.string(), problem);
    for (const std::string& problem : loaded.errors) WE_LOG_WARN(log(), "{}: {}", path.string(), problem);
    wheelbri::applyDeviceLabels(loaded.labels, mapper);
    WE_LOG_INFO(log(), "names of '{}' ({}, {}) from {}", loaded.labels.name, wheel.name,
                wheelbri::deviceKindName(loaded.labels.kind), path.string());
}

// The device's names from the Wheel setup tab (its <vid>-<pid>.ini), or just its ids and `fallbackName`.
wheelbri::DeviceLabels savedDeviceLabels(const wheelbri::DeviceId& id, const std::string& fallbackName) {
    wheelbri::DeviceLabels labels;
    if (const auto device = rigDevice(id)) {
        labels = wheelbri::labelsOf(*device);
        if (labels.name.empty()) labels.name = fallbackName;
        return labels;
    }
    const std::filesystem::path path =
        wheelbri::service::deviceLabelsFolder() / wheelbri::deviceLabelsFileName(id.vendorId, id.productId);
    if (std::ifstream file(path, std::ios::binary); file) {
        std::stringstream text;
        text << file.rdbuf();
        labels = wheelbri::loadDeviceLabels(wheelbri::IniDocument::parse(text.str()).document).labels;
    }
    if (labels.name.empty()) labels.name = fallbackName;
    labels.vendorId = id.vendorId;
    labels.productId = id.productId;
    return labels;
}

// The accessories the mapping reads besides the wheel (pedals, a shifter, a button box: MapperConfig::accessories),
// opened by their USB ids. Every one must be there: a mapping with its pedals missing would drive the game wrong.
struct Accessories {
    std::array<std::unique_ptr<wheelbri::service::Wheel>, wheelbri::MapperConfig::kMaxAccessories> devices;
    std::array<wheelbri::DeviceLabels, wheelbri::MapperConfig::kMaxAccessories> labels{};

    [[nodiscard]] std::array<wheelbri::IInputSource*, wheelbri::MapperConfig::kMaxAccessories> sources() const {
        std::array<wheelbri::IInputSource*, wheelbri::MapperConfig::kMaxAccessories> result{};
        for (std::size_t i = 0; i < devices.size(); ++i) result[i] = devices[i].get();
        return result;
    }
    void close() {
        for (auto& device : devices) device.reset();
    }
};

// Opens every accessory of `mapper`; false (and why in the log) if one is missing, or is the wheel itself.
bool openAccessories(wheelbri::service::WheelBackend& backend, const wheelbri::MapperConfig& mapper,
                     const wheelbri::service::WheelInfo& wheel, Accessories& accessories, const bool quiet) {
    accessories.close();
    for (std::size_t i = 0; i < mapper.accessories.size(); ++i) {
        const wheelbri::DeviceId& id = mapper.accessories[i];
        if (id.empty()) continue;
        const std::string ids = wheelbri::deviceId(id.vendorId, id.productId);
        accessories.labels[i] = savedDeviceLabels(id, ids);
        if (id.vendorId == wheel.vendorId && id.productId == wheel.productId) {
            WE_LOG_ERROR(log(), "the mapping names the wheel's own id {} as an accessory: write its sources without "
                                "'{}/' (or save the mapping again in the Mapping tab)",
                         ids, ids);
            return false;
        }
        if (quiet && !wheelbri::service::wheelConnected(backend, ids)) return false;
        accessories.devices[i] = wheelbri::service::openWheel(backend, ids);
        if (accessories.devices[i] == nullptr) {
            WE_LOG_ERROR(log(), "accessory '{}' ({}) is not connected or cannot be read: plug it in, or map without it "
                                "in the Mapping tab",
                         accessories.labels[i].name, ids);
            return false;
        }
        WE_LOG_INFO(log(), "accessory {} = '{}' ({}){}", i + 1, accessories.labels[i].name, ids,
                    " (not hidden from games: only the wheel is)");
    }
    return true;
}

// Whether the wheel and every accessory are connected now (for waiting after one was lost). Logs nothing.
bool allConnected(wheelbri::service::WheelBackend& backend, const wheelbri::EmulatorConfig& config) {
    if (!wheelbri::service::wheelConnected(backend, config.sourceDeviceMatch)) return false;
    for (const wheelbri::DeviceId& id : config.mapper.accessories) {
        if (!id.empty() && !wheelbri::service::wheelConnected(backend, wheelbri::deviceId(id.vendorId, id.productId))) {
            return false;
        }
    }
    return true;
}

// --stop-file: another program (scripts/linux/wheelbri.sh) asks the service to stop by creating this file; the
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
        "usage: wheelbri-service [--config FILE] [--list] [--print] [--ui] [--ffb-test] [--old-files ACTION]\n"
        "                        [--stop-file FILE] [--virtual-g920 [--no-hide]] [--unhide]\n"
        "  --config FILE  INI configuration (see config/moza-r3-as-g920.ini)\n"
        "  --list         list joysticks and exit\n"
        "  --print        show live raw and mapped values\n"
        "  --ui           open a window showing how every axis and button maps to the emulated wheel\n"
        "  --ffb-test     play a short, gentle force feedback sequence on the wheel and exit\n"
        "  --stop-file FILE  stop cleanly when FILE appears (it is deleted then; also at start)\n"
        "  --virtual-g920 while running, a virtual G920 for every game through wheelbri's own driver\n"
        "                 (driver\\install-driver.ps1 once; Administrator; not with the shim);\n"
        "                 the real wheel is hidden from games meanwhile by wheelbri's own filter\n"
        "                 (driver\\hide\\install-hide.ps1 once), unless --no-hide\n"
        "  --unhide       after a crash: make the real wheel visible to games again and exit\n"
        "  --app          a window with Start / Stop instead of running at once (what wheelbri.exe\n"
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
            if (!wheelbri::parseOldFilesAction(options.oldFiles)) {
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
    wheelbri::EmulatorConfig config;
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
    const auto parsed = wheelbri::IniDocument::parse(result.text);
    auto loaded = wheelbri::loadEmulatorConfig(parsed.document);
    result.config = std::move(loaded.config);
    result.problems = parsed.errors;
    result.problems.insert(result.problems.end(), loaded.errors.begin(), loaded.errors.end());
    return result;
}

void logEffectiveConfig(const wheelbri::EmulatorConfig& config) {
    WE_LOG_INFO(log(), "source_device='{}' target_profile='{}' state_port={} ffb_port={} loop_hz={} game_effects={} "
                       "ffb_invert={}",
                config.sourceDeviceMatch, config.targetProfile, config.statePort, config.ffbPort, config.loopHz,
                config.gameFfb, config.ffbInvert);
    WE_LOG_DEBUG(log(), "ffb gain={:.2f} max_magnitude={:.2f} max_slew={:.1f}/s", config.ffb.masterGain,
                 config.ffb.maxMagnitude, config.ffb.maxSlewPerSecond);
}

void listDevices(wheelbri::service::WheelBackend& backend) {
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
void printLive(const wheelbri::Pipeline& pipeline) {
    const auto& raw = pipeline.lastRaw();
    const auto& state = pipeline.lastState();
    const auto& report = pipeline.lastReport();
    std::printf("\rraw %+.3f %+.3f %+.3f %+.3f | steer %+.3f thr %.2f brk %.2f clt %.2f btn %08x hat %2d"
                " | target steer %5d thr %5d brk %5d   ",
                raw.axes[0], raw.axes[1], raw.axes[2], raw.axes[3], state.steering, state.throttle, state.brake,
                state.clutch, state.buttons, state.hat, report.steering, report.throttle, report.brake);
    std::fflush(stdout);
}

void logLoopStats(const char* what, const wheelbri::LoopStats& stats, const wheelbri::Pipeline& pipeline) {
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
    void endWindow(const wheelbri::Pipeline& pipeline) {
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

    void logSummary(const wheelbri::Pipeline& pipeline) const {
        logLoopStats("since the last report", sinceReport_, pipeline);
    }

private:
    wheelbri::LoopStats window_;
    wheelbri::LoopStats sinceReport_;
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
    static std::unique_ptr<GameFfb> open(const wheelbri::EmulatorConfig& config, wheelbri::service::Wheel& wheel,
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
        ffb->listener_ = wheelbri::UdpLoopbackListener::create(config.ffbPort);
        if (ffb->listener_ == nullptr) {
            status = "unavailable: port " + std::to_string(config.ffbPort) + " is in use";
            WE_LOG_ERROR(log(), "game force feedback unavailable: cannot listen on port {}", config.ffbPort);
            return nullptr;
        }
        ffb->controller_ = std::make_unique<wheelbri::FfbController>(*ffb->sink_, config.ffb);
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
    void setTelemetry(wheelbri::TelemetryRecorder* telemetry) { telemetry_ = telemetry; }

    void pump(const std::uint64_t nowNs) {
        std::array<std::uint8_t, 256> buffer{};
        while (const auto size = listener_->receive(buffer)) {
            if (const auto packet = wheelbri::decodeFfbPacket(std::span<const std::uint8_t>(buffer.data(), *size))) {
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

    [[nodiscard]] const wheelbri::FfbController& controller() const { return *controller_; }

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
        const wheelbri::FfbUsage& usage = controller_->usage();
        if (usage.anyForceNs() == loggedUsageNs_) {
            return;
        }
        loggedUsageNs_ = usage.anyForceNs();
        WE_LOG_INFO(log(), "ffb usage since the start: game active {:.1f} min, forces on {:.1f} min "
                           "(share: of the game's time; capped / slew: of that effect's time)",
                    static_cast<double>(usage.gameNs()) / 60e9, static_cast<double>(usage.anyForceNs()) / 60e9);
        WE_LOG_INFO(log(), "ffb usage:   {}", wheelbri::FfbUsage::tableHeader());
        for (const auto type : usage.usedTypes()) {
            WE_LOG_INFO(log(), "ffb usage:   {}", usage.tableRow(type));
        }
        WE_LOG_INFO(log(), "ffb usage: {}", usage.histogramLine());
    }

private:
    GameFfb() = default;

    std::unique_ptr<wheelbri::IFfbSink> sink_;
    std::unique_ptr<wheelbri::UdpLoopbackListener> listener_;
    std::unique_ptr<wheelbri::FfbController> controller_;
    wheelbri::TelemetryRecorder* telemetry_ = nullptr;
    std::uint64_t lastPackets_ = 0;
    std::uint64_t loggedUsageNs_ = 0;
    std::optional<std::tuple<std::size_t, std::uint64_t, std::uint64_t, bool>> lastLogged_;
    wheelbri::logging::RateLimiter malformedLimiter_{std::chrono::seconds(5)};
};

// Local recording of the wheel, the mapping, what the game sees and the game's force feedback.
// Returns nullptr when turned off or when the files cannot be created (the emulator runs anyway).
std::unique_ptr<wheelbri::TelemetryRecorder> openTelemetry(const wheelbri::EmulatorConfig& config,
                                                           const std::string& configPath,
                                                           const std::string& configText,
                                                           wheelbri::service::Wheel& source) {
    const wheelbri::TelemetryConfig& settings = config.telemetry;
    if (!settings.enabled) {
        WE_LOG_INFO(log(), "telemetry off ([telemetry] enabled = false)");
        return nullptr;
    }
    const std::filesystem::path directory =
        settings.directory.empty() ? std::filesystem::path(wheelbri::service::defaultLogFile()).parent_path()
                                   : std::filesystem::path(settings.directory);

    char device[64];
    std::snprintf(device, sizeof(device), "%04x:%04x, %d axes, %d buttons, %d hats", source.info().vendorId,
                  source.info().productId, source.axes(), source.buttons(), source.hats());
    std::vector<std::string> header = {
        std::string("wheelbri-service ") + WHEELBRI_VERSION + " on " + platform::name(),
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
    auto recorder = wheelbri::TelemetryRecorder::open(settings, directory, wheelbri::WallClock::now(), header, error);
    if (recorder == nullptr) {
        WE_LOG_ERROR(log(), "telemetry off: {}", error);
        return nullptr;
    }
    WE_LOG_INFO(log(), "telemetry: recording {} rows/s to {} (game force feedback: {}), a new file every day",
                settings.rateHz, recorder->samplesPath().string(), recorder->ffbPath().filename().string());
    return recorder;
}

// The folders [retention] looks after: the log folder and, if elsewhere, the telemetry folder.
std::vector<std::filesystem::path> managedFolders(const wheelbri::EmulatorConfig& config,
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

using wheelbri::service::UiFrame;

wheelbri::service::TelemetryFiles telemetryFiles(const wheelbri::TelemetryRecorder* recorder) {
    wheelbri::service::TelemetryFiles files;
    if (recorder != nullptr) {
        files.on = true;
        files.samplesPath = recorder->samplesPath();
        files.ffbPath = recorder->ffbPath();
        files.rows = recorder->rowsWritten();
        files.ffbRows = recorder->ffbRowsWritten();
    }
    return files;
}

// What the window says about the virtual devices (--virtual-g920), see SessionInfo::devices.
struct DeviceSummary {
    std::string text;
    bool warning = false;
};

int runLoop(const wheelbri::EmulatorConfig& config, const std::string& configPath, const std::string& configText,
            const std::filesystem::path& logFolder, const bool logToFile, wheelbri::service::Wheel& source,
            const Accessories& accessories, const bool print, wheelbri::service::UiHost* const host,
            const DeviceSummary& devices, std::optional<UiFrame>& lastFrame) {
    const auto profile = wheelbri::makeProfile(config.targetProfile);
    if (profile == nullptr) {
        WE_LOG_CRITICAL(log(), "unknown target_profile '{}' (known: {})", config.targetProfile,
                        wheelbri::kKnownProfiles);
        return 2;
    }
    const auto publisher = wheelbri::UdpLoopbackPublisher::create(config.statePort);
    if (publisher == nullptr) {
        WE_LOG_CRITICAL(log(), "cannot publish state, the game shim would receive nothing");
        return 1;
    }

    const wheelbri::Mapper mapper(config.mapper);
    wheelbri::Pipeline pipeline(source, mapper, *profile, *publisher);
    pipeline.setAccessories(accessories.sources());

    // Test clutch while no clutch pedal is connected: ClutchPackets on kTestClutchPort (TestClutch.hpp).
    const auto clutchListener = wheelbri::UdpLoopbackListener::create(wheelbri::kTestClutchPort);
    if (clutchListener != nullptr) {
        WE_LOG_INFO(log(), "test clutch: listening on 127.0.0.1:{}", wheelbri::kTestClutchPort);
    }
    wheelbri::TestClutch testClutch;
    // Pedals pressed but the game never plays force feedback on the G920: it drives from another controller.
    wheelbri::GameUseWatch gameUse;
    bool testClutchOn = false;

    std::string ffbStatus;
    const auto gameFfb = GameFfb::open(config, source, ffbStatus);
    const auto telemetry = openTelemetry(config, configPath, configText, source);
    if (gameFfb != nullptr) {
        gameFfb->setTelemetry(telemetry.get());
    }

    // Old logs and telemetry: now, and again after every midnight.
    wheelbri::service::Housekeeping housekeeping(config.retention, managedFolders(config, logFolder));
    WE_LOG_INFO(log(), "old files: {}", housekeeping.status());
    housekeeping.run(wheelbri::WallClock::now());
    auto nextMidnight = wheelbri::startOfDay(
        wheelbri::addDays(wheelbri::localDate(wheelbri::WallClock::now()), 1).value_or(""));

    if (host != nullptr) {
        wheelbri::service::SessionInfo info;
        info.sourceName = source.name();
        info.vendorId = source.info().vendorId;
        info.productId = source.info().productId;
        info.axes = source.axes();
        info.buttons = source.buttons();
        info.targetProfile = config.targetProfile;
        info.mapper = config.mapper;
        info.accessories = accessories.labels;
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
                    if (const auto pressed = wheelbri::ClutchPacket::decode(std::span(datagram).first(*size))) {
                        testClutch.offer(*pressed, tickStart);
                    }
                }
                clutchOverride = testClutch.active(tickStart);
                if (clutchOverride.has_value() != testClutchOn) {
                    testClutchOn = clutchOverride.has_value();
                    WE_LOG_INFO(log(), testClutchOn
                                           ? "test clutch: controlled by ClutchPackets, the mapped clutch is ignored"
                                           : "test clutch: ended, the mapped clutch is used again");
                }
            }
            stages.lap(TickStages::Input, mark);
            if (!pipeline.tick(clutchOverride)) {
                const int lost = pipeline.lostDevice();
                WE_LOG_ERROR(log(), "{} lost after {} ticks (unplugged, or a USB reset?)",
                             lost <= 0 ? std::string("physical wheel")
                                       : "accessory '" + accessories.labels[static_cast<std::size_t>(lost - 1)].name + "'",
                             ticks);
                result = kWheelLost;
                break;
            }
            stages.lap(TickStages::Pipeline, mark);
            ++ticks;
            if (gameFfb != nullptr) {
                gameFfb->pump(platform::nowNs());
            }
            if (gameFfb != nullptr) {
                const wheelbri::WheelState& state = pipeline.lastState();
                const bool pedal = state.throttle > 0.1F || state.brake > 0.1F;
                const std::uint64_t packets = gameFfb->controller().stats().packets;
                if (const auto change = gameUse.update(platform::nowMs(), pedal, packets)) {
                    if (*change) {
                        WE_LOG_WARN(log(), "the game is NOT USING the {}: pedals pressed for {} s and it has never "
                                           "played force feedback on it, so it drives from another controller (a "
                                           "gamepad or the keyboard). Close the game "
                                           "with WheelBri running, start it again and pick the wheel in its controls "
                                           "settings",
                                    profile->identity().shortName, wheelbri::GameUseWatch::kPedalMs / 1000);
                    } else {
                        WE_LOG_INFO(log(), "the game uses the {} now (its force feedback arrives)",
                                    profile->identity().shortName);
                    }
                }
            }
            stages.lap(TickStages::Ffb, mark);
            if (telemetry != nullptr) {
                const std::uint64_t now = platform::nowNs();
                if (telemetry->due(now)) {
                    wheelbri::TelemetrySample sample;
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
                    sharedFrame.inputs = pipeline.lastInputs();
                    sharedFrame.state = pipeline.lastState();
                    sharedFrame.report = pipeline.lastReport();
                    sharedFrame.ffb.reset();
                    if (gameFfb != nullptr) {
                        sharedFrame.ffb.emplace(gameFfb->controller());
                    }
                    sharedFrame.telemetry = telemetryFiles(telemetry.get());
                    sharedFrame.gameIgnoresWheel = gameUse.warning();
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
                static wheelbri::logging::RateLimiter limiter(std::chrono::seconds(5));
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
                    static wheelbri::logging::RateLimiter limiter(std::chrono::seconds(5));
                    WE_LOG_WARN_LIMITED(log(), limiter,
                                        "loop: woke {:.0f} ms late from a {:.2f} ms sleep: Windows did not schedule "
                                        "the service (PC busy?)",
                                        static_cast<double>(woke - nextTick) / 1e6,
                                        static_cast<double>(nextTick - tickEnd) / 1e6);
                }
            } else {
                if (tickEnd - nextTick > 100 * platform::kNsPerMs) {
                    static wheelbri::logging::RateLimiter limiter(std::chrono::seconds(5));
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
                if (nextMidnight && wheelbri::WallClock::now() >= *nextMidnight) {
                    const auto wall = wheelbri::WallClock::now();
                    WE_LOG_INFO(log(), "new day {}: new log and telemetry files, checking old ones",
                                wheelbri::localDate(wall));
                    housekeeping.run(wall);
                    nextMidnight = wheelbri::startOfDay(wheelbri::addDays(wheelbri::localDate(wall), 1).value_or(""));
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
        loaded.config.retention.action = *wheelbri::parseOldFilesAction(options.oldFiles);
    }
    return loaded;
}

// Logging (once per process), the timer resolution and the start-up banner. Returns whether the log file opened.
bool startProcess(const Options& options, wheelbri::logging::LogConfig logConfig, const bool console) {
    logConfig.console = logConfig.console && console;
    const bool logFileOk = wheelbri::logging::init(logConfig, wheelbri::service::defaultLogFile());
    platform::init();
    wheelbri::service::logStartupBanner(options.configPath);
    return logFileOk;
}

std::filesystem::path logFolderOf(const wheelbri::EmulatorConfig& config) {
    const std::string logFile =
        config.logging.filePath.empty() ? wheelbri::service::defaultLogFile() : config.logging.filePath;
    std::filesystem::path folder = std::filesystem::path(logFile).parent_path();
    return folder.empty() ? std::filesystem::current_path() : folder;
}

// One session: the wheel opened, the virtual devices created, the loop run until stopped (gStop, the stop
// file, or the host's window), and everything undone again. Logging must be up.
int runSession(const Options& options, const LoadedConfig& loaded, const bool logFileOk,
               wheelbri::service::UiHost* const host) {
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
    // A copy: the wheel's names from the Wheel setup tab replace the config's once the wheel is open.
    wheelbri::EmulatorConfig config = loaded.config;
    logEffectiveConfig(config);

    const auto backend = wheelbri::service::startWheelBackend();
    if (backend == nullptr) {
        return 1;
    }
    WE_LOG_INFO(log(), "reading the wheel through {}", backend->describe());
    if (options.list) {
        listDevices(*backend);
        return 0;
    }
    if (options.unhide) {
        wheelbri::service::HideFilter::clear();
        return 0;
    }
    // One session at a time on this computer: a second would send the game a second, interleaved wheel.
    std::string instanceError;
    const auto instance = platform::SingleInstance::acquire("wheelbri-service", instanceError);
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
                               "start it from an Administrator prompt, or use wheelbri.exe, which asks for it");
        return 1;
    }
    // A wheel still hidden for a service that crashed would be hidden from this one too.
    wheelbri::service::HideFilter::clearStale();
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
    // The rig hidden from games while the session runs: every device the Wheel setup tab defined.
    std::string rigName;
    const auto hideTargets = rigHideTargets(rigName);
    if (options.virtualG920 && options.hide) {
        if (hideTargets.empty()) {
            WE_LOG_WARN(log(), "hide: no rig yet{}: nothing is hidden from games (define the devices in Wheel setup)",
                        rigName.empty() ? "" : " ('" + rigName + "' lists no device)");
        } else {
            ensureHideFilter(hideTargets);
        }
    }
    if (cancelled()) {
        WE_LOG_INFO(log(), "stopped before the session started");
        return 0;
    }

    auto source = wheelbri::service::openWheel(*backend, config.sourceDeviceMatch);
    if (source == nullptr) {
        std::fprintf(stderr, "no joystick matching '%s'. Connected devices:\n", config.sourceDeviceMatch.c_str());
        listDevices(*backend);
        return 1;
    }
    applySavedDeviceLabels(source->info(), config.mapper);
    Accessories accessories;
    if (!options.ffbTest && !openAccessories(*backend, config.mapper, source->info(), accessories, false)) {
        return 1;
    }

    if (options.ffbTest) {
        const auto sink = source->openForceFeedback(config.ffbInvert);
        if (sink == nullptr) {
            return 1;
        }
        return wheelbri::service::runFfbSelfTest(*sink, config.ffb) ? 0 : 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    const std::filesystem::path logFolder = logFolderOf(config);
    // Our own driver's G920 (driver/): there while the loop runs, removed when it returns or the process ends.
    std::unique_ptr<wheelbri::service::VirtualG920> g920;
    if (options.virtualG920) {
        std::string error;
        g920 = wheelbri::service::VirtualG920::create(error, cancelled);
        if (g920 == nullptr) {
            if (cancelled()) {
                WE_LOG_INFO(log(), "stopped while the virtual G920 was being created");
                return 0;
            }
            WE_LOG_CRITICAL(log(), "--virtual-g920: {}", error);
            return 1;
        }
    }
    // The rig hidden from games meanwhile, or a game may take a real device instead of the G920 (NFS does): with
    // wheelbri's own filter (driver/hide) on every device of the rig, which lets only this process open them.
    std::unique_ptr<wheelbri::service::HideFilter> hideFilter;
    std::vector<wheelbri::service::HideFilter::Check> hideChecks;
    if (g920 != nullptr && options.hide && !hideTargets.empty()) {
        hideChecks = wheelbri::service::HideFilter::check(hideTargets);
        bool any = false;
        for (const auto& c : hideChecks) any = any || c.state == wheelbri::service::HideFilter::State::Ready;
        if (any) {
            hideFilter = std::make_unique<wheelbri::service::HideFilter>();
            if (!hideFilter->hide()) hideFilter.reset();
        }
    }
    // One line for the window: what games will see besides the G920, and what is missing.
    DeviceSummary devices;
    if (g920 != nullptr) {
        devices.text = "virtual G920 on";
        if (!options.hide) {
            devices.text += ", rig left visible (--no-hide)";
        } else if (hideTargets.empty()) {
            devices.text += ", NOTHING HIDDEN: no rig yet (Wheel setup)";
            devices.warning = true;
        } else {
            int hidden = 0;
            std::string notHidden;
            for (const auto& c : hideChecks) {
                if (c.state == wheelbri::service::HideFilter::State::Ready && hideFilter != nullptr) {
                    ++hidden;
                } else if (c.state != wheelbri::service::HideFilter::State::NotConnected) {
                    notHidden += (notHidden.empty() ? "" : ", ") + c.target.name + " (" +
                                 (hideFilter == nullptr && c.state == wheelbri::service::HideFilter::State::Ready
                                      ? std::string("setting not written")
                                      : wheelbri::service::HideFilter::stateName(c.state)) +
                                 ")";
                }
            }
            devices.text += ", rig hidden (" + std::to_string(hidden) + " of " + std::to_string(hideTargets.size()) +
                            " devices)";
            if (!notHidden.empty()) {
                devices.text += ", NOT HIDDEN: " + notHidden + ": restart Windows";
                devices.warning = true;
                WE_LOG_ERROR(log(), "hide: games can see {}: the filter does not run there yet (the hide lines above "
                                    "say why)",
                             notHidden);
            }
        }
    }
    int result = 0;
    std::optional<UiFrame> lastFrame;
    std::uint64_t sessionStartMs = platform::nowMs();
    int quickLosses = 0;   // lost again soon after it came back: wait longer before the next try
    while (!gStop) {
        sessionStartMs = platform::nowMs();
        result = runLoop(config, options.configPath, loaded.text, logFolder, logFileOk, *source, accessories,
                         options.print, host, devices, lastFrame);
        if (result != kWheelLost || gStop) {
            break;
        }
        // The wheel is gone (unplugged, USB reset, power). Keep the virtual devices: the game keeps its G920, which
        // reads centred with the pedals released until the wheel is back. Then the same session carries on.
        source.reset();
        accessories.close();
        quickLosses = platform::nowMs() - sessionStartMs < 10'000 ? quickLosses + 1 : 0;
        const std::uint64_t retryEveryMs = quickLosses == 0 ? 1000 : std::min<std::uint64_t>(30'000, 5000ULL * static_cast<std::uint64_t>(quickLosses));
        WE_LOG_WARN(log(), "waiting for the wheel and its accessories to come back (plug it in again; Stop to give up); the virtual "
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
                allConnected(*backend, config)) {
                source = wheelbri::service::openWheel(*backend, config.sourceDeviceMatch);
                if (source != nullptr && !openAccessories(*backend, config.mapper, source->info(), accessories, true)) {
                    source.reset();   // an accessory is not ready yet: all again next time
                    accessories.close();
                }
                if (source == nullptr) {
                    static wheelbri::logging::RateLimiter limiter(std::chrono::seconds(30));
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
    std::unique_ptr<wheelbri::service::UiHost> window;
    if (options.ui) {
        window = wheelbri::service::makeSessionWindow(options.configPath);
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
    // --config wins; then the config picked in the app last time, if it is still there; then the default.
    std::string pickedGone;
    if (options.configPath.empty()) {
        const std::string picked = wheelbri::readConfigSelection(wheelbri::service::appStateFile());
        std::error_code ec;
        if (!picked.empty() && std::filesystem::is_regular_file(picked, ec)) {
            options.configPath = picked;
        } else {
            pickedGone = picked;
            options.configPath = defaultConfigPath();
        }
    }
#ifdef _WIN32
    options.virtualG920 = !options.noVirtualG920;
#endif
    options.print = false;
    const LoadedConfig loaded = loadForRun(options);
    const bool logFileOk = startProcess(options, loaded.config.logging, consoleLogging);

    if (!pickedGone.empty()) {
        WE_LOG_WARN(log(), "the config picked last time is gone ({}): using {}", pickedGone,
                    options.configPath.empty() ? std::string("the built-in defaults") : options.configPath);
    }

    wheelbri::service::AppSettings settings;
    settings.configPath = options.configPath;
    if (const std::string builtIn = defaultConfigPath(); !builtIn.empty()) {
        settings.builtInConfigFolder = std::filesystem::path(builtIn).parent_path();
    }
    settings.userConfigFolder = wheelbri::service::userConfigFolder();
    settings.stateFile = wheelbri::service::appStateFile();
    settings.mode = options.virtualG920
                        ? std::string("creates the virtual G920") +
                              (options.hide ? " and hides the real wheel from games" : "")
                        : std::string("sends it to the in-game shim (dinput8.dll)");
    settings.logFolder = logFolderOf(loaded.config);
    settings.devicesFolder = wheelbri::service::deviceLabelsFolder();
    settings.runSession = [options, logFileOk](wheelbri::service::UiHost& host, const std::string& configPath) {
        Options run = options;
        run.configPath = configPath;   // the config picked in the app
        const LoadedConfig fresh = loadForRun(run);
        return runSession(run, fresh, logFileOk, &host);
    };
    settings.requestStop = [] { gStop = true; };
    settings.clearStop = [] { gStop = false; };
    return wheelbri::service::runApp(settings);
}

}  // namespace

int main(int argc, char** argv) {
    wheelbri::service::moveFoldersFromOldName();   // before anything reads the user's folders
    Options options;
    if (!parseOptions(argc, argv, options)) {
        return 2;
    }
#ifdef WHEELBRI_APP
    constexpr bool kAppBuild = true;   // wheelbri.exe: a window program, no console
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
    wheelbri::logging::shutdown();
    return exitCode;
}
