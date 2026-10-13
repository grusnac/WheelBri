#include "MappingEditView.hpp"

#include "Platform.hpp"
#include "Version.hpp"

#include "wheelbri/AxisTuning.hpp"
#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/DeviceMatch.hpp"
#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/FileText.hpp"
#include "wheelbri/GameProfiles.hpp"
#include "wheelbri/GameRotation.hpp"
#include "wheelbri/GameSettings.hpp"
#include "wheelbri/GameTab.hpp"
#include "wheelbri/IniDocument.hpp"
#include "wheelbri/Logging.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <span>
#include <utility>

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.mapping");
    return logger;
}

using Colour = ui::Colour;

// The same colours as the rest of the window (App.cpp, WheelSetupView.cpp).
constexpr Colour kPanel{44, 48, 56};
constexpr Colour kStripe{30, 33, 39};
constexpr Colour kPopup{36, 40, 48};
constexpr Colour kText{222, 226, 232};
constexpr Colour kDim{120, 126, 136};
constexpr Colour kHeading{120, 190, 255};
constexpr Colour kActive{64, 190, 100};
constexpr Colour kWarning{214, 72, 64};
constexpr Colour kCaution{206, 164, 58};
constexpr Colour kDark{16, 18, 20};

constexpr std::uint64_t kDevicesEveryMs = 2000;
constexpr std::array<const char*, 4> kAxisNames = {"Steering", "Throttle", "Brake", "Clutch"};
constexpr std::array<const char*, 4> kHatNames = {"Up", "Right", "Down", "Left"};

// The popups: 24 in from the sides and 56 from the top; as wide and tall as the canvas allows (popupW, popupH), their
// entries in more columns when wider (popupColumns).
constexpr float kPopupX = 24;
constexpr float kPopupY = 56;


// The game full lock slider: 90 to 1080 degrees, in steps of 10 (typed values go up to tuning::kMaxRotationDeg).
constexpr float kSliderX = 132;
constexpr float kSliderMinDeg = 90.0F;
constexpr float kSliderMaxDeg = 1080.0F;
constexpr float kSliderStepDeg = 10.0F;
constexpr std::array<float, 6> kPresetsDeg = {180.0F, 360.0F, 540.0F, 720.0F, 900.0F, 1080.0F};
constexpr std::size_t kMaxTyped = 12;

std::string fit(std::string text, const std::size_t chars) {
    if (text.size() > chars) text = text.substr(0, chars - 2) + "..";
    return text;
}

// `text` in lines of at most `chars`, broken at spaces; at most `maxLines`, the last one cut with "..".
std::vector<std::string> wrap(const std::string& text, const std::size_t chars, const std::size_t maxLines) {
    std::vector<std::string> lines;
    std::string rest = text;
    while (!rest.empty() && lines.size() < maxLines) {
        if (rest.size() <= chars || lines.size() + 1 == maxLines) {
            lines.push_back(fit(rest, chars));
            break;
        }
        std::size_t cut = rest.rfind(' ', chars);
        if (cut == std::string::npos || cut == 0) cut = chars;
        lines.push_back(rest.substr(0, cut));
        rest = rest.substr(std::min(rest.size(), cut + 1));
    }
    return lines;
}

std::string degrees(const float value) { return tuning::formatNumber(value, 1); }

// `path` is UTF-8 (as the picker keeps it).
std::string readFile(const std::string& path) {
    const std::filesystem::path file = pathFromUtf8(path);
    return readFileText(file.empty() ? std::filesystem::path(path) : file).value_or(std::string());
}

DeviceId idOf(const DeviceLabels& labels) { return {labels.vendorId, labels.productId}; }

// What a rig control is called in this tab: its name (empty when it has none: the caller says its number).
std::string controlName(const RigControl& control) { return control.label; }

std::string deviceName(const DeviceLabels& labels) {
    return labels.name.empty() ? deviceId(labels.vendorId, labels.productId) : labels.name;
}

}  // namespace

MappingEditView::MappingEditView(ui::UiWindow& window, std::filesystem::path rigsFolder, std::filesystem::path stateFile,
                                 ConfigPicker& picker)
    : window_(window), rigsFolder_(std::move(rigsFolder)), stateFile_(std::move(stateFile)), picker_(picker) {}

// --- state ----------------------------------------------------------------------------------------------------

void MappingEditView::update() {
    if (picker_.changes() != seenChanges_ || picker_.selected() != path_) {
        seenChanges_ = picker_.changes();
        load();
        return;
    }
    if (devicesReadMs_ == 0 || platform::nowMs() - devicesReadMs_ >= kDevicesEveryMs) {
        refreshDevices();   // the names and the devices the sources are picked from
    }
}

void MappingEditView::refreshDevices() {
    // The active rig, read again now and then: the Wheel setup tab may just have saved it, or picked another.
    rigName_ = readAppSetting(stateFile_, "rig");
    std::vector<std::string> errors;
    rig_ = rigName_.empty() ? Rig{} : readRig(rigsFolder_, rigName_, errors).value_or(Rig{});
    devices_.clear();
    for (const RigDevice& device : rig_.devices) devices_.push_back(wheelbri::labelsOf(device));
    devicesReadMs_ = platform::nowMs();
}

void MappingEditView::load() {
    mode_ = Mode::Idle;
    field_ = Field::None;
    dragging_ = false;
    path_ = picker_.selected();
    loaded_ = false;
    profile_.reset();
    builtIn_ = picker_.builtIn();
    refreshDevices();
    if (path_.empty()) {
        status_ = std::string("No config: config/") + picker_.builtInName() +
                  " is not next to the program.";
        statusError_ = true;
        return;
    }
    fileText_ = readFile(path_);
    if (fileText_.empty()) {
        status_ = "Cannot read " + path_;
        statusError_ = true;
        WE_LOG_WARN(log(), "mapping: cannot read {}", path_);
        return;
    }
    const auto parsed = IniDocument::parse(fileText_);
    const auto result = loadEmulatorConfig(parsed.document);
    targetName_ = result.config.targetProfile.empty() ? std::string("g920") : result.config.targetProfile;
    profile_ = makeProfile(result.config.targetProfile);
    if (profile_ == nullptr) profile_ = makeProfile("g920");
    saved_ = mappingModel(result.config);
    edit_ = saved_;
    loaded_ = true;
    if (!parsed.errors.empty() || !result.errors.empty()) {
        status_ = "The config has problems (Start lists them): " +
                  (parsed.errors.empty() ? result.errors.front() : parsed.errors.front());
        statusError_ = true;
    } else {
        status_ = !builtIn_ ? "Your own mapping (saved). Default goes back to WheelBri's."
                            : "WheelBri's default mapping. Change a source and Save to keep your own.";
        statusError_ = false;
    }
    WE_LOG_INFO(log(), "mapping: editing {}", path_);
}

void MappingEditView::save() {
    if (!loaded_) return;
    // A control is used once: as one input's role assignment or as one button's secondary.
    if (profile_ != nullptr) {
        if (const std::vector<std::string> twice = controlsUsedTwice(edit_, *profile_); !twice.empty()) {
            status_ = "Not saved: a control is used twice (red in its picker): " + twice.front() +
                      (twice.size() > 1 ? " (+" + std::to_string(twice.size() - 1) + " more)" : "") + ".";
            statusError_ = true;
            for (const std::string& line : twice) WE_LOG_INFO(log(), "mapping: used twice: {}", line);
            return;
        }
    }
    const MappingWriteResult written = writeMapping(fileText_, edit_);
    if (!written.error.empty()) {
        status_ = "Not saved: " + written.error + ".";
        statusError_ = true;
        return;
    }
    // The built-in default stays as it is: the user's mapping goes to their own file (made from it).
    const std::filesystem::path target = builtIn_ ? picker_.ownPath() : std::filesystem::path(path_);
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    std::string text;
    if (builtIn_) {
        text = std::string("; WheelBri: your own mapping for the ") + picker_.name() +
               " (the Mapping tab's Save; Default deletes it)\r\n";
    }
    text += written.text;
    // Written next to it and then swapped in, so a full disk or a crash never leaves half a mapping.
    if (!writeFileReplacing(target, text)) {
        status_ = "Could not write " + pathToUtf8(target);
        statusError_ = true;
        WE_LOG_ERROR(log(), "mapping: {}", status_);
        return;
    }
    WE_LOG_INFO(log(), "mapping: saved {}", pathToUtf8(target));
    if (builtIn_) {
        picker_.refresh();   // Start uses the own file from now on
        update();
    } else {
        fileText_ = written.text;
        saved_ = edit_;
    }
    status_ = "Saved. The next Start uses it.";
    statusError_ = false;
}

void MappingEditView::dropOwnEdits() {
    std::string error;
    if (!picker_.dropOwnEdits(error)) {
        status_ = "Not dropped: " + error;
        statusError_ = true;
        return;
    }
    update();
    status_ = "Your own mapping is gone: WheelBri's default mapping again.";
    statusError_ = false;
}

