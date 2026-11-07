// SPDX-License-Identifier: GPL-3.0-or-later
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
#include "ServiceLoop.hpp"
#include "SessionUi.hpp"
#include "VirtualWheelDevice.hpp"
#ifndef _WIN32
#include "UhidProcess.hpp"
#endif
#include "Wheel.hpp"

#include "wheelbri/AutoMap.hpp"
#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/DeviceLabels.hpp"
#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/AsyncFfbSink.hpp"
#include "wheelbri/FfbController.hpp"
#include "wheelbri/FfbPacket.hpp"
#include "wheelbri/FileText.hpp"
#include "wheelbri/FfbIdleWatch.hpp"
#include "wheelbri/GameUseWatch.hpp"
#include "wheelbri/HidWheelDevice.hpp"
#include "wheelbri/HideIds.hpp"
#include "wheelbri/LiveMapping.hpp"
#include "wheelbri/LogLevelPacket.hpp"
#include "wheelbri/Logging.hpp"
#include "wheelbri/MappingEdit.hpp"
#include "wheelbri/Pipeline.hpp"
#include "wheelbri/Rig.hpp"
#include "wheelbri/SenderGate.hpp"
#include "wheelbri/ShimGames.hpp"
#include "wheelbri/Telemetry.hpp"
#include "wheelbri/TrueForce.hpp"
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
#include <memory>
#include <map>
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
#include <string_view>
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

std::atomic<bool> gStop{false};
// A session is between its start and the end of its clean-up (the console close handler waits for it).
std::atomic<bool> gSessionActive{false};

void onSignal(int) { gStop = true; }
void requestStop() { gStop = true; }
bool sessionActive() { return gSessionActive; }

using wheelbri::service::kWheelLost;

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
    bool virtualDevice = false;                   // --virtual-wheel: our own driver's G920 (driver/)
    bool uhid = false;                          // Linux --virtual-wheel: wheelbri-uhid's G920 (tools/uhid)
    bool virtualWheelOutside = false;           // --virtual-wheel-outside: wheelbri.sh runs wheelbri-uhid itself
    bool hide = true;                           // --no-hide: leave the real wheel visible to games
    bool unhide = false;                        // --unhide: make the real wheel visible again and exit
    bool prepareHide = false;                   // --prepare-hide: put the hide filter on the saved rig's devices, exit
    bool app = false;                           // --app: the window with Start / Stop (always in wheelbri.exe)
    bool noVirtualDevice = false;                 // --no-virtual-wheel: the app without its virtual wheel
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
        if (!wheelbri::hideids::mayHide(device.id)) {
            WE_LOG_WARN(log(), "hide: '{}' ({}) is in the rig but is never hidden (the virtual wheel, or no id)",
                        device.name, wheelbri::hideids::deviceIdText(device.id));
            continue;
        }
        targets.push_back({device.id, device.name.empty() ? wheelbri::hideids::deviceIdText(device.id) : device.name});
    }
    return targets;
}

// With --virtual-wheel (Start in the app): the virtual wheel needs wheelbri's own driver. When Windows does not have it
// (a fresh PC, after clean-pc, or the driver was uninstalled), Start installs it now with driver\install-driver.ps1,
// from the package built in this source tree or installed with the program (driver/x64/Release); the service runs as
// Administrator for --virtual-wheel anyway. When that is not possible, VirtualWheelDevice::create says what is missing.
void ensureWheelDriver(wheelbri::service::PrivilegedSteps& steps) {
    std::string message;
    (void)steps.installWheelDriver([] { return gStop.load(); }, message);
}

using privileged::HideSetup;

