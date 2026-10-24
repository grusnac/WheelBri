#include "ConfigPicker.hpp"
#include "UiStyle.hpp"

#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/Logging.hpp"

#include <system_error>
#include <utility>

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.config");
    return logger;
}

using Colour = ui::Colour;

// The same colours as the rest of the window (App.cpp, MappingEditView.cpp, WheelSetupView.cpp).
constexpr Colour kText = style::kText;
constexpr Colour kDim = style::kDim;

constexpr float kRowHeight = 16;
constexpr float kTargetWidth = 52;   // a Target button: "G920", "G25", "G27", "G923"
constexpr float kWayWidth = 72;      // a Mode button: "Virtual", "Shim"

// What each Target is for (its button's hover text).
std::string targetAbout(const ConfigPicker::Target target) {
    switch (target) {
        case ConfigPicker::Target::G25:
            return "G25: for older games (Race Driver: GRID). Its button order and centring spring are set below.";
        case ConfigPicker::Target::G27:
            return "G27: for older games. Its button order and centring spring are set below.";
        case ConfigPicker::Target::G920:
            return "G920: the Xbox wheel, WheelBri's default and the one tried most in games.";
        case ConfigPicker::Target::G923:
            return "G923: the G920's successor.";
    }
    return {};
}

}  // namespace

std::string ConfigPicker::name() const {
    return std::string(targetName(target_)) + (way_ == Way::VirtualDevice ? " Mode Virtual" : " Mode Shim");
}

const char* ConfigPicker::targetName(const Target target) {
    switch (target) {
        case Target::G25: return "G25";
        case Target::G27: return "G27";
        case Target::G923: return "G923";
        case Target::G920: break;
    }
    return "G920";
}

const char* ConfigPicker::profileName(const Target target) {
    switch (target) {
        case Target::G25: return "g25";
        case Target::G27: return "g27";
        case Target::G923: return "g923";
        case Target::G920: break;
    }
    return "g920";
}

const char* ConfigPicker::wayName(const Way way) { return way == Way::VirtualDevice ? "Virtual" : "Shim"; }

std::optional<ConfigPicker::Target> ConfigPicker::parseTarget(const std::string_view profile) {
    if (profile == profileName(Target::G920)) return Target::G920;
    if (profile == profileName(Target::G25)) return Target::G25;
    if (profile == profileName(Target::G27)) return Target::G27;
    if (profile == profileName(Target::G923)) return Target::G923;
    return std::nullopt;
}

std::optional<ConfigPicker::Way> ConfigPicker::parseWay(const std::string_view way) {
    if (way == "virtual") return Way::VirtualDevice;
    if (way == "shim") return Way::Shim;
    return std::nullopt;
}

std::string ConfigPicker::builtInName(const Target target) { return std::string(profileName(target)) + ".ini"; }

std::array<std::string, 2> ConfigPicker::olderOwnNames(const Target target) {
    const std::string profile = profileName(target);
    return {"my-" + profile + "-virtual-device.ini", "my-" + profile + "-in-game-shim.ini"};
}

ConfigPicker::ConfigPicker(ui::UiWindow& window, Settings settings)
    : window_(window), settings_(std::move(settings)) {
    // Not set yet: the virtual G920, as before there was a choice. Set to something else: the same, and the log says.
    const std::string way = readAppSetting(settings_.stateFile, "way");
    way_ = parseWay(way).value_or(Way::VirtualDevice);
    if (!way.empty() && !parseWay(way)) {
        WE_LOG_WARN(log(), "[app] way = '{}' is neither virtual nor shim: Virtual", way);
    }
    if (!settings_.virtualAvailable) {
        way_ = Way::Shim;   // app.ini keeps what it says
        WE_LOG_INFO(log(), "Mode: Shim only (Mode Virtual needs Windows 11)");
    }
    const std::string target = readAppSetting(settings_.stateFile, "target");
    target_ = parseTarget(target).value_or(Target::G920);
    if (!target.empty() && !parseTarget(target)) {
        WE_LOG_WARN(log(), "[app] target = '{}' is not a wheel WheelBri emulates (g25, g27, g920, g923): the G920",
                    target);
    }
    refresh();
    WE_LOG_INFO(log(), "target {}; config {}", name(), selected_.empty() ? "(none)" : selected_);
}

bool ConfigPicker::virtualDeviceSupported(const std::uint32_t windowsBuild) {
    return windowsBuild == 0 || windowsBuild >= kVirtualDeviceMinBuild;
}

std::filesystem::path ConfigPicker::builtInPath() const { return settings_.builtInFolder / builtInName(); }

std::filesystem::path ConfigPicker::ownPath() const {
    return settings_.userFolder / ("my-" + builtInName());
}

void ConfigPicker::adoptOlderOwn() {
    // Once per target ([app] older_mapping_<target> = checked): Default deletes my-<target>.ini, and the older files
    // must not bring it back.
    const std::string key = std::string("older_mapping_") + profileName(target_);
    if (!readAppSetting(settings_.stateFile, key).empty()) return;
    std::error_code ec;
    const std::filesystem::path own = ownPath();
    if (std::filesystem::exists(own, ec)) {
        static_cast<void>(writeAppSetting(settings_.stateFile, key, "checked"));
        return;
    }
    // Both modes had their own mapping before: the newer of the two is the one for both now. The older files stay.
    std::filesystem::path newest;
    std::filesystem::file_time_type newestTime{};
    for (const std::string& name : olderOwnNames(target_)) {
        const std::filesystem::path older = settings_.userFolder / name;
        if (!std::filesystem::is_regular_file(older, ec)) continue;
        const auto time = std::filesystem::last_write_time(older, ec);
        if (ec) continue;
        if (newest.empty() || time > newestTime) {
            newest = older;
            newestTime = time;
        }
    }
    if (newest.empty() || !std::filesystem::is_directory(settings_.userFolder, ec)) {
        static_cast<void>(writeAppSetting(settings_.stateFile, key, "checked"));
        return;
    }
    std::filesystem::copy_file(newest, own, ec);
    if (ec) {
        WE_LOG_WARN(log(), "cannot copy your mapping {} to {}: {}", newest.string(), own.string(), ec.message());
        return;
    }
    WE_LOG_INFO(log(), "your mapping {} is now {}, for both modes (the older file is kept)", newest.string(),
                own.string());
    static_cast<void>(writeAppSetting(settings_.stateFile, key, "checked"));
}

