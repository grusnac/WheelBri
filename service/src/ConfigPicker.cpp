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
constexpr float kFieldWidth = 220;
constexpr float kRowHeight = 16;

}  // namespace

const char* ConfigPicker::name(const Way way) {
    return way == Way::VirtualDevice ? "G920 virtual device" : "G920 in-game shim";
}

const char* ConfigPicker::builtInName(const Way way) {
    return way == Way::VirtualDevice ? "g920-virtual-device.ini" : "g920-in-game-shim.ini";
}

ConfigPicker::ConfigPicker(ui::UiWindow& window, Settings settings)
    : window_(window), settings_(std::move(settings)) {
    way_ = readAppSetting(settings_.stateFile, "way") == "shim" ? Way::Shim : Way::VirtualDevice;
    refresh();
    WE_LOG_INFO(log(), "target {}; config {}", name(way_), selected_.empty() ? "(none)" : selected_);
}

std::filesystem::path ConfigPicker::builtInPath() const { return settings_.builtInFolder / builtInName(way_); }

std::filesystem::path ConfigPicker::ownPath() const {
    return settings_.userFolder / (std::string("my-") + builtInName(way_));
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
    WE_LOG_INFO(log(), "own mapping dropped ({}): {} uses the default {} again", own.string(), name(way_),
                builtInPath().string());
    refresh();
    return true;
}

void ConfigPicker::choose(const Way way) {
    if (way == way_) return;
    way_ = way;
    if (!writeAppSetting(settings_.stateFile, "way", way == Way::Shim ? "shim" : "virtual")) {
        WE_LOG_WARN(log(), "cannot save the target in {}", settings_.stateFile.string());
    }
    refresh();
    WE_LOG_INFO(log(), "target {}; config {}", name(way_), selected_.empty() ? "(none)" : selected_);
}

bool ConfigPicker::onClick(const float x, const float y) {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) {
            const auto action = it->action;   // a copy: the list belongs to the last draw
            action();
            return true;
        }
    }
    return false;
}

void ConfigPicker::draw(const float x0, const float y, const bool canChange) {
    hits_.clear();
    window_.text(x0, y + 4, "Target", kDim);
    float x = x0 + 64;
    // Two targets: either arrow goes to the other one.
    const Way other = way_ == Way::VirtualDevice ? Way::Shim : Way::VirtualDevice;
    const auto arrow = [&](const char* label) {
        window_.fill(x, y, kArrowWidth, kRowHeight, canChange ? kPanel : kDark);
        window_.outline(x, y, kArrowWidth, kRowHeight, canChange ? kDim : kPanel);
        window_.text(x + (kArrowWidth - ui::kCharWidth) / 2, y + 4, label, canChange ? kText : kDim);
        if (canChange) hits_.push_back({x, y, kArrowWidth, kRowHeight, [this, other] { choose(other); }});
        x += kArrowWidth + 4;
    };
    arrow("<");
    window_.fill(x, y, kFieldWidth, kRowHeight, kStripe);
    const std::string label = name(way_);
    window_.text(x + (kFieldWidth - static_cast<float>(label.size()) * ui::kCharWidth) / 2, y + 4, label,
                 canChange ? kHeading : kText);
    x += kFieldWidth + 4;
    arrow(">");
}

}  // namespace wheelbri::service