// With --virtual-wheel (Start in the app): wheelbri's hide filter on every connected device of the rig, put there now
// if it is not yet, so Start alone leaves games only the G920. Which devices: the rig's ids, nothing else
// (install-hide.ps1 -Devices with the HID instance ids of their game controllers, as DirectInput lists them). It
// installs from the package built in this source tree or installed with the program (driver/hide/x64/Release); the
// service runs as Administrator for --virtual-wheel anyway. Done before the wheel is opened: a device takes the filter
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
    bool backIn = true;           // every one without the filter has our driver: a Stop took the filter out
    for (const HideFilter::Check& c : HideFilter::check(targets)) {
        for (const std::string& id : c.interfaces) all.push_back(id);
        if (c.state == HideFilter::State::NotInstalled) {
            for (const std::string& id : c.interfaces) {
                backIn = backIn && std::find(c.detached.begin(), c.detached.end(), id) != c.detached.end();
            }
            // The exact interfaces DirectInput lists for it (the script skips those that have the filter already).
            install.insert(install.end(), c.interfaces.begin(), c.interfaces.end());
        } else if (c.state == HideFilter::State::NeedsRestart) {
            waitingNames += (waitingNames.empty() ? "" : ", ") + c.target.name;
        }
    }
    if (all.empty()) return HideSetup::NothingToDo;
    if (install.empty() && waitingNames.empty()) return HideSetup::Running;   // every connected device runs it already
    if (say) {
        say(install.empty() ? std::string("Windows restarts the wheel")
            : backIn        ? std::string("restarting the wheel with the hide filter")
                            : std::string(wheelbri::service::kStepFirstHide));
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

// The rig's game-controller interfaces that are connected (`listedOnly`: only those with the filter in their stack).
std::vector<std::string> hideInterfaces(const std::vector<wheelbri::service::HideFilter::Target>& targets,
                                        const bool listedOnly) {
    using wheelbri::service::HideFilter;
    std::vector<std::string> interfaces;
    for (const HideFilter::Check& c : HideFilter::check(targets, /*quiet=*/true)) {
        if (listedOnly && c.state != HideFilter::State::Ready && c.state != HideFilter::State::NeedsRestart) continue;
        for (const std::string& id : c.interfaces) {
            const bool out = std::find(c.detached.begin(), c.detached.end(), id) != c.detached.end();
            if (!listedOnly || !out) interfaces.push_back(id);
        }
    }
    return interfaces;
}

// Mode Shim, at Start: the hide filter out of the rig's stack when it is still in (a session that crashed, a WheelBri
// from before Stop took it out), before the wheel is opened (its restart would be refused). Only then is the
// Administrator part needed; without it the filter stays (logged).
void takeLeftoverHideFilterOff(const std::vector<wheelbri::service::HideFilter::Target>& targets,
                               wheelbri::service::UiHost* const host) {
    if (targets.empty()) return;
    const std::vector<std::string> interfaces = hideInterfaces(targets, /*listedOnly=*/true);
    if (interfaces.empty()) return;
    WE_LOG_INFO(log(), "hide: the hide filter is still in the rig's stack: taking it off (Mode Shim needs none)");
    wheelbri::service::PrivilegedSteps::Problem problem{};
    std::string message;
    const auto steps = wheelbri::service::PrivilegedSteps::open(problem, message);
    if (steps == nullptr) {
        WE_LOG_WARN(log(), "hide: the hide filter stays in the rig's stack (it costs CPU): {}", message);
        return;
    }
    if (host != nullptr) host->step("taking the hide filter off the wheel");
    if (steps->takeHideFilterOff(interfaces, [] { return gStop.load(); }, message) == HideSetup::Waiting) {
        WE_LOG_WARN(log(), "hide: {}", message);
    }
    if (host != nullptr) host->step({});
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
        "                        [--stop-file FILE] [--virtual-wheel [--no-hide]] [--virtual-wheel-outside]\n"
        "                        [--unhide] [--prepare-hide]\n"
        "                        [--app [--no-virtual-wheel]] [--telemetry-tab] [--ffb-tab]\n"
        "  --config FILE  INI configuration (see config/g920.ini)\n"
        "  --list         list joysticks and exit\n"
        "  --print        show live raw and mapped values\n"
        "  --ui           open a window showing how every axis and button maps to the emulated wheel\n"
        "  --ffb-test     play a short, gentle force feedback sequence on the wheel and exit\n"
        "  --stop-file FILE  stop cleanly when FILE appears (it is deleted then; also at start)\n"
        "  --virtual-wheel while running, a virtual wheel for every game (the config's target_profile: G25,\n"
        "                 G27, G920 or G923) through wheelbri's own driver\n"
        "                 (driver\\install-driver.ps1 once; Administrator; not with the shim);\n"
        "                 the real wheel is hidden from games meanwhile by wheelbri's own filter\n"
        "                 (driver\\hide\\install-hide.ps1 once), unless --no-hide.\n"
        "                 Linux: starts wheelbri-uhid (install-udev-rules.sh once); the game's launch\n"
        "                 options hide the rig (scripts/linux/wheelbri.sh)\n"
        "  --virtual-wheel-outside\n"
        "                 another program runs the virtual wheel (wheelbri.sh starts wheelbri-uhid itself): the\n"
        "                 pedals go out as for --virtual-wheel (game profiles applied), nothing started or hidden\n"
        "  --unhide       after a crash: make the real wheel visible to games again and exit\n"
        "  --prepare-hide put the hide filter on the saved rig's devices and exit (the installer; Administrator)\n"
        "  --app          a window with Start / Stop instead of running at once (what wheelbri.exe\n"
        "                 does); on Windows it starts with --virtual-wheel unless --no-virtual-wheel\n"
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
        } else if (arg == "--virtual-wheel" || arg == "--virtual-g920") {   // --virtual-g920: its old name
#ifdef _WIN32
            options.virtualDevice = true;
#else
            options.uhid = true;   // Linux: wheelbri-uhid, started and stopped with the session
#endif
        } else if (arg == "--app") {
            options.app = true;
        } else if (arg == "--virtual-wheel-outside") {
            options.virtualWheelOutside = true;
        } else if (arg == "--no-virtual-wheel" || arg == "--no-virtual-g920") {
            options.noVirtualDevice = true;
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
    // The games the user added may have [game_tuning] lines too.
    const std::vector<wheelbri::GameProfile> userGames = wheelbri::service::loadUserGames();
    auto loaded = wheelbri::loadEmulatorConfig(parsed.document, userGames);
    result.config = std::move(loaded.config);
    result.problems = parsed.errors;
    result.problems.insert(result.problems.end(), loaded.errors.begin(), loaded.errors.end());
    result.warnings = parsed.warnings;
    return result;
}

void logEffectiveConfig(const wheelbri::EmulatorConfig& config) {
    WE_LOG_INFO(log(), "source_device='{}' target_profile='{}' state_port={} ffb_port={} state_rate_hz={} "
                       "game_effects={} ffb_invert={}",
                config.sourceDeviceMatch, config.targetProfile, config.statePort, config.ffbPort, config.stateRateHz,
                config.gameFfb, config.ffbInvert);
    WE_LOG_DEBUG(log(), "ffb gain={:.2f} max_magnitude={:.2f} max_slew={:.1f}/s frozen_force_s={:.1f}",
                 config.ffb.masterGain, config.ffb.maxMagnitude, config.ffb.maxSlewPerSecond,
                 config.ffb.frozenForceSeconds);
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

wheelbri::ShimSources shimDllPaths();

#if defined(_WIN32)
// Windows: who may send game force feedback (S7; SenderGate.hpp). Any program of the user can send to 127.0.0.1:41330
// and the forces reach a direct-drive wheel, so only the virtual wheel's driver host (WUDFHost.exe in System32) and a
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
        WE_LOG_WARN(log(), "ffb: ignoring force feedback from {} (pid {}, port {}): only the virtual wheel's driver "
                           "and games with WheelBri's shim may send it ({} similar suppressed)",
                    image.empty() ? std::string("an unknown program") : wheelbri::pathToUtf8(image), *pid, port,
                    suppressed);
    }
    return wheelbri::SenderVerdict::Refuse;
}
#elif defined(__linux__)
// Linux: who may send game force feedback (SenderGate.hpp). 127.0.0.1:41330 is open to every user of the machine and
// the forces reach a direct-drive wheel, so only a socket of the service's own user (a game's shim under Proton,
// wheelbri-uhid) is let through. The system cannot say: let through (the limiter still caps every force), with a
// warning, as on Windows.
wheelbri::SenderVerdict lookupFfbSender(const std::uint16_t port) {
    static wheelbri::logging::RateLimiter limiter(std::chrono::seconds(30));
    const std::optional<bool> ours = platform::udpPortIsOurUser(port);
    std::uint64_t suppressed = 0;
    if (!ours) {
        if (limiter.allow(suppressed)) {
            WE_LOG_WARN(log(), "ffb: cannot tell whose program sends from port {}; letting it through", port);
        }
        return wheelbri::SenderVerdict::Unknown;
    }
    if (*ours) return wheelbri::SenderVerdict::Accept;
    if (limiter.allow(suppressed)) {
        WE_LOG_WARN(log(), "ffb: ignoring force feedback from port {}: another user's program (or none any more); only "
                           "this user's games and wheelbri-uhid may send it ({} similar suppressed)",
                    port, suppressed);
    }
    return wheelbri::SenderVerdict::Refuse;
}
#endif

// The game's force feedback, sent by the shim or the virtual wheel, limited and played on the physical wheel.
class GameFfb final : public wheelbri::service::LoopFfb {
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
        // The target's centring spring, if its own software had one (the G25's and G27's: CentringSpring.hpp).
        if (const auto target = wheelbri::makeProfile(config.targetProfile);
            target != nullptr && target->identity().centringSpring) {
            ffb->controller_->setCentringSpring(config.centringSpringPercent);
        }
#if defined(_WIN32) || defined(__linux__)
        wheelbri::SenderGate::Settings gate;
        gate.acceptUnknown = true;   // the lookup failing must not take a game's force feedback away
        ffb->gate_ = std::make_unique<wheelbri::SenderGate>(lookupFfbSender, gate);
#endif
        WE_LOG_INFO(log(), "game force feedback on: all the game asks, through the safety limits (the wheel's own app "
                           "sets how strong)");
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