void ConfigPicker::refresh() {
    adoptOlderOwn();
    std::error_code ec;
    const std::filesystem::path own = ownPath();
    const std::filesystem::path builtIn = builtInPath();
    const bool hasOwn = std::filesystem::is_regular_file(own, ec);
    const bool hasBuiltIn = std::filesystem::is_regular_file(builtIn, ec);
    const std::string selected = hasOwn ? own.string() : hasBuiltIn ? builtIn.string() : std::string();
    if (selected != selected_ || builtIn_ != !hasOwn) ++changes_;
    selected_ = selected;
    builtIn_ = !hasOwn;
    // The command line service (--ui) and the next start read it from here.
    if (!selected_.empty() && !writeConfigSelection(settings_.stateFile, selected_)) {
        WE_LOG_WARN(log(), "cannot save the config in {}", settings_.stateFile.string());
    }
}

bool ConfigPicker::dropOwnEdits(std::string& error) {
    std::error_code ec;
    const std::filesystem::path own = ownPath();
    if (!std::filesystem::remove(own, ec) && ec) {
        error = "cannot delete " + own.string() + ": " + ec.message();
        WE_LOG_WARN(log(), "{}", error);
        return false;
    }
    WE_LOG_INFO(log(), "own mapping dropped ({}): {} uses the default {} again", own.string(), name(),
                builtInPath().string());
    refresh();
    return true;
}

void ConfigPicker::choose(const Way way) {
    if (locked_) return;
    if (way == Way::VirtualDevice && !settings_.virtualAvailable) return;   // Virtual is greyed: never asked
    way_ = way;
    if (!writeAppSetting(settings_.stateFile, "way", way == Way::Shim ? "shim" : "virtual")) {
        WE_LOG_WARN(log(), "cannot save the way in {}", settings_.stateFile.string());
    }
    chosen();
}

void ConfigPicker::choose(const Target target) {
    if (locked_) return;
    target_ = target;
    if (!writeAppSetting(settings_.stateFile, "target", profileName(target))) {
        WE_LOG_WARN(log(), "cannot save the target in {}", settings_.stateFile.string());
    }
    chosen();
}

void ConfigPicker::chosen() {
    refresh();
    WE_LOG_INFO(log(), "target {}; config {}", name(), selected_.empty() ? "(none)" : selected_);
}

bool ConfigPicker::onClick(const float x, const float y) {
    if (locked_) {   // a session runs: also the buttons of a draw from before it started do nothing
        hits_.clear();
        return false;
    }
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) {
            const auto action = it->action;   // a copy: the list belongs to the last draw
            hits_.clear();                    // stale until the next draw: a second click before it does nothing
            action();
            return true;
        }
    }
    return false;
}

float ConfigPicker::draw(const float x0, const float y, const bool canChangeNow, const TipSink& tip) {
    hits_.clear();
    const bool canChange = canChangeNow && !locked_;
    float x = x0;
    // One button of a row, side by side with the next (their edges overlap): filled blue when chosen, outlined when
    // it can be chosen, greyed when not (`available` false, or nothing can change now: the chosen one then keeps a
    // grey fill, so what runs still shows).
    const auto segment = [&](const char* label, const float w, const bool chosen, const bool available,
                             const std::string& about, const std::function<void()>& action) {
        const bool on = canChange && available;
        if (chosen && !on) {
            window_.fill(x, y, w, kRowHeight, style::kPanel);
            window_.outline(x, y, w, kRowHeight, style::kFrame);
            const float textX = x + (w - static_cast<float>(std::string_view(label).size()) * ui::kCharWidth) / 2;
            window_.text(textX, y + 4, label, kText);
        } else {
            style::button(window_, x, y, w, kRowHeight, label, chosen, on);
        }
        if (on && !chosen) hits_.push_back({x, y, w, kRowHeight, action});
        if (tip) tip(x, y, w, kRowHeight, canChangeNow && !locked_ ? about : about + " Fixed while WheelBri runs.");
        x += w - 1;
    };
    window_.text(x, y + 4, "Target", kDim);
    x += 64;
    for (const Target t : {Target::G25, Target::G27, Target::G920, Target::G923}) {
        segment(targetName(t), kTargetWidth, t == target_, true, targetAbout(t), [this, t] { choose(t); });
    }
    x += 1 + 16;
    window_.text(x, y + 4, "Mode", kDim);
    x += 48;
    segment(wayName(Way::VirtualDevice), kWayWidth, way_ == Way::VirtualDevice, settings_.virtualAvailable,
            settings_.virtualAvailable
                ? "Virtual: a virtual wheel every game sees; your rig is hidden while WheelBri runs (Windows 11)."
                : "Mode Virtual needs Windows 11: its driver installs from build 22000.",
            [this] { choose(Way::VirtualDevice); });
    segment(wayName(Way::Shim), kWayWidth, way_ == Way::Shim, true,
            "Shim: a dinput8.dll next to each game on the Games tab shows the Target to that game only.",
            [this] { choose(Way::Shim); });
    return x + 1 + 4;
}

}  // namespace wheelbri::service
