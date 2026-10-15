// wheelbri-service: reads the physical wheel, maps it and publishes the emulated wheel's state
// to the in-game shim over UDP loopback, plays the game's force feedback on the wheel, and records
// telemetry of both directions for later tuning.

#include "App.hpp"
#include "AppLogLevel.hpp"
#include "FfbSelfTest.hpp"
#include "GameWatch.hpp"
#include "HelperProbe.hpp"
#include "HideFilter.hpp"
#include "Privileged.hpp"
#include "PrivilegedSteps.hpp"
#include "Housekeeping.hpp"
#include "MappingView.hpp"
#include "Platform.hpp"
#include "ServiceLogging.hpp"
#include "SessionUi.hpp"
#include "VirtualG920.hpp"
#ifndef _WIN32
#include "UhidProcess.hpp"
#endif
#include "Wheel.hpp"

#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/DeviceLabels.hpp"
#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/AsyncFfbSink.hpp"
#include "wheelbri/FfbController.hpp"
#include "wheelbri/FfbPacket.hpp"
#include "wheelbri/FileText.hpp"
#include "wheelbri/FfbIdleWatch.hpp"
#include "wheelbri/GameUseWatch.hpp"
#include "wheelbri/HidG920.hpp"
#include "wheelbri/HidHideCli.hpp"
#include "wheelbri/LogLevelPacket.hpp"
#include "wheelbri/Logging.hpp"
#include "wheelbri/MappingEdit.hpp"
#include "wheelbri/Pipeline.hpp"
#include "wheelbri/Rig.hpp"
#include "wheelbri/SenderGate.hpp"
#include "wheelbri/ShimGames.hpp"
#include "wheelbri/Telemetry.hpp"
#include "wheelbri/TestClutch.hpp"
#include "wheelbri/UdpLoopback.hpp"
#include "wheelbri/WaitFor.hpp"
#include "wheelbri/EventLoopControl.hpp"
#include "wheelbri/WakePlanner.hpp"

#include <algorithm>
#include <initializer_list>
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
namespace privileged = wheelbri::service::privileged;

constexpr std::uint64_t kStatsWindowNs = 10 * platform::kNsPerSecond;
constexpr std::uint64_t kUsageLogNs = 10 * 60 * platform::kNsPerSecond;   // force feedback usage table in the log
constexpr std::uint64_t kWakeLogNs = 60 * platform::kNsPerSecond;         // the loop's wake-ups in the log

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
    bool uhid = false;                          // Linux --virtual-g920: wheelbri-uhid's G920 (tools/uhid)
    bool hide = true;                           // --no-hide: leave the real wheel visible to games
    bool unhide = false;                        // --unhide: make the real wheel visible again and exit
    bool prepareHide = false;                   // --prepare-hide: put the hide filter on the saved rig's devices, exit
    bool app = false;                           // --app: the window with Start / Stop (always in wheelbri.exe)
    bool noVirtualG920 = false;                 // --no-virtual-g920: the app without its virtual G920
    bool telemetryTab = false;                  // --telemetry-tab: show the Telemetry tab (hidden otherwise)
    bool ffbTab = false;                        // --ffb-tab: show the Force feedback tab (hidden otherwise)
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

// With --virtual-g920 (Start in the app): the virtual G920 needs wheelbri's G920 driver. When Windows does not have it
// (a fresh PC, after clean-pc, or the driver was uninstalled), Start installs it now with driver\install-driver.ps1,
// from the package built in this source tree or installed with the program (driver/x64/Release); the service runs as
// Administrator for --virtual-g920 anyway. When that is not possible, VirtualG920::create says what is missing.
void ensureG920Driver(wheelbri::service::PrivilegedSteps& steps) {
    std::string message;
    (void)steps.installG920Driver([] { return gStop.load(); }, message);
}

using privileged::HideSetup;

// With --virtual-g920 (Start in the app): wheelbri's hide filter on every connected device of the rig, put there now
// if it is not yet, so Start alone leaves games only the G920. Which devices: the rig's ids, nothing else
// (install-hide.ps1 -Devices with the HID instance ids of their game controllers, as DirectInput lists them). It
// installs from the package built in this source tree or installed with the program (driver/hide/x64/Release); the
// service runs as Administrator for --virtual-g920 anyway. Done before the wheel is opened: a device takes the filter
// only when Windows restarts it, which nothing may hold it open for (on 2026-10-05 the session opened the wheel a
// second after the installer and the filter never started: "NOT HIDDEN ... restart Windows"). So this waits, opening
// nothing, until every device runs the filter, restarting a device itself when Windows does not (HideFilter::
// waitUntilStarted), and says who keeps one when that fails.
// `say`: what it is doing when that takes a while (the app shows it next to Start).
HideSetup ensureHideFilter(const std::vector<wheelbri::service::HideFilter::Target>& targets,
                           wheelbri::service::PrivilegedSteps& steps,
                           const std::function<void(const std::string&)>& say = {}) {
#ifdef _WIN32
    using wheelbri::service::HideFilter;
    std::vector<std::string> install;   // no filter yet: the installer puts it on
    std::vector<std::string> all; // every connected game-controller interface of the rig
    std::string waitingNames;     // set but not started (put on earlier, by the installer for example)
    for (const HideFilter::Check& c : HideFilter::check(targets)) {
        for (const std::string& id : c.interfaces) all.push_back(id);
        if (c.state == HideFilter::State::NotInstalled) {
            // The exact interfaces DirectInput lists for it (the script skips those that have the filter already).
            install.insert(install.end(), c.interfaces.begin(), c.interfaces.end());
        } else if (c.state == HideFilter::State::NeedsRestart) {
            waitingNames += (waitingNames.empty() ? "" : ", ") + c.target.name;
        }
    }
    if (all.empty()) return HideSetup::NothingToDo;
    if (install.empty() && waitingNames.empty()) return HideSetup::Running;   // every connected device runs it already
    if (say) {
        say(install.empty() ? std::string("Windows restarts the wheel") : std::string(wheelbri::service::kStepFirstHide));
    }
    std::string message;
    const HideSetup result = steps.putHideFilter(install, all, [] { return gStop.load(); }, message);
    if (say) say({});
    return result;
#else
    (void)targets;
    (void)steps;
    (void)say;
    return HideSetup::NothingToDo;
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
        accessories.devices[i] = wheelbri::service::openWheel(backend, ids, quiet);
        if (accessories.devices[i] == nullptr) {
            WE_LOG_ERROR(log(), "accessory '{}' ({}) is not connected or cannot be read: plug it in, or map without it "
                                "in the Mapping tab",
                         accessories.labels[i].name, ids);
            return false;
        }
        WE_LOG_INFO(log(), "accessory {} = '{}' ({})", i + 1, accessories.labels[i].name, ids);
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
        "                        [--stop-file FILE] [--virtual-g920 [--no-hide]] [--unhide] [--prepare-hide]\n"
        "                        [--telemetry-tab] [--ffb-tab]\n"
        "  --config FILE  INI configuration (see config/g920.ini)\n"
        "  --list         list joysticks and exit\n"
        "  --print        show live raw and mapped values\n"
        "  --ui           open a window showing how every axis and button maps to the emulated wheel\n"
        "  --ffb-test     play a short, gentle force feedback sequence on the wheel and exit\n"
        "  --stop-file FILE  stop cleanly when FILE appears (it is deleted then; also at start)\n"
        "  --virtual-g920 while running, a virtual G920 for every game through wheelbri's own driver\n"
        "                 (driver\\install-driver.ps1 once; Administrator; not with the shim);\n"
        "                 the real wheel is hidden from games meanwhile by wheelbri's own filter\n"
        "                 (driver\\hide\\install-hide.ps1 once), unless --no-hide.\n"
        "                 Linux: starts wheelbri-uhid (install-udev-rules.sh once); the game's launch\n"
        "                 options hide the rig (scripts/linux/wheelbri.sh)\n"
        "  --unhide       after a crash: make the real wheel visible to games again and exit\n"
        "  --prepare-hide put the hide filter on the saved rig's devices and exit (the installer; Administrator)\n"
        "  --app          a window with Start / Stop instead of running at once (what wheelbri.exe\n"
        "                 does); on Windows it starts with --virtual-g920 unless --no-virtual-g920\n"
        "  --telemetry-tab, --ffb-tab\n"
        "                 show the window's Telemetry / Force feedback tab (hidden without these)\n"
        "  --old-files delete|archive|keep\n"
        "                 logs and telemetry older than [retention] older_than_days (2): delete them (default),\n"
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
#ifdef _WIN32
            options.virtualG920 = true;
#else
            options.uhid = true;   // Linux: wheelbri-uhid, started and stopped with the session
#endif
        } else if (arg == "--app") {
            options.app = true;
        } else if (arg == "--no-virtual-g920") {
            options.noVirtualG920 = true;
        } else if (arg == "--telemetry-tab") {
            options.telemetryTab = true;
        } else if (arg == "--ffb-tab") {
            options.ffbTab = true;
        } else if (arg == "--no-hide") {
            options.hide = false;
        } else if (arg == "--unhide") {
            options.unhide = true;
        } else if (arg == "--prepare-hide") {
            options.prepareHide = true;
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
    std::vector<std::string> warnings;          // settings ignored on purpose (logged once logging is up)
    std::vector<std::string> notes;             // what the rig changed (its dead zones), logged at info
};

