#include "MappingEditView.hpp"
#include "GameWatch.hpp"

#include "Platform.hpp"
#include "ServiceLogging.hpp"
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
#include "wheelbri/GameTuning.hpp"
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
constexpr std::array<float, 7> kPresetsDeg = {180.0F, 270.0F, 360.0F, 540.0F, 720.0F, 900.0F, 1080.0F};
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
std::string controlName(const RigControl& control) { return control.displayName(); }

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
    reloadGames();
    const auto result = loadEmulatorConfig(parsed.document, userGames_);
    targetName_ = result.config.targetProfile.empty() ? std::string("g920") : result.config.targetProfile;
    profile_ = makeProfile(result.config.targetProfile);
    if (profile_ == nullptr) profile_ = makeProfile("g920");
    saved_ = mappingModel(result.config);
    edit_ = saved_;
    baseFfb_ = result.config.ffb;
    baseFfbInvert_ = result.config.ffbInvert;
    baseGameFfb_ = result.config.gameFfb;
    baseCurve_ = {result.config.mapper.throttle.gamma, result.config.mapper.brake.gamma,
                  result.config.mapper.clutch.gamma};
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
        text = std::string("; WheelBri: your own mapping for the ") +
               ConfigPicker::targetName(picker_.target()) +
               ", both modes (the Mapping tab's Save; Default deletes it)\r\n";
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

bool MappingEditView::gameValuesChanged() const {
    return edit_.gameRotations != saved_.gameRotations || edit_.gameSettings != saved_.gameSettings ||
           edit_.gameFfbGains != saved_.gameFfbGains || edit_.gameTunings != saved_.gameTunings;
}

// The Game profiles page has no Save: its values are saved as they change, and only they (the Mapping tab's unsaved
// changes stay unsaved, still there to Save or Revert).
void MappingEditView::saveGameValues() {
    MappingModel pending = edit_;
    edit_ = saved_;
    edit_.gameRotations = pending.gameRotations;
    edit_.gameSettings = pending.gameSettings;
    edit_.gameFfbGains = pending.gameFfbGains;
    edit_.gameTunings = pending.gameTunings;
    save();   // saved_ is then this (or read again from the user's own file, made from the default)
    if (statusError_) {
        // Not saved: the value goes back to the saved one (not tried again every frame), and the page says why.
        pending.gameRotations = saved_.gameRotations;
        pending.gameSettings = saved_.gameSettings;
        pending.gameFfbGains = saved_.gameFfbGains;
        pending.gameTunings = saved_.gameTunings;
    } else {
        status_ = locked_ && path_ == sessionPath_ ? "Saved: in use now." : "Saved. The next Start uses it.";
    }
    edit_ = std::move(pending);
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
        case Slot::Button:
        case Slot::GameButton: break;   // GameButton: pickedSource / setPicked
    }
    return edit_.buttons[index];
}

// --- input -------------------------------------------------------------------------------------------------------

void MappingEditView::setLocked(const bool locked) {
    // Called every frame. A session that starts ends what the Mapping tab was editing; the Game profiles page's values
    // stay editable while it runs (live tuning).
    if (locked && !locked_) {
        sessionPath_ = path_;   // the mapping the session reads its per-game values from
        if (page_ != Page::Games) field_ = Field::None;
        dragging_ = false;
        if (pickKind_ != Slot::GameButton) mode_ = Mode::Idle;
    }
    locked_ = locked;
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
            const std::span<const GameProfile> games = games_;
            if (index >= games.size()) return {};
            return degrees(rotationWhileRunning(games[index], edit_.gameRotations).value_or(edit_.steeringRotationDeg));
        }
        case Field::GameSetting: {
            const std::span<const GameProfile> games = games_;
            const std::size_t g = index / kMaxGameSettings;
            const std::size_t s = index % kMaxGameSettings;
            if (g >= games.size() || s >= games[g].settings.size()) return {};
            const std::optional<float> value = gameSettingValue(games[g], games[g].settings[s], edit_.gameSettings);
            return value ? tuning::formatNumber(*value, 2) : std::string();
        }
        case Field::FfbGain: {
            const std::span<const GameProfile> games = games_;
            return index < games.size() ? tuning::formatNumber(gameFfbGain(games[index], edit_.gameFfbGains), 2)
                                        : std::string();
        }
        case Field::Rotation: return degrees(edit_.steeringRotationDeg);
        case Field::Tuning: {
            const std::size_t picked = selectedGameIndex();
            if (picked >= games_.size()) return {};
            const GameProfile& game = games_[picked];
            const GameFfb ffb = gameFfbOf(game);
            const GameTuning* const own = tuningOf(game.id);
            const auto t = static_cast<TuningField>(index);
            switch (t) {
                case TuningField::ThrottleCurve:
                case TuningField::BrakeCurve:
                case TuningField::ClutchCurve: {
                    const std::size_t p = index;
                    return tuning::formatNumber(own != nullptr && own->pedalCurve[p] ? *own->pedalCurve[p]
                                                                                     : baseCurve_[p],
                                                2);
                }
                case TuningField::Strength: return tuning::formatNumber(ffb.safety.masterGain, 2);
                case TuningField::Cap: return tuning::formatNumber(ffb.safety.maxMagnitude, 2);
                case TuningField::Slew: return tuning::formatNumber(ffb.safety.maxSlewPerSecond, 1);
                case TuningField::Fade: return tuning::formatNumber(ffb.safety.frozenForceSeconds, 1);
                default: {
                    const std::size_t g = index - static_cast<std::size_t>(TuningField::Constant);
                    return g < kFfbGroupCount ? tuning::formatNumber(ffb.gains[g], 2) : std::string();
                }
            }
        }
        case Field::None: break;
    }
    return {};
}