// --- devices and sources ---------------------------------------------------------------------------------------

bool MappingEditView::missing(const SourceRef& source, const bool axis) const {
    // The default mapping names controls of the rig it was made for; one this rig has not named stays mapped but
    // shows it (pick another, or name it in the Rig tab).
    if (!source.mapped() || rig_.devices.empty()) return false;
    const RigControl* const control = rigControl(source, axis);
    return control == nullptr || !control->defined();
}

const DeviceLabels* MappingEditView::wheelLabels() const {
    // No source_device (the built-in configs): the rig's wheel, as a session takes it.
    std::optional<DeviceId> exact = parseDeviceId(edit_.sourceDevice);
    if (edit_.sourceDevice.empty()) {
        const RigDevice* wheel = rig_.wheel();
        if (wheel == nullptr && !rig_.devices.empty()) wheel = &rig_.devices.front();
        if (wheel == nullptr) return nullptr;
        exact = wheel->id;
    }
    for (const DeviceLabels& labels : devices_) {
        if (exact ? idOf(labels) == *exact
                  : deviceMatches(labels.name, labels.vendorId, labels.productId, edit_.sourceDevice)) {
            return &labels;
        }
    }
    return nullptr;
}

const DeviceLabels* MappingEditView::labelsOf(const SourceRef& source) const {
    if (source.device.empty()) return wheelLabels();
    for (const DeviceLabels& labels : devices_) {
        if (idOf(labels) == source.device) return &labels;
    }
    return nullptr;
}

const RigControl* MappingEditView::rigControl(const SourceRef& source, const bool axis) const {
    const DeviceLabels* const labels = labelsOf(source);
    if (labels == nullptr || !source.mapped()) return nullptr;
    const RigDevice* const device = rig_.find(DeviceId{labels->vendorId, labels->productId});
    const auto index = static_cast<std::size_t>(source.index);
    if (device == nullptr || index >= (axis ? device->axes.size() : device->buttons.size())) return nullptr;
    return axis ? &device->axes[index] : &device->buttons[index];
}

std::string MappingEditView::deviceText(const SourceRef& source) const {
    const DeviceLabels* const labels = labelsOf(source);
    return labels != nullptr       ? deviceName(*labels)
           : source.device.empty() ? std::string("the wheel")
                                   : deviceId(source.device.vendorId, source.device.productId);
}

std::string MappingEditView::describe(const SourceRef& source, const bool axis) const {
    if (!source.mapped()) return "(nothing)";
    const std::string number = (axis ? "axis " : "button ") + std::to_string(source.index);
    // "Pedals: Gas": the control's name; its number when it has none.
    const RigControl* const control = rigControl(source, axis);
    std::string name = control != nullptr ? controlName(*control) : std::string();
    if (name.empty()) name = number;
    if (missing(source, axis)) name += "  (not in the rig)";
    return fit(deviceText(source), 20) + ": " + name;
}

std::string MappingEditView::describeFull(const SourceRef& source, const bool axis) const {
    if (!source.mapped()) return {};
    // "Moza R3: 'A', button 0": the hover text, with what the short form leaves out.
    std::string text = deviceText(source) + ": ";
    const std::string number = (axis ? "axis " : "button ") + std::to_string(source.index);
    const RigControl* const control = rigControl(source, axis);
    text += control != nullptr && !control->label.empty() ? "'" + control->label + "', " + number : number;
    if (missing(source, axis)) text += ", not in the rig";
    return text;
}

SourceRef& MappingEditView::slot(const Slot kind, const std::size_t index) {
    switch (kind) {
        case Slot::Axis: return edit_.axes[index];
        case Slot::Hat: return edit_.hat[index];
        case Slot::Button: break;
    }
    return edit_.buttons[index];
}

// --- input -------------------------------------------------------------------------------------------------------

void MappingEditView::setLocked(const bool locked) {
    locked_ = locked;
    if (locked_) {
        field_ = Field::None;   // a session started: nothing is edited until it stops
        dragging_ = false;
    }
}

void MappingEditView::onClick(const float x, const float y) {
    // A click anywhere ends the typing first (the value if it is usable; otherwise the status says why), then acts.
    if (field_ != Field::None) {
        commitTyping();
        field_ = Field::None;
    }
    clickX_ = x;
    if (mode_ == Mode::Idle && picker_.onClick(x, y)) return;   // the row at the top
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) {
            const auto action = it->action;   // a copy: the list belongs to the last draw
            hits_.clear();                    // stale until the next draw: a second click before it does nothing
            action();
            return;
        }
    }
}

void MappingEditView::onKey(const char key) {
    if (key != ui::kEscape) return;
    if (field_ != Field::None) {
        field_ = Field::None;   // Escape while typing: drop what was typed
        return;
    }
    mode_ = Mode::Idle;
}

void MappingEditView::onText(const std::string& text) {
    if (field_ == Field::None) return;
    for (const char c : text) {
        if (c == ui::kBackspace) {
            if (!typed_.empty()) typed_.pop_back();
        } else if (c == ui::kEnter) {
            if (commitTyping()) field_ = Field::None;   // not usable: keep typing, the status says why
            return;
        } else if (((c >= '0' && c <= '9') || c == '.' || c == '%' || c == ' ' ||
                    ((field_ == Field::PerGame || field_ == Field::GameSteering) &&
                     (c == '/' || c == 'o' || c == 'f' || c == 'O' || c == 'F'))) &&
                   typed_.size() < kMaxTyped) {
            typed_ += c;
        }
    }
}

void MappingEditView::onPointer(const float x, const float /*y*/, const bool leftDown) {
    if (!dragging_) return;
    if (!leftDown || locked_ || mode_ != Mode::Idle) {
        dragging_ = false;
        return;
    }
    setRotationFromX(x);
}

void MappingEditView::setRotationFromX(const float x) {
    const float t = std::clamp((x - kSliderX) / sliderW(), 0.0F, 1.0F);
    const float value = kSliderMinDeg + t * (kSliderMaxDeg - kSliderMinDeg);
    edit_.steeringRotationDeg =
        std::clamp(std::round(value / kSliderStepDeg) * kSliderStepDeg, kSliderMinDeg, kSliderMaxDeg);
}

std::string MappingEditView::fieldText(const Field field, const std::size_t index) const {
    switch (field) {
        case Field::PerGame:
        case Field::GameSteering: {
            // The rotation the game is played with: its own, else the Mapping tab's (one value, two places).
            const std::span<const GameProfile> games = gameProfiles();
            if (index >= games.size()) return {};
            return degrees(rotationWhileRunning(games[index], edit_.gameRotations).value_or(edit_.steeringRotationDeg));
        }
        case Field::GameSetting: {
            const std::span<const GameProfile> games = gameProfiles();
            const std::size_t g = index / kMaxGameSettings;
            const std::size_t s = index % kMaxGameSettings;
            if (g >= games.size() || s >= games[g].settings.size()) return {};
            const std::optional<float> value = gameSettingValue(games[g], games[g].settings[s], edit_.gameSettings);
            return value ? tuning::formatNumber(*value, 2) : std::string();
        }
        case Field::FfbGain: {
            const std::span<const GameProfile> games = gameProfiles();
            return index < games.size() ? tuning::formatNumber(gameFfbGain(games[index], edit_.gameFfbGains), 2)
                                        : std::string();
        }
        case Field::Rotation: return degrees(edit_.steeringRotationDeg);
        case Field::None: break;
    }
    return {};
}

void MappingEditView::startTyping(const Field field, const std::size_t index) {
    if (locked_) return;
    field_ = field;
    fieldIndex_ = index;
    typed_ = fieldText(field, index);
}

