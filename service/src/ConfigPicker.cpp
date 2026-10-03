#include "ConfigPicker.hpp"

#include "Platform.hpp"

#include "wheelbri/Logging.hpp"

#include <algorithm>
#include <utility>

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.config");
    return logger;
}

using Colour = ui::Colour;

// The same colours as the rest of the window (App.cpp, WheelSetupView.cpp).
constexpr Colour kPanel{44, 48, 56};
constexpr Colour kStripe{30, 33, 39};
constexpr Colour kText{222, 226, 232};
constexpr Colour kDim{120, 126, 136};
constexpr Colour kHeading{120, 190, 255};
constexpr Colour kActive{64, 190, 100};
constexpr Colour kWarning{214, 72, 64};
constexpr Colour kCaution{206, 164, 58};
constexpr Colour kDark{16, 18, 20};

std::string fit(std::string text, const std::size_t chars) {
    if (text.size() > chars) text = text.substr(0, chars - 3) + "...";
    return text;
}

bool samePath(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    if (std::filesystem::equivalent(a, b, ec)) return true;
    return a.lexically_normal() == b.lexically_normal();
}

}  // namespace

ConfigPicker::ConfigPicker(ui::UiWindow& window, Settings settings)
    : window_(window), settings_(std::move(settings)), selected_(settings_.initial) {
    refresh();
    WE_LOG_INFO(log(), "config: {} ({} to pick from)", selected_.empty() ? std::string("built-in defaults") : selected_,
                configs_.size());
}

void ConfigPicker::refresh() {
    configs_ = listConfigs(settings_.builtInFolder, settings_.userFolder);
    // A config given on the command line that is in neither folder is shown too.
    if (!selected_.empty() &&
        std::none_of(configs_.begin(), configs_.end(), [this](const ConfigEntry& c) { return samePath(c.path, selected_); })) {
        const std::filesystem::path path(selected_);
        configs_.push_back({path.stem().string(), path, false});
    }
    index_ = 0;
    for (std::size_t i = 0; i < configs_.size(); ++i) {
        if (!selected_.empty() && samePath(configs_[i].path, selected_)) index_ = i;
    }
    if (selected_.empty() && !configs_.empty()) selected_ = configs_[0].path.string();
    resetFrom_ = current() != nullptr ? resetTemplateOf(*current(), configs_) : std::nullopt;
}

const ConfigEntry* ConfigPicker::current() const {
    return index_ < configs_.size() ? &configs_[index_] : nullptr;
}

void ConfigPicker::message(std::string text, const bool error) {
    message_ = std::move(text);
    messageError_ = error;
}

void ConfigPicker::adopt(const std::filesystem::path& path, const std::string& note) {
    selected_ = path.string();
    refresh();
    pick(index_);
    message(note, false);
}

void ConfigPicker::pick(const std::size_t index) {
    if (index >= configs_.size()) return;
    ++changes_;
    index_ = index;
    selected_ = configs_[index].path.string();
    resetFrom_ = resetTemplateOf(configs_[index], configs_);
    mode_ = Mode::Idle;
    message_.clear();
    if (!writeConfigSelection(settings_.stateFile, selected_)) {
        WE_LOG_WARN(log(), "cannot save the picked config in {}", settings_.stateFile.string());
    }
    WE_LOG_INFO(log(), "config picked: {}", selected_);
}

void ConfigPicker::create() {
    const std::string name = [this] {
        std::string text = newName_;
        while (!text.empty() && text.back() == ' ') text.pop_back();
        return text;
    }();
    if (newFrom_ >= configs_.size() || !configs_[newFrom_].builtIn) {
        message("Pick the built-in config to start from.", true);
        return;
    }
    const ConfigEntry from = configs_[newFrom_];
    const ConfigResult result = createConfig(settings_.userFolder, name, from, configs_);
    if (!result.ok) {
        message("Not created: " + result.error + ".", true);
        return;
    }
    WE_LOG_INFO(log(), "config '{}' made from {}: {}", name, from.path.string(), result.path.string());
    selected_ = result.path.string();
    refresh();
    pick(index_);
    message("Made '" + name + "' from " + from.name + " and picked it: Start uses it.", false);
}