    void pump(const std::uint64_t nowNs) override {
        std::array<std::uint8_t, 256> buffer{};
        // At most kMaxPerTick a tick: a flood must not keep the loop (and the wheel's input) here; the rest waits.
        for (std::size_t n = 0; n < kMaxPerTick; ++n) {
            std::uint16_t sender = 0;
            const auto size = listener_->receive(buffer, &sender);
            if (!size) break;
            if (gate_ != nullptr && !gate_->allow(sender, nowNs)) continue;
            if (const auto packet = wheelbri::decodeFfbPacket(std::span<const std::uint8_t>(buffer.data(), *size))) {
                controller_->onPacket(*packet, nowNs, sender);
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

    [[nodiscard]] const wheelbri::FfbController& controller() const override { return *controller_; }
    // The socket the game's packets arrive on (the loop waits for it).
    [[nodiscard]] std::intptr_t socket() const { return listener_->nativeSocket(); }

    // Called every 10 s; logs only when something changed: effects playing, failures, watchdog stops,
    // or the game starting / stopping to send.
    void logStatsIfChanged() override {
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
    void logUsage() override {
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
    std::unique_ptr<wheelbri::SenderGate> gate_;   // Windows and Linux: who may send (lookupFfbSender)
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

// What the window says about the virtual devices (--virtual-wheel), see SessionInfo::devices.
struct DeviceSummary {
    std::string text;
    bool warning = false;
};

// The state to its reader and, with [emulator] trueforce on, to the virtual G923's TrueForce interface too
// (TrueForce.hpp), with the wheel's rotation: the interface reports the wheel's angle over it, and the driver's HID++
// reports it as the wheel's rotation (ACC divides its steering by that).
// The rotation follows the session's (setRotation): the mapping's, or a running game's own, changed live too.
class TeePublisher final : public wheelbri::IStatePublisher {
public:
    TeePublisher(wheelbri::IStatePublisher& first, wheelbri::IStatePublisher* second, const float rotationDeg)
        : first_(first), second_(second), rotationDeg_(toWire(rotationDeg)) {}
    bool publish(const wheelbri::StatePacket& packet) override {
        wheelbri::StatePacket withRotation = packet;
        withRotation.rotationDeg = rotationDeg_.load(std::memory_order_relaxed);
        if (second_ != nullptr) (void)second_->publish(withRotation);
        return first_.publish(withRotation);
    }
    // From the window's thread; the next packet carries it. Whether it changed.
    bool setRotation(const float rotationDeg) {
        return rotationDeg_.exchange(toWire(rotationDeg), std::memory_order_relaxed) != toWire(rotationDeg);
    }

private:
    static std::uint16_t toWire(const float rotationDeg) {
        return static_cast<std::uint16_t>(std::clamp(std::lround(rotationDeg), 0L, 65535L));
    }

    wheelbri::IStatePublisher& first_;
    wheelbri::IStatePublisher* second_;
    std::atomic<std::uint16_t> rotationDeg_;
};

// The config file read again while the session runs, prepared as Start prepares it (loadForRun, the wheel's labels):
// nullopt when it cannot be used (the problems are in `problems`).
using ReloadConfig = std::function<std::optional<wheelbri::EmulatorConfig>(std::vector<std::string>& problems)>;

using wheelbri::service::LoopShared;

// What the loop asks of the system, the service's own: the monotonic clock, platform::WaitSet over the devices and
// the force feedback socket, Ctrl+C / the stop file (gStop), and the state socket for the log level.
class SystemLoop final : public wheelbri::service::LoopSystem {
public:
    explicit SystemLoop(wheelbri::UdpLoopbackPublisher& publisher) : publisher_(publisher) {}
    [[nodiscard]] platform::WaitSet& waitSet() { return waitSet_; }

    std::uint64_t nowNs() override { return platform::nowNs(); }
    int waitUntil(const std::uint64_t deadlineNs) override { return waitSet_.waitUntil(deadlineNs); }
    [[nodiscard]] std::string waitFailureText() const override { return waitSet_.failureText(); }
    void sleepNs(const std::uint64_t ns) override { platform::sleepNs(ns); }
    [[nodiscard]] bool stopRequested() const override { return gStop; }
    void publishLogLevel(const wheelbri::LogLevel level) override {
        static_cast<void>(publisher_.publishLogLevel(level));
    }
    void threadStarted() override {
        // Above normal, so a game that is loading (all cores busy) does not starve the input loop.
        if (std::string error; !platform::raiseThreadPriority(error)) {
            WE_LOG_WARN(log(), "cannot raise the loop's thread priority: {}", error);
        }
    }

private:
    static_assert(platform::WaitSet::kFailed == wheelbri::kWaitFailed &&
                  platform::WaitSet::kTimedOut == wheelbri::kWaitTimedOut);
    wheelbri::UdpLoopbackPublisher& publisher_;
    platform::WaitSet waitSet_;
};

// runLoop's other thread: the window (kWindowFramesPerSecond, 60 fps), old files (every 250 ms, and again after every
// midnight), the config read again when it changes, and the game that runs (every 2 s). With or without a window, so
// headless sessions follow the config and the game too. It keeps the mappers it makes for the whole session (the
// loop may still hold one), so it must outlive the loop's thread.
class WindowSide {
public:
    WindowSide(wheelbri::EmulatorConfig& config, const ReloadConfig& reload, const std::string& configPath,
               wheelbri::Pipeline& pipeline, TeePublisher& statePublisher,
               wheelbri::service::Housekeeping& housekeeping, wheelbri::service::UiHost* const host,
               const bool routePedals, LoopShared& shared)
        : config_(config),
          reload_(reload),
          configPath_(configPath),
          pipeline_(pipeline),
          statePublisher_(statePublisher),
          housekeeping_(housekeeping),
          host_(host),
          shared_(shared),
          gameWatch_(gameSession(config, routePedals), config.mapper.steering.targetRotationDeg,
                     config.gameRotations, wheelbri::service::loadUserGames()),
          shownMapping_(std::make_shared<const wheelbri::MapperConfig>(config.mapper)) {
        if (!configPath_.empty()) {
            std::error_code ec;
            configWritten_ = std::filesystem::last_write_time(wheelbri::pathFromUtf8(configPath_), ec);
        }
        nextMidnight_ =
            wheelbri::startOfDay(wheelbri::addDays(wheelbri::localDate(wheelbri::WallClock::now()), 1).value_or(""));
    }

    // Until the loop is done (shared.done) or the window is closed.
    void run();

private:
    static wheelbri::GameSessionConfig gameSession(const wheelbri::EmulatorConfig& config, const bool routePedals) {
        // Game profiles: applied here for the virtual wheel / wheelbri-uhid, by the shim itself for the in-game shim.
        wheelbri::GameSessionConfig session;
        session.target = config.targetProfile;
        session.routePedals = routePedals;
        session.tunings = config.gameTunings;
        return session;
    }
    void everyQuarterSecond(std::uint64_t now);
    void followConfig();
    void checkGame();
    void sayWaiting(std::vector<std::string> lines, bool problems);
    void followRotation();
    [[nodiscard]] UiFrame frame() const;

    wheelbri::EmulatorConfig& config_;
    const ReloadConfig& reload_;
    const std::string& configPath_;
    wheelbri::Pipeline& pipeline_;
    TeePublisher& statePublisher_;
    wheelbri::service::Housekeeping& housekeeping_;
    wheelbri::service::UiHost* host_;
    LoopShared& shared_;

    wheelbri::service::GameWatch gameWatch_;
    std::optional<wheelbri::WallClock::time_point> nextMidnight_;   // old files checked again then
    std::uint64_t lastHousekeepingNs_ = 0;
    // The game that runs, looked for every 2 s (a few milliseconds; never on the loop): its pedal profile.
    std::uint64_t lastGameCheckNs_ = 0;
    bool gameChecked_ = false;
    // When the config file was written as last read (read again when it changes): `config_` is what that write
    // said, Start's config with the changes taken while running (also across a wheel that came back).
    std::filesystem::file_time_type configWritten_{};
    // What the log said waits for the next Start (or is wrong in the file): said again only when it changes.
    std::vector<std::string> saidWaiting_;
    // The mappings the Mapping tab changed to while the session runs (LiveMapping.hpp), kept for the session (the
    // loop may still hold the one before). `mappingVersion_` counts them; the game mappers are made over the one in
    // use (their key has the version).
    std::vector<std::unique_ptr<wheelbri::Mapper>> liveMappers_;
    std::uint64_t mappingVersion_ = 0;
    std::shared_ptr<const wheelbri::MapperConfig> shownMapping_;
    // The mapping while a game with its own values runs (pedal invert / curve, buttons): one Mapper per game and set of
    // values, kept for the whole session (the loop may still hold the last one).
    std::map<std::string, std::unique_ptr<wheelbri::Mapper>> gameMappers_;
};

void WindowSide::sayWaiting(std::vector<std::string> lines, const bool problems) {
    if (lines == saidWaiting_) return;
    saidWaiting_ = lines;
    for (const std::string& line : lines) {
        if (problems) {
            WE_LOG_WARN(log(), "config changed, not used while running until it is fixed: {}", line);
        } else {
            WE_LOG_INFO(log(), "config changed: {} from the next Start", line);
        }
    }
}

// The rotation the virtual wheel reports: the mapping's or the running game's own (GameWatch::rotationNow).
void WindowSide::followRotation() {
    if (statePublisher_.setRotation(gameWatch_.rotationNow())) {
        WE_LOG_INFO(log(), "steering rotation: the virtual wheel reports {} deg now", gameWatch_.rotationNow());
    }
}

// The config changed (the Games tab and, while running, the Mapping tab save as they change): read it again and use
// at once what a running session can (the per-game values, GameWatch::setValues; the mapping, LiveMapping.hpp).
// Everything else waits for the next Start, and the log says so.
void WindowSide::followConfig() {
    if (configPath_.empty()) return;
    std::error_code ec;
    const auto written = std::filesystem::last_write_time(wheelbri::pathFromUtf8(configPath_), ec);
    if (ec || written == configWritten_) return;
    configWritten_ = written;
    std::vector<std::string> problems;
    const auto next = reload_(problems);
    if (!next) {
        sayWaiting(std::move(problems), true);
        return;
    }
    if (next->gameTunings != config_.gameTunings || next->gameRotations != config_.gameRotations) {
        config_.gameTunings = next->gameTunings;
        config_.gameRotations = next->gameRotations;
        gameWatch_.setValues(config_.gameTunings, config_.gameRotations);
        WE_LOG_INFO(log(), "game: your per-game values changed in {}: in use now", configPath_);
        gameChecked_ = false;   // look now, not in up to 2 s
    }
    const wheelbri::LiveMappingChange change = wheelbri::liveMappingChange(config_, *next);
    if (change.mapper) {
        liveMappers_.push_back(std::make_unique<wheelbri::Mapper>(*change.mapper));
        pipeline_.setMapper(*liveMappers_.back());
        ++mappingVersion_;
        shownMapping_ = std::make_shared<const wheelbri::MapperConfig>(*change.mapper);
        WE_LOG_INFO(log(), "mapping: changed in {}: in use now", configPath_);
        gameChecked_ = false;   // a game's own mapping is made again over this one
    }
    if (change.buttonOrder) {
        pipeline_.setButtonOrder(*change.buttonOrder);
        WE_LOG_INFO(log(), "buttons: the {} order now", wheelbri::toString(*change.buttonOrder));
    }
    wheelbri::applyLiveMappingChange(config_, change);
    if (change.rotationChanged) {
        gameWatch_.setMapping(config_.mapper.steering.targetRotationDeg);
        followRotation();
    }
    sayWaiting(change.waiting, false);
}

void WindowSide::checkGame() {
    try {
        const auto decision = gameWatch_.check();
        pipeline_.setPedalRoute(decision.route);
        // The game's own mapping (made once per game, kept for the session), else the mapping's.
        const wheelbri::Mapper* gameMapper = nullptr;
        if (!decision.mapperGame.empty() && decision.tuning != nullptr) {
            // One per game and set of values: a change made while it runs gets a new one (the loop may still use
            // the old one, so none is ever thrown away during the session).
            std::string key = std::to_string(mappingVersion_) + "|" + decision.mapperGame;
            for (const auto& [name, value] : wheelbri::tuningLines(*decision.tuning)) {
                key += "|" + name + "=" + value;
            }
            auto& made = gameMappers_[key];
            if (made == nullptr) {
                std::vector<std::string> problems;
                made = std::make_unique<wheelbri::Mapper>(
                    wheelbri::tunedMapper(config_.mapper, *decision.tuning, &problems));
                for (const std::string& problem : problems) {
                    WE_LOG_WARN(log(), "game {}: {}", decision.mapperGame, problem);
                }
                WE_LOG_INFO(log(), "game {}: its own pedal and button values are in use", decision.mapperGame);
            }
            gameMapper = made.get();
        }
        if (pipeline_.gameMapper() != gameMapper) pipeline_.setGameMapper(gameMapper);
        followRotation();   // a game with its own rotation started or ended
    } catch (const std::exception& e) {   // never ends the session; the pedals stay as they are
        WE_LOG_WARN(log(), "game: looking for known games failed: {}", e.what());
    }
}

void WindowSide::everyQuarterSecond(const std::uint64_t now) {
    if (stopRequested()) {
        WE_LOG_INFO(log(), "stop requested ({} appeared)", gStopFile.string());
        gStop = true;
    }
    housekeeping_.pump();
    followConfig();
    constexpr std::uint64_t kGameCheckNs = 2 * platform::kNsPerSecond;
    if (!gameChecked_ || now - lastGameCheckNs_ >= kGameCheckNs) {
        lastGameCheckNs_ = now;
        gameChecked_ = true;
        checkGame();
    }
    if (nextMidnight_ && wheelbri::WallClock::now() >= *nextMidnight_) {
        const auto wall = wheelbri::WallClock::now();
        WE_LOG_INFO(log(), "new day {}: new log and telemetry files, checking old ones", wheelbri::localDate(wall));
        housekeeping_.run(wall);
        nextMidnight_ = wheelbri::startOfDay(wheelbri::addDays(wheelbri::localDate(wall), 1).value_or(""));
    }
}

// The loop's latest copy with what this side knows added.
UiFrame WindowSide::frame() const {
    std::optional<UiFrame> frame;   // copy-constructed: its force feedback copy cannot be assigned
    {
        const std::lock_guard lock(shared_.frameMutex);
        if (shared_.frame.ready) {
            frame.emplace(shared_.frame);
        }
    }
    if (!frame) {
        frame.emplace();
    }
    frame->oldFiles = housekeeping_.status();
    frame->game = gameWatch_.status();
    frame->gameWarning = gameWatch_.warning();
    frame->gameRotation = gameWatch_.rotationStatus();
    frame->mapping = shownMapping_;
    frame->mappingVersion = mappingVersion_;
    frame->buttonOrder = config_.buttonOrder;
    if (const auto& advice = gameWatch_.wayAdvice()) {
        frame->gameWayTitle = advice->title;
        frame->gameWayText = advice->text;
    }
    frame->gameWayNotices = gameWatch_.wayNotices();
    const wheelbri::service::GameWatch::Live live = gameWatch_.live();
    frame->gameId = live.id;
    frame->gameProfileApplies = live.profileApplies;
    frame->gamePid = live.pid;
    const std::uint64_t nowMs = platform::nowMs();
    frame->gameForMs = live.id.empty() || nowMs < live.sinceMs ? 0 : nowMs - live.sinceMs;
    frame->gameStarts = live.starts;
    frame->oldFilesFailed = housekeeping_.failed();
    return std::move(*frame);
}

void WindowSide::run() {
    constexpr std::uint64_t kFrameNs = platform::kNsPerSecond / wheelbri::kWindowFramesPerSecond;
    std::uint64_t nextFrame = platform::nowNs();
    while (!shared_.done) {
        const std::uint64_t now = platform::nowNs();
        if (now - lastHousekeepingNs_ >= 250 * platform::kNsPerMs) {
            lastHousekeepingNs_ = now;
            everyQuarterSecond(now);
        }
        if (host_ == nullptr) {
            platform::sleepMs(50);
            continue;
        }
        if (!host_->frame(frame())) {
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
}

// `config` is the session's: changes to the mapping made while it runs are taken into it (LiveMapping.hpp), so a run
// after the wheel came back starts with them.
int runLoop(wheelbri::EmulatorConfig& config, const ReloadConfig& reload, const std::string& configPath,
            const std::string& configText,
            const std::filesystem::path& logFolder, const bool logToFile, wheelbri::service::Wheel& source,
            const Accessories& accessories, const bool print, wheelbri::service::UiHost* const host,
            const DeviceSummary& devices, std::optional<UiFrame>& lastFrame, const bool routePedals) {
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
    std::unique_ptr<wheelbri::UdpLoopbackPublisher> trueForceState;
    if (const wheelbri::VirtualWheel* wheel = wheelbri::virtualWheel(config.targetProfile);
        config.trueForce && wheel != nullptr && wheel->trueForce) {
        trueForceState = wheelbri::UdpLoopbackPublisher::create(wheelbri::kTrueForceStatePort);
        if (trueForceState) {
            WE_LOG_INFO(log(), "trueforce: the state goes to the TrueForce interface too (127.0.0.1:{})",
                        wheelbri::kTrueForceStatePort);
        }
    }
    TeePublisher statePublisher(*publisher, trueForceState.get(), config.mapper.steering.targetRotationDeg);

    const wheelbri::Mapper mapper(config.mapper);
    wheelbri::Pipeline pipeline(source, mapper, *profile, statePublisher);
    pipeline.setAccessories(accessories.sources());
    // The G25's and G27's buttons in their native order if the mapping says so ([buttons] order).
    pipeline.setButtonOrder(config.buttonOrder);
    if (pipeline.nativeButtons()) {
        WE_LOG_INFO(log(), "buttons: the {}'s native order ([buttons] order = native)", profile->identity().shortName);
    }
    pipeline.limitSending(platform::kNsPerSecond / static_cast<std::uint64_t>(config.stateRateHz));

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

    if (host != nullptr) {
        wheelbri::service::SessionInfo info;
        info.sourceName = source.name();
        info.vendorId = source.info().vendorId;
        info.productId = source.info().productId;
        info.axes = source.axes();
        info.buttons = source.buttons();
        info.targetProfile = config.targetProfile;
        info.mapper = config.mapper;
        info.buttonOrder = config.buttonOrder;
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

    // The loop (wheel input, state to the game, force feedback, telemetry) runs on its own thread (ServiceLoop); the
    // window, the config, the game and old files stay on this one (WindowSide). A window that Windows holds up (a game
    // going full screen, dragging it) then only freezes the window, never the wheel or its force feedback. They share
    // only LoopShared (and the pipeline's and TeePublisher's thread-safe setters).
    LoopShared shared;
    WindowSide windowSide(config, reload, configPath, pipeline, statePublisher, housekeeping, host, routePedals,
                          shared);
    // The loop sleeps until the wheel, an accessory or the game's force feedback has something new
    // (platform::WaitSet), and wakes by itself only for what must run on time (core EventLoopControl, WakePlanner).
    // Whatever cannot notify it (a device DirectInput only polls, a socket the wait cannot take) still gets read at
    // every pass, and the loop then also polls every millisecond; everything else still wakes it at once.
    SystemLoop system(*publisher);
    std::vector<std::string> accessoryNames;
    for (const auto& label : accessories.labels) accessoryNames.push_back(label.name);
    wheelbri::service::ServiceLoop loop({.pipeline = pipeline,
                                         .system = system,
                                         .shared = shared,
                                         .targetName = profile->identity().shortName,
                                         .accessoryNames = std::move(accessoryNames),
                                         .ffb = gameFfb.get(),
                                         .telemetry = telemetry.get(),
                                         .print = print,
                                         .window = host != nullptr,
                                         .settings = {}});
    loop.addDevice(system.waitSet().addEvent(source.waitSource()) >= 0, source.info().name);
    for (const auto& accessory : accessories.devices) {
        if (accessory != nullptr) {
            loop.addDevice(system.waitSet().addEvent(accessory->waitSource()) >= 0, accessory->info().name);
        }
    }
    if (gameFfb != nullptr) loop.addGameFfbSocket(system.waitSet().addSocket(gameFfb->socket()) >= 0);
    int loopResult = 0;
    std::thread loopThread([&] {
        platform::nameThisThread("wheelbri loop");
        // An exception must not escape this thread (std::terminate: no clean-up, the wheel left hidden and its
        // forces playing). It ends the session instead; runSession then undoes everything.
        try {
            loopResult = loop.run();
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
        shared.done = true;
    });

    try {
        windowSide.run();
    } catch (...) {
        shared.stop = true;   // never leave the loop thread running behind an error here
        loopThread.join();
        throw;
    }
    shared.stop = true;
    loopThread.join();
    if (shared.frame.ready) {
        lastFrame.reset();
        lastFrame.emplace(shared.frame);   // what the window keeps showing if the wheel was lost
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
    // [emulator] from_rig: the steering, pedals, D-pad and core buttons the mapping leaves out come from what the rig's
    // controls are (AutoMap.hpp); what the rig cannot give is said, and left unmapped.
    if (rig) {
        for (const std::string& notice : wheelbri::applyRig(loaded.config, *rig)) {
            loaded.warnings.push_back("from the rig: " + notice);
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

// --prepare-hide: the installer, after a reinstall that kept the user's settings, puts the filter on the saved rig's
// devices now, so the first Start does not have to. Prints one line (the installer shows it); the exit code is
// HideSetup's.
int prepareHide() {
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
    if (result == HideSetup::Running) {
        // Set up for the first Start; out of the stack until then (it costs CPU while it is in), so that Start
        // only puts it back in.
        std::string message;
        (void)steps->takeHideFilterOff(hideInterfaces(targets, /*listedOnly=*/false), {}, message);
    }
    std::puts(result == HideSetup::Running   ? "the rig's devices are hidden from games whenever WheelBri runs"
              : result == HideSetup::Waiting ? "the filter is on; it starts after the restart of Windows"
              : result == HideSetup::NothingToDo
                  ? "none of the rig's devices is plugged in: the first Start sets up the hiding"
                  : "could not set up the hiding (see the hide installer's log): the first Start tries again");
    return static_cast<int>(result);
}

// Waits (up to WaitSettings' timeout) for the wheel `match` names when it is not connected: false when stopped
// meanwhile (`cancelled`). Whether it came or not, the caller then opens it (and says what is connected if it cannot).
bool waitForWheel(wheelbri::service::WheelBackend& backend, const std::string& match,
                  wheelbri::service::UiHost* const host, const std::function<bool()>& cancelled) {
    if (wheelbri::service::wheelConnected(backend, match)) return true;
    const wheelbri::WaitSettings wait;
    WE_LOG_INFO(log(), "the wheel ('{}') is not connected yet: waiting up to {} s for it (Windows may be restarting "
                       "it); Stop gives up",
                match, wait.timeoutMs / 1000);
    std::uint64_t waitedMs = 0;
    if (host != nullptr) host->step("waiting for the wheel (up to " + std::to_string(wait.timeoutMs / 1000) + " s)");
    // Each check of the wheel at once when Windows says a device came (else every 250 ms, for Stop as well).
    wheelbri::ChangeCounter& changes = platform::DeviceChanges::shared().counter();
    const wheelbri::WaitOutcome outcome = wheelbri::waitFor(
        wait, [&] { return wheelbri::service::wheelConnected(backend, match); }, cancelled,
        [&changes](const std::uint64_t ms) {
            const std::uint64_t start = platform::nowMs();
            (void)changes.waitForChange(changes.count(), ms);
            return platform::nowMs() - start;
        },
        &waitedMs);
    if (host != nullptr) host->step({});
    if (outcome == wheelbri::WaitOutcome::Cancelled) {
        WE_LOG_INFO(log(), "stopped while waiting for the wheel");
        return false;
    }
    if (outcome == wheelbri::WaitOutcome::Ready) {
        WE_LOG_INFO(log(), "the wheel is there after {:.1f} s", static_cast<double>(waitedMs) / 1000.0);
    } else {
        WE_LOG_WARN(log(), "the wheel did not appear within {} s", waitedMs / 1000);
    }
    return true;
}

// A mapped axis the device does not have reads centred or released for ever: say so (Start still goes on).
void warnMissingAxes(const wheelbri::MapperConfig& mapper, const wheelbri::service::Wheel& wheel,
                     const Accessories& accessories) {
    std::array<int, wheelbri::kMaxInputDevices> axisCounts{};
    axisCounts.fill(-1);
    axisCounts[0] = wheel.axes();
    for (std::size_t i = 0; i < accessories.devices.size(); ++i) {
        if (accessories.devices[i] != nullptr) axisCounts[i + 1] = accessories.devices[i]->axes();
    }
    for (const std::string& problem : wheelbri::missingMappedAxes(mapper, axisCounts)) {
        WE_LOG_ERROR(log(), "mapping: {}; it will not move (map another axis in the Mapping tab)", problem);
    }
}

// After the wheel was lost: waits until it and its accessories are all back and open (`source`, `accessories`),
// trying every `retryEveryMs` (the first try at once unless `lostQuickly`), keeping the window answering with the
// last picture of the wheel marked lost. False when stopped meanwhile (Stop, the stop file, the window closed).
bool waitForWheelBack(wheelbri::service::WheelBackend& backend, const wheelbri::EmulatorConfig& config,
                      wheelbri::service::UiHost* const host, std::optional<UiFrame>& lastFrame,
                      const std::uint64_t retryEveryMs, const bool lostQuickly,
                      std::unique_ptr<wheelbri::service::Wheel>& source, Accessories& accessories) {
    // By the clock (a frame of the window can take a while), never by counting sleeps.
    const std::uint64_t startMs = platform::nowMs();
    std::uint64_t nextTryMs = lostQuickly ? retryEveryMs : 0;
    std::uint64_t nextNoteMs = 60'000;
    std::uint64_t waitedMs = 0;
    while (!gStop && source == nullptr) {
        if (stopRequested()) {
            WE_LOG_INFO(log(), "stop requested ({} appeared)", gStopFile.string());
            gStop = true;
            break;
        }
        waitedMs = platform::nowMs() - startMs;
        // Keeps the window answering (the last picture of the wheel stays, marked lost); false: it was closed.
        if (lastFrame) lastFrame->wheelLostMs = std::max<std::uint64_t>(waitedMs, 1);
        if (host != nullptr && !host->frame(lastFrame ? *lastFrame : UiFrame{})) {
            WE_LOG_INFO(log(), "window closed: stopping");
            gStop = true;
            break;
        }
        if (waitedMs >= nextTryMs) {
            nextTryMs = waitedMs + retryEveryMs;
            if (allConnected(backend, config)) {
                source = wheelbri::service::openWheel(backend, config.sourceDeviceMatch, /*quiet=*/true);
                if (source != nullptr && !openAccessories(backend, config.mapper, source->info(), accessories, true)) {
                    source.reset();   // an accessory is not ready yet: all again next time
                    accessories.close();
                }
                if (source == nullptr) {
                    static wheelbri::logging::RateLimiter limiter(std::chrono::seconds(30));
                    WE_LOG_WARN_LIMITED(log(), limiter, "the wheel is back but cannot be opened yet; trying again");
                }
            }
        }
        if (source == nullptr) {
            platform::sleepMs(50);
            if (waitedMs >= nextNoteMs) {
                WE_LOG_WARN(log(), "still waiting for the wheel ({} min)", nextNoteMs / 60'000);
                nextNoteMs += 60'000;
            }
        }
    }
    if (source == nullptr) return false;
    WE_LOG_INFO(log(), "the wheel is back after {:.1f} s: carrying on",
                static_cast<double>(platform::nowMs() - startMs) / 1000.0);
    return true;
}

// The window's line with the virtual wheel on: what games will see besides it (the rig hidden or not, and which of
// its devices the filter does not run on yet).
DeviceSummary virtualWheelSummary(const std::string& wheelName, const bool hide,
                                  const std::vector<wheelbri::service::HideFilter::Target>& hideTargets,
                                  const std::vector<wheelbri::service::HideFilter::Check>& hideChecks,
                                  const bool rigHidden) {
    DeviceSummary devices;
    devices.text = "virtual " + wheelName + " on";
    if (!hide) {
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
    return devices;
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
    if (options.prepareHide) return prepareHide();
    // Until the end of this function (its clean-up included), the console close handler waits.
    gSessionActive = true;
    struct SessionActive {
        ~SessionActive() { gSessionActive = false; }
    } sessionActiveGuard;
    platform::installConsoleStopHandler(requestStop, sessionActive);

    // The virtual wheel and hiding the rig need Administrator: this process's own when it is elevated, else the
    // WheelBri Helper's (a standard user).
    std::unique_ptr<wheelbri::service::PrivilegedSteps> steps;
    if (options.virtualDevice) {
        wheelbri::service::PrivilegedSteps::Problem problem{};
        std::string problemText;
        steps = wheelbri::service::PrivilegedSteps::open(problem, problemText);
        if (steps == nullptr) {
            WE_LOG_CRITICAL(log(), "{}: the virtual wheel and hiding the rig need Administrator or the WheelBri "
                                   "Helper: {}",
                            problem == wheelbri::service::PrivilegedSteps::Problem::NoHelper ? "HELPER NOT INSTALLED"
                                                                                             : "HELPER REFUSED",
                            problemText);
            return 1;
        }
        WE_LOG_INFO(log(), "the virtual wheel and hiding the rig: {}", steps->how());
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
    if (steps != nullptr) ensureWheelDriver(*steps);
    // Hidden before the filter is put on: the device restart that loads it is a moment in which any program may open
    // the wheel (a browser reacts to a game controller arriving at once), and the filter cannot take back a handle it
    // let through (2026-11-02: Firefox got in there, then held the wheel so Stop could not take the filter off).
    // While the filter is not loaded yet this setting does nothing; from its first moment in the stack it refuses
    // everyone but this process. Undone by showRig at the end, or by `steps` going (any early return).
    // Windows only: elsewhere wheelbri-uhid hides the rig its own way.
    bool rigHidden = false;
#ifdef _WIN32
    if (steps != nullptr && options.virtualDevice && options.hide && !gStop && !hideTargets.empty()) {
        std::string message;
        rigHidden = steps->hideRig(message);
        if (!rigHidden) {
            WE_LOG_CRITICAL(log(), "hide: the rig is NOT HIDDEN from games, so the session does not start: {}",
                            message);
            return 1;
        }
    }
#endif
    if (options.virtualDevice && options.hide && !gStop) {
        if (hideTargets.empty()) {
            WE_LOG_WARN(log(), "hide: no rig yet{}: nothing is hidden from games (define the devices in the Rig tab)",
                        rigName.empty() ? "" : " ('" + rigName + "' lists no device)");
        } else {
            const HideSetup hidden = ensureHideFilter(hideTargets, *steps, [host](const std::string& what) {
                if (host != nullptr) host->step(what);
            });
            // Never Running with the rig in sight of games: a game would take the real wheel next to the target.
            if ((hidden == HideSetup::Waiting || hidden == HideSetup::Failed) && !gStop) {
                WE_LOG_CRITICAL(log(), "hide: the rig is NOT HIDDEN from games, so the session does not start (the "
                                       "hide lines above say why): close what uses the wheel, or unplug it and plug "
                                       "it in again, then press Start again");
                return 1;
            }
        }
    }
    // Mode Shim needs no filter: one left in the rig's stack (a crash, an older WheelBri) costs CPU for nothing.
    if (!options.virtualDevice && !gStop) takeLeftoverHideFilterOff(hideTargets, host);
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
    if (!waitForWheel(*backend, config.sourceDeviceMatch, host, cancelled)) return 0;
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
    if (!options.ffbTest) warnMissingAxes(config.mapper, *source, accessories);

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
    // The G25 and G27 in the Logitech order (the button order of Logitech's driver) with Logitech's older-wheel force
    // feedback driver installed: their classic identity, which that driver drives (VirtualWheel::classic); the same
    // device to games either way. Otherwise Windows' HID PID driver, as in the Native order.
    if (const wheelbri::VirtualWheel* classic = wheelbri::classicVirtualWheel(config.targetProfile);
        classic != nullptr && steps != nullptr) {
        const bool logitechOrder = config.buttonOrder == wheelbri::ButtonOrder::Driver;
        const bool installed = wheelbri::service::classicFfbDriverInstalled(*classic);
        if (logitechOrder && installed) {
            virtualWheel = classic;
            WE_LOG_INFO(log(), "the virtual {}: Logitech's force feedback driver (Logitech order, G HUB's older-wheel "
                               "driver installed)", classic->instancePrefix);
        } else {
            WE_LOG_INFO(log(), "the virtual {}: Windows' force feedback driver ({})", classic->instancePrefix,
                        logitechOrder ? "Logitech order, but G HUB's older-wheel driver is not installed"
                                      : "Native order");
        }
    }
    const std::string wheelName = virtualWheel != nullptr ? std::string(virtualWheel->instancePrefix) : "G920";
    bool g920 = false;
    if (steps != nullptr) {
        if (virtualWheel == nullptr) {
            WE_LOG_CRITICAL(log(), "--virtual-wheel: no virtual device for target_profile '{}'", config.targetProfile);
            return 1;
        }
        std::string error;
        std::string wheelId;
        g920 = steps->createVirtualWheel(*virtualWheel, config.trueForce, cancelled, wheelId, error);
        if (!g920) {
            if (cancelled()) {
                WE_LOG_INFO(log(), "stopped while the virtual {} was being created", wheelName);
                return 0;
            }
            WE_LOG_CRITICAL(log(), "--virtual-wheel: {}", error);
            return 1;
        }
        // TrueForce needs Logitech's SDK, which games load from G HUB's folder: without it they cannot use it.
        if (config.trueForce && virtualWheel->trueForce) {
            if (const std::string sdk = wheelbri::service::trueForceSdkPath(); !sdk.empty()) {
                WE_LOG_INFO(log(), "trueforce: Logitech's TrueForce SDK is installed ({}): a game that uses TrueForce "
                                   "streams its force to the virtual {}'s TrueForce interface; a game that uses "
                                   "DirectInput alone gets its effects one by one, as on a G920", sdk, wheelName);
            } else {
                WE_LOG_WARN(log(), "trueforce: Logitech's TrueForce SDK is not installed (Program Files\\Logi\\"
                                   "trueforce): no game can use the virtual {}'s TrueForce until Logitech G HUB is "
                                   "installed; force feedback works without it, as on a G920", wheelName);
            }
        }
    }
#ifndef _WIN32
    // Linux: the virtual wheel is wheelbri-uhid, there until this function returns (Stop, or the window closed).
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
    // wheelbri's own filter (driver/hide) on every device of the rig, which lets only this process open them. Usually
    // hidden already, before the filter was put on (above); then not again (that would show it for a moment).
    std::vector<wheelbri::service::HideFilter::Check> hideChecks;
    if (g920 && options.hide && !hideTargets.empty()) {
        hideChecks = wheelbri::service::HideFilter::check(hideTargets);
        bool any = false;
        for (const auto& c : hideChecks) any = any || c.state == wheelbri::service::HideFilter::State::Ready;
        if (any && !rigHidden) {
            std::string message;
            rigHidden = steps->hideRig(message);
            if (!rigHidden) {
                WE_LOG_CRITICAL(log(), "hide: the rig is NOT HIDDEN from games, so the session does not start: {}",
                                message);
                return 1;
            }
        }
    }
    // One line for the window: what games will see besides the G920, and what is missing.
    DeviceSummary devices;
    if (g920) devices = virtualWheelSummary(wheelName, options.hide, hideTargets, hideChecks, rigHidden);
#ifndef _WIN32
    if (uhid != nullptr) {
        // Hiding the rig is the game's launch option on Linux (SDL_GAMECONTROLLER_IGNORE_DEVICES, wheelbri.sh).
        devices.text =
            "virtual " + wheelName + " on (" + uhid->describe() + "); hide the rig in the game's launch options";
    }
#endif
    // The config read again while the session runs (runLoop): as Start read it, with the wheel's own labels.
    const ReloadConfig reload = [&options, &source](std::vector<std::string>& problems)
        -> std::optional<wheelbri::EmulatorConfig> {
        LoadedConfig next = loadForRun(options);
        if (next.fileMissing) {
            problems.push_back("cannot open config file '" + options.configPath + "'");
            return std::nullopt;
        }
        if (!next.problems.empty()) {
            problems = std::move(next.problems);
            return std::nullopt;
        }
        if (source != nullptr) applySavedDeviceLabels(source->info(), next.config.mapper);
        return std::move(next.config);
    };
    int result = 0;
    std::optional<UiFrame> lastFrame;
    int quickLosses = 0;   // lost again soon after it came back: wait longer before the next try
    while (!gStop) {
        const std::uint64_t sessionStartMs = platform::nowMs();
        result = runLoop(config, reload, options.configPath, loaded.text, logFolder, logFileOk, *source, accessories,
                         options.print, host, devices, lastFrame,
                         /*routePedals=*/options.virtualDevice || options.uhid || options.virtualWheelOutside);
        if (result != kWheelLost || gStop) {
            break;
        }
        // The wheel is gone (unplugged, USB reset, power). Keep the virtual devices: the game keeps its G920, which
        // reads centred with the pedals released until the wheel is back. Then the same session carries on.
        source.reset();
        accessories.close();
        quickLosses = platform::nowMs() - sessionStartMs < 10'000 ? quickLosses + 1 : 0;
        const std::uint64_t retryEveryMs =
            quickLosses == 0 ? 1000
                             : std::min<std::uint64_t>(30'000, 5000ULL * static_cast<std::uint64_t>(quickLosses));
        WE_LOG_WARN(log(), "waiting for the wheel and its accessories to come back (plug it in again; Stop to give "
                           "up); the virtual devices stay, centred and released{}",
                    quickLosses > 0 ? "; it was lost again within 10 s of coming back (" + std::to_string(quickLosses) +
                                          " times in a row), so it is tried every " +
                                          std::to_string(retryEveryMs / 1000) + " s"
                                    : std::string());
        if (!waitForWheelBack(*backend, config, host, lastFrame, retryEveryMs, quickLosses > 0, source,
                              accessories)) {
            result = 0;   // stopped while waiting: a normal stop
            break;
        }
    }
    if (steps != nullptr) {
        // Out of the rig's stack until the next Start: every report of the wheel goes through the filter's host while
        // it is in. Nothing of ours may hold the wheel meanwhile (its restart would be refused), and the rig stays
        // hidden until it is out.
        source.reset();
        accessories.close();
        if (options.hide && !hideTargets.empty()) {
            if (host != nullptr) host->step("taking the hide filter off the wheel");
            std::string message;
            if (steps->takeHideFilterOff(hideInterfaces(hideTargets, /*listedOnly=*/false), {}, message) ==
                HideSetup::Waiting) {
                WE_LOG_WARN(log(), "hide: {}", message);
            }
            if (host != nullptr) host->step({});
        }
        steps->showRig();
        steps->removeVirtualWheel();
    }
#ifndef _WIN32
    if (uhid != nullptr && !uhid->running()) {
        WE_LOG_ERROR(log(), "virtual wheel: wheelbri-uhid ended during the session (its log says why)");
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
// program (the Windows package), shim/ and shim/x86/ next to bin/ (the Linux bundle: bin/wheelbri), or build/shim/
// and build-x86/shim/ (next to build/service/ in the source tree). Each empty when there is none (not built).
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
    sources.x86 = first({here / "shim" / "x86" / "dinput8.dll", build / "shim" / "x86" / "dinput8.dll",
                         build.parent_path() / x86Build / "shim" / "dinput8.dll"});
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
    options.virtualDevice = !options.noVirtualDevice;
#else
    options.uhid = !options.noVirtualDevice;
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
                                               const bool virtualDevice, const wheelbri::ButtonOrder order,
                                               const bool trueForce) {
        Options run = options;
        run.configPath = configPath;   // the config the Mapping tab shows
#ifdef _WIN32
        run.virtualDevice = virtualDevice;   // the Mapping tab's target: G920 virtual device or G920 in-game shim
#else
        run.uhid = virtualDevice;   // Linux: the session starts wheelbri-uhid for the virtual wheel
#endif
        LoadedConfig fresh = loadForRun(run);
        // The Mapping tab's Order picks the file (my-g25.ini, my-g25-native.ini) and wins over its [buttons] order:
        // WheelBri's default g25.ini serves both.
        fresh.config.buttonOrder = order;
        // Its TrueForce switch wins over [emulator] trueforce (off where Logitech's SDK is missing).
        fresh.config.trueForce = trueForce;
        return runSession(run, fresh, logFileOk, &host);
    };
    settings.trueForceSdk = [] { return wheelbri::service::trueForceSdkPath(); };
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
    // Before the log and the other statics go: a Hello to the helper still running would use them.
    wheelbri::service::HelperProbe::stopShared();
    wheelbri::logging::shutdown();
    return exitCode;
}