bool MappingEditView::commitTyping() {
    if (field_ == Field::None) return true;
    const std::optional<float> number = tuning::parseNumber(typed_);
    std::string problem;
    if (field_ == Field::PerGame || field_ == Field::GameSteering) {
        // "180" or "off" for one game: Mapping's and Game config's steering rotation are the same value.
        const std::span<const GameProfile> games = gameProfiles();
        const std::optional<GameRotationOverride> parsed = parseGameRotation(typed_);
        if (!parsed) {
            problem = "'" + typed_ + "': expected degrees (180) or off; from " +
                      tuning::formatNumber(tuning::kMinRotationDeg) + " to " +
                      tuning::formatNumber(tuning::kMaxRotationDeg);
        } else if (fieldIndex_ < games.size()) {
            edit_.gameRotations[games[fieldIndex_].id] = *parsed;
        }
    } else if (field_ == Field::GameSetting) {
        // A game's own setting: 0 to 1, as the game keeps it.
        const std::span<const GameProfile> games = gameProfiles();
        const std::size_t g = fieldIndex_ / kMaxGameSettings;
        const std::size_t s = fieldIndex_ % kMaxGameSettings;
        if (!number || *number < kGameSettingMin || *number > kGameSettingMax) {
            problem = "'" + typed_ + "': expected a number from " + tuning::formatNumber(kGameSettingMin) + " to " +
                      tuning::formatNumber(kGameSettingMax);
        } else if (g < games.size() && s < games[g].settings.size()) {
            edit_.gameSettings[games[g].id][games[g].settings[s].id] = *number;
        }
    } else if (field_ == Field::FfbGain) {
        // A game's force feedback gain: the game's pushing forces times this while it runs.
        const std::span<const GameProfile> games = gameProfiles();
        if (!number || *number < kFfbGainMin || *number > kFfbGainMax) {
            problem = "'" + typed_ + "': expected a number from " + tuning::formatNumber(kFfbGainMin) + " to " +
                      tuning::formatNumber(kFfbGainMax);
        } else if (fieldIndex_ < games.size()) {
            edit_.gameFfbGains[games[fieldIndex_].id] = *number;
        }
    } else if (!number) {
        problem = "'" + typed_ + "' is not a number";
    } else {
        const float value = *number;
        switch (field_) {
            case Field::Rotation:
                problem = tuning::rotationProblem(value);
                if (problem.empty()) edit_.steeringRotationDeg = value;
                break;
            case Field::PerGame:
            case Field::GameSteering:
            case Field::GameSetting:
            case Field::FfbGain:
            case Field::None: break;
        }
    }
    if (!problem.empty()) {
        status_ = "Not changed: " + problem + ".";
        statusError_ = true;
        return false;
    }
    if (statusError_) status_.clear();   // the last typing's complaint is answered
    statusError_ = false;
    return true;
}

// --- drawing ----------------------------------------------------------------------------------------------------

void MappingEditView::hit(const float x, const float y, const float w, const float h, std::function<void()> action) {
    hits_.push_back({x, y, w, h, std::move(action)});
}

float MappingEditView::button(const float x, const float y, const std::string& label, std::function<void()> action,
                              const bool on, const bool enabled, const float width) {
    const float w = width > 0 ? width : static_cast<float>(label.size()) * ui::kCharWidth + 14;
    window_.fill(x, y, w, 16, !enabled ? kDark : on ? kHeading : kPanel);
    window_.outline(x, y, w, 16, enabled ? (on ? kHeading : kDim) : kPanel);
    const float textX = x + (w - static_cast<float>(label.size()) * ui::kCharWidth) / 2;
    window_.text(textX, y + 4, label, !enabled ? kDim : on ? kDark : kText);
    if (enabled) hit(x, y, w, 16, std::move(action));
    return w;
}

void MappingEditView::drawField(const float x, const float y, const float w, const Field field,
                                const std::string& value, const bool changed, const std::size_t index) {
    const bool typing = field_ == field && fieldIndex_ == index;
    window_.fill(x, y, w, 16, typing ? kDark : kStripe);
    window_.outline(x, y, w, 16, typing ? kHeading : changed ? kCaution : kPanel);
    const auto chars = static_cast<std::size_t>((w - 12) / ui::kCharWidth);
    if (typing) {
        const std::string shown = typed_ + "_";
        window_.text(x + 6, y + 4, shown.size() > chars ? shown.substr(shown.size() - chars) : shown, kText);
    } else {
        window_.text(x + 6, y + 4, fit(value, chars), locked_ ? kDim : kText);
    }
    if (!locked_) hit(x, y, w, 16, [this, field, index] { startTyping(field, index); });
}

void MappingEditView::drawSource(const float x, const float y, const float w, const Slot kind,
                                 const std::size_t index) {
    const SourceRef& source = slot(kind, index);
    const bool changed = kind == Slot::Axis     ? source != saved_.axes[index]
                         : kind == Slot::Hat    ? source != saved_.hat[index]
                                                : source != saved_.buttons[index];
    // Used twice (an older mapping): red, and where in a tooltip (Save refuses it).
    const EntryState state =
        source.mapped() ? entryState({source, kind == Slot::Axis},
                                     {kind == Slot::Axis  ? ControlUse::Kind::Axis
                                      : kind == Slot::Hat ? ControlUse::Kind::Hat
                                                          : ControlUse::Kind::Button,
                                      index})
                        : EntryState{};
    window_.fill(x, y, w, 16, kStripe);
    window_.outline(x, y, w, 16, state.twice ? kWarning : changed ? kCaution : kPanel);
    if (state.twice) {
        tip(x, y, w, 16, state.tip);
    } else if (source.mapped()) {
        tip(x, y, w, 16, describeFull(source, kind == Slot::Axis));
    }
    const auto chars = static_cast<std::size_t>((w - 12) / ui::kCharWidth);
    window_.text(x + 6, y + 4, fit(describe(source, kind == Slot::Axis), chars),
                 missing(source, kind == Slot::Axis) ? kCaution : source.mapped() ? kText : kDim);
    hit(x, y, w, 16, [this, kind, index] {
        mode_ = Mode::Pick;
        pickKind_ = kind;
        pickIndex_ = index;
    });
}

void MappingEditView::draw() {
    hits_.clear();
    tips_.clear();
    const std::string target = profile_ != nullptr ? profile_->identity().shortName
                                                   : std::string(ConfigPicker::targetName(picker_.target()));
    // The top row: the title left; Default, Save and Revert right, left of the version (Version.hpp). The second row:
    // Target and Run as left; with Run as Shim, the app draws the shim's game at its right (App: gamesRowX).
    const float revertX = canvasW() - kVersionReserve - 80;
    const float saveX = revertX - 8 - 80;
    const float defaultX = saveX - 8 - 104;
    // Left of Default: "custom" while the user's own saved mapping is in use, nothing with WheelBri's default.
    constexpr std::string_view kCustom = "custom";
    const float customX = defaultX - 10 - static_cast<float>(kCustom.size()) * ui::kCharWidth;
    if (!builtIn_) {
        window_.text(customX, 6, std::string(kCustom), kCaution);
        tip(customX - 4, 2, defaultX - customX, 16, "Your saved mapping, not the default.");
    }
    const float titleEnd = builtIn_ ? defaultX : customX;
    const auto subtitle = [&](const float x, const std::string& text) {
        window_.text(x, 4, fit(text, static_cast<std::size_t>(std::max(0.0F, (titleEnd - 12 - x) / ui::kCharWidth))),
                     kDim);
    };
    if (page_ == Page::Games) {
        window_.text(4, 4, "GAME PROFILES", kHeading);
        subtitle(116, "the games WheelBri changes something for, and your own values for them");
    } else {
        window_.text(4, 4, "MAPPING", kHeading);
        subtitle(68, "what your controls do on the " + target);
    }

    // The target (fixed inputs, picked with < >) and how it runs.
    // The target is the Mapping tab's alone: the Game profiles page does not show it (its values are saved with the
    // same mapping, so Default, Save and Revert stay).
    if (page_ != Page::Games) {
        picker_.draw(4, 18, !locked_);
    }
    const bool canSave = loaded_ && dirty() && !locked_;
    button(defaultX, 2, "Default", [this] { dropOwnEdits(); }, false, !builtIn_ && !locked_, 104);
    button(saveX, 2, "Save", [this] { save(); }, canSave, canSave, 80);
    button(revertX, 2, "Revert", [this] {
        edit_ = saved_;
        status_ = "Changes dropped.";
        statusError_ = false;
    }, false, canSave, 80);
    if (!loaded_) {
        window_.text(4, 60, fit(status_, 118 + extraChars()), kWarning);
        return;
    }
    if (page_ == Page::Games) {
        drawGames();
        return;
    }

    // No wheel line: there is one rig, and its wheel is device 0 (Rig::wheel, or the config's source_device).
    float y = 44;
    if (rig_.devices.empty()) {
        window_.text(4, y,
                     rigName_.empty() ? "No rig yet: define your devices and their controls in the Rig tab first."
                                      : "The rig has no devices yet: name their controls in the Rig tab.",
                     kCaution);
        y += 22;
    }

    // Axes, left; D-pad, right.
    window_.text(4, y, "AXES", kHeading);
    window_.text(48, y, "click a source to pick another", kDim);
    // The axes' and the D-pad's source fields share what a wider window adds.
    const float half = extraW() / 2;
    window_.text(620 + half, y, "D-PAD", kHeading);
    for (std::size_t i = 0; i < kAxisNames.size(); ++i) {
        const float row = y + 16 + static_cast<float>(i) * 20;
        window_.text(4, row + 4, kAxisNames[i], kText);
        drawSource(80, row, 420 + half, Slot::Axis, i);
        button(508 + half, row, "Invert", [this, i] { edit_.invert[i] = !edit_.invert[i]; }, edit_.invert[i], true, 70);
        window_.text(620 + half, row + 4, kHatNames[i], kText);
        drawSource(668 + half, row, 288 + half, Slot::Hat, i);
    }

    // The target's buttons (the ones its profile names; the others are not seen by games).
    y += 16 + 4 * 20 + 12;
    window_.text(4, y, "BUTTONS", kHeading);
    window_.text(68, y, target + " button <- your control", kDim);
    std::size_t cell = 0;
    float gridEnd = y + 16;
    bool gap = false;   // an unnamed button before this one: a new group (the G920's shifter) starts on its own row
    for (std::size_t index = 0; index < edit_.buttons.size(); ++index) {
        const std::string_view name = profile_->buttonName(index);
        if (name.empty()) {
            gap = cell > 0;
            continue;
        }
        if (gap && cell % 2 != 0) ++cell;
        gap = false;
        const float x = 4 + static_cast<float>(cell % 2) * (478 + half);   // two columns across the canvas
        const float row = y + 16 + static_cast<float>(cell / 2) * 20;
        ++cell;
        gridEnd = row + 16;
        char label[32];
        std::snprintf(label, sizeof(label), "%2zu %.10s", index, std::string(name).c_str());
        window_.text(x, row + 4, label, kText);
        // The first source (a button), "or" a second one (a button, or a one-sided axis past a threshold).
        const float quarter = half / 2;   // each column's fields share its extra width
        drawSource(x + 112, row, 206 + quarter, Slot::Button, index);
        window_.text(x + 322 + quarter, row + 4, "or", kDim);
        drawAltSource(x + 342 + quarter, row, 126 + quarter, index);
    }

    drawRotation(gridEnd + 14);

    const float hint = contentH() - 52;
    window_.text(4, hint, "Sources are the rig's controls (named in the Rig tab): click one to pick what drives it.",
                 kDim);
    window_.text(4, hint + 12, "Save writes the config; Start uses it. Accessories must be plugged in to start.",
                 kDim);
    const std::string state = dirty() ? "unsaved changes" : "saved";
    window_.text(4, hint + 28, fit(status_.empty() ? state : status_, 118 + extraChars()),
                 statusError_ ? kWarning : status_.empty() ? (dirty() ? kCaution : kDim) : kActive);

    popupTips_ = tips_.size();   // a popup's own tips come after the tab's (which it covers)
    if (mode_ == Mode::Pick) drawPicker();
    if (mode_ == Mode::PickAlt) drawAltPicker();
}

