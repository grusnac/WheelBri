#include "ConfigPicker.hpp"

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
constexpr Colour kPanel{44, 48, 56};
constexpr Colour kStripe{30, 33, 39};
constexpr Colour kText{222, 226, 232};
constexpr Colour kDim{120, 126, 136};
constexpr Colour kHeading{120, 190, 255};
constexpr Colour kDark{16, 18, 20};

constexpr float kArrowWidth = 24;
constexpr float kTargetWidth = 72;   // "G920", "G25", "G27", "G923"
constexpr float kWayWidth = 80;      // "Virtual", "Shim"
constexpr float kRowHeight = 16;

}  // namespace

std::string ConfigPicker::name() const {
    return std::string(targetName(target_)) + (way_ == Way::VirtualDevice ? " virtual device" : " in-game shim");
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
        WE_LOG_INFO(log(), "Mode: Shim only (the virtual device needs Windows 11)");
    }
    const std::string target = readAppSetting(settings_.stateFile, "target");
    target_ = parseTarget(target).value_or(Target::G920);
    if (!target.empty() && !parseTarget(target)) {
        WE_LOG_WARN(log(), "[app] target = '{}' is not a wheel WheelBri emulates (g920, g25, g27, g923): the G920",
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
    if (way == Way::VirtualDevice && !settings_.virtualAvailable) return;   // the arrows are off: never asked
    way_ = way;
    if (!writeAppSetting(settings_.stateFile, "way", way == Way::Shim ? "shim" : "virtual")) {
        WE_LOG_WARN(log(), "cannot save the way in {}", settings_.stateFile.string());
    }
    chosen();
}

void ConfigPicker::choose(const Target target) {
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

float ConfigPicker::draw(const float x0, const float y, const bool canChange) {
    hits_.clear();
    float x = x0;
    // One picker: a label, <, the value, >. `back` (<) and `step` (>); `step` null: nothing to go to (the arrows are
    // drawn off).
    const auto picker = [&](const char* label, const float labelWidth, const std::string& value, const float width,
                            const std::function<void()>& back, const std::function<void()>& step) {
        window_.text(x, y + 4, label, kDim);
        x += labelWidth;
        const bool on = canChange && step != nullptr;
        const auto arrow = [&](const char* glyph, const std::function<void()>& action) {
            window_.fill(x, y, kArrowWidth, kRowHeight, on ? kPanel : kDark);
            window_.outline(x, y, kArrowWidth, kRowHeight, on ? kDim : kPanel);
            window_.text(x + (kArrowWidth - ui::kCharWidth) / 2, y + 4, glyph, on ? kText : kDim);
            if (on) hits_.push_back({x, y, kArrowWidth, kRowHeight, action});
            x += kArrowWidth + 4;
        };
        arrow("<", back);
        window_.fill(x, y, width, kRowHeight, kStripe);
        window_.text(x + (width - static_cast<float>(value.size()) * ui::kCharWidth) / 2, y + 4, value,
                     canChange ? kHeading : kText);
        x += width + 4;
        arrow(">", step);
    };
    // The targets in a ring (G920, G25, G27, G923): < the one before, > the one after.
    constexpr int kTargets = 4;
    const int t = static_cast<int>(target_);
    const auto previous = static_cast<Target>((t + kTargets - 1) % kTargets);
    const auto next = static_cast<Target>((t + 1) % kTargets);
    picker("Target", 64, targetName(target_), kTargetWidth, [this, previous] { choose(previous); },
           [this, next] { choose(next); });
    x += 16;
    // Two ways: either arrow goes to the other one.
    const Way other = way_ == Way::VirtualDevice ? Way::Shim : Way::VirtualDevice;
    std::function<void()> step;
    if (settings_.virtualAvailable) step = [this, other] { choose(other); };
    picker("Mode", 56, wayName(way_), kWayWidth, step, step);
    return x;
}

}  // namespace wheelbri::service