void MappingEditView::startTyping(const Field field, const std::size_t index) {
    if (!editable()) return;
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
        const std::span<const GameProfile> games = games_;
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
        const std::span<const GameProfile> games = games_;
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
        const std::span<const GameProfile> games = games_;
        if (!number || *number < kFfbGainMin || *number > kFfbGainMax) {
            problem = "'" + typed_ + "': expected a number from " + tuning::formatNumber(kFfbGainMin) + " to " +
                      tuning::formatNumber(kFfbGainMax);
        } else if (fieldIndex_ < games.size()) {
            edit_.gameFfbGains[games[fieldIndex_].id] = *number;
        }
    } else if (field_ == Field::Tuning) {
        // One of the picked game's own numbers: checked as the config reader checks it ([game_tuning]).
        const std::size_t picked = selectedGameIndex();
        static constexpr std::array<const char*, 13> kKeys = {
            "throttle_curve", "brake_curve", "clutch_curve", "ffb_strength", "ffb_constant", "ffb_periodic",
            "ffb_spring", "ffb_damper", "ffb_friction", "ffb_inertia", "ffb_cap", "ffb_slew", "ffb_fade_s"};
        if (picked < games_.size() && fieldIndex_ < kKeys.size()) {
            const std::string id = games_[picked].id;
            GameTuning trial = tuningOf(id) != nullptr ? *tuningOf(id) : GameTuning{};
            problem = applyTuningKey(trial, kKeys[fieldIndex_], typed_, MapperConfig::kMaxTargetButtons);
            const auto t = static_cast<TuningField>(fieldIndex_);
            if (problem.empty() && (t == TuningField::Cap || t == TuningField::Slew)) {
                // Lower only: a game never gets past the mapping's limits.
                const float base = t == TuningField::Cap ? baseFfb_.maxMagnitude : baseFfb_.maxSlewPerSecond;
                const float wanted = t == TuningField::Cap ? *trial.ffbCap : *trial.ffbSlew;
                if (wanted > base + 1e-5F) {
                    problem = "'" + typed_ + "': a game's limit can only be lower than the mapping's (" +
                              tuning::formatNumber(base) + ")";
                }
            }
            if (problem.empty()) {
                tuningFor(id) = trial;
                if (t == TuningField::Constant || t == TuningField::Periodic) edit_.gameFfbGains.erase(id);
            } else {
                problem = "'" + typed_ + "': " + problem;
            }
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
            case Field::Tuning:
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
        window_.text(x + 6, y + 4, fit(value, chars), !editable() ? kDim : kText);
    }
    if (editable()) hit(x, y, w, 16, [this, field, index] { startTyping(field, index); });
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
    // Target and Mode left; with Mode Shim, the app draws the shim's game at its right (App: gamesRowX).
    const float revertX = canvasW() - kVersionReserve - 80;
    const float saveX = revertX - 8 - 80;
    const float defaultX = saveX - 8 - 104;
    // Left of Default: "custom" while the user's own saved mapping is in use, nothing with WheelBri's default.
    constexpr std::string_view kCustom = "custom";
    const float customX = defaultX - 10 - static_cast<float>(kCustom.size()) * ui::kCharWidth;
    if (!builtIn_ && page_ != Page::Games) {
        window_.text(customX, 6, std::string(kCustom), kCaution);
        tip(customX - 4, 2, defaultX - customX, 16, "Your saved mapping, not the default.");
    }
    // The Game profiles page has no Default, Save or Revert: its subtitle reaches the version.
    const float titleEnd = page_ == Page::Games ? revertX + 80 : builtIn_ ? defaultX : customX;
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

    // The target (fixed inputs, picked with < >) and how it runs, "custom", Default, Save and Revert: the Mapping tab's
    // alone. The Game profiles page saves its own values as they change (saveGameValues).
    if (page_ == Page::Games) {
        if (!loaded_) {
            window_.text(4, 60, fit(status_, 118 + extraChars()), kWarning);
            return;
        }
        if (editable() && gameValuesChanged()) saveGameValues();
        drawGames();
        popupTips_ = tips_.size();
        if (mode_ == Mode::Pick) drawPicker();
        return;
    }
    picker_.draw(4, 18, !locked_);
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
    // Related buttons together (the profile's groups: FACE, PADDLES, STICKS, SYSTEM; the SHIFTER below both columns).
    const ButtonLayout layout = layoutButtons(*profile_);
    const float gridTop = y + 16;
    for (const ButtonCell& cell : layout.cells) {
        const float x = 4 + static_cast<float>(cell.column) * (478 + half);   // two columns across the canvas
        const float row = gridTop + cell.y;
        if (cell.heading) {
            window_.text(x, row + 3, cell.text, kDim);
            continue;
        }
        const std::size_t index = cell.button;
        char label[32];
        std::snprintf(label, sizeof(label), "%2zu %.10s", index, std::string(profile_->buttonName(index)).c_str());
        window_.text(x + 8, row + 4, label, kText);
        // The first source (a button), "or" a second one (a button, or a one-sided axis past a threshold).
        const float quarter = half / 2;   // each column's fields share its extra width
        drawSource(x + 112, row, 206 + quarter, Slot::Button, index);
        window_.text(x + 322 + quarter, row + 4, "or", kDim);
        drawAltSource(x + 342 + quarter, row, 126 + quarter, index);
    }
    const float gridEnd = gridTop + layout.height - 4;

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
                    std::fabs(edit_.steeringRotationDeg - preset) < 0.5F, !locked_, 40) + 4;   // 7 fit in 960
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
    for (const GameProfile& game : games_) {
        if (game.id == id) {
            if (selectedGame_ != id) field_ = Field::None;
            selectedGame_ = id;
            return;
        }
    }
}