void MappingEditView::tip(const float x, const float y, const float w, const float h, std::string text) {
    tips_.push_back({x, y, w, h, std::move(text)});
}

void MappingEditView::drawTooltip() {
    if (!hoverInside_) return;
    // The last one drawn wins; with a popup open only its own (it covers the tab below it).
    const std::size_t first = mode_ != Mode::Idle ? popupTips_ : 0;
    for (auto it = tips_.rbegin(); it != tips_.rend() - static_cast<std::ptrdiff_t>(first); ++it) {
        if (hoverX_ < it->x || hoverX_ >= it->x + it->w || hoverY_ < it->y || hoverY_ >= it->y + it->h) continue;
        const float kMaxW = canvasW() - 20;
        const std::string text = fit(it->text, static_cast<std::size_t>((kMaxW - 12) / ui::kCharWidth));
        const float w = static_cast<float>(text.size()) * ui::kCharWidth + 12;
        constexpr float h = 18;
        // Below and right of the pointer; moved in at the window's right edge, above the pointer at the bottom.
        float x = std::min(hoverX_ + 12, canvasW() - w - 2);
        float y = hoverY_ + 18;
        if (y + h > contentH() - 2) y = hoverY_ - h - 6;
        x = std::max(x, 2.0F);
        window_.fill(x, y, w, h, kDark);
        window_.outline(x, y, w, h, kHeading);
        window_.text(x + 6, y + 5, text, kText);
        return;
    }
}

MappingEditView::EntryState MappingEditView::entryState(const ControlRef& control, const ControlUse& self) const {
    EntryState state;
    if (profile_ == nullptr) return state;
    const std::vector<ControlUse> uses = usesOf(edit_, *profile_, control);
    std::vector<std::string> others;
    for (const ControlUse& use : uses) {
        if (use == self) {
            state.mine = true;
        } else {
            others.push_back(describeUse(use, *profile_));
        }
    }
    state.taken = !others.empty();
    state.twice = uses.size() > 1;
    for (std::size_t i = 0; i < others.size(); ++i) state.tip += (i > 0 ? "; " : "") + others[i];
    if (state.twice) state.tip = "Used twice: " + (state.mine ? "here; " : std::string()) + state.tip;
    return state;
}

void MappingEditView::drawEntry(const float x, const float y, const float w, const std::string& text,
                                const EntryState& state, std::function<void()> action) {
    const bool greyed = state.taken && !state.mine;
    window_.fill(x, y, w, 16, state.mine ? kHeading : greyed ? kDark : kPanel);
    if (state.twice) window_.outline(x, y, w, 16, kWarning);
    const auto chars = static_cast<std::size_t>((w - 8) / ui::kCharWidth);
    window_.text(x + 4, y + 4, fit(text, chars), state.mine ? kDark : greyed ? kDim : kText);
    if (!state.tip.empty()) tip(x, y, w, 16, state.tip);
    if (!greyed && action) hit(x, y, w, 16, std::move(action));
}

void MappingEditView::drawRotation(const float top) {
    float y = top;
    window_.text(4, y, "STEERING ROTATION", kHeading);
    window_.text(148, y, "full lock to full lock, in degrees", kDim);

    // The steering rotation: the slider (90 to 1080), the typed number (up to 2520) and the usual values.
    y += 16;
    window_.text(4, y + 4, "Rotation", kText);
    const float shown = std::clamp(edit_.steeringRotationDeg, kSliderMinDeg, kSliderMaxDeg);
    const float filled = (shown - kSliderMinDeg) / (kSliderMaxDeg - kSliderMinDeg) * sliderW();
    window_.fill(kSliderX, y + 6, sliderW(), 4, kStripe);
    window_.fill(kSliderX, y + 6, filled, 4, locked_ ? kDim : kHeading);
    window_.fill(kSliderX + filled - 3, y + 1, 6, 14, locked_ ? kDim : kText);
    window_.outline(kSliderX, y + 6, sliderW(), 4,
                    edit_.steeringRotationDeg != saved_.steeringRotationDeg ? kCaution : kPanel);
    if (!locked_) {
        hit(kSliderX - 4, y, sliderW() + 8, 16, [this] {
            setRotationFromX(clickX_);   // the press sets it; holding the button drags it (onPointer)
            dragging_ = true;
        });
    }
    drawField(kSliderX + sliderW() + 12, y, 64, Field::Rotation, degrees(edit_.steeringRotationDeg),
              edit_.steeringRotationDeg != saved_.steeringRotationDeg);
    window_.text(kSliderX + sliderW() + 80, y + 4, "deg", kDim);
    float x = kSliderX + sliderW() + 112;
    for (const float preset : kPresetsDeg) {
        x += button(x, y, degrees(preset), [this, preset] { edit_.steeringRotationDeg = preset; },
                    std::fabs(edit_.steeringRotationDeg - preset) < 0.5F, !locked_, 46) + 4;
    }
    window_.text(kSliderX, y + 20, "90", kDim);
    window_.text(kSliderX + sliderW() - 32, y + 20, "1080", kDim);

    // WheelBri passes the wheel through 1:1: the wheel's own software sets where its end stop is.
    y += 34;
    for (const std::string& line : wrap("Set your wheel's own software (Pit House for Moza) to " +
                                            degrees(edit_.steeringRotationDeg) +
                                            " deg too: WheelBri passes the wheel through 1:1 and never changes that "
                                            "setting, so the wheel's end stop is the game's full lock.",
                                        118 + extraChars(), 2)) {
        window_.text(4, y, line, kCaution);
        y += 12;
    }

    // A game can have its own (NFS Payback: 180): the Game profiles tab.
    y += 4;
    window_.text(4, y, "A game can steer with its own rotation (NFS Payback: 180): the Game profiles tab (G) sets it.",
                 kDim);
}

// --- the Games page ---------------------------------------------------------------------------------------------

namespace {

// The Games page: the list on the left, the picked game's profile right of it.
constexpr float kGameListW = 196;
constexpr float kGameX = 216;
constexpr float kGameValueX = kGameX + 168;   // the values' fields, and the lines under them
constexpr std::uint64_t kFileStateEveryMs = 2000;

std::string clockText(const std::uint64_t ms) {
    const std::uint64_t seconds = ms / 1000;
    char text[32];
    std::snprintf(text, sizeof(text), "%llu:%02llu:%02llu", static_cast<unsigned long long>(seconds / 3600),
                  static_cast<unsigned long long>(seconds / 60 % 60), static_cast<unsigned long long>(seconds % 60));
    return text;
}

}  // namespace

void MappingEditView::selectGame(const std::string& id) {
    for (const GameProfile& game : gameProfiles()) {
        if (game.id == id) {
            if (selectedGame_ != id) field_ = Field::None;
            selectedGame_ = id;
            return;
        }
    }
}