void ConfigPicker::reset() {
    mode_ = Mode::Idle;
    const ConfigEntry* const config = current();
    if (config == nullptr) return;
    const ConfigResult result = resetConfig(*config, configs_);
    if (!result.ok) {
        message("Not reset: " + result.error + ".", true);
        WE_LOG_WARN(log(), "config '{}' not reset: {}", config->name, result.error);
        return;
    }
    WE_LOG_INFO(log(), "config '{}' reset to {}: {}", config->name, resetFrom_ ? resetFrom_->path.string() : "?",
                result.path.string());
    ++changes_;
    message("'" + config->name + "' is " + (resetFrom_ ? resetFrom_->name : std::string("its built-in")) +
                " again: every change is gone. The next Start uses it.",
            false);
    refresh();
}

void ConfigPicker::openFolder() {
    const ConfigEntry* const config = current();
    if (config == nullptr) return;
    std::string error;
    if (!window_.openFolder(config->path.parent_path(), error)) {
        WE_LOG_WARN(log(), "cannot open the config's folder: {}", error);
    }
}

// --- input ------------------------------------------------------------------------------------------------

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

void ConfigPicker::onText(const std::string& text) {
    if (mode_ != Mode::New) return;
    for (const char c : text) {
        if (c == ui::kBackspace) {
            if (!newName_.empty()) newName_.pop_back();
        } else if (c == ui::kEnter) {
            create();   // picks the new config and closes New, or says why not
            return;
        } else if (newName_.size() < kMaxConfigName && !(newName_.empty() && c == ' ') &&
                   static_cast<unsigned char>(c) >= ' ' && static_cast<unsigned char>(c) < 0x7F) {
            newName_ += c;   // configNameProblem says what is not allowed when Create is pressed
        }
    }
}

void ConfigPicker::onKey(const char key) {
    if (key == ui::kEscape) mode_ = Mode::Idle;
}

// --- drawing ------------------------------------------------------------------------------------------------

float ConfigPicker::button(const float x, const float y, const std::string& label, std::function<void()> action,
                           const bool on, const bool enabled, const float width) {
    const float w = width > 0 ? width : static_cast<float>(label.size()) * ui::kCharWidth + 14;
    window_.fill(x, y, w, 16, !enabled ? kDark : on ? kHeading : kPanel);
    window_.outline(x, y, w, 16, enabled ? (on ? kHeading : kDim) : kPanel);
    const float textX = x + (w - static_cast<float>(label.size()) * ui::kCharWidth) / 2;
    window_.text(textX, y + 4, label, !enabled ? kDim : on ? kDark : kText);
    if (enabled) hits_.push_back({x, y, w, 16, std::move(action)});
    return w;
}