std::size_t MappingEditView::selectedGameIndex() const {
    const std::span<const GameProfile> games = games_;
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
    const std::span<const GameProfile> games = games_;
    float y = 46;
    if (games.empty()) {
        window_.text(4, y, "No game profiles in this build.", kWarning);
        return;
    }
    const std::size_t picked = selectedGameIndex();

    // The list: the built-in profiles, then the games the user added; the one that runs now in green.
    window_.text(4, y, "PROFILES", kHeading);
    y += 16;
    for (std::size_t g = 0; g < games.size(); ++g) {
        if (y > contentH() - 150) break;   // keep the buttons and the hint clear
        const GameProfile& game = games[g];
        const bool on = g == picked;
        const bool runs = game.id == now_.id;
        window_.fill(4, y, kGameListW, 18, on ? kHeading : kPanel);
        window_.text(10, y + 5, fit(game.label, 15), on ? kDark : kText);
        if (runs) {
            window_.fill(4 + kGameListW - 64, y + 3, 58, 12, kActive);
            window_.text(4 + kGameListW - 60, y + 5, "running", kDark);
        } else {
            const std::string tag = userGame(game.id) ? "yours" : "built in";
            window_.text(4 + kGameListW - 6 - static_cast<float>(tag.size()) * ui::kCharWidth, y + 5, tag,
                         on ? kDark : kDim);
        }
        const std::string id = game.id;
        hit(4, y, kGameListW, 18, [this, id] { selectGame(id); });
        y += 22;
    }
    y += 4;
    button(4, y, "Add game...", [this] { addGame(); }, false, !locked_ && platform::canPickFile(), 104);
    const std::string pickedId = games[picked].id;
    button(4 + 112, y, "Remove", [this, pickedId] { removeGame(pickedId); }, false, !locked_ && userGame(pickedId),
           84);
    y += 22;
    const char* const addLines[] = {"Add game: pick its .exe;", "WheelBri finds it by that", "name while it runs.",
                                    "Every value here is its", "own; Default: back to", "the Mapping tab's."};
    const char* const noDialog[] = {"No file dialog here yet:", "add games in games.ini", "next to app.ini.", "",
                                    "Default: back to the", "Mapping tab's."};
    for (const char* line : platform::canPickFile() ? addLines : noDialog) {
        window_.text(4, y, line, kDim);
        y += 12;
    }
    window_.fill(kGameX - 8, 42, 1, contentH() - 100, kPanel);

    // The picked game: its name and whether it runs, its sub-tabs, then the one shown.
    float top = drawGameProfile(picked, 46);
    const GameProfile& game = games[picked];
    if (gameTab_ == GameTab::File && game.settings.empty()) gameTab_ = GameTab::Axes;
    float x = kGameX;
    const auto tab = [&](const GameTab which, const char* label) {
        x += button(x, top + 4, label, [this, which] { gameTab_ = which; }, gameTab_ == which, true) + 6;
    };
    tab(GameTab::Axes, "Axes");
    tab(GameTab::Ffb, "Force feedback");
    tab(GameTab::Buttons, "Buttons");
    if (!game.settings.empty()) tab(GameTab::File, "Game file");
    top += 28;
    switch (gameTab_) {
        case GameTab::Axes:
            top = drawGameMapping(picked, top);
            drawGamePedals(picked, top + 10);
            break;
        case GameTab::Ffb: drawGameForces(picked, top); break;
        case GameTab::Buttons: drawGameButtons(picked, top); break;
        case GameTab::File: drawGameSettings(picked, top); break;
    }

    const float hint = contentH() - 52;
    window_.text(4, hint,
                 locked_ ? "Your values are saved as you change them."
                         : "Your values are saved as you change them and used from the next Start.",
                 kDim);
    if (!locked_) {
        window_.text(4, hint + 12, "While WheelBri runs they can be changed too: they apply at once.", kDim);
    } else if (path_ == sessionPath_) {
        window_.text(4, hint + 12,
                     fit("Running: changes apply at once (pedal axes in Mode Shim, and Add game: from the next Start).",
                         118 + extraChars()),
                     kActive);
    } else {
        window_.text(4, hint + 12,
                     fit("Running with WheelBri's default mapping: your changes are kept in your own mapping and used "
                         "from the next Start.",
                         118 + extraChars()),
                     kCaution);
    }
    window_.text(4, hint + 28, fit(status_.empty() ? std::string("saved") : status_, 118 + extraChars()),
                 statusError_ ? kWarning : status_.empty() ? kDim : kActive);
}