std::size_t MappingEditView::selectedGameIndex() const {
    const std::span<const GameProfile> games = gameProfiles();
    for (std::size_t g = 0; g < games.size(); ++g) {
        if (games[g].id == selectedGame_) return g;
    }
    // None picked yet: the one that runs, else the first for this target, else the first.
    for (std::size_t g = 0; g < games.size(); ++g) {
        if (games[g].id == now_.id) return g;
    }
    for (std::size_t g = 0; g < games.size(); ++g) {
        if (profileFitsTarget(games[g], targetName_)) return g;
    }
    return 0;
}

void MappingEditView::drawGames() {
    const std::span<const GameProfile> games = gameProfiles();
    float y = 46;
    if (games.empty()) {
        window_.text(4, y, "No game profiles in this build.", kWarning);
        return;
    }
    const std::size_t picked = selectedGameIndex();

    // The list: every built-in profile; the one that runs now in green.
    window_.text(4, y, "PROFILES", kHeading);
    y += 16;
    for (std::size_t g = 0; g < games.size(); ++g) {
        const GameProfile& game = games[g];
        const bool on = g == picked;
        const bool runs = game.id == now_.id;
        window_.fill(4, y, kGameListW, 18, on ? kHeading : kPanel);
        window_.text(10, y + 5, fit(game.label, 16), on ? kDark : kText);
        if (runs) {
            window_.fill(4 + kGameListW - 64, y + 3, 58, 12, kActive);
            window_.text(4 + kGameListW - 60, y + 5, "running", kDark);
        }
        const std::string id = game.id;
        hit(4, y, kGameListW, 18, [this, id] { selectGame(id); });
        y += 22;
    }
    y += 6;
    for (const char* line : {"Only games that need", "something changed have", "a profile; every other",
                             "game plays with the", "Mapping tab as it is."}) {
        window_.text(4, y, line, kDim);
        y += 12;
    }
    y += 8;
    for (const char* line : {"While WheelBri runs,", "this tab opens by itself", "when a game with a", "profile starts."}) {
        window_.text(4, y, line, kDim);
        y += 12;
    }
    window_.fill(kGameX - 8, 42, 1, contentH() - 100, kPanel);

    // The picked game.
    // Its three sections, in this order: mapping, force feedback, the game's own config.
    float top = drawGameProfile(picked, 46);
    top = drawGameMapping(picked, top + 10);
    top = drawGameFfb(picked, top + 10);
    drawGameSettings(picked, top + 10);

    const float hint = contentH() - 52;
    window_.text(4, hint,
                 "Your values are saved with the mapping (Save) and used from the next Start; Default goes back to the "
                 "profile's.",
                 kDim);
    window_.text(4, hint + 12, "While WheelBri runs they are shown, not changed.", kDim);
    const std::string state = dirty() ? "unsaved changes" : "saved";
    window_.text(4, hint + 28, fit(status_.empty() ? state : status_, 118 + extraChars()),
                 statusError_ ? kWarning : status_.empty() ? (dirty() ? kCaution : kDim) : kActive);
}

float MappingEditView::drawGameProfile(const std::size_t index, float y) {
    const GameProfile& game = gameProfiles()[index];
    window_.text(kGameX, y, game.label, kHeading);
    window_.text(kGameX + static_cast<float>(game.label.size() + 2) * ui::kCharWidth, y,
                 fit("program " + game.program, gameChars() - game.label.size() - 2),
                 kDim);
    y += 16;
    // A game that works one way only ([game] ways): a session run the other way tells the user when it starts.
    if (!game.virtualDevice || !game.shim) {
        const std::string only = game.shim ? "Run as Shim only" : "Run as Virtual only";
        for (const std::string& line : wrap(only + ": " + game.waysNote + ".", gameChars(), 2)) {
            window_.text(kGameX, y, line, kCaution);
            y += 12;
        }
    }

    // Now: the game that runs (while a session does), else how it is found.
    if (now_.id == game.id) {
        const std::string line =
            "RUNNING, pid " + std::to_string(now_.pid) + ", for " + clockText(now_.forMs) + ": its profile is in use";
        window_.text(kGameX, y, fit(line, gameChars()), kActive);
        y += 12;
        std::string applied;
        if (!now_.rotation.empty()) applied += "steering " + now_.rotation;
        if (std::fabs(now_.ffbGain - 1.0F) > 1e-4F) {
            applied += (applied.empty() ? "" : ", ") + std::string("force feedback x") +
                       tuning::formatNumber(now_.ffbGain, 2);
        }
        if (!describeOverlay(game).empty()) applied += (applied.empty() ? "" : ", ") + std::string("pedals as below");
        if (!applied.empty()) {
            window_.text(kGameX, y, fit("now: " + applied, gameChars()), kText);
            y += 12;
        }
        if (!now_.warning.empty()) {
            window_.text(kGameX, y, fit(now_.warning, gameChars()), kWarning);
            y += 12;
        }
    } else if (now_.session) {
        window_.text(kGameX, y,
                     fit(now_.id.empty() ? std::string("not running: WheelBri looks for it every 2 s")
                                         : "not running (another game with a profile runs)",
                         gameChars()),
                     kDim);
        y += 12;
    } else {
        window_.text(kGameX, y,
                     fit("While WheelBri runs (Start) it finds the game when it starts, and uses this profile.",
                         gameChars()),
                     kDim);
        y += 12;
    }
    return y;
}

// A section's heading on the Game profiles page: its name, and what it holds (dim).
void MappingEditView::gameSection(const float y, const std::string& name, const std::string& about) {
    window_.text(kGameX, y, name, kHeading);
    window_.text(kGameX + 160, y, fit(about, gameChars() - 20), kDim);
}

float MappingEditView::drawGameMapping(const std::size_t index, float y) {
    const GameProfile& game = gameProfiles()[index];
    const std::string id = game.id;
    gameSection(y, "MAPPING", "how the rig's controls reach the game while it runs");
    y += 16;

    // The profile's own remapping (built in): the pedals the game reads on other axes.
    if (const std::string pedals = describeOverlay(game); !pedals.empty()) {
        window_.text(kGameX, y, "Pedals", kText);
        for (const std::string& line : wrap(pedals, valueChars(), 2)) {
            window_.text(kGameValueX, y, line, kText);
            y += 12;
        }
        window_.text(kGameValueX, y, fit("built into the profile: the game reads them on other axes", valueChars()),
                     kDim);
        y += 18;
    }

    // Its steering rotation ([game_rotation]): the profile's, the user's own, or off (the Mapping tab's).
    const bool own = edit_.gameRotations.count(id) != 0;
    const std::optional<float> rotation = rotationWhileRunning(game, edit_.gameRotations);
    const bool rotationChanged = own != (saved_.gameRotations.count(id) != 0) ||
                                 (own && !(edit_.gameRotations.at(id) == saved_.gameRotations.at(id)));
    window_.text(kGameX, y + 4, "Steering rotation", kText);
    // The degrees it is played with (Off: the Mapping tab's), the same value as Game config's steering rotation.
    drawField(kGameValueX, y, 80, Field::PerGame, degrees(rotation.value_or(edit_.steeringRotationDeg)),
              rotationChanged, index);
    window_.text(kGameValueX + 84, y + 4, "deg", kDim);
    button(kGameValueX + 116, y, rotation ? "On" : "Off", [this, id, rotation, &game] {
        if (rotation) {
            edit_.gameRotations[id] = GameRotationOverride{true, {}};   // off: the Mapping tab's in this game too
        } else if (game.rotationDeg) {
            edit_.gameRotations.erase(id);                               // back to the profile's
        } else {
            edit_.gameRotations[id] = GameRotationOverride{false, edit_.steeringRotationDeg};
        }
    }, rotation.has_value(), !locked_, 48);
    button(kGameValueX + 168, y, "Default", [this, id] { edit_.gameRotations.erase(id); }, false, !locked_ && own, 72);
    y += 20;
    std::string about;
    if (game.rotationDeg) about = "Profile: " + degrees(*game.rotationDeg) + " deg (" + game.rotationNote + "). ";
    if (rotation) {
        about += "Set your wheel's own software to " + degrees(*rotation) + " deg while you play it.";
    } else {
        about += "Off: the Mapping tab's rotation (" + degrees(edit_.steeringRotationDeg) + " deg, your wheel's too).";
    }
    if (hasSteeringSetting(game)) {
        const float written = rotation.value_or(edit_.steeringRotationDeg);
        about += " Game config's Save to file writes " + degrees(written) + " deg into the game's settings" +
                 (written >= 1000.0F ? " (it keeps 3 digits: 100 to 999)." : ".");
    }
    for (const std::string& line : wrap(about, valueChars(), 3)) {
        window_.text(kGameValueX, y, line, kDim);
        y += 12;
    }
    return y;
}

