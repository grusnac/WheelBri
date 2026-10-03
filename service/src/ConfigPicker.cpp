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

// The same colours as the rest of the window (App.cpp, MappingEditView.cpp).
constexpr Colour kPanel{44, 48, 56};
constexpr Colour kText{222, 226, 232};
constexpr Colour kDim{120, 126, 136};
constexpr Colour kHeading{120, 190, 255};
constexpr Colour kDark{16, 18, 20};

constexpr const char* kBuiltInName = "rig-as-g920.ini";
constexpr const char* kOwnName = "my-g920.ini";

}  // namespace

const char* ConfigPicker::describe(const Way way) {
    return way == Way::VirtualDevice ? "the virtual G920 (every game sees it)"
                                     : "the in-game shim (dinput8.dll next to the game)";
}

ConfigPicker::ConfigPicker(ui::UiWindow& window, Settings settings)
    : window_(window),
      settings_(std::move(settings)),
      builtInPath_(settings_.builtInFolder / kBuiltInName),
      ownPath_(settings_.userFolder / kOwnName) {
    way_ = readAppSetting(settings_.stateFile, "way") == "shim" ? Way::Shim : Way::VirtualDevice;
    refresh();
    WE_LOG_INFO(log(), "G920 through {}; config {}", describe(way_), selected_.empty() ? "(none)" : selected_);
}

void ConfigPicker::refresh() {
    std::error_code ec;
    const bool own = std::filesystem::is_regular_file(ownPath_, ec);
    const bool builtIn = std::filesystem::is_regular_file(builtInPath_, ec);
    const std::string selected = own ? ownPath_.string() : builtIn ? builtInPath_.string() : std::string();
    if (selected != selected_ || builtIn_ != !own) ++changes_;
    selected_ = selected;
    builtIn_ = !own;
    // The command line service (--ui) and the next start read it from here.
    if (!selected_.empty() && !writeConfigSelection(settings_.stateFile, selected_)) {
        WE_LOG_WARN(log(), "cannot save the config in {}", settings_.stateFile.string());
    }
}

bool ConfigPicker::dropOwnEdits(std::string& error) {
    std::error_code ec;
    if (!std::filesystem::remove(ownPath_, ec) && ec) {
        error = "cannot delete " + ownPath_.string() + ": " + ec.message();
        WE_LOG_WARN(log(), "{}", error);
        return false;
    }
    WE_LOG_INFO(log(), "own mapping dropped ({}): the rig fills {} again", ownPath_.string(), builtInPath_.string());
    refresh();
    return true;
}

void ConfigPicker::choose(const Way way) {
    if (way == way_) return;
    way_ = way;
    if (!writeAppSetting(settings_.stateFile, "way", way == Way::Shim ? "shim" : "virtual")) {
        WE_LOG_WARN(log(), "cannot save the choice in {}", settings_.stateFile.string());
    }
    WE_LOG_INFO(log(), "G920 through {}", describe(way_));
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
    window_.text(x0, y + 4, "G920 via", kDim);
    float x = x0 + 76;
    const auto choice = [&](const std::string& label, const Way way) {
        const bool on = way_ == way;
        constexpr float kWidth = 150;
        window_.fill(x, y, kWidth, 16, !canChange && !on ? kDark : on ? kHeading : kPanel);
        window_.outline(x, y, kWidth, 16, on ? kHeading : canChange ? kDim : kPanel);
        const float textX = x + (kWidth - static_cast<float>(label.size()) * ui::kCharWidth) / 2;
        window_.text(textX, y + 4, label, on ? kDark : canChange ? kText : kDim);
        if (canChange) hits_.push_back({x, y, kWidth, 16, [this, way] { choose(way); }});
        x += kWidth + 6;
    };
    choice("virtual device", Way::VirtualDevice);
    choice("in-game shim", Way::Shim);
}

}  // namespace wheelbri::service