float MappingEditView::drawGameProfile(const std::size_t index, float y) {
    const GameProfile& game = games_[index];
    window_.text(kGameX, y, game.label, kHeading);
    window_.text(kGameX + static_cast<float>(game.label.size() + 2) * ui::kCharWidth, y,
                 fit("program " + game.program, gameChars() - game.label.size() - 2),
                 kDim);
    y += 16;
    // A game that works one way only ([game] ways): a session run the other way tells the user when it starts.
    if (!game.virtualDevice || !game.shim) {
        const std::string only = game.shim ? "Mode Shim only" : "Mode Virtual only";
        for (const std::string& line : wrap(only + ": " + game.waysNote + ".", gameChars(), 2)) {
            window_.text(kGameX, y, line, kCaution);
            y += 12;
        }
    }

    // Now: the game that runs (while a session does), else how it is found.
    if (now_.id == game.id) {
        const std::string line =
            "RUNNING, pid " + std::to_string(now_.pid) + ", for " + clockText(now_.forMs) +
            (now_.profileApplies ? ": its profile and your values are in use" : ": your values are in use");
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
    const GameProfile& game = games_[index];
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
    }, rotation.has_value(), editable(), 48);
    button(kGameValueX + 168, y, "Default", [this, id] { edit_.gameRotations.erase(id); }, false, editable() && own,
           72);
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

float MappingEditView::drawGameSettings(const std::size_t index, float y) {
    const GameProfile& game = games_[index];
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
        }, false, editable() && own.has_value(), 72);
        window_.text(kGameValueX + 248, y + 4,
                     fit("profile " + tuning::formatNumber(*setting.value, 2) + ", 0 to 1", 26), kDim);
        y += 20;
    }
    return y;
}

// --- the picked game's own values (GameTuning.hpp) ----------------------------------------------------------------

namespace {

// The axes a pedal can be read from, in the order < and > go through them (clutch also none).
constexpr std::array<ObjectGuid, 6> kPedalAxes = {ObjectGuid::YAxis,  ObjectGuid::ZAxis,  ObjectGuid::RxAxis,
                                                  ObjectGuid::RyAxis, ObjectGuid::RzAxis, ObjectGuid::Slider};
constexpr std::array<const char*, 3> kPedalNames = {"Throttle", "Brake", "Clutch"};
constexpr std::array<const char*, kFfbGroupCount> kGroupNames = {"Steering force", "Vibration", "Centring spring",
                                                                 "Damper", "Friction", "Inertia"};
constexpr std::array<const char*, kFfbGroupCount> kGroupAbout = {
    "constant force: the road, the tyres", "sine, square, ...: kerbs, engine, rumble", "spring: pulls to the centre",
    "damper: resists fast turning", "friction: resists any turning", "inertia: a heavier wheel"};

std::string onOff(const bool on) { return on ? "On" : "Off"; }

}  // namespace

const GameTuning* MappingEditView::tuningOf(const std::string& id) const {
    const auto found = edit_.gameTunings.find(id);
    return found == edit_.gameTunings.end() ? nullptr : &found->second;
}

GameTuning& MappingEditView::tuningFor(const std::string& id) { return edit_.gameTunings[id]; }

void MappingEditView::dropEmptyTuning(const std::string& id) {
    if (const auto found = edit_.gameTunings.find(id); found != edit_.gameTunings.end() && found->second.empty()) {
        edit_.gameTunings.erase(found);
    }
}

bool MappingEditView::tuningChanged(const std::string& id) const {
    const auto a = edit_.gameTunings.find(id);
    const auto b = saved_.gameTunings.find(id);
    const GameTuning none;
    return !((a == edit_.gameTunings.end() ? none : a->second) == (b == saved_.gameTunings.end() ? none : b->second));
}

GameFfb MappingEditView::gameFfbOf(const GameProfile& game) const {
    const bool fits = profileFitsTarget(game, targetName_);
    std::optional<float> legacy;
    if (const auto found = edit_.gameFfbGains.find(game.id); found != edit_.gameFfbGains.end()) legacy = found->second;
    return resolveGameFfb(baseFfb_, baseFfbInvert_, baseGameFfb_, fits ? &game : nullptr, legacy, tuningOf(game.id));
}