float MappingEditView::drawGameFfb(const std::size_t index, float y) {
    const GameProfile& game = gameProfiles()[index];
    const std::string id = game.id;
    gameSection(y, "FORCE FEEDBACK", "what WheelBri does to the game's forces while it runs");
    y += 16;

    // Its force feedback gain ([game_ffb_gain]): WheelBri's, on the game's pushing forces.
    const bool ownGain = edit_.gameFfbGains.count(id) != 0;
    const auto savedGain = saved_.gameFfbGains.find(id);
    const bool gainChanged = ownGain != (savedGain != saved_.gameFfbGains.end()) ||
                             (ownGain && std::fabs(edit_.gameFfbGains.at(id) - savedGain->second) > 1e-5F);
    window_.text(kGameX, y + 4, "Force feedback gain", kText);
    drawField(kGameValueX, y, 80, Field::FfbGain, tuning::formatNumber(gameFfbGain(game, edit_.gameFfbGains), 2),
              gainChanged, index);
    button(kGameValueX + 168, y, "Default", [this, id] { edit_.gameFfbGains.erase(id); }, false, !locked_ && ownGain,
           72);
    y += 20;
    for (const std::string& line :
         wrap("Times its steering force and rumble, before the cap; its springs and dampers stay as they are. "
              "Profile: " +
                  tuning::formatNumber(game.ffbGain.value_or(1.0F), 2) +
                  (game.ffbNote.empty() ? std::string() : " (" + game.ffbNote + ")") + "; from " +
                  tuning::formatNumber(kFfbGainMin, 2) + " to " + tuning::formatNumber(kFfbGainMax, 2) + ".",
              valueChars(), 3)) {
        window_.text(kGameValueX, y, line, kDim);
        y += 12;
    }
    return y;
}

float MappingEditView::drawGameSettings(const std::size_t index, float y) {
    const GameProfile& game = gameProfiles()[index];
    const std::string id = game.id;
    if (game.settings.empty()) return y;   // nothing of its own to change: no Game config section at all
    gameSection(y, "GAME CONFIG", "the game's own settings: written into its file only by Save to file");
    y += 16;
    window_.text(kGameX, y, fit(game.settingsNote, gameChars()), kDim);
    y += 16;

    // What the file holds now against the values below (as shown, saved or not), read every 2 s.
    const GameFilePlace place = gameFilePlace(game);
    const std::vector<GameSettingText> wanted = gameValues(game);
    std::string key = id + "|" + pathToUtf8(place.root) + "|" + place.relative;
    for (const GameSettingText& value : wanted) key += "|" + value.name + "=" + value.text;
    const std::uint64_t nowMs = platform::nowMs();
    if (!fileState_ || key != fileStateKey_ || nowMs - fileStateReadMs_ >= kFileStateEveryMs) {
        fileState_ = readGameSettingsState(place.root, place.relative, wanted);
        fileStateKey_ = key;
        fileStateReadMs_ = nowMs;
    }
    const GameSettingsFileState& file = *fileState_;
    using Status = GameSettingsFileState::Status;
    window_.text(kGameX, y, "File", kText);
    const std::string where = place.located           ? pathToUtf8(place.root / pathFromUtf8(place.relative))
                              : !place.root.empty()   ? "Documents/" + game.settingsFile
                                                      : game.settingsFile;
    window_.text(kGameValueX, y, fit(where, valueChars()), place.located ? kText : kDim);
    y += 12;
    std::string fileLine;
    Colour fileColour = kDim;
    switch (file.status) {
        case Status::NotHere:
            fileLine = "not found here: Locate it";
            fileColour = kCaution;
            break;
        case Status::NoFile:
            fileLine = place.located ? "no longer there: Locate it again"
                                     : "not found: start the game once (it writes it), or Locate it";
            fileColour = kCaution;
            break;
        case Status::Unreadable:
            fileLine = "cannot be read: " + file.why;
            fileColour = kWarning;
            break;
        case Status::Read:
            if (file.asSet()) {
                fileLine = "holds the values below";
                fileColour = kActive;
            } else {
                fileLine = "not the values below yet: Save to file writes them (game closed)";
                fileColour = kCaution;
            }
            break;
    }
    window_.text(kGameValueX, y, fit(fileLine, valueChars()), fileColour);
    y += 16;
    // The file found: Save to file. Not found yet: Locate it first (the button then says Save to file).
    const bool found = file.status == Status::Read || file.status == Status::Unreadable;
    if (found) {
        button(kGameValueX, y, "Save to file", [this, index] { saveGameFile(index); }, !file.asSet(),
               !locked_ && file.status == Status::Read, 120);
        button(kGameValueX + 128, y, "Locate...", [this, index] { locateGameFile(index); }, false,
               !locked_ && platform::canPickFile(), 88);
        window_.text(kGameValueX + 224, y + 4, fit("keeps the original as .before-wheelbri", valueChars() - 28),
                     kDim);
    } else {
        button(kGameValueX, y, "Locate file", [this, index] { locateGameFile(index); }, true,
               !locked_ && platform::canPickFile(), 120);
        const std::string name = pathToUtf8(pathFromUtf8(game.settingsFile).filename());
        window_.text(kGameValueX + 128, y + 4,
                     fit(platform::canPickFile() ? "find the game's " + name + " file; then Save to file"
                                                 : "no file dialog on this system yet",
                         valueChars() - 16),
                     kDim);
    }
    y += 22;
    if (file.status == Status::Read) window_.text(fileColumnX(), y - 2, "in the file", kDim);
    y += 10;

    for (std::size_t s = 0; s < game.settings.size() && s < kMaxGameSettings; ++s) {
        if (y > contentH() - 52 - 20) break;   // keep clear of the hint
        const GameSetting& setting = game.settings[s];
        // A "steering" setting is the steering rotation: named and edited as in Mapping above, one value.
        window_.text(kGameX, y + 4, fit(setting.value ? gameSettingLabel(setting.id) : "steering rotation", 19), kText);
        const std::optional<std::string> inFile =
            s < file.entries.size() ? file.entries[s].inFile : std::optional<std::string>();
        // What the file holds, right: amber while it is not what the next Start writes.
        if (file.status == Status::Read) {
            const bool asWanted = s < wanted.size() && inFile == wanted[s].text;
            window_.text(fileColumnX(), y + 4, fit(inFile.value_or(std::string("not found")), 13),
                         asWanted ? kDim : kCaution);
        }
        tip(kGameX, y, fileColumnX() - kGameX, 16,
            setting.name + " in the game's settings file" +
                (setting.value ? ": profile " + tuning::formatNumber(*setting.value, 2) + ", 0 to 1"
                               : ": the steering rotation, in degrees"));
        if (!setting.value) {
            // The same value as Mapping's steering rotation: changing either changes both.
            const bool ownRotation = edit_.gameRotations.count(id) != 0;
            const bool rotationChanged =
                ownRotation != (saved_.gameRotations.count(id) != 0) ||
                (ownRotation && !(edit_.gameRotations.at(id) == saved_.gameRotations.at(id)));
            const float deg = rotationWhileRunning(game, edit_.gameRotations).value_or(edit_.steeringRotationDeg);
            drawField(kGameValueX, y, 80, Field::GameSteering, degrees(deg), rotationChanged, index);
            window_.text(kGameValueX + 84, y + 4, "deg", kDim);
            window_.text(kGameValueX + 248, y + 4, fit("same as in Mapping", 26), kDim);
            y += 20;
            continue;
        }
        const auto ownOf = [&](const GameSettingOverrides& values) -> std::optional<float> {
            const auto byGame = values.find(id);
            if (byGame == values.end()) return std::nullopt;
            const auto it = byGame->second.find(setting.id);
            return it == byGame->second.end() ? std::nullopt : std::optional<float>(it->second);
        };
        const std::optional<float> own = ownOf(edit_.gameSettings);
        const std::optional<float> saved = ownOf(saved_.gameSettings);
        const bool changed = own.has_value() != saved.has_value() || (own && std::fabs(*own - *saved) > 1e-5F);
        drawField(kGameValueX, y, 80, Field::GameSetting, tuning::formatNumber(own.value_or(*setting.value), 2),
                  changed, index * kMaxGameSettings + s);
        const std::string settingId = setting.id;
        button(kGameValueX + 168, y, "Default", [this, id, settingId] {
            if (const auto byGame = edit_.gameSettings.find(id); byGame != edit_.gameSettings.end()) {
                byGame->second.erase(settingId);
                if (byGame->second.empty()) edit_.gameSettings.erase(byGame);
            }
        }, false, !locked_ && own.has_value(), 72);
        window_.text(kGameValueX + 248, y + 4,
                     fit("profile " + tuning::formatNumber(*setting.value, 2) + ", 0 to 1", 26), kDim);
        y += 20;
    }
    return y;
}

MappingEditView::GameFilePlace MappingEditView::gameFilePlace(const GameProfile& game) const {
    // The one the user located (app state: game_file.<id>), else the profile's under Documents (Windows only).
    if (const std::string located = readAppSetting(stateFile_, "game_file." + game.id); !located.empty()) {
        const std::filesystem::path file = pathFromUtf8(located);
        return {file.parent_path(), pathToUtf8(file.filename()), true};
    }
    return {platform::documentsFolder(), game.settingsFile, false};
}