// Runs before logging exists (logging is configured by this file), so problems are collected, not logged.
LoadedConfig loadConfig(const std::string& path) {
    LoadedConfig result;
    if (path.empty()) {
        return result;
    }
    // Paths are UTF-8 here (as the app shows and stores them); narrow strings would be opened in the ANSI code page.
    const std::filesystem::path filePath = wheelbri::pathFromUtf8(path);
    std::ifstream file(filePath.empty() ? std::filesystem::path(path) : filePath);
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
    WE_LOG_INFO(log(), "source_device='{}' target_profile='{}' state_port={} ffb_port={} state_rate_hz={} "
                       "game_effects={} ffb_invert={}",
                config.sourceDeviceMatch, config.targetProfile, config.statePort, config.ffbPort, config.stateRateHz,
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

// Where the time of one pass of the loop goes, to find the cause of a slow pass.
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

// A pass this long means the game gets no fresh input; it is logged at once, with where the time went.
constexpr std::uint64_t kStallNs = 50 * platform::kNsPerMs;

wheelbri::ShimSources shimDllPaths();

#if defined(_WIN32)
// Windows: who may send game force feedback (S7; SenderGate.hpp). Any program of the user can send to 127.0.0.1:41330
// and the forces reach a direct-drive wheel, so only the virtual G920's driver host (WUDFHost.exe in System32) and a
// game whose folder holds WheelBri's shim are let through. Not found any more: refused; the system cannot say: let
// through (the limiter still caps every force), with a warning.
wheelbri::SenderVerdict lookupFfbSender(const std::uint16_t port) {
    static const std::filesystem::path systemFolder = platform::systemFolder();
    static const wheelbri::ShimSources shimDlls = shimDllPaths();
    static wheelbri::logging::RateLimiter limiter(std::chrono::seconds(30));
    const std::optional<std::uint32_t> pid = platform::udpPortOwner(port);
    if (!pid) {
        std::uint64_t suppressed = 0;
        if (limiter.allow(suppressed)) {
            WE_LOG_WARN(log(), "ffb: cannot tell which program sends from port {}; letting it through", port);
        }
        return wheelbri::SenderVerdict::Unknown;
    }
    const std::filesystem::path image = *pid != 0 ? platform::processImage(*pid) : std::filesystem::path();
    if (*pid != 0 && image.empty()) {
        // Windows would not say which program it is: as when the port's owner is unknown, the game's force feedback
        // must not be lost to a failed lookup (it still goes through the safety limiter).
        std::uint64_t suppressed = 0;
        if (limiter.allow(suppressed)) {
            WE_LOG_WARN(log(), "ffb: cannot read which program pid {} (port {}) is ({}); letting its force feedback through",
                        *pid, port, platform::lastProcessImageError());
        }
        return wheelbri::SenderVerdict::Unknown;
    }
    if (!image.empty()) {
        if (const std::string why = platform::lastProcessImageError(); !why.empty()) {
            // Once per process: the check runs again every few seconds, and the answer does not change.
            static std::uint32_t namedPid = 0;
            if (*pid != namedPid) {
                namedPid = *pid;
                WE_LOG_INFO(log(), "ffb: sender pid {} is {}, named by its id ({})", *pid, wheelbri::pathToUtf8(image), why);
            }
        }
        if (wheelbri::isDriverHostImage(image, systemFolder)) return wheelbri::SenderVerdict::Accept;
        // The game found last time, still running as the same program: accepted again without reading its folder
        // (shimState compares the game's dinput8.dll with ours byte by byte, too slow for the loop every 2 s).
        static std::uint32_t acceptedPid = 0;
        static std::filesystem::path acceptedImage;
        if (*pid == acceptedPid && image == acceptedImage) return wheelbri::SenderVerdict::Accept;
        const std::filesystem::path folder = image.parent_path();
        const wheelbri::ShimState state =
            wheelbri::shimState(folder, wheelbri::shimSourceFor(folder, shimDlls));
        if (state == wheelbri::ShimState::Installed || state == wheelbri::ShimState::Outdated) {
            WE_LOG_DEBUG(log(), "ffb: sender {} (pid {}) is a game with WheelBri's shim", wheelbri::pathToUtf8(image),
                         *pid);
            acceptedPid = *pid;
            acceptedImage = image;
            return wheelbri::SenderVerdict::Accept;
        }
    }
    std::uint64_t suppressed = 0;
    if (limiter.allow(suppressed)) {
        WE_LOG_WARN(log(), "ffb: ignoring force feedback from {} (pid {}, port {}): only the virtual G920's driver and "
                           "games with WheelBri's shim may send it ({} similar suppressed)",
                    image.empty() ? std::string("an unknown program") : wheelbri::pathToUtf8(image), *pid, port,
                    suppressed);
    }
    return wheelbri::SenderVerdict::Refuse;
}
#endif

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
        auto wheelSink = wheel.openForceFeedback(config.ffbInvert);
        if (wheelSink == nullptr) {
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
        // The wheel's driver takes up to ~30 ms for one call (starting an effect): on a thread of its own, so the
        // loop never waits for it (AsyncFfbSink.hpp).
        // Its priority raised as the loop's: with a game loading every core, Windows would let a normal thread wait.
        ffb->sink_ = std::make_unique<wheelbri::AsyncFfbSink>(std::move(wheelSink), [] {
            platform::nameThisThread("wheelbri ffb worker");
            if (std::string error; !platform::raiseThreadPriority(error)) {
                WE_LOG_WARN(log(), "cannot raise the force feedback thread's priority: {}", error);
            }
        });
        ffb->controller_ = std::make_unique<wheelbri::FfbController>(*ffb->sink_, config.ffb);
#if defined(_WIN32)
        wheelbri::SenderGate::Settings gate;
        gate.acceptUnknown = true;   // the lookup failing must not take a game's force feedback away
        ffb->gate_ = std::make_unique<wheelbri::SenderGate>(lookupFfbSender, gate);
#endif
        WE_LOG_INFO(log(), "game force feedback on: max {:.0f}% of the wheel's torque, gain {:.2f}",
                    config.ffb.maxMagnitude * 100.0F, config.ffb.masterGain);
        return ffb;
    }

    ~GameFfb() { stopForces(); }

    // Every force off now (the loop ended). Safe to call again; the next packet from the game starts them again.
    void stopForces() {
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
        // At most kMaxPerTick a tick: a flood must not keep the loop (and the wheel's input) here; the rest waits.
        for (std::size_t n = 0; n < kMaxPerTick; ++n) {
            std::uint16_t sender = 0;
            const auto size = listener_->receive(buffer, &sender);
            if (!size) break;
            if (gate_ != nullptr && !gate_->allow(sender, nowNs)) continue;
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
        // What reached the wheel since the last tick, and how long it took (the log's delay table, ffb-*.log).
        const std::size_t applied = sink_->drainApplied(applied_);
        for (std::size_t i = 0; i < applied; ++i) {
            delays_.record(applied_[i]);
            if (telemetry_ != nullptr) {
                telemetry_->writeFfbApplied(nowNs, applied_[i]);
            }
        }
    }

    [[nodiscard]] const wheelbri::FfbController& controller() const { return *controller_; }
    // A game profile's force feedback gain (FfbController::setGameGain); cheap when it does not change.
    void setGameGain(const float gain) { controller_->setGameGain(gain); }
    // The socket the game's packets arrive on (the loop waits for it).
    [[nodiscard]] std::intptr_t socket() const { return listener_->nativeSocket(); }

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
                           "watchdog stops, the wheel's driver took at most {:.1f} ms for one call",
                    receiving ? "sending" : "quiet", stats.activeSlots, stats.packets, stats.plays, stats.playFailures,
                    stats.watchdogStops, static_cast<double>(sink_->longestCallUs()) / 1000.0);
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
        if (delays_.any()) {
            WE_LOG_INFO(log(), "ffb delay to the wheel since the start (wait: before the wheel's driver was called; "
                               "wheel: the call){}",
                        sink_->droppedApplied() > 0
                            ? ", " + std::to_string(sink_->droppedApplied()) + " calls not counted"
                            : std::string());
            for (std::size_t g = 0; g < wheelbri::FfbDelayStats::kGroups; ++g) {
                if (const std::string line = delays_.line(static_cast<wheelbri::FfbDelayStats::Group>(g));
                    !line.empty()) {
                    WE_LOG_INFO(log(), "ffb delay:   {}", line);
                }
            }
        }
    }

private:
    GameFfb() = default;

    static constexpr std::size_t kMaxPerTick = 256;

    std::unique_ptr<wheelbri::AsyncFfbSink> sink_;   // before controller_, which uses it: destroyed after it
    std::unique_ptr<wheelbri::UdpLoopbackListener> listener_;
    std::unique_ptr<wheelbri::SenderGate> gate_;   // Windows only: who may send (lookupFfbSender)
    std::unique_ptr<wheelbri::FfbController> controller_;
    wheelbri::TelemetryRecorder* telemetry_ = nullptr;
    std::uint64_t lastPackets_ = 0;
    std::uint64_t loggedUsageNs_ = 0;
    std::optional<std::tuple<std::size_t, std::uint64_t, std::uint64_t, bool>> lastLogged_;
    std::array<wheelbri::AsyncFfbSink::Applied, wheelbri::AsyncFfbSink::kAppliedCapacity> applied_{};
    wheelbri::FfbDelayStats delays_;
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
        "target_profile: " + config.targetProfile + ", state_rate_hz: " + std::to_string(config.stateRateHz) +
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
    WE_LOG_INFO(log(),
                "telemetry: recording {} rows/s to {} (game force feedback: {}), a new file every start and every day",
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

// Brings `files` up to date. On the loop's thread: the paths are copied (which allocates) only when they changed (a
// new day's files), not every frame.
void updateTelemetryFiles(wheelbri::service::TelemetryFiles& files, const wheelbri::TelemetryRecorder* recorder) {
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

// What the window says about the virtual devices (--virtual-g920), see SessionInfo::devices.
struct DeviceSummary {
    std::string text;
    bool warning = false;
};

int runLoop(const wheelbri::EmulatorConfig& config, const std::string& configPath, const std::string& configText,
            const std::filesystem::path& logFolder, const bool logToFile, wheelbri::service::Wheel& source,
            const Accessories& accessories, const bool print, wheelbri::service::UiHost* const host,
            const DeviceSummary& devices, std::optional<UiFrame>& lastFrame, const bool listenTestClutch,
            const bool routePedals) {
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

    // The app's log level to whatever reads the state (the shim, the virtual G920's driver, wheelbri-uhid): its own
    // packet, from the same socket (LogLevelPacket.hpp).
    wheelbri::LogLevelBeacon levelBeacon;

    const wheelbri::Mapper mapper(config.mapper);
    wheelbri::Pipeline pipeline(source, mapper, *profile, *publisher);
    pipeline.setAccessories(accessories.sources());
    pipeline.limitSending(platform::kNsPerSecond / static_cast<std::uint64_t>(config.stateRateHz));

    // Test clutch while no clutch pedal is connected: ClutchPackets on kTestClutchPort (TestClutch.hpp). A developer
    // tool: not with the in-game shim, whose session starts only what the shim needs.
    const auto clutchListener =
        listenTestClutch ? wheelbri::UdpLoopbackListener::create(wheelbri::kTestClutchPort) : nullptr;
    if (clutchListener != nullptr) {
        WE_LOG_INFO(log(), "test clutch: listening on 127.0.0.1:{}", wheelbri::kTestClutchPort);
    }
    wheelbri::TestClutch testClutch;
    // Pedals pressed but the game never plays force feedback on the G920: it drives from another controller.
    wheelbri::GameUseWatch gameUse;
    // Force feedback loaded but none of it started while you drive (NFS 2015 at the start of a race): alt-tab.
    wheelbri::FfbIdleWatch ffbIdle;
    bool testClutchOn = false;

    std::string ffbStatus;
    const auto gameFfb = GameFfb::open(config, source, ffbStatus);
    // A game profile's force feedback gain ([ffb] gain): set by the window thread's GameWatch, read by the loop.
    std::atomic<float> gameFfbGain{1.0F};
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

    constexpr std::uint64_t kPrintEveryNs = 100'000'000;   // --print: 10 lines a second
    const wheelbri::WakeSettings wakeSettings;
    // The window's copy of the state, kWindowFramesPerSecond (60) times a second, as often as the window draws.
    const std::uint64_t frameEveryNs = wakeSettings.windowNs;

    // The loop sleeps until the wheel, an accessory, the game's force feedback or the test clutch has something new
    // (platform::WaitSet), and wakes by itself only for what must run on time (core EventLoopControl, WakePlanner).
    // Whatever cannot notify it (a device DirectInput only polls, a socket the wait cannot take) still gets read at
    // every pass, and the loop then also polls every millisecond; everything else still wakes it at once.
    platform::WaitSet waitSet;
    std::vector<wheelbri::WakeReason> wakeReasons;   // what each of waitSet's sources is
    static_assert(platform::WaitSet::kFailed == wheelbri::kWaitFailed &&
                  platform::WaitSet::kTimedOut == wheelbri::kWaitTimedOut);
    wheelbri::EventLoopControl control;
    const auto pollingNote = [&]() {
        return std::to_string(control.settings().pollNs / platform::kNsPerMs) + " ms";
    };
    const auto addDevice = [&](const wheelbri::service::Wheel& device) {
        if (waitSet.addEvent(device.waitSource()) < 0) {
            WE_LOG_WARN(log(), "loop: '{}' gives no notification of new input (DirectInput polls it): the loop polls "
                               "it every {}",
                        device.info().name, pollingNote());
            control.pollBecause(wheelbri::PollReason::NoNotification);
            return;
        }
        wakeReasons.push_back(wheelbri::WakeReason::Wheel);
    };
    addDevice(source);
    for (const auto& accessory : accessories.devices) {
        if (accessory != nullptr) addDevice(*accessory);
    }
    const auto addSocket = [&](const std::intptr_t socket, const wheelbri::WakeReason reason, const char* what) {
        if (waitSet.addSocket(socket) < 0) {
            WE_LOG_WARN(log(), "loop: cannot wait for {} packets: the loop reads them every {}", what, pollingNote());
            control.pollBecause(wheelbri::PollReason::SocketNotWaitable);
            return;
        }
        wakeReasons.push_back(reason);
    };
    if (gameFfb != nullptr) addSocket(gameFfb->socket(), wheelbri::WakeReason::GameFfb, "force feedback");
    if (clutchListener != nullptr) {
        addSocket(clutchListener->nativeSocket(), wheelbri::WakeReason::TestClutch, "test clutch");
    }

    // The loop (wheel input, state to the game, force feedback, telemetry) runs on its own thread; the
    // window and old-file checks stay on this one. A window that Windows holds up (a game going full screen,
    // dragging it) then only freezes the window, never the wheel or its force feedback.
    std::mutex frameMutex;
    UiFrame sharedFrame;
    std::atomic<bool> stopLoop{false};
    std::atomic<bool> loopDone{false};
    int loopResult = 0;

    const auto tickLoop = [&]() -> int {
        // Above normal, so a game that is loading (all cores busy) does not starve the input loop.
        if (std::string error; !platform::raiseThreadPriority(error)) {
            WE_LOG_WARN(log(), "cannot raise the loop's thread priority: {}", error);
        }
        if (control.polling()) {
            WE_LOG_INFO(log(), "running on events, and polling every {} ({}); press Ctrl+C to stop", pollingNote(),
                        wheelbri::describe(control.pollReason()));
        } else {
            WE_LOG_INFO(log(), "running on events: it wakes when the wheel or the game has something new, else every "
                               "{} ms at most; press Ctrl+C to stop",
                        wakeSettings.idleNs / platform::kNsPerMs);
        }
        wheelbri::WakeStats wakeStats;
        wheelbri::Periodic frameDue(frameEveryNs);
        wheelbri::Periodic printDue(kPrintEveryNs);
        const std::uint64_t loopStart = platform::nowNs();
        std::uint64_t lastTickStart = loopStart;
        std::uint64_t windowStart = loopStart;
        std::uint64_t ticks = 0;
        std::uint64_t lastPeriodNs = 0;
        std::uint64_t lastWorkNs = 0;
        std::uint64_t lastUsageLogNs = loopStart;
        std::uint64_t lastWakeLogNs = loopStart;
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
            if (!pipeline.tick(clutchOverride, tickStart)) {
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
                gameFfb->setGameGain(gameFfbGain.load(std::memory_order_relaxed));
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
                const wheelbri::FfbController& controller = gameFfb->controller();
                if (const auto change = ffbIdle.update(platform::nowMs(), pedal, controller.lastingLoaded(),
                                                       controller.lastingWanted())) {
                    if (*change) {
                        WE_LOG_WARN(log(), "the game loaded its force feedback ({} effects) but has started none of "
                                           "them while you drove for {} s, so the wheel stays limp. Alt-tab out of "
                                           "the game and back once: it then starts them (NFS 2015 does this at the "
                                           "start of some races)",
                                    controller.lastingLoaded(), wheelbri::FfbIdleWatch::kPedalMs / 1000);
                    } else {
                        WE_LOG_INFO(log(), "the game's force feedback plays again (or it unloaded it)");
                    }
                }
            }
            if (const wheelbri::LogLevel level = wheelbri::service::AppLogLevel::shared().current();
                levelBeacon.due(platform::nowMs(), level)) {
                static_cast<void>(publisher->publishLogLevel(level));
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
            if (print && printDue.due(tickStart)) {
                printLive(pipeline);
            }
            if (host != nullptr && frameDue.due(tickStart)) {
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
                    updateTelemetryFiles(sharedFrame.telemetry, telemetry.get());
                    sharedFrame.gameIgnoresWheel = gameUse.warning();
                    sharedFrame.gameFfbIdle = ffbIdle.warning();
                    sharedFrame.ready = true;
                }
            }
            stages.lap(TickStages::Ui, mark);

            const std::uint64_t tickEnd = platform::nowNs();
            const std::uint64_t period = tickStart - lastTickStart;
            lastTickStart = tickStart;
            lastPeriodNs = period;
            lastWorkNs = tickEnd - tickStart;
            wakeStats.record(control.wokeBy(), tickEnd - tickStart);
            if (tickEnd - tickStart >= kStallNs) {
                static wheelbri::logging::RateLimiter limiter(std::chrono::seconds(5));
                WE_LOG_WARN_LIMITED(log(), limiter,
                                    "loop: one tick took {:.0f} ms, the game got no fresh input meanwhile: {}",
                                    static_cast<double>(tickEnd - tickStart) / 1e6, stages.describe());
            }
            if (tickEnd - windowStart >= kStatsWindowNs) {
                // Once a minute: how often it woke and why.
                if (tickEnd - lastWakeLogNs >= kWakeLogNs) {
                    WE_LOG_INFO(log(), "loop: {}; {}{}; since start, notifications missed {}, late {}; packet #{}, {} "
                                       "publish failures",
                                wakeStats.describe(tickEnd - lastWakeLogNs),
                                control.polling() ? "polling: " : "on events",
                                control.polling() ? wheelbri::describe(control.pollReason()) : "",
                                control.missedNotifications(), control.lateNotifications(), pipeline.sequence(),
                                pipeline.publishFailures());
                    wakeStats.reset();
                    lastWakeLogNs = tickEnd;
                }
                if (gameFfb != nullptr) {
                    gameFfb->logStatsIfChanged();
                    if (tickEnd - lastUsageLogNs >= kUsageLogNs) {
                        gameFfb->logUsage();
                        lastUsageLogNs = tickEnd;
                    }
                }
                windowStart = tickEnd;
            }

            // Until something new arrives, or what must run on time is due (core EventLoopControl, WakePlanner; every
            // millisecond while the loop polls); then at least minGapNs after this pass started (a source that signals
            // without pause cannot spin the loop). It polls as well, for the rest of the session, when the timer keeps
            // finding new input (the devices' notifications miss it), and only sleeps once the wait itself failed.
            wheelbri::EventLoopControl::Pass pass;
            pass.startNs = tickStart;
            pass.inputChanged = pipeline.inputChanged();
            pass.windowOpen = host != nullptr;
            pass.forcesPlaying = gameFfb != nullptr && gameFfb->controller().stats().activeSlots > 0;
            control.afterPass(pass);
            const std::uint64_t deadline = control.deadlineNs();
            int ready = wheelbri::kWaitTimedOut;
            if (control.canWait()) {
                ready = waitSet.waitUntil(deadline);
            } else if (const std::uint64_t now = platform::nowNs(); deadline > now) {
                platform::sleepNs(deadline - now);
            }
            const std::uint64_t woke = platform::nowNs();
            if (woke > deadline && woke - deadline >= kStallNs) {
                // Not our work: the system let the loop sleep far longer than asked (busy, or asleep: a Steam Deck
                // with its screen off sleeps, and a few seconds late is that).
                static wheelbri::logging::RateLimiter limiter(std::chrono::seconds(5));
                WE_LOG_WARN_LIMITED(log(), limiter,
                                    "loop: woke {:.0f} ms after its deadline: the system did not run the service for "
                                    "that long ({})",
                                    static_cast<double>(woke - deadline) / 1e6,
                                    woke - deadline >= 2'000'000'000ULL ? "asleep?" : "busy?");
            }
            const wheelbri::EventLoopControl::Woke next = control.afterWait(ready, wakeReasons, woke);
            if (next.startedPolling) {
                if (control.pollReason() == wheelbri::PollReason::WaitFailed) {
                    WE_LOG_WARN(log(), "loop: cannot wait for the devices ({}); the loop polls every {} for the rest "
                                       "of this session",
                                waitSet.failureText(), pollingNote());
                } else {
                    WE_LOG_WARN(log(), "loop: the timer found new input {} times within {} s that no notification "
                                       "announced: the devices' notifications cannot be trusted; the loop also polls "
                                       "every {} for the rest of this session",
                                control.settings().missThreshold,
                                control.settings().missWindowNs / platform::kNsPerSecond, pollingNote());
                }
            }
            if (next.sleepNs > 0) platform::sleepNs(next.sleepNs);
        }
        if (print) {
            std::puts("");
        }
        WE_LOG_INFO(log(), "loop, since the last report: {}; packet #{}",
                    wakeStats.describe(platform::nowNs() - lastWakeLogNs), pipeline.sequence());
        if (gameFfb != nullptr) {
            gameFfb->logUsage();
        }
        if (gStop) {
            WE_LOG_INFO(log(), "stopped by signal after {} ticks", ticks);
        }
        return result;
    };

    std::thread loopThread([&] {
        platform::nameThisThread("wheelbri loop");
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
        // Nothing updates the forces (or runs their watchdog) once the loop has ended, and the clean-up that stops
        // them waits for the window side to notice, which can take a while (a window being dragged): stop them now.
        if (gameFfb != nullptr) {
            try {
                gameFfb->stopForces();
            } catch (...) {
                WE_LOG_ERROR(log(), "loop: stopping the game's forces failed");
            }
        }
        loopDone = true;
    });

    // This thread: the window (kWindowFramesPerSecond, 60 fps) and old files (every 250 ms, and again after every
    // midnight).
    try {
        constexpr std::uint64_t kFrameNs = platform::kNsPerSecond / wheelbri::kWindowFramesPerSecond;
        std::uint64_t lastHousekeepingNs = 0;
        // The game that runs, looked for every 2 s here (a few milliseconds; never on the loop): its pedal profile.
        // This thread runs with or without a window, so headless sessions get it too.
        constexpr std::uint64_t kGameCheckNs = 2 * platform::kNsPerSecond;
        std::uint64_t lastGameCheckNs = 0;
        bool gameChecked = false;
        // Game profiles: applied here for the virtual G920 / wheelbri-uhid, by the shim itself for the in-game shim.
        wheelbri::service::GameWatch gameWatch(
            config.targetProfile, routePedals,
            config.mapper.steering.targetRotationDeg,
            config.gameRotations, config.gameFfbGains);
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
                if (!gameChecked || now - lastGameCheckNs >= kGameCheckNs) {
                    lastGameCheckNs = now;
                    gameChecked = true;
                    try {
                        const auto decision = gameWatch.check();
                        pipeline.setPedalRoute(decision.route);
                        gameFfbGain.store(decision.ffbGain, std::memory_order_relaxed);   // a game's [ffb] gain
                    } catch (const std::exception& e) {   // never ends the session; the pedals stay as they are
                        WE_LOG_WARN(log(), "game: looking for known games failed: {}", e.what());
                    }
                }
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
            frame->game = gameWatch.status();
            frame->gameWarning = gameWatch.warning();
            frame->gameRotation = gameWatch.rotationStatus();
            if (const auto& advice = gameWatch.wayAdvice()) {
                frame->gameWayTitle = advice->title;
                frame->gameWayText = advice->text;
            }
            frame->gameWayNotices = gameWatch.wayNotices();
            {
                const wheelbri::service::GameWatch::Live live = gameWatch.live();
                frame->gameId = live.id;
                frame->gamePid = live.pid;
                const std::uint64_t nowMs = platform::nowMs();
                frame->gameForMs = live.id.empty() || nowMs < live.sinceMs ? 0 : nowMs - live.sinceMs;
                frame->gameStarts = live.starts;
                frame->gameFfbGain = live.ffbGain;
            }
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

std::filesystem::path logFolderOf(const wheelbri::EmulatorConfig& config);

// The config, with --old-files applied and the rig's wheel when it names none.
LoadedConfig loadForRun(const Options& options) {
    LoadedConfig loaded = loadConfig(options.configPath);
    if (!options.oldFiles.empty()) {
        loaded.config.retention.action = *wheelbri::parseOldFilesAction(options.oldFiles);
    }
#if defined(_WIN32)
    // Only when running as Administrator: the config is a file any program of the user can edit, so it must not choose
    // where the Administrator writes; log and telemetry files then only go under the user's WheelBri folders. A
    // standard user writes wherever they may write anyway.
    if (platform::isElevated()) {
        if (auto& file = loaded.config.logging.filePath;
            !file.empty() && !wheelbri::service::isInsideUserFolders(wheelbri::pathFromUtf8(file))) {
            loaded.warnings.push_back("log file '" + file + "' is outside the WheelBri folders, using the default log file");
            file.clear();
        }
        if (auto& folder = loaded.config.telemetry.directory;
            !folder.empty() && !wheelbri::service::isInsideUserFolders(wheelbri::pathFromUtf8(folder))) {
            loaded.warnings.push_back("telemetry folder '" + folder +
                                      "' is outside the WheelBri folders, using the log folder");
            folder.clear();
        }
        for (const std::filesystem::path& folder :
             {logFolderOf(loaded.config), wheelbri::pathFromUtf8(loaded.config.telemetry.directory)}) {
            for (std::string& note : wheelbri::service::removeLinkedLogFiles(folder)) {
                loaded.warnings.push_back(std::move(note));
            }
        }
    }
#endif
    const std::string rigName = wheelbri::readAppSetting(wheelbri::service::appStateFile(), "rig");
    std::vector<std::string> rigErrors;
    const auto rig =
        rigName.empty() ? std::nullopt : wheelbri::readRig(wheelbri::service::rigsFolder(), rigName, rigErrors);
    // No source_device in the config (the built-in ones have none; an older Mapping tab's Change set one): the wheel is
    // the active rig's (Rig::wheel: its first device of kind wheel, else its first). Without a rig it stays empty and
    // the session says so.
    if (loaded.config.sourceDeviceMatch.empty()) {
        if (rig) {
            const wheelbri::RigDevice* const wheel = rig->wheel();
            if (wheel != nullptr && !wheel->id.empty()) {
                loaded.config.sourceDeviceMatch = wheelbri::deviceId(wheel->id.vendorId, wheel->id.productId);
            }
        }
    }
    // A control is used once (one input's assignment or one button's secondary): an older mapping that uses one
    // twice still runs as written, but both inputs press together.
    if (const auto profile = wheelbri::makeProfile(loaded.config.targetProfile)) {
        for (const std::string& line : wheelbri::controlsUsedTwice(wheelbri::mappingModel(loaded.config), *profile)) {
            loaded.warnings.push_back("a control used twice: " + line + " (the Mapping tab marks it red)");
        }
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
    for (const std::string& warning : loaded.warnings) {
        WE_LOG_WARN(log(), "config {}: {}", options.configPath, warning);
    }
    for (const std::string& note : loaded.notes) {
        WE_LOG_INFO(log(), "rig: {}", note);
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
    // One session at a time on this computer: a second would send the game a second, interleaved wheel.
    std::string instanceError;
    const auto instance = platform::SingleInstance::acquire("wheelbri-service", instanceError);
    if (instance == nullptr) {
        // --unhide too: a session that is running hides the rig on purpose (the game would see it next to the G920).
        WE_LOG_CRITICAL(log(), "{}{}", instanceError, options.unhide ? " (--unhide: stop that session first)" : "");
        return 1;
    }
    if (options.unhide) {
        wheelbri::service::HideFilter::clear();
        return 0;
    }
    if (options.prepareHide) {
        // The installer, after a reinstall that kept the user's settings: the filter on the saved rig's devices now,
        // so the first Start does not have to. Prints one line (the installer shows it); the exit code is HideSetup's.
        if (!platform::isElevated()) {
            std::puts("--prepare-hide needs Administrator");
            return 1;
        }
        std::string rigName;
        const auto targets = rigHideTargets(rigName);
        if (targets.empty()) {
            std::puts(rigName.empty() ? "no saved rig: the first Start sets up the hiding"
                                      : "the saved rig lists no device to hide");
            return static_cast<int>(HideSetup::NothingToDo);
        }
        wheelbri::service::PrivilegedSteps::Problem problem{};
        std::string problemText;
        const auto steps = wheelbri::service::PrivilegedSteps::open(problem, problemText);
        if (steps == nullptr) {
            std::puts(problemText.c_str());
            return 1;
        }
        const HideSetup result = ensureHideFilter(targets, *steps);
        std::puts(result == HideSetup::Running   ? "the rig's devices are hidden from games whenever WheelBri runs"
                  : result == HideSetup::Waiting ? "the filter is on; it starts after the restart of Windows"
                  : result == HideSetup::NothingToDo
                      ? "none of the rig's devices is plugged in: the first Start sets up the hiding"
                      : "could not set up the hiding (see the hide installer's log): the first Start tries again");
        return static_cast<int>(result);
    }
    // Until the end of this function (its clean-up included), the console close handler waits.
    gSessionActive = true;
    struct SessionActive {
        ~SessionActive() { gSessionActive = false; }
    } sessionActiveGuard;
    platform::installConsoleStopHandler(requestStop, sessionActive);

    // The virtual G920 and hiding the rig need Administrator: this process's own when it is elevated, else the
    // WheelBri Helper's (a standard user).
    std::unique_ptr<wheelbri::service::PrivilegedSteps> steps;
    if (options.virtualG920) {
        wheelbri::service::PrivilegedSteps::Problem problem{};
        std::string problemText;
        steps = wheelbri::service::PrivilegedSteps::open(problem, problemText);
        if (steps == nullptr) {
            WE_LOG_CRITICAL(log(), "{}: the virtual G920 and hiding the rig need Administrator or the WheelBri "
                                   "Helper: {}",
                            problem == wheelbri::service::PrivilegedSteps::Problem::NoHelper ? "HELPER NOT INSTALLED"
                                                                                             : "HELPER REFUSED",
                            problemText);
            return 1;
        }
        WE_LOG_INFO(log(), "the virtual G920 and hiding the rig: {}", steps->how());
        steps->shareLogFolder();
        steps->setDriverLogLevel(wheelbri::service::AppLogLevel::shared().current());
    }
    // A wheel still hidden for a service that crashed would be hidden from this one too (through the helper, the
    // helper clears that itself).
    if (platform::isElevated()) wheelbri::service::HideFilter::clearStale();
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
    if (steps != nullptr) ensureG920Driver(*steps);
    if (options.virtualG920 && options.hide && !gStop) {
        if (hideTargets.empty()) {
            WE_LOG_WARN(log(), "hide: no rig yet{}: nothing is hidden from games (define the devices in the Rig tab)",
                        rigName.empty() ? "" : " ('" + rigName + "' lists no device)");
        } else {
            (void)ensureHideFilter(hideTargets, *steps, [host](const std::string& what) {
                if (host != nullptr) host->step(what);
            });
        }
    }
    if (cancelled()) {
        WE_LOG_INFO(log(), "stopped before the session started");
        return 0;
    }

    if (config.sourceDeviceMatch.empty()) {
        WE_LOG_CRITICAL(log(), "no wheel to read: there is no rig with a wheel yet (set it up in the Rig tab) and the "
                               "config names none ([emulator] source_device)");
        return 2;
    }
    // A wheel Windows is still restarting (after a reinstall, or while a driver or filter is put on it) is gone for
    // a few seconds: wait a little for it instead of failing at once. Stop gives up.
    if (!wheelbri::service::wheelConnected(*backend, config.sourceDeviceMatch)) {
        const wheelbri::WaitSettings wait;
        WE_LOG_INFO(log(), "the wheel ('{}') is not connected yet: waiting up to {} s for it (Windows may be restarting "
                           "it); Stop gives up",
                    config.sourceDeviceMatch, wait.timeoutMs / 1000);
        std::uint64_t waitedMs = 0;
        if (host != nullptr) host->step("waiting for the wheel (up to " + std::to_string(wait.timeoutMs / 1000) + " s)");
        // Each check of the wheel at once when Windows says a device came (else every 250 ms, for Stop as well).
        wheelbri::ChangeCounter& changes = platform::DeviceChanges::shared().counter();
        const wheelbri::WaitOutcome outcome = wheelbri::waitFor(
            wait, [&] { return wheelbri::service::wheelConnected(*backend, config.sourceDeviceMatch); }, cancelled,
            [&changes](const std::uint64_t ms) {
                const std::uint64_t start = platform::nowMs();
                (void)changes.waitForChange(changes.count(), ms);
                return platform::nowMs() - start;
            },
            &waitedMs);
        if (host != nullptr) host->step({});
        if (outcome == wheelbri::WaitOutcome::Cancelled) {
            WE_LOG_INFO(log(), "stopped while waiting for the wheel");
            return 0;
        }
        if (outcome == wheelbri::WaitOutcome::Ready) {
            WE_LOG_INFO(log(), "the wheel is there after {:.1f} s", static_cast<double>(waitedMs) / 1000.0);
        } else {
            WE_LOG_WARN(log(), "the wheel did not appear within {} s", waitedMs / 1000);
        }
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
    if (!options.ffbTest) {
        // A mapped axis the device does not have reads centred or released for ever: say so (Start still goes on).
        std::array<int, wheelbri::kMaxInputDevices> axisCounts{};
        axisCounts.fill(-1);
        axisCounts[0] = source->axes();
        for (std::size_t i = 0; i < accessories.devices.size(); ++i) {
            if (accessories.devices[i] != nullptr) axisCounts[i + 1] = accessories.devices[i]->axes();
        }
        for (const std::string& problem : wheelbri::missingMappedAxes(config.mapper, axisCounts)) {
            WE_LOG_ERROR(log(), "mapping: {}; it will not move (map another axis in the Mapping tab)", problem);
        }
    }

    if (options.ffbTest) {
        const auto sink = source->openForceFeedback(config.ffbInvert);
        if (sink == nullptr) {
            return 1;
        }
        return wheelbri::service::runFfbSelfTest(*sink, config.ffb, [] { return gStop.load(); }) ? 0 : 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#ifdef SIGHUP
    // Linux: the terminal closed, or Steam's wrapper (wheelbri.sh) went away: the same clean stop, never cut off.
    std::signal(SIGHUP, onSignal);
#endif
    const std::filesystem::path logFolder = logFolderOf(config);
    // Our own driver's wheel (driver/), the config's target (a G920 or a G25): there while the loop runs, removed when
    // it returns or the process ends.
    const wheelbri::VirtualWheel* virtualWheel = wheelbri::virtualWheel(config.targetProfile);
    const std::string wheelName = virtualWheel != nullptr ? std::string(virtualWheel->instancePrefix) : "G920";
    bool g920 = false;
    if (steps != nullptr) {
        if (virtualWheel == nullptr) {
            WE_LOG_CRITICAL(log(), "--virtual-g920: no virtual device for target_profile '{}'", config.targetProfile);
            return 1;
        }
        std::string error;
        std::string g920Id;
        g920 = steps->createVirtualG920(*virtualWheel, cancelled, g920Id, error);
        if (!g920) {
            if (cancelled()) {
                WE_LOG_INFO(log(), "stopped while the virtual {} was being created", wheelName);
                return 0;
            }
            WE_LOG_CRITICAL(log(), "--virtual-g920: {}", error);
            return 1;
        }
    }
#ifndef _WIN32
    // Linux: the virtual G920 is wheelbri-uhid, there until this function returns (Stop, or the window closed).
    std::unique_ptr<wheelbri::service::UhidProcess> uhid;
    if (options.uhid) {
        if (virtualWheel == nullptr) {
            WE_LOG_CRITICAL(log(), "virtual wheel: no virtual device for target_profile '{}'", config.targetProfile);
            return 1;
        }
        std::string error;
        uhid = wheelbri::service::UhidProcess::start(logFolder, config.targetProfile, error);
        if (uhid == nullptr) {
            WE_LOG_CRITICAL(log(), "virtual {}: {}", wheelName, error);
            return 1;
        }
    }
#endif
    // The rig hidden from games meanwhile, or a game may take a real device instead of the G920 (NFS does): with
    // wheelbri's own filter (driver/hide) on every device of the rig, which lets only this process open them.
    bool rigHidden = false;
    std::vector<wheelbri::service::HideFilter::Check> hideChecks;
    if (g920 && options.hide && !hideTargets.empty()) {
        hideChecks = wheelbri::service::HideFilter::check(hideTargets);
        bool any = false;
        for (const auto& c : hideChecks) any = any || c.state == wheelbri::service::HideFilter::State::Ready;
        if (any) {
            std::string message;
            rigHidden = steps->hideRig(message);
            if (!rigHidden) WE_LOG_ERROR(log(), "hide: {}", message);
        }
    }
    // One line for the window: what games will see besides the G920, and what is missing.
    DeviceSummary devices;
    if (g920) {
        devices.text = "virtual " + wheelName + " on";
        if (!options.hide) {
            devices.text += ", rig left visible (--no-hide)";
        } else if (hideTargets.empty()) {
            devices.text += ", NOTHING HIDDEN: no rig yet (Rig tab)";
            devices.warning = true;
        } else {
            int hidden = 0;
            std::string notHidden;
            for (const auto& c : hideChecks) {
                if (c.state == wheelbri::service::HideFilter::State::Ready && rigHidden) {
                    ++hidden;
                } else if (c.state != wheelbri::service::HideFilter::State::NotConnected) {
                    notHidden += (notHidden.empty() ? "" : ", ") + c.target.name + " (" +
                                 (!rigHidden && c.state == wheelbri::service::HideFilter::State::Ready
                                      ? std::string("setting not written")
                                      : wheelbri::service::HideFilter::stateName(c.state)) +
                                 ")";
                }
            }
            devices.text += ", rig hidden (" + std::to_string(hidden) + " of " + std::to_string(hideTargets.size()) +
                            " devices)";
            if (!notHidden.empty()) {
                devices.text += ", NOT HIDDEN: " + notHidden + ": close apps using it or replug it, then Start";
                devices.warning = true;
                WE_LOG_ERROR(log(), "hide: games can see {}: the filter does not run there yet (the hide lines above "
                                    "say why)",
                             notHidden);
            }
        }
    }
#ifndef _WIN32
    if (uhid != nullptr) {
        // Hiding the rig is the game's launch option on Linux (SDL_GAMECONTROLLER_IGNORE_DEVICES, wheelbri.sh).
        devices.text =
            "virtual " + wheelName + " on (" + uhid->describe() + "); hide the rig in the game's launch options";
    }
#endif
    int result = 0;
    std::optional<UiFrame> lastFrame;
    std::uint64_t sessionStartMs = platform::nowMs();
    int quickLosses = 0;   // lost again soon after it came back: wait longer before the next try
    while (!gStop) {
        sessionStartMs = platform::nowMs();
        result = runLoop(config, options.configPath, loaded.text, logFolder, logFileOk, *source, accessories,
                         options.print, host, devices, lastFrame, /*listenTestClutch=*/options.virtualG920,
                         /*routePedals=*/options.virtualG920 || options.uhid);
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
                source = wheelbri::service::openWheel(*backend, config.sourceDeviceMatch, /*quiet=*/true);
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
    if (steps != nullptr) {
        steps->showRig();
        steps->removeVirtualG920();
    }
#ifndef _WIN32
    if (uhid != nullptr && !uhid->running()) {
        WE_LOG_ERROR(log(), "virtual G920: wheelbri-uhid ended during the session (its log says why)");
    }
#endif
    return result;
}

// The command line service: one session, with the --ui window when asked for.
int run(const Options& options) {
    const LoadedConfig loaded = loadForRun(options);
    const bool logFileOk = startProcess(options, loaded.config.logging, true);
    wheelbri::service::shareLogFolder(logFolderOf(loaded.config));
    std::unique_ptr<wheelbri::service::UiHost> window;
    if (options.ui) {
        window = wheelbri::service::makeSessionWindow(options.configPath);
    }
    return runSession(options, loaded, logFileOk, window.get());
}

// The config used when none is given: config/g920.ini (the target's default mapping) next to the program, or one or two
// folders up (build/service/ in the source tree), or under the current folder.
std::string defaultConfigPath() {
    const std::filesystem::path exe = platform::executablePath();
    std::vector<std::filesystem::path> bases;
    if (!exe.empty()) {
        bases = {exe.parent_path(), exe.parent_path().parent_path(), exe.parent_path().parent_path().parent_path()};
    }
    bases.push_back(std::filesystem::current_path());
    for (const auto& base : bases) {
        const std::filesystem::path candidate = base / "config" / "g920.ini";
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate.lexically_normal().string();
        }
    }
    return {};
}

// WheelBri's own dinput8.dll, which the app puts into games, 64-bit and 32-bit: shim\ and shim\x86\ next to the
// program (the installed package), or build/shim/ and build-x86/shim/ (next to build/service/ in the source tree).
// Each empty when there is none (Linux, or not built).
wheelbri::ShimSources shimDllPaths() {
    const std::filesystem::path exe = platform::executablePath();
    if (exe.empty()) return {};
    const auto first = [](std::initializer_list<std::filesystem::path> candidates) {
        for (const std::filesystem::path& candidate : candidates) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(candidate, ec)) return candidate.lexically_normal();
        }
        return std::filesystem::path();
    };
    const std::filesystem::path here = exe.parent_path();
    const std::filesystem::path build = here.parent_path();   // build/ when the program is build/service/x.exe
    wheelbri::ShimSources sources;
    sources.x64 = first({here / "shim" / "dinput8.dll", build / "shim" / "dinput8.dll"});
    std::filesystem::path x86Build = build.filename();   // build-x86/ (scripts/make/shim-x86.sh) next to build/
    x86Build += "-x86";
    sources.x86 =
        first({here / "shim" / "x86" / "dinput8.dll", build.parent_path() / x86Build / "shim" / "dinput8.dll"});
    return sources;
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
#else
    options.uhid = !options.noVirtualG920;
#endif
    options.print = false;
    const LoadedConfig loaded = loadForRun(options);
    const bool logFileOk = startProcess(options, loaded.config.logging, consoleLogging);
    wheelbri::service::shareLogFolder(logFolderOf(loaded.config));
    // The Logs tab's Level button, remembered (warn the first time): this process now; the shim, the driver and
    // wheelbri-uhid from the session's packets; the helper from its Hellos; the drivers' registry value at each Start
    // and, while a session runs, at once.
    wheelbri::service::AppLogLevel::shared().start(wheelbri::service::appStateFile());
    wheelbri::service::AppLogLevel::shared().addListener([](const wheelbri::LogLevel level) {
        const bool running = gSessionActive;
        wheelbri::service::HelperProbe::shared().levelChanged(level, running);
        if (running && platform::isElevated()) {
            std::string message;
            if (!wheelbri::service::privileged::writeLogLevel(level, message)) {
                WE_LOG_WARN(log(), "logs: the drivers keep their log level until the next Start: {}", message);
            }
        }
    });

    if (!pickedGone.empty()) {
        WE_LOG_WARN(log(), "the config picked last time is gone ({}): using {}", pickedGone,
                    options.configPath.empty() ? std::string("the built-in defaults") : options.configPath);
    }

    wheelbri::service::AppSettings settings;
    if (const std::string builtIn = defaultConfigPath(); !builtIn.empty()) {
        settings.builtInConfigFolder = std::filesystem::path(builtIn).parent_path();
    }
    settings.userConfigFolder = wheelbri::service::userConfigFolder();
    settings.stateFile = wheelbri::service::appStateFile();
    settings.logFolder = logFolderOf(loaded.config);
    settings.devicesFolder = wheelbri::service::deviceLabelsFolder();
    settings.shimDlls = shimDllPaths();
    if (!settings.builtInConfigFolder.empty()) settings.shimSettings = settings.builtInConfigFolder / "wheelbri-shim.ini";
    settings.runSession = [options, logFileOk](wheelbri::service::UiHost& host, const std::string& configPath,
                                               const bool virtualDevice) {
        Options run = options;
        run.configPath = configPath;   // the config the Mapping tab shows
#ifdef _WIN32
        run.virtualG920 = virtualDevice;   // the Mapping tab's target: G920 virtual device or G920 in-game shim
#else
        run.uhid = virtualDevice;   // Linux: the session starts wheelbri-uhid for the virtual G920
#endif
        const LoadedConfig fresh = loadForRun(run);
        return runSession(run, fresh, logFileOk, &host);
    };
    settings.requestStop = [] { gStop = true; };
    settings.clearStop = [] { gStop = false; };
    return wheelbri::service::runApp(settings);
}

}  // namespace

int main(int argc, char** argv) {
#ifdef WHEELBRI_APP
    constexpr bool kAppBuild = true;   // wheelbri.exe: a window program, no console
#else
    constexpr bool kAppBuild = false;
#endif
    int exitCode = 1;
    try {
        wheelbri::service::moveFoldersFromOldName();   // before anything reads the user's folders
        // As Administrator it must not write through a link the user's programs put in its folders (a standard user
        // can only reach what they could reach anyway).
        if (const std::string problem = platform::isElevated() ? wheelbri::service::userFoldersProblem() : std::string();
            !problem.empty()) {
            wheelbri::service::reportStartupProblem(problem, kAppBuild);
            return 1;
        }
        Options options;
        if (!parseOptions(argc, argv, options)) {
            return 2;
        }
        wheelbri::service::MappingView::setOptionalTabs(options.telemetryTab, options.ffbTab);
        exitCode = kAppBuild || options.app ? runAppMode(options, !kAppBuild) : run(options);
    } catch (const std::exception& e) {
        WE_LOG_CRITICAL(log(), "unhandled exception: {}", e.what());
        if (kAppBuild) wheelbri::service::reportStartupProblem(std::string("stopped by an error: ") + e.what(), true);
    } catch (...) {
        WE_LOG_CRITICAL(log(), "unhandled non-standard exception");
    }
    WE_LOG_INFO(log(), "exit code {}", exitCode);
    wheelbri::logging::shutdown();
    return exitCode;
}
