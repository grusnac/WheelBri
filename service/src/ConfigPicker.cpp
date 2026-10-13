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
constexpr float kTargetWidth = 72;   // "G920", "G25"
constexpr float kWayWidth = 80;      // "Virtual", "Shim"
constexpr float kRowHeight = 16;

}  // namespace

std::string ConfigPicker::name() const {
    return std::string(targetName(target_)) + (way_ == Way::VirtualDevice ? " virtual device" : " in-game shim");
}

const char* ConfigPicker::targetName(const Target target) { return target == Target::G25 ? "G25" : "G920"; }

const char* ConfigPicker::profileName(const Target target) { return target == Target::G25 ? "g25" : "g920"; }

const char* ConfigPicker::wayName(const Way way) { return way == Way::VirtualDevice ? "Virtual" : "Shim"; }

std::string ConfigPicker::builtInName(const Target target, const Way way) {
    return std::string(profileName(target)) +
           (way == Way::VirtualDevice ? "-virtual-device.ini" : "-in-game-shim.ini");
}

ConfigPicker::ConfigPicker(ui::UiWindow& window, Settings settings)
    : window_(window), settings_(std::move(settings)) {
    way_ = readAppSetting(settings_.stateFile, "way") == "shim" ? Way::Shim : Way::VirtualDevice;
    target_ = readAppSetting(settings_.stateFile, "target") == "g25" ? Target::G25 : Target::G920;
    refresh();
    WE_LOG_INFO(log(), "target {}; config {}", name(), selected_.empty() ? "(none)" : selected_);
}

std::filesystem::path ConfigPicker::builtInPath() const { return settings_.builtInFolder / builtInName(); }

std::filesystem::path ConfigPicker::ownPath() const {
    return settings_.userFolder / ("my-" + builtInName());
}

void ConfigPicker::refresh() {
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
    if (way == way_) return;
    way_ = way;
    if (!writeAppSetting(settings_.stateFile, "way", way == Way::Shim ? "shim" : "virtual")) {
        WE_LOG_WARN(log(), "cannot save the way in {}", settings_.stateFile.string());
    }
    chosen();
}

void ConfigPicker::choose(const Target target) {
    if (target == target_) return;
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
    // One picker: a label, <, the value, >. `step` null: nothing to go to (the arrows are drawn off).
    const auto picker = [&](const char* label, const float labelWidth, const std::string& value, const float width,
                            const std::function<void()>& step) {
        window_.text(x, y + 4, label, kDim);
        x += labelWidth;
        const bool on = canChange && step != nullptr;
        const auto arrow = [&](const char* glyph) {
            window_.fill(x, y, kArrowWidth, kRowHeight, on ? kPanel : kDark);
            window_.outline(x, y, kArrowWidth, kRowHeight, on ? kDim : kPanel);
            window_.text(x + (kArrowWidth - ui::kCharWidth) / 2, y + 4, glyph, on ? kText : kDim);
            if (on) hits_.push_back({x, y, kArrowWidth, kRowHeight, step});
            x += kArrowWidth + 4;
        };
        arrow("<");
        window_.fill(x, y, width, kRowHeight, kStripe);
        window_.text(x + (width - static_cast<float>(value.size()) * ui::kCharWidth) / 2, y + 4, value,
                     canChange ? kHeading : kText);
        x += width + 4;
        arrow(">");
    };
    // The target and the way: two of each, so either arrow goes to the other one.
    const Target otherTarget = target_ == Target::G920 ? Target::G25 : Target::G920;
    picker("Target", 64, targetName(target_), kTargetWidth, [this, otherTarget] { choose(otherTarget); });
    x += 16;
    const Way other = way_ == Way::VirtualDevice ? Way::Shim : Way::VirtualDevice;
    picker("Run as", 56, wayName(way_), kWayWidth, [this, other] { choose(other); });
    return x;
}

}  // namespace wheelbri::service