std::vector<GameSettingText> MappingEditView::gameValues(const GameProfile& game) const {
    // As shown (saved or not): the game's own rotation if it has one, else the Mapping tab's.
    const float steeringDeg = rotationWhileRunning(game, edit_.gameRotations).value_or(edit_.steeringRotationDeg);
    return gameSettingTexts(game, edit_.gameSettings, steeringDeg);
}

void MappingEditView::locateGameFile(const std::size_t index) {
    const GameProfile& game = gameProfiles()[index];
    const std::filesystem::path name = pathFromUtf8(game.settingsFile).filename();
    std::filesystem::path start = platform::documentsFolder();
    if (!start.empty()) {
        // Its folder under Documents when that is there, else Documents.
        std::error_code ec;
        const std::filesystem::path folder = (start / pathFromUtf8(game.settingsFile)).parent_path();
        if (std::filesystem::is_directory(folder, ec)) start = folder;
    }
    const std::optional<std::filesystem::path> picked =
        platform::pickFile("Locate " + game.label + "'s settings file (" + pathToUtf8(name) + ")", start,
                           pathToUtf8(name));
    if (!picked) return;   // cancelled
    // Only a file that holds the game's settings: every one of them found in it, once.
    const std::vector<GameSettingText> values = gameValues(game);
    const GameSettingsFileState state =
        readGameSettingsState(picked->parent_path(), pathToUtf8(picked->filename()), values);
    const bool all = state.status == GameSettingsFileState::Status::Read &&
                     std::all_of(state.entries.begin(), state.entries.end(),
                                 [](const GameSettingsFileState::Entry& e) { return e.inFile.has_value(); });
    if (!all) {
        status_ = "Not " + game.label + "'s settings file: " +
                  (state.status == GameSettingsFileState::Status::Read ? std::string("its settings are not in it")
                                                                       : state.why) + ".";
        statusError_ = true;
        WE_LOG_WARN(log(), "game settings: {}: {} is not its settings file", game.label, pathToUtf8(*picked));
        return;
    }
    if (!writeAppSetting(stateFile_, "game_file." + game.id, pathToUtf8(*picked))) {
        status_ = "Could not remember the file in " + pathToUtf8(stateFile_) + ".";
        statusError_ = true;
        return;
    }
    fileState_.reset();
    status_ = "Found " + game.label + "'s settings file: Save to file writes the values into it.";
    statusError_ = false;
    WE_LOG_INFO(log(), "game settings: {}: settings file located: {}", game.label, pathToUtf8(*picked));
}

void MappingEditView::saveGameFile(const std::size_t index) {
    const GameProfile& game = gameProfiles()[index];
    // The game reads the file when it starts and writes its own values back when it ends: only with it closed.
    if (const auto programs = platform::runningPrograms()) {
        for (const RunningProgram& program : *programs) {
            if (programName(program.name) == game.program) {
                status_ = game.label + " is running: close it first, then Save to file.";
                statusError_ = true;
                return;
            }
        }
    }
    const GameFilePlace place = gameFilePlace(game);
    const GameSettingsFileResult result = writeGameSettingsFile(place.root, place.relative, gameValues(game));
    fileState_.reset();
    using Outcome = GameSettingsFileResult::Outcome;
    switch (result.outcome) {
        case Outcome::Written: {
            std::string written;
            for (const GameSettingsEdit::Change& change : result.changes) {
                written += (written.empty() ? "" : ", ") + change.name + " " + change.before + " -> " + change.after;
            }
            WE_LOG_INFO(log(), "game settings: {}: written to {}: {} (the original is kept as {})", game.label,
                        pathToUtf8(result.file), written, pathToUtf8(result.backup));
            status_ = "Written to " + game.label + "'s settings file.";
            statusError_ = false;
            break;
        }
        case Outcome::AsSet:
            status_ = game.label + "'s settings file already holds these values.";
            statusError_ = false;
            break;
        case Outcome::NoFile:
            status_ = "No settings file there: start the game once, or Locate it.";
            statusError_ = true;
            break;
        case Outcome::Refused:
        case Outcome::Failed:
            status_ = "Not written: " + result.why + ".";
            statusError_ = true;
            WE_LOG_WARN(log(), "game settings: {}: not written to {}: {}", game.label, pathToUtf8(result.file),
                        result.why);
            break;
    }
}

std::string MappingEditView::axisName(const SourceRef& source) const {
    const RigControl* const control = rigControl(source, true);
    const std::string name = control != nullptr ? controlName(*control) : std::string();
    return name.empty() ? "axis " + std::to_string(source.index) : name;
}

std::string MappingEditView::shortAlt(const ButtonRef& alt) const {
    if (alt.axis) return describeAlt(alt);
    const RigControl* const control = rigControl(alt.source, false);
    const std::string name = control != nullptr ? controlName(*control) : std::string();
    return name.empty() ? "button " + std::to_string(alt.source.index) : name;
}

std::string MappingEditView::describeAlt(const ButtonRef& alt) const {
    if (!alt.mapped()) return "(nothing)";
    if (!alt.axis) return describe(alt.source, false);
    // "Hand > 50%": the axis' name, and where it presses the button.
    return axisName(alt.source) + " > " + tuning::formatPercent(alt.threshold);
}

void MappingEditView::drawAltSource(const float x, const float y, const float w, const std::size_t index) {
    const std::vector<ButtonRef>& list = edit_.secondaries[index];
    const bool changed = list != saved_.secondaries[index];
    const ControlUse self{ControlUse::Kind::Secondary, index};
    const bool twice = std::any_of(list.begin(), list.end(), [this, &self](const ButtonRef& ref) {
        return entryState({ref.source, ref.axis}, self).twice;
    });
    window_.fill(x, y, w, 16, kStripe);
    window_.outline(x, y, w, 16, twice ? kWarning : changed ? kCaution : kPanel);
    // The first, and how many more: "Hand > 50% +2".
    const std::string more = list.size() > 1 ? " +" + std::to_string(list.size() - 1) : std::string();
    const auto chars = static_cast<std::size_t>((w - 12) / ui::kCharWidth);
    const std::string first =
        list.empty() ? std::string("(nothing)") : fit(shortAlt(list.front()), chars - std::min(chars - 3, more.size()));
    const bool absent = std::any_of(list.begin(), list.end(),
                                    [this](const ButtonRef& ref) { return missing(ref.source, ref.axis); });
    window_.text(x + 6, y + 4, fit(first + more, chars), absent ? kCaution : list.empty() ? kDim : kText);
    if (!list.empty()) {
        std::string all;
        for (const ButtonRef& ref : list) all += (all.empty() ? "" : ", ") + describeAlt(ref);
        tip(x, y, w, 16, (twice ? "A control here is used twice (red in the picker). " : "") +
                             std::string("Also pressed by: ") + all);
    }
    hit(x, y, w, 16, [this, index] {
        mode_ = Mode::PickAlt;
        pickIndex_ = index;
        altFocus_ = 0;
    });
}

