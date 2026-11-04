#include "ConfigPicker.hpp"
#include "UiStyle.hpp"

#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/FileText.hpp"
#include "wheelbri/IniDocument.hpp"
#include "wheelbri/IniEditor.hpp"
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
constexpr float kOrderWidth = 128;   // an Order button: "Logitech", "Native"; a TrueForce one: "Off", "On"
constexpr const char* kTrueForceKey = "trueforce_g923";

// What each Target is for (its button's hover text).
std::string targetAbout(const ConfigPicker::Target target) {
    switch (target) {
        case ConfigPicker::Target::G25:
            return "G25: for older games (Race Driver: GRID). Its button order is picked under Target, its centring "
                   "spring below.";
        case ConfigPicker::Target::G27:
            return "G27: for older games. Its button order is picked under Target, its centring spring below.";
        case ConfigPicker::Target::G920:
            return "G920: the Xbox wheel, WheelBri's default and the one tried most in games.";
        case ConfigPicker::Target::G923:
            return "G923: the G920's successor, with TrueForce (picked under Target).";
    }
    return {};
}

// The file's [buttons] order (Driver when it has none or cannot be read).
ButtonOrder fileOrder(const std::filesystem::path& path) {
    const auto text = readFileText(path);
    if (!text) return ButtonOrder::Driver;
    const auto line = IniDocument::parse(*text).document.get("buttons", "order");
    return line ? parseButtonOrder(*line).value_or(ButtonOrder::Driver) : ButtonOrder::Driver;
}

// `from` written to `to` with [buttons] order = `order`, everything else as it was. False if it cannot be.
bool copyWithOrder(const std::filesystem::path& from, const std::filesystem::path& to, const ButtonOrder order) {
    const auto text = readFileText(from);
    if (!text) return false;
    IniEditor editor(*text);
    editor.set("buttons", "order", std::string(toString(order)));
    return writeFileReplacing(to, editor.text());
}

// The file's [emulator] trueforce (or its older name trueforce_probe), false when it has none or cannot be read.
bool fileTrueForce(const std::filesystem::path& path) {
    const auto text = readFileText(path);
    if (!text) return false;
    const IniDocument document = IniDocument::parse(*text).document;
    auto line = document.get("emulator", "trueforce");
    if (!line) line = document.get("emulator", "trueforce_probe");
    return line && (*line == "true" || *line == "yes" || *line == "1");
}

}  // namespace

std::string ConfigPicker::name() const {
    return std::string(targetName(target_)) + (order_ == ButtonOrder::Native ? " Native" : "") +
           (way_ == Way::VirtualDevice ? " Mode Virtual" : " Mode Shim");
}

bool ConfigPicker::hasButtonOrders(const Target target) { return target == Target::G25 || target == Target::G27; }

bool ConfigPicker::hasTrueForce(const Target target) { return target == Target::G923; }

bool ConfigPicker::trueForceAtStart() const {
    return trueForce_ && trueForceInstalled() && hasTrueForce(target_) && way_ == Way::VirtualDevice;
}

std::string ConfigPicker::ownName(const Target target, const ButtonOrder order) {
    return "my-" + std::string(profileName(target)) + (order == ButtonOrder::Native ? "-native" : "") + ".ini";
}