bool MappingEditView::userGame(const std::string& id) const {
    return std::any_of(userGames_.begin(), userGames_.end(), [&id](const GameProfile& g) { return g.id == id; });
}

void MappingEditView::reloadGames() {
    userGames_ = loadUserGames();
    games_.assign(gameProfiles().begin(), gameProfiles().end());
    for (const GameProfile& game : userGames_) {
        const bool builtIn = std::any_of(games_.begin(), games_.end(), [&game](const GameProfile& g) {
            return g.id == game.id || g.program == game.program;
        });
        if (!builtIn) games_.push_back(game);
    }
}

void MappingEditView::addGame() {
    const std::optional<std::filesystem::path> picked =
        platform::pickFile("Add a game: pick its program (.exe)", {}, "");
    if (!picked) return;   // cancelled
    const std::string program = programName(pathToUtf8(picked->filename()));
    if (program.size() < 5 || program.compare(program.size() - 4, 4, ".exe") != 0) {
        status_ = "Not added: pick the game's program, a .exe file.";
        statusError_ = true;
        return;
    }
    for (const GameProfile& game : games_) {
        if (game.program == program) {
            selectGame(game.id);
            status_ = game.label + " is in the list already (" + program + ").";
            statusError_ = false;
            return;
        }
    }
    std::string base = gameIdFor(program);
    if (base.empty()) base = "game";
    std::string id = base;
    const auto taken = [this](const std::string& candidate) {
        return std::any_of(games_.begin(), games_.end(), [&candidate](const GameProfile& g) { return g.id == candidate; });
    };
    for (int n = 2; taken(id); ++n) id = base + std::to_string(n);
    GameProfile game;
    game.id = id;
    std::string label = pathToUtf8(picked->stem());
    if (label.size() > 20) label.resize(20);
    game.label = label;
    game.program = program;
    std::vector<GameProfile> next = userGames_;
    next.push_back(game);
    const std::filesystem::path file = userGamesFile();
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (!writeFileReplacing(file, formatUserGames(next))) {
        status_ = "Not added: cannot write " + pathToUtf8(file) + ".";
        statusError_ = true;
        return;
    }
    WE_LOG_INFO(log(), "games: added {} ({}) to {}", game.label, game.program, pathToUtf8(file));
    reloadGames();
    selectGame(id);
    gameTab_ = GameTab::Axes;
    status_ = game.label + " added: WheelBri finds " + program + " while it runs. Set its own values here.";
    statusError_ = false;
}

void MappingEditView::removeGame(const std::string& id) {
    std::vector<GameProfile> next;
    std::string label;
    for (const GameProfile& game : userGames_) {
        if (game.id == id) {
            label = game.label;
        } else {
            next.push_back(game);
        }
    }
    if (label.empty()) return;   // a built-in profile: never removed
    // Its own values go first (saved as they change), so the mapping never names a game that is gone.
    edit_.gameTunings.erase(id);
    if (gameValuesChanged()) saveGameValues();
    if (statusError_) return;
    if (!writeFileReplacing(userGamesFile(), formatUserGames(next))) {
        status_ = "Not removed: cannot write " + pathToUtf8(userGamesFile()) + ".";
        statusError_ = true;
        return;
    }
    WE_LOG_INFO(log(), "games: removed {} ({})", label, id);
    reloadGames();
    selectedGame_.clear();
    status_ = label + " removed, with its own values.";
    statusError_ = false;
}

SourceRef MappingEditView::pickedSource() const {
    if (pickKind_ == Slot::GameButton) {
        const std::size_t picked = selectedGameIndex();
        if (picked < games_.size()) {
            if (const GameTuning* const own = tuningOf(games_[picked].id)) {
                if (const auto it = own->buttons.find(static_cast<int>(pickIndex_)); it != own->buttons.end()) {
                    return it->second;
                }
            }
        }
        return edit_.buttons[pickIndex_];   // as mapped
    }
    return const_cast<MappingEditView*>(this)->slot(pickKind_, pickIndex_);
}

void MappingEditView::setPicked(const SourceRef& source) {
    if (pickKind_ != Slot::GameButton) {
        slot(pickKind_, pickIndex_) = source;
        return;
    }
    const std::size_t picked = selectedGameIndex();
    if (picked >= games_.size()) return;
    const std::string id = games_[picked].id;
    if (source == edit_.buttons[pickIndex_]) {
        // The mapping's own: no value of the game's needed.
        tuningFor(id).buttons.erase(static_cast<int>(pickIndex_));
        dropEmptyTuning(id);
    } else {
        tuningFor(id).buttons[static_cast<int>(pickIndex_)] = source;
    }
}