void MappingEditView::drawAltPicker() {
    hit(0, 0, canvasW(), contentH(), [] {});   // clicks outside the popup do nothing
    window_.fill(kPopupX, kPopupY, popupW(), popupH(), kPopup);
    window_.outline(kPopupX, kPopupY, popupW(), popupH(), kHeading);
    const float left = kPopupX + 12;
    const std::size_t target = pickIndex_;
    const std::string targetName = profile_->identity().shortName + " " + std::string(profile_->buttonName(target));
    window_.text(left, kPopupY + 10, "What else presses " + targetName + "? Its secondary assignments:", kHeading);
    button(kPopupX + popupW() - 100, kPopupY + 6, "Done", [this] { mode_ = Mode::Idle; }, false, true, 88);
    std::vector<ButtonRef>& list = edit_.secondaries[target];
    const std::size_t max = MapperConfig::kMaxSecondaries;
    button(left, kPopupY + 28, "(clear all)", [&list] { list.clear(); }, false, !list.empty(), 100);
    window_.text(left + 112, kPopupY + 32,
                 std::to_string(list.size()) + " of " + std::to_string(max) +
                     ". Click a control below to add or remove it; click an axis here for its threshold.",
                 kDim);

    // The ones it has, as chips (an axis' chip picks it for the threshold row; at most 8, so two rows of 5 or one).
    const int kColumns = popupColumns();
    const float kChipW = entryW(kColumns);
    const auto chipsPerRow = static_cast<std::size_t>(kColumns);
    if (altFocus_ >= list.size()) altFocus_ = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const float x = left + static_cast<float>(i % chipsPerRow) * (kChipW + 4);
        const float y = kPopupY + 48 + static_cast<float>(i / chipsPerRow) * 18;
        const bool focused = list[i].axis && i == altFocus_;
        window_.fill(x, y, kChipW, 16, focused ? kHeading : kStripe);
        window_.outline(x, y, kChipW, 16, kHeading);
        window_.text(x + 4, y + 4, fit(shortAlt(list[i]), static_cast<std::size_t>((kChipW - 8) / ui::kCharWidth)),
                     focused ? kDark : kText);
        tip(x, y, kChipW, 16, list[i].axis ? describeAlt(list[i]) : describeFull(list[i].source, false));
        if (list[i].axis) hit(x, y, kChipW, 16, [this, i] { altFocus_ = i; });
    }
    // An axis presses it past a threshold of its travel (from where it rests).
    float y = kPopupY + 88;
    if (altFocus_ < list.size() && list[altFocus_].axis) {
        ButtonRef& focused = list[altFocus_];
        window_.text(left, y + 4, fit(axisName(focused.source), 16) + " pressed past", kDim);
        float x = left + 248;
        for (const float threshold : {0.25F, 0.4F, 0.5F, 0.6F, 0.75F}) {
            x += button(x, y, tuning::formatPercent(threshold), [&focused, threshold] {
                focused.threshold = threshold;
            }, std::fabs(focused.threshold - threshold) < 0.001F, true, 52) + 4;
        }
        window_.text(x + 8, y + 4, "of its travel", kDim);
    }

    const DeviceLabels* const wheel = wheelLabels();
    std::vector<const DeviceLabels*> order;
    if (wheel != nullptr) order.push_back(wheel);
    for (const DeviceLabels& device : devices_) {
        if (&device != wheel) order.push_back(&device);
    }
    y = kPopupY + 112;
    const float kEntryW = kChipW;
    if (devices_.empty()) {
        window_.text(left, y, "The rig has no devices yet: define them in the Rig tab and Save.", kCaution);
    }
    const ControlUse self{ControlUse::Kind::Secondary, target};
    for (const DeviceLabels* const entry : order) {
        const DeviceLabels& device = *entry;
        if (y > kPopupY + popupH() - 30) break;
        const RigDevice* const rigDevice = rig_.find(idOf(device));
        if (rigDevice == nullptr) continue;
        const bool isWheel = wheel != nullptr && idOf(device) == idOf(*wheel);
        window_.text(left, y, fit(deviceName(device), 40) + "  " + deviceId(device.vendorId, device.productId) + "  " +
                                  (isWheel ? "(the wheel)" : deviceKindName(device.kind)),
                     kText);
        y += 14;
        int column = 0;
        const auto entryAt = [&](const bool axis, const std::size_t index, const RigControl& control) {
            const SourceRef source{isWheel ? DeviceId{} : idOf(device), static_cast<int>(index)};
            EntryState state = entryState({source, axis}, self);
            if (!state.mine && !state.taken && list.size() >= max) {
                state.taken = true;   // full: nothing more is added
                state.tip = "At most " + std::to_string(max) + " secondary assignments per button";
            }
            const float x = left + static_cast<float>(column) * (kEntryW + 4);
            const std::string name = controlName(control);
            const ButtonRef ref{source, axis, control.invert, 0.5F};
            drawEntry(x, y, kEntryW, (axis ? "axis " : "") + std::to_string(index) + " " + name, state,
                      [this, &list, ref, mine = state.mine] {
                          const auto same = [&ref](const ButtonRef& b) {
                              return b.axis == ref.axis && b.source == ref.source;
                          };
                          if (mine) {
                              list.erase(std::remove_if(list.begin(), list.end(), same), list.end());
                              altFocus_ = 0;
                              return;
                          }
                          list.push_back(ref);
                          if (ref.axis) altFocus_ = list.size() - 1;   // its threshold next
                      });
            if (++column == kColumns) {
                column = 0;
                y += 18;
            }
        };
        for (std::size_t b = 0; b < rigDevice->buttons.size(); ++b) {
            if (rigDevice->buttons[b].defined()) entryAt(false, b, rigDevice->buttons[b]);
        }
        // Every named axis: a pedal or a handbrake lever is what makes sense (one-sided), the user knows which.
        for (std::size_t a = 0; a < rigDevice->axes.size(); ++a) {
            if (rigDevice->axes[a].defined()) entryAt(true, a, rigDevice->axes[a]);
        }
        if (column != 0) y += 18;
        y += 8;
    }
    window_.text(left, kPopupY + popupH() - 20,
                 "Greyed: used elsewhere (hover to see where). Axes: a pedal or a lever, pressed past the threshold.",
                 kDim);
}

void MappingEditView::drawPicker() {
    hit(0, 0, canvasW(), contentH(), [] {});   // clicks outside the popup do nothing
    window_.fill(kPopupX, kPopupY, popupW(), popupH(), kPopup);
    window_.outline(kPopupX, kPopupY, popupW(), popupH(), kHeading);
    const bool axis = pickKind_ == Slot::Axis;
    const std::string what = pickKind_ == Slot::Axis ? kAxisNames[pickIndex_]
                             : pickKind_ == Slot::Hat ? std::string("D-pad ") + kHatNames[pickIndex_]
                                                      : "button " + std::to_string(pickIndex_) + " " +
                                                            std::string(profile_->buttonName(pickIndex_));
    const float left = kPopupX + 12;
    window_.text(left, kPopupY + 10, "What drives " + what + "?", kHeading);
    const SourceRef current = slot(pickKind_, pickIndex_);
    // What is being picked, as usesOf names it: a control used anywhere else is greyed (its use in a tooltip).
    const ControlUse self{pickKind_ == Slot::Axis  ? ControlUse::Kind::Axis
                          : pickKind_ == Slot::Hat ? ControlUse::Kind::Hat
                                                   : ControlUse::Kind::Button,
                          pickIndex_};
    button(kPopupX + popupW() - 100, kPopupY + 6, "Cancel", [this] { mode_ = Mode::Idle; }, false, true, 88);
    window_.text(left + 140, kPopupY + 32, "Greyed: used elsewhere (hover to see where); clear it there first.",
                 kDim);
    button(left, kPopupY + 28, "(nothing)", [this] {
        slot(pickKind_, pickIndex_) = {};
        mode_ = Mode::Idle;
    }, !current.mapped(), true, 120);

    const DeviceLabels* const wheel = wheelLabels();
    // The wheel first, then the accessories.
    std::vector<const DeviceLabels*> order;
    if (wheel != nullptr) order.push_back(wheel);
    for (const DeviceLabels& device : devices_) {
        if (&device != wheel) order.push_back(&device);
    }
    float y = kPopupY + 54;
    const int kColumns = popupColumns();   // more columns in a wider window
    const float kEntryW = entryW(kColumns);
    if (devices_.empty()) {
        window_.text(left, y, "The rig has no devices yet: define them in the Rig tab and Save.", kCaution);
    }
    for (const DeviceLabels* const entry : order) {
        const DeviceLabels& device = *entry;
        if (y > kPopupY + popupH() - 30) break;
        const bool isWheel = wheel != nullptr && idOf(device) == idOf(*wheel);
        window_.text(left, y, fit(deviceName(device), 40) + "  " + deviceId(device.vendorId, device.productId) + "  " +
                                  (isWheel ? "(the wheel)" : deviceKindName(device.kind)),
                     kText);
        y += 14;
        int column = 0;
        const std::size_t count = axis ? device.axes.size() : device.buttons.size();
        for (std::size_t index = 0; index < count; ++index) {
            const RigDevice* const rigDevice = rig_.find(idOf(device));
            const RigControl* const control =
                rigDevice == nullptr ? nullptr : axis ? &rigDevice->axes[index] : &rigDevice->buttons[index];
            if (control == nullptr || !control->defined()) continue;
            const SourceRef source{isWheel ? DeviceId{} : idOf(device), static_cast<int>(index)};
            const float x = left + static_cast<float>(column) * (kEntryW + 4);
            EntryState state = entryState({source, axis}, self);
            state.mine = state.mine || source == current;
            const std::string text = std::to_string(index) + " " +
                                     controlName(*control);
            const bool invert = control->invert;
            drawEntry(x, y, kEntryW, text, state, [this, source, invert] {
                slot(pickKind_, pickIndex_) = source;
                if (pickKind_ == Slot::Axis) edit_.invert[pickIndex_] = invert;   // as the rig says it rests
                mode_ = Mode::Idle;
            });
            if (++column == kColumns) {
                column = 0;
                y += 18;
            }
        }
        const RigDevice* const rigDevice = rig_.find(idOf(device));
        const bool anyDefined =
            rigDevice != nullptr &&
            (axis ? std::any_of(rigDevice->axes.begin(), rigDevice->axes.end(), [](const RigControl& c) { return c.defined(); })
                  : std::any_of(rigDevice->buttons.begin(), rigDevice->buttons.end(),
                                [](const RigControl& c) { return c.defined(); }));
        if (column == 0 && !anyDefined) {
            window_.text(left, y, axis ? "no axis named: name them in the Rig tab" : "no button named: name them in the Rig tab",
                         kDim);
            y += 14;
        } else if (column != 0) {
            y += 18;
        }
        y += 8;
    }
}

}  // namespace wheelbri::service