std::string ConfigPicker::renumberedKey(const Target target) {
    return std::string("renumbered_") + profileName(target);
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
        WE_LOG_INFO(log(), "Mode: Shim only (Mode Virtual needs {})", windowsName(kVirtualDeviceMinBuild));
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

std::string ConfigPicker::windowsName(const std::uint32_t windowsBuild) {
    if (windowsBuild == 0) return {};
    if (windowsBuild < 9200) return "Windows 7";
    if (windowsBuild < 9600) return "Windows 8";
    if (windowsBuild < 10240) return "Windows 8.1";
    if (windowsBuild < 22000) return "Windows 10";
    return "Windows 11";
}

std::filesystem::path ConfigPicker::builtInPath() const { return settings_.builtInFolder / builtInName(); }

std::filesystem::path ConfigPicker::ownPath() const { return settings_.userFolder / ownName(target_, order_); }

void ConfigPicker::adoptOlderOwn() {
    // Once per target ([app] older_mapping_<target> = checked): Default deletes my-<target>.ini, and the older files
    // must not bring it back.
    const std::string key = std::string("older_mapping_") + profileName(target_);
    if (!readAppSetting(settings_.stateFile, key).empty()) return;
    std::error_code ec;
    const std::filesystem::path own = settings_.userFolder / ownName(target_, ButtonOrder::Driver);
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

void ConfigPicker::adoptOlderOrder() {
    if (!hasButtonOrders(target_)) return;
    const std::string key = std::string("order_") + profileName(target_);
    if (!readAppSetting(settings_.stateFile, key).empty()) return;
    const std::filesystem::path driver = settings_.userFolder / ownName(target_, ButtonOrder::Driver);
    const std::filesystem::path native = settings_.userFolder / ownName(target_, ButtonOrder::Native);
    std::error_code ec;
    ButtonOrder order = ButtonOrder::Driver;
    if (std::filesystem::is_regular_file(driver, ec) && fileOrder(driver) == ButtonOrder::Native) {
        // The user's mapping was made for Native: it is the Native one now, and the Logitech driver one starts as a
        // copy of it (its numbers as Logitech's driver gives them).
        if (!std::filesystem::exists(native, ec) && !copyWithOrder(driver, native, ButtonOrder::Native)) {
            WE_LOG_WARN(log(), "cannot copy your mapping {} to {}", driver.string(), native.string());
            return;   // tried again next time
        }
        if (!copyWithOrder(driver, driver, ButtonOrder::Driver)) {
            WE_LOG_WARN(log(), "cannot set order = driver in {}", driver.string());
        }
        order = ButtonOrder::Native;
        WE_LOG_INFO(log(), "your {} mapping said order = native: it is now {} (Native)", targetName(target_),
                    native.string());
    }
    static_cast<void>(writeAppSetting(settings_.stateFile, key, std::string(toString(order))));
}

void ConfigPicker::readOrder() {
    order_ = ButtonOrder::Driver;
    if (!hasButtonOrders(target_)) return;
    const std::string kept = readAppSetting(settings_.stateFile, std::string("order_") + profileName(target_));
    order_ = parseButtonOrder(kept).value_or(ButtonOrder::Driver);
}

void ConfigPicker::readTrueForce() {
    std::string kept = readAppSetting(settings_.stateFile, kTrueForceKey);
    if (kept.empty()) {
        // Before the switch it was set by hand in the G923's file: the user's own mapping if there is one.
        std::error_code ec;
        const std::filesystem::path own = settings_.userFolder / ownName(Target::G923, ButtonOrder::Driver);
        const std::filesystem::path file =
            std::filesystem::is_regular_file(own, ec) ? own : settings_.builtInFolder / builtInName(Target::G923);
        kept = fileTrueForce(file) ? "on" : "off";
        if (kept == "on") WE_LOG_INFO(log(), "TrueForce: on, as {} says ([emulator] trueforce)", file.string());
        static_cast<void>(writeAppSetting(settings_.stateFile, kTrueForceKey, kept));
    }
    trueForce_ = kept == "on";
}

void ConfigPicker::checkTrueForceSdk() {
    const std::string sdk = settings_.trueForceSdk ? settings_.trueForceSdk() : std::string();
    if (!sdkChecked_ || sdk != trueForceSdk_) {
        if (sdk.empty()) {
            WE_LOG_INFO(log(), "TrueForce: Logitech's TrueForce SDK is not installed (it comes with Logitech G HUB): "
                               "the G923's TrueForce stays off");
        } else {
            WE_LOG_INFO(log(), "TrueForce: Logitech's TrueForce SDK found ({})", sdk);
        }
    }
    trueForceSdk_ = sdk;
    sdkChecked_ = true;
    sdkCheckedAt_ = std::chrono::steady_clock::now();
}

void ConfigPicker::refresh() {
    adoptOlderOwn();
    adoptOlderOrder();
    readOrder();
    readTrueForce();
    checkTrueForceSdk();
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
    // Its buttons' numbers no longer need checking: the default has none of the user's.
    if (order_ == ButtonOrder::Native) {
        static_cast<void>(writeAppSetting(settings_.stateFile, renumberedKey(target_), ""));
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

void ConfigPicker::choose(const ButtonOrder order) {
    if (locked_ || !hasButtonOrders(target_) || order == order_) return;
    const std::filesystem::path driver = settings_.userFolder / ownName(target_, ButtonOrder::Driver);
    const std::filesystem::path native = settings_.userFolder / ownName(target_, ButtonOrder::Native);
    std::error_code ec;
    // The first Native: the user's own Logitech driver mapping goes with them, its buttons' game numbers changed.
    if (order == ButtonOrder::Native && !std::filesystem::exists(native, ec) &&
        std::filesystem::is_regular_file(driver, ec)) {
        if (copyWithOrder(driver, native, ButtonOrder::Native)) {
            static_cast<void>(writeAppSetting(settings_.stateFile, renumberedKey(target_), "pending"));
            WE_LOG_INFO(log(), "your {} mapping copied for Native: {}", targetName(target_), native.string());
        } else {
            WE_LOG_WARN(log(), "cannot copy your mapping {} to {}: Native starts from the default", driver.string(),
                        native.string());
        }
    }
    if (!writeAppSetting(settings_.stateFile, std::string("order_") + profileName(target_),
                         std::string(toString(order)))) {
        WE_LOG_WARN(log(), "cannot save the button order in {}", settings_.stateFile.string());
    }
    ++changes_;   // another mapping even when both are WheelBri's default (the same file)
    chosen();
}

void ConfigPicker::chooseTrueForce(const bool on) {
    if (locked_ || !hasTrueForce(target_) || on == trueForce_) return;
    if (on && !trueForceInstalled()) return;   // On is greyed: never asked
    trueForce_ = on;
    if (!writeAppSetting(settings_.stateFile, kTrueForceKey, on ? "on" : "off")) {
        WE_LOG_WARN(log(), "cannot save TrueForce in {}", settings_.stateFile.string());
    }
    WE_LOG_INFO(log(), "TrueForce {} for the G923 (at the next Start)", on ? "on" : "off");
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
    return hits_.dispatch(x, y);
}

float ConfigPicker::rowsHeight() const {
    return hasButtonOrders(target_) || hasTrueForce(target_) ? 2 * kRowHeight + 2 : kRowHeight;
}

float ConfigPicker::draw(const float x0, float y, const bool canChangeNow, const TipSink& tip) {
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
        if (on && !chosen) hits_.add(x, y, w, kRowHeight, action);
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
    if (settings_.virtualAvailable) {
        segment(wayName(Way::VirtualDevice), kWayWidth, way_ == Way::VirtualDevice, true,
                "Virtual: a virtual wheel every game sees; your rig is hidden while WheelBri runs.",
                [this] { choose(Way::VirtualDevice); });
        segment(wayName(Way::Shim), kWayWidth, way_ == Way::Shim, true,
                "Shim: a dinput8.dll next to each game on the Games tab shows the Target to that game only.",
                [this] { choose(Way::Shim); });
    } else {
        // No Virtual button at all: Shim alone, chosen and locked.
        const std::string windows = settings_.windowsName.empty() ? "this Windows" : settings_.windowsName;
        segment(wayName(Way::Shim), kWayWidth, true, false,
                "On " + windows + ", only Mode Shim is available: Mode Virtual's driver needs " +
                    windowsName(kVirtualDeviceMinBuild) + ".",
                [] {});
    }
    const float rowEnd = x + 1 + 4;
    if (hasButtonOrders(target_)) {
        // A second row under Target (the first one's right end is the shim's game, App: gamesRowX).
        const std::string target = targetName(target_);
        x = x0;
        y += kRowHeight + 2;
        window_.text(x, y + 4, "Order", kDim);
        if (tip) {
            tip(x, y, 60, kRowHeight,
                "Which numbers the game gets the " + target + "'s buttons by. Each order keeps its own mapping. "
                "Takes effect at the next Start.");
        }
        x += 64;
        segment("Logitech", kOrderWidth, order_ == ButtonOrder::Driver, true,
                "Numbered as with Logitech's driver, from 1. For most games.",
                [this] { choose(ButtonOrder::Driver); });
        segment("Native", kOrderWidth, order_ == ButtonOrder::Native, true,
                "Numbered as the wheel reports them, from 0. For games that expect it (ACC).",
                [this] { choose(ButtonOrder::Native); });
    }
    if (hasTrueForce(target_)) {
        // Installing G HUB while the app is open: On becomes available within a few seconds.
        if (std::chrono::steady_clock::now() - sdkCheckedAt_ >= std::chrono::seconds(kSdkCheckSeconds)) {
            checkTrueForceSdk();
        }
        x = x0;
        y += kRowHeight + 2;
        window_.text(x, y + 4, "TrueForce", kDim);
        if (tip) {
            tip(x, y, 60, kRowHeight,
                "Games that use Logitech's TrueForce (ACC, DiRT Rally 2.0) send their force through it, as to a real "
                "G923. Takes effect at the next Start.");
        }
        x += 64;
        const bool virtualMode = way_ == Way::VirtualDevice;
        const bool installed = trueForceInstalled();
        // What Start runs with shows as chosen: Off without the SDK, whatever was picked.
        const bool on = trueForce_ && installed;
        const std::string shimOnly = "Mode Virtual only: in Mode Shim the game talks to your own wheel.";
        segment("Off", kOrderWidth, !on, virtualMode,
                virtualMode ? "Off: a G923 without TrueForce. Games send their effects one by one, as to a G920."
                            : shimOnly,
                [this] { chooseTrueForce(false); });
        segment("On", kOrderWidth, on, virtualMode && installed,
                !virtualMode ? shimOnly
                : installed  ? "On: the virtual G923 has TrueForce, as a real one. Closest to a real G923 in ACC."
                             : "On needs Logitech G HUB installed (it need not run): its TrueForce files "
                               "(Program Files\\Logi\\trueforce) are not on this PC.",
                [this] { chooseTrueForce(true); });
        if (!installed) {
            window_.text(x + 8, y + 4, "needs Logitech G HUB", style::kCaution);
            if (tip) {
                tip(x + 4, y, 22 * ui::kCharWidth, kRowHeight,
                    "Install Logitech G HUB for TrueForce: games load Logitech's TrueForce from its folder.");
            }
        }
    }
    return rowEnd;
}

}  // namespace wheelbri::service