float MappingEditView::drawGamePedals(const std::size_t index, float y) {
    const GameProfile& game = games_[index];
    const std::string id = game.id;
    const GameTuning* const own = tuningOf(id);
    const GameTuning* const saved = saved_.gameTunings.count(id) != 0 ? &saved_.gameTunings.at(id) : nullptr;
    const std::string target = profile_ != nullptr ? profile_->identity().shortName : targetName_;
    gameSection(y, "GAME'S PEDALS", "the axis of the " + target + " the game reads each pedal from");
    y += 16;
    const bool fits = profileFitsTarget(game, targetName_);
    const JoystickLayout device = layoutFor(targetName_, 0);
    const JoystickLayout profileLayout = fits ? overlay(device, game) : device;
    const JoystickLayout shown = own != nullptr ? tunedLayout(profileLayout, *own) : profileLayout;
    const std::array<ObjectGuid, 3> axes = {shown.throttle, shown.brake, shown.clutch};
    const std::array<ObjectGuid, 3> profileAxes = {profileLayout.throttle, profileLayout.brake, profileLayout.clutch};
    for (std::size_t p = 0; p < 3; ++p) {
        window_.text(kGameX, y + 4, kPedalNames[p], kText);
        const bool mine = own != nullptr && own->pedalAxes[p].has_value();
        const bool changed = mine != (saved != nullptr && saved->pedalAxes[p].has_value()) ||
                             (mine && saved != nullptr && saved->pedalAxes[p] != own->pedalAxes[p]);
        // < and > go through the axes (the clutch also through none).
        const auto step = [this, id, p, axes](const int by) {
            std::vector<ObjectGuid> choices(kPedalAxes.begin(), kPedalAxes.end());
            if (p == 2) choices.push_back(ObjectGuid::Any);
            const auto at = std::find(choices.begin(), choices.end(), axes[p]);
            const auto i = at == choices.end() ? 0 : static_cast<int>(at - choices.begin());
            const auto n = static_cast<int>(choices.size());
            tuningFor(id).pedalAxes[p] = choices[static_cast<std::size_t>(((i + by) % n + n) % n)];
        };
        button(kGameValueX, y, "<", [step] { step(-1); }, false, editable(), 24);
        window_.fill(kGameValueX + 28, y, 72, 16, kStripe);
        window_.outline(kGameValueX + 28, y, 72, 16, changed ? kCaution : kPanel);
        window_.text(kGameValueX + 34, y + 4, std::string(wheelbri::axisName(axes[p])), mine ? kText : kDim);
        button(kGameValueX + 104, y, ">", [step] { step(1); }, false, editable(), 24);
        button(kGameValueX + 136, y, "Default", [this, id, p] {
            tuningFor(id).pedalAxes[p].reset();
            dropEmptyTuning(id);
        }, false, editable() && mine, 72);
        window_.text(kGameValueX + 216, y + 4,
                     fit(std::string(fits && profileAxes[p] != (p == 0   ? device.throttle
                                                                 : p == 1 ? device.brake
                                                                          : device.clutch)
                                         ? "profile: "
                                         : "default: ") +
                             std::string(wheelbri::axisName(profileAxes[p])),
                         valueChars() - 27),
                     kDim);
        y += 20;
    }
    // In Mode Virtual the virtual wheel has three pedal axes; other axes need Mode Shim.
    if (picker_.virtualDevice() && !routePedals(device, shown)) {
        for (const std::string& line : wrap("Mode Virtual: the virtual " + target + " has only y, rz and slider "
                                            "for pedals, each once; these are used in Mode Shim only.",
                                            gameChars(), 2)) {
            window_.text(kGameX, y, line, kCaution);
            y += 12;
        }
    }
    y += 8;

    gameSection(y, "YOUR PEDALS", "this game only; the rest as in the Mapping tab");
    y += 16;
    for (std::size_t p = 0; p < 3; ++p) {
        window_.text(kGameX, y + 4, kPedalNames[p], kText);
        const bool ownInvert = own != nullptr && own->pedalInvert[p].has_value();
        const bool invert = ownInvert ? *own->pedalInvert[p] : edit_.invert[p + 1];
        button(kGameValueX, y, invert ? "Inverted" : "Normal", [this, id, p, invert] {
            tuningFor(id).pedalInvert[p] = !invert;
        }, ownInvert, editable(), 88);
        const bool ownCurve = own != nullptr && own->pedalCurve[p].has_value();
        const bool curveChanged = ownCurve != (saved != nullptr && saved->pedalCurve[p].has_value()) ||
                                  (ownCurve && saved != nullptr && saved->pedalCurve[p] != own->pedalCurve[p]);
        window_.text(kGameValueX + 100, y + 4, "curve", kDim);
        drawField(kGameValueX + 148, y, 56, Field::Tuning,
                  tuning::formatNumber(ownCurve ? *own->pedalCurve[p] : baseCurve_[p], 2), curveChanged, p);
        button(kGameValueX + 212, y, "Default", [this, id, p] {
            tuningFor(id).pedalInvert[p].reset();
            tuningFor(id).pedalCurve[p].reset();
            dropEmptyTuning(id);
        }, false, editable() && (ownInvert || ownCurve), 72);
        y += 20;
    }
    window_.text(kGameValueX, y, fit("curve: 1 as it is, below 1 quicker at first, above 1 slower (0.1 to 10)",
                                     valueChars()),
                 kDim);
    return y + 12;
}