void ConfigPicker::draw(const float x0, const float y0, const bool canChange) {
    hits_.clear();
    if (!canChange) mode_ = Mode::Idle;
    window_.text(x0, y0, "MAPPING CONFIG", kHeading);
    window_.text(x0 + 124, y0, "what Start uses: how the wheel's axes and buttons become the emulated wheel's", kDim);

    const ConfigEntry* const config = current();
    const std::size_t count = configs_.size();
    float x = x0;
    const float row = y0 + 16;
    x += button(x, row, "<", [this, count] { pick((index_ + count - 1) % count); }, false, canChange && count > 1, 24) + 4;
    window_.fill(x, row, 340, 16, kStripe);
    window_.text(x + 6, row + 4,
                 config == nullptr ? std::string("no config found: built-in defaults")
                                   : fit(config->name, 28) + (config->builtIn ? "  (built-in)" : "  (yours)"),
                 config == nullptr ? kCaution : kText);
    x += 344;
    x += button(x, row, ">", [this, count] { pick((index_ + 1) % count); }, false, canChange && count > 1, 24) + 8;
    window_.text(x, row + 4, count == 0 ? "" : std::to_string(index_ + 1) + " of " + std::to_string(count), kDim);
    x += 72;
    const bool anyBuiltIn = std::any_of(configs_.begin(), configs_.end(), [](const ConfigEntry& c) { return c.builtIn; });
    x += button(x, row, "New", [this] {
        mode_ = Mode::New;
        newName_.clear();
        message_.clear();
        // Start from the picked config's built-in: the picked one itself, or the one it was made from.
        const ConfigEntry* const base = current() != nullptr && current()->builtIn ? current()
                                        : resetFrom_                                ? &*resetFrom_
                                                                                    : nullptr;
        newFrom_ = configs_.size();
        for (std::size_t i = 0; i < configs_.size(); ++i) {
            if (!configs_[i].builtIn) continue;
            if (newFrom_ == configs_.size() || (base != nullptr && configs_[i].path == base->path)) newFrom_ = i;
        }
    }, mode_ == Mode::New, canChange && anyBuiltIn, 64) + 6;
    x += button(x, row, "Reset", [this] {
        mode_ = Mode::ConfirmReset;
        message_.clear();
    }, mode_ == Mode::ConfirmReset, canChange && resetFrom_.has_value(), 64) + 6;
    button(x, row, "Open folder", [this] { openFolder(); }, false, config != nullptr, 0);

    window_.text(x0, row + 22, config == nullptr ? std::string() : fit(config->path.string(), 112), kDim);

    float y = row + 40;
    if (!canChange) {
        window_.text(x0, y, "Stop to pick another config, make a new one or reset this one.", kDim);
    } else if (mode_ == Mode::New) {
        window_.text(x0, y + 4, "New config", kHeading);
        const float field = x0 + 96;
        const float width = static_cast<float>(kMaxConfigName) * ui::kCharWidth + 16;
        window_.fill(field, y, width, 16, kDark);
        window_.outline(field, y, width, 16, kHeading);
        const bool blink = (platform::nowMs() / 500) % 2 == 0;
        window_.text(field + 6, y + 4, newName_ + (blink ? "_" : ""), kText);
        hits_.push_back({field, y, width, 16, [] {}});
        float bx = field + width + 8;
        bx += button(bx, y, "Create", [this] { create(); }, true, !newName_.empty(), 70) + 6;
        button(bx, y, "Cancel", [this] { mode_ = Mode::Idle; }, false, true, 70);
        y += 20;
        window_.text(x0, y + 4, "Start from", kDim);
        float cx = field;
        for (std::size_t i = 0; i < configs_.size(); ++i) {
            if (!configs_[i].builtIn) continue;
            cx += button(cx, y, configs_[i].name, [this, i] { newFrom_ = i; }, newFrom_ == i, true) + 6;
        }
        y += 20;
        if (!message_.empty()) {
            window_.text(x0, y + 4, fit(message_, 112), messageError_ ? kWarning : kActive);
        } else {
            window_.text(x0, y + 4, "Type its name (letters, digits, spaces, - _ . ( )); Enter or Create makes it, Esc drops it.",
                         kDim);
        }
    } else if (mode_ == Mode::ConfirmReset && config != nullptr && resetFrom_) {
        window_.text(x0, y + 4,
                     fit("Reset '" + config->name + "'? It becomes a copy of " + resetFrom_->name +
                             " again and every change in it is lost.",
                         112),
                     kCaution);
        y += 20;
        float bx = x0;
        bx += button(bx, y, "Yes, reset", [this] { reset(); }, true, true, 100) + 6;
        button(bx, y, "Cancel", [this] { mode_ = Mode::Idle; }, false, true, 70);
    } else {
        std::string about;
        if (config == nullptr) {
            about = "No config found next to the program: Start uses the built-in defaults.";
        } else if (config->builtIn) {
            about = "Built in: the defaults, never changed. New makes a copy of your own that Reset can put back.";
        } else if (resetFrom_) {
            about = "Yours, made from " + resetFrom_->name + ": Reset makes it that again.";
        } else {
            about = "Not made with New: Reset has no built-in config to put back.";
        }
        window_.text(x0, y, fit(about, 112), kDim);
        if (!message_.empty()) window_.text(x0, y + 16, fit(message_, 112), messageError_ ? kWarning : kActive);
    }
}

}  // namespace wheelbri::service