float MappingEditView::drawGameForces(const std::size_t index, float y) {
    const GameProfile& game = games_[index];
    const std::string id = game.id;
    const GameTuning* const own = tuningOf(id);
    const GameFfb ffb = gameFfbOf(game);
    gameSection(y, "FORCE FEEDBACK",
                now_.id == id && now_.ffbLive
                    ? "running: blue what the game asks, green what reaches the wheel; changes apply at once"
                    : "times what the game asks; the cap and the slew limit always apply");
    y += 16;
    const auto row = [&](const std::string& label, const TuningField field, const std::string& value,
                         const bool mine, const std::string& about, std::function<void()> reset) {
        window_.text(kGameX, y + 4, label, kText);
        const bool changed = tuningChanged(id) && mine;
        drawField(kGameValueX, y, 72, Field::Tuning, value, changed, static_cast<std::size_t>(field));
        button(kGameValueX + 80, y, "Default", std::move(reset), false, editable() && mine, 72);
        window_.text(kGameValueX + 160, y + 4, fit(about, valueChars() - 20), kDim);
        y += 20;
    };
    row("Overall strength", TuningField::Strength, tuning::formatNumber(ffb.safety.masterGain, 2),
        own != nullptr && own->ffbStrength.has_value(),
        "0 to 1; the Mapping tab's: " + tuning::formatNumber(baseFfb_.masterGain, 2), [this, id] {
            tuningFor(id).ffbStrength.reset();
            dropEmptyTuning(id);
        });
    y += 4;
    for (std::size_t g = 0; g < kFfbGroupCount; ++g) {
        const bool mine = own != nullptr && own->ffbGains[g].has_value();
        std::string about = std::string(kGroupAbout[g]);
        if (!mine && std::fabs(ffb.gains[g] - 1.0F) > 1e-4F) about = "the profile's; " + about;
        // While the game runs: what it asks (blue) and what reaches the wheel (green), instead of the text.
        if (now_.id == id && now_.ffbLive) {
            constexpr float kBarW = 72;
            const float barX = kGameValueX + 160;
            window_.fill(barX, y + 4, kBarW, 8, kStripe);
            window_.fill(barX, y + 4, kBarW * now_.asked[g], 8, kHeading);
            window_.fill(barX + kBarW + 6, y + 4, kBarW, 8, kStripe);
            window_.fill(barX + kBarW + 6, y + 4, kBarW * now_.sent[g], 8, kActive);
            const auto percent = [](const float v) {
                return std::to_string(static_cast<int>(v * 100.0F + 0.5F)) + "%";
            };
            about = now_.asked[g] > 0.0F || now_.sent[g] > 0.0F
                        ? "                    " + percent(now_.asked[g]) + " -> " + percent(now_.sent[g])
                        : "                    not used now";
        }
        row(kGroupNames[g], static_cast<TuningField>(static_cast<std::size_t>(TuningField::Constant) + g),
            tuning::formatNumber(ffb.gains[g], 2), mine, about, [this, id, g] {
                tuningFor(id).ffbGains[g].reset();
                if (g <= static_cast<std::size_t>(FfbGroup::Periodic)) edit_.gameFfbGains.erase(id);
                dropEmptyTuning(id);
            });
    }
    window_.text(kGameValueX, y, fit("0 turns that kind off, 1 as the game sends it, up to 4", valueChars()), kDim);
    y += 18;
    row("Cap", TuningField::Cap, tuning::formatNumber(ffb.safety.maxMagnitude, 2),
        own != nullptr && own->ffbCap.has_value(),
        "lower only: at most the Mapping tab's " + tuning::formatNumber(baseFfb_.maxMagnitude, 2), [this, id] {
            tuningFor(id).ffbCap.reset();
            dropEmptyTuning(id);
        });
    row("Slew limit", TuningField::Slew, tuning::formatNumber(ffb.safety.maxSlewPerSecond, 1),
        own != nullptr && own->ffbSlew.has_value(),
        "per second, lower only: at most " + tuning::formatNumber(baseFfb_.maxSlewPerSecond, 1), [this, id] {
            tuningFor(id).ffbSlew.reset();
            dropEmptyTuning(id);
        });
    row("Fade a frozen push", TuningField::Fade, tuning::formatNumber(ffb.safety.frozenForceSeconds, 1),
        own != nullptr && own->ffbFadeSeconds.has_value(), "seconds a push held unchanged plays; 0 = never",
        [this, id] {
            tuningFor(id).ffbFadeSeconds.reset();
            dropEmptyTuning(id);
        });
    y += 4;
    const auto toggle = [&](const std::string& label, const bool value, const bool mine,
                            std::function<void()> flip, std::function<void()> reset, const std::string& about) {
        window_.text(kGameX, y + 4, label, kText);
        button(kGameValueX, y, onOff(value), std::move(flip), mine, editable(), 72);
        button(kGameValueX + 80, y, "Default", std::move(reset), false, editable() && mine, 72);
        window_.text(kGameValueX + 160, y + 4, fit(about, valueChars() - 20), kDim);
        y += 20;
    };
    toggle("Turn the other way", ffb.invert != baseFfbInvert_, own != nullptr && own->ffbInvert.has_value(),
           [this, id, ffb] { tuningFor(id).ffbInvert = !ffb.invert; },
           [this, id] {
               tuningFor(id).ffbInvert.reset();
               dropEmptyTuning(id);
           },
           "when this game's forces pull the wrong way");
    toggle("Game effects", ffb.effects, own != nullptr && own->ffbEffects.has_value(),
           [this, id, ffb] { tuningFor(id).ffbEffects = !ffb.effects; },
           [this, id] {
               tuningFor(id).ffbEffects.reset();
               dropEmptyTuning(id);
           },
           "Off: no force feedback from this game at all");
    return y;
}

float MappingEditView::drawGameButtons(const std::size_t index, float y) {
    const GameProfile& game = games_[index];
    const std::string id = game.id;
    const GameTuning* const own = tuningOf(id);
    const std::string target = profile_ != nullptr ? profile_->identity().shortName : targetName_;
    gameSection(y, "BUTTONS", target + " button <- your control, this game only (dim: as in the Mapping tab)");
    y += 16;
    if (profile_ == nullptr) return y;
    const ButtonLayout layout = layoutButtons(*profile_);
    const float columnW = (canvasW() - kGameX - 4) / 2;
    for (const ButtonCell& cell : layout.cells) {
        const float x = kGameX + static_cast<float>(cell.column) * columnW;
        const float row = y + cell.y;
        if (row > contentH() - 52 - 20) break;
        if (cell.heading) {
            window_.text(x, row + 3, cell.text, kDim);
            continue;
        }
        const std::size_t b = cell.button;
        char label[32];
        std::snprintf(label, sizeof(label), "%2zu %.10s", b, std::string(profile_->buttonName(b)).c_str());
        window_.text(x + 4, row + 4, label, kText);
        std::optional<SourceRef> ownSource;
        if (own != nullptr) {
            if (const auto it = own->buttons.find(static_cast<int>(b)); it != own->buttons.end()) ownSource = it->second;
        }
        const bool mine = ownSource.has_value();
        const SourceRef source = ownSource.value_or(edit_.buttons[b]);
        const float fieldX = x + 120;
        const float fieldW = columnW - 120 - 84;
        window_.fill(fieldX, row, fieldW, 16, kStripe);
        window_.outline(fieldX, row, fieldW, 16, mine && tuningChanged(id) ? kCaution : kPanel);
        window_.text(fieldX + 6, row + 4, fit(describe(source, false), static_cast<std::size_t>((fieldW - 12) / 8)),
                     mine ? kText : kDim);
        if (source.mapped()) tip(fieldX, row, fieldW, 16, describeFull(source, false));
        if (editable()) {
            hit(fieldX, row, fieldW, 16, [this, b] {
                mode_ = Mode::Pick;
                pickKind_ = Slot::GameButton;
                pickIndex_ = b;
            });
        }
        button(fieldX + fieldW + 4, row, "Default", [this, id, b] {
            tuningFor(id).buttons.erase(static_cast<int>(b));
            dropEmptyTuning(id);
        }, false, editable() && mine, 72);
    }
    return y + layout.height;
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
    const GameProfile& game = games_[index];
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
    const GameProfile& game = games_[index];
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
    const bool forGame = pickKind_ == Slot::GameButton;
    const std::string what = pickKind_ == Slot::Axis ? kAxisNames[pickIndex_]
                             : pickKind_ == Slot::Hat ? std::string("D-pad ") + kHatNames[pickIndex_]
                                                      : "button " + std::to_string(pickIndex_) + " " +
                                                            std::string(profile_->buttonName(pickIndex_)) +
                                                            (forGame ? " in " + games_[selectedGameIndex()].label
                                                                     : std::string());
    const float left = kPopupX + 12;
    window_.text(left, kPopupY + 10, "What drives " + what + "?", kHeading);
    const SourceRef current = pickedSource();
    // What is being picked, as usesOf names it: a control used anywhere else is greyed (its use in a tooltip).
    const ControlUse self{pickKind_ == Slot::Axis  ? ControlUse::Kind::Axis
                          : pickKind_ == Slot::Hat ? ControlUse::Kind::Hat
                                                   : ControlUse::Kind::Button,
                          pickIndex_};
    button(kPopupX + popupW() - 100, kPopupY + 6, "Cancel", [this] { mode_ = Mode::Idle; }, false, true, 88);
    window_.text(left + 140, kPopupY + 32,
                 forGame ? "For this game only: any control, also one the Mapping tab uses elsewhere."
                         : "Greyed: used elsewhere (hover to see where); clear it there first.",
                 kDim);
    button(left, kPopupY + 28, "(nothing)", [this] {
        setPicked({});
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
            // For one game a control may be used again (it swaps for that game): nothing greyed then.
            EntryState state = forGame ? EntryState{} : entryState({source, axis}, self);
            state.mine = forGame ? source == current : state.mine || source == current;
            const std::string text = std::to_string(index) + " " +
                                     controlName(*control);
            const bool invert = control->invert;
            drawEntry(x, y, kEntryW, text, state, [this, source, invert] {
                setPicked(source);
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
