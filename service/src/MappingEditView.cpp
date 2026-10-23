#include "MappingEditView.hpp"
#include "UiStyle.hpp"
#include "GameWatch.hpp"
#include "HoverText.hpp"

#include "Platform.hpp"
#include "ServiceLogging.hpp"
#include "Version.hpp"

#include "wheelbri/AutoMap.hpp"
#include "wheelbri/AxisTuning.hpp"
#include "wheelbri/CentringSpring.hpp"
#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/DeviceMatch.hpp"
#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/FfbLimits.hpp"
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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <utility>

namespace wheelbri::service {

namespace {

// "G923" from "g923": a target's config name as the window shows it.
std::string upperName(std::string name) {
    for (char& c : name) c = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    return name;
}

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.mapping");
    return logger;
}

using Colour = ui::Colour;

// The same colours as the rest of the window (App.cpp, WheelSetupView.cpp).
constexpr Colour kPanel = style::kPanel;
constexpr Colour kStripe = style::kField;
constexpr Colour kText = style::kText;
constexpr Colour kDim = style::kDim;
constexpr Colour kHeading = style::kHeading;
constexpr Colour kActive = style::kActive;
constexpr Colour kWarning = style::kWarning;
constexpr Colour kCaution = style::kCaution;
constexpr Colour kDark = style::kDark;

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
// The centring spring's usual values (percent; 0 = off); any whole number from 0 to 100 can be typed.
constexpr std::array<float, 6> kSpringPresets = {0.0F, 10.0F, 25.0F, 50.0F, 75.0F, 100.0F};

std::string percent(const float value) { return tuning::formatNumber(value) + "%"; }
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

std::optional<std::filesystem::path> MappingEditView::pickFile(const std::string& title,
                                                              const std::filesystem::path& folder,
                                                              const std::string& fileName) const {
    return system_.pickFile ? system_.pickFile(title, folder, fileName) : platform::pickFile(title, folder, fileName);
}

bool MappingEditView::canPickFile() const { return system_.canPickFile ? system_.canPickFile() : platform::canPickFile(); }

std::filesystem::path MappingEditView::documentsFolder() const {
    return system_.documentsFolder ? system_.documentsFolder() : platform::documentsFolder();
}

std::optional<std::vector<RunningProgram>> MappingEditView::runningPrograms() const {
    return system_.runningPrograms ? system_.runningPrograms() : platform::runningPrograms();
}

std::uint64_t MappingEditView::nowMs() const { return system_.nowMs ? system_.nowMs() : platform::nowMs(); }

// --- state ----------------------------------------------------------------------------------------------------

void MappingEditView::update() {
    if (picker_.changes() != seenChanges_ || picker_.selected() != path_) {
        seenChanges_ = picker_.changes();
        load();
        return;
    }
    if (devicesReadMs_ == 0 || nowMs() - devicesReadMs_ >= kDevicesEveryMs) {
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
    devicesReadMs_ = nowMs();
    // What the Rig maps by itself follows it: another "It is" shows at once (it is never a line of the file).
    if (loaded_ && profile_ != nullptr) {
        rigNotices_ = followRig(saved_, rig_, *profile_);
        (void)followRig(edit_, rig_, *profile_);
    }
}

void MappingEditView::load() {
    // Another Target: its own mapping, its own undo list.
    if (const int target = static_cast<int>(picker_.target()); target != undoTarget_) {
        undo_.clear();
        undoTarget_ = target;
    }
    notSaved_.reset();
    undoing_ = false;
    mode_ = Mode::Idle;
    field_ = Field::None;
    dragging_ = false;
    thresholdDragging_ = false;
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
    // WheelBri's older force limits (0.70 / 0.80) in the user's own files: now 1 / 1, the wheel's app sets how strong.
    const int movedLimits = moveOldLimits();
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
    rigNotices_ = followRig(saved_, rig_, *profile_);
    edit_ = saved_;
    baseFfb_ = result.config.ffb;
    baseFfbInvert_ = result.config.ffbInvert;
    baseGameFfb_ = result.config.gameFfb;
    loaded_ = true;
    if (!parsed.errors.empty() || !result.errors.empty()) {
        status_ = "The config has problems (Start lists them): " +
                  (parsed.errors.empty() ? result.errors.front() : parsed.errors.front());
        statusError_ = true;
    } else {
        status_ = !builtIn_ ? "Your own mapping (saved). Default goes back to WheelBri's."
                            : "WheelBri's default mapping. Change a source: it is saved as your own.";
        statusError_ = false;
    }
    if (!statusError_ && !rigNotices_.empty()) {
        status_ += " From the Rig: " + rigNotices_.front() +
                   (rigNotices_.size() > 1 ? " (+" + std::to_string(rigNotices_.size() - 1) + " more)" : "") + ".";
    }
    if (movedLimits > 0 && !statusError_) {
        status_ = "Force feedback: strength and cap are now 1.00 (were 0.70 / 0.80) in " +
                  std::to_string(movedLimits) + (movedLimits == 1 ? " mapping" : " mappings") +
                  ". Set how strong in the wheel's own app (Pit House); lower it there before the next race.";
    }
    if (movedLimits > 0) toast_ = "Force limits now 1.00 / 1.00";
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
               ", both modes (written by the Mapping tab; Default deletes it)\r\n";
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
    toast_ = page_ == Page::Games ? std::string() : "Mapping saved";   // the Games page says its own
}

void MappingEditView::autoSave() {
    if (!loaded_ || locked_ || !dirty()) return;
    if (field_ != Field::None || dragging_ || thresholdDragging_ || mode_ != Mode::Idle) return;   // not done yet
    if (notSaved_ && *notSaved_ == edit_) return;                           // said why already: not again
    const MappingModel before = saved_;
    const bool undoing = std::exchange(undoing_, false);
    save();
    if (statusError_ || dirty()) {   // not saved (or read back differently): not tried again until the next change
        notSaved_ = edit_;
        return;
    }
    notSaved_.reset();
    toast_.clear();   // every change saves: no message for each
    status_ = undoing ? "Undone and saved." : "Saved. Start uses it.";
    if (!undoing) {
        undo_.push_back(before);
        if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    }
}

void MappingEditView::undo() {
    if (!loaded_ || locked_) return;
    field_ = Field::None;
    if (dirty()) {   // a change that could not be saved: dropped
        edit_ = saved_;
        notSaved_.reset();
        status_ = "Change dropped.";
        statusError_ = false;
        return;
    }
    if (undo_.empty()) return;
    edit_ = undo_.back();
    undo_.pop_back();
    undoing_ = true;   // autoSave writes it, without putting it on the list again
    WE_LOG_INFO(log(), "mapping: undo ({} more)", undo_.size());
}

bool MappingEditView::gameValuesChanged() const {
    return edit_.gameRotations != saved_.gameRotations || edit_.gameSettings != saved_.gameSettings ||
           edit_.gameFfbGains != saved_.gameFfbGains || edit_.gameTunings != saved_.gameTunings;
}

// The Games page has no Save: its values are saved as they change, and only they (the Mapping tab's unsaved
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
        toast_ = locked_ && path_ == sessionPath_ ? "Saved: in use now" : "Saved";
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
    toast_ = "Back to WheelBri's mapping";
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

int MappingEditView::gameNumber(const std::size_t b) const {
    return gameButtonNumber(*profile_, edit_.buttonOrder, b);
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

bool MappingEditView::followsRig(const Slot kind, const std::size_t index) const {
    if (!edit_.fromRig) return false;
    const AutoInputs& autos = edit_.autoInputs;
    switch (kind) {
        case Slot::Axis: return index < autos.axes.size() && autos.axes[index];
        case Slot::Hat: return index < autos.hat.size() && autos.hat[index];
        case Slot::Button: return index < autos.buttons.size() && autos.buttons[index];
        case Slot::GameButton: break;
    }
    return false;
}

// --- input -------------------------------------------------------------------------------------------------------

void MappingEditView::setLocked(const bool locked) {
    // Called every frame. A session that starts ends what the Mapping tab was editing; the Games page's values
    // stay editable while it runs (live tuning).
    if (locked && !locked_) {
        sessionPath_ = path_;   // the mapping the session reads its per-game values from
        if (page_ != Page::Games) field_ = Field::None;
        dragging_ = false;
        thresholdDragging_ = false;
        if (pickKind_ != Slot::GameButton && mode_ != Mode::AddButton) mode_ = Mode::Idle;
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
    // The Target and Mode row at the top: the Mapping page's only (the Games page shows them, never changes
    // them), and never while a session runs.
    if (mode_ == Mode::Idle && page_ == Page::Mapping && !locked_ && picker_.onClick(x, y)) return;
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
    if (listDragging_) {   // the Games list's bar: the list follows it, and keeps its width once it is let go
        if (leftDown && page_ == Page::Games) {
            const float width = std::clamp(x - listGrab_, kGameListMinW, maxGameListW());
            if (width != gameListW_) gripPressMs_ = 0;   // moved: a drag, not the first press of a double-click
            gameListW_ = width;
            return;
        }
        listDragging_ = false;
        keepListWidth();
        return;
    }
    if (thresholdDragging_) {
        if (!leftDown || locked_ || mode_ != Mode::PickAlt) {
            thresholdDragging_ = false;
            return;
        }
        setThresholdFromX(x);
        return;
    }
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

ButtonRef* MappingEditView::axisSecondary(const std::size_t index) {
    if (pickIndex_ >= edit_.secondaries.size()) return nullptr;
    std::vector<ButtonRef>& list = edit_.secondaries[pickIndex_];
    return index < list.size() && list[index].axis ? &list[index] : nullptr;
}

void MappingEditView::setThresholdFromX(const float x) {
    ButtonRef* const focused = axisSecondary(altFocus_);
    if (focused == nullptr) return;
    const float t = std::clamp((x - thresholdSliderX_) / thresholdSliderW_, 0.0F, 1.0F);
    focused->threshold = std::round(t * 100.0F) / 100.0F;
}

std::string MappingEditView::fieldText(const Field field, const std::size_t index) const {
    switch (field) {
        case Field::PerGame:
        case Field::GameSteering: {
            // The rotation the game is played with: its own, else the Mapping tab's (one value, two places).
            const std::span<const GameProfile> games = games_;
            if (index >= games.size()) return {};
            return degrees(rotationWhileRunning(games[index], edit_.gameRotations,
                                                profileFitsTarget(games[index], targetName_))
                               .value_or(edit_.steeringRotationDeg));
        }
        case Field::GameSetting: {
            const std::span<const GameProfile> games = games_;
            const std::size_t g = index / kMaxGameSettings;
            const std::size_t s = index % kMaxGameSettings;
            if (g >= games.size() || s >= games[g].settings.size()) return {};
            const std::optional<float> value = gameSettingValue(games[g], games[g].settings[s], edit_.gameSettings);
            return value ? tuning::formatNumber(*value, 2) : std::string();
        }
        case Field::Rotation: return degrees(edit_.steeringRotationDeg);
        case Field::CentringSpring: return tuning::formatNumber(edit_.centringSpringPercent);
        case Field::Threshold: {
            const ButtonRef* const focused = const_cast<MappingEditView*>(this)->axisSecondary(index);
            return focused != nullptr ? tuning::formatNumber(focused->threshold * 100.0F) : std::string();
        }
        case Field::Tuning: {
            const std::size_t picked = selectedGameIndex();
            if (picked >= games_.size()) return {};
            const GameProfile& game = games_[picked];
            const GameFfb ffb = gameFfbOf(game);
            const auto t = static_cast<TuningField>(index);
            switch (t) {
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
    } else if (field_ == Field::Tuning) {
        // One of the picked game's own numbers: checked as the config reader checks it ([game_tuning]).
        const std::size_t picked = selectedGameIndex();
        static constexpr std::array<const char*, 10> kKeys = {
            "ffb_strength", "ffb_constant", "ffb_periodic", "ffb_spring", "ffb_damper",
            "ffb_friction", "ffb_inertia",  "ffb_cap",      "ffb_slew",   "ffb_fade_s"};
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
            case Field::CentringSpring:
                if (value < 0.0F || value > kCentringSpringMaxPercent) {
                    problem = "the centring spring goes from 0 (off) to " +
                              tuning::formatNumber(kCentringSpringMaxPercent) + "%";
                } else {
                    edit_.centringSpringPercent = std::round(value);   // whole percent, as Logitech's slider
                }
                break;
            case Field::Threshold:
                // The whole travel, in whole percent (as the slider sets it).
                if (!(value >= kMinButtonThreshold * 100.0F && value <= kMaxButtonThreshold * 100.0F)) {
                    problem = "an axis presses a button from 0% (as soon as it moves) to 100% (at its end)";
                } else if (ButtonRef* const focused = axisSecondary(fieldIndex_)) {
                    focused->threshold = std::round(value) / 100.0F;
                }
                break;
            case Field::PerGame:
            case Field::GameSteering:
            case Field::GameSetting:
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
    style::button(window_, x, y, w, 16, label, on, enabled);
    if (enabled) hit(x, y, w, 16, std::move(action));
    return w;
}

void MappingEditView::drawField(const float x, const float y, const float w, const Field field,
                                const std::string& value, const bool changed, const std::size_t index) {
    const bool typing = field_ == field && fieldIndex_ == index;
    window_.fill(x, y, w, 16, kStripe);
    window_.outline(x, y, w, 16, typing ? kHeading : changed ? kCaution : style::kFieldEdge);
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
    window_.outline(x, y, w, 16, state.twice ? kWarning : changed ? kCaution : style::kFieldEdge);
    // From the Rig (what the control "is"): RIG at the right; picking a source makes it the user's.
    const bool rigs = source.mapped() && followsRig(kind, index);
    if (state.twice) {
        tip(x, y, w, 16, state.tip);
    } else if (source.mapped()) {
        tip(x, y, w, 16, describeFull(source, kind == Slot::Axis) + (rigs ? ". From the Rig: what it is there" : ""));
    }
    constexpr std::string_view kRigTag = "RIG";
    const float tagW = rigs ? static_cast<float>(kRigTag.size() + 1) * ui::kCharWidth : 0.0F;
    if (rigs) window_.text(x + w - 6 - tagW + ui::kCharWidth, y + 4, std::string(kRigTag), kHeading);
    const auto chars = static_cast<std::size_t>(std::max(0.0F, w - 12 - tagW) / ui::kCharWidth);
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
    if (page_ == Page::Mapping) autoSave();
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
    // The Games page has no Default, Save or Revert: its subtitle reaches the version.
    const float titleEnd = page_ == Page::Games ? revertX + 80 : builtIn_ ? defaultX : customX;
    const auto subtitle = [&](const float x, const std::string& text) {
        window_.text(x, 4, fit(text, static_cast<std::size_t>(std::max(0.0F, (titleEnd - 12 - x) / ui::kCharWidth))),
                     kDim);
    };
    if (page_ == Page::Games) {
        window_.text(4, 4, "GAMES", kHeading);
        subtitle(52, "every game: your own values for it and, for Mode Shim, its folder and the shim there");
    } else {
        window_.text(4, 4, "MAPPING", kHeading);
        // EDIT: this page changes the mapping (while a session runs the tab shows the LIVE view instead).
        window_.fill(66, 2, 40, 12, kPanel);
        window_.text(70, 4, "EDIT", kText);
        subtitle(114, "what your controls do on the " + target);
    }

    // The target (fixed inputs, picked with < >) and how it runs, "custom", Default, Save and Revert: the Mapping tab's
    // alone. The Games page saves its own values as they change (saveGameValues).
    if (page_ == Page::Games) {
        // Which Target and Mode the values below are for (each target keeps its own): shown here, changed on the
        // Mapping tab only, and never from Start to Stop.
        const std::string target = ConfigPicker::targetName(picker_.target());
        const std::string way = ConfigPicker::wayName(picker_.way());
        window_.text(4, 22, "Target", kDim);
        window_.text(4 + 7 * ui::kCharWidth, 22, target, kHeading);
        const float modeX = 4 + static_cast<float>(8 + target.size()) * ui::kCharWidth + 16;
        window_.text(modeX, 22, "Mode", kDim);
        window_.text(modeX + 5 * ui::kCharWidth, 22, way, kHeading);
        const float noteX = modeX + static_cast<float>(6 + way.size()) * ui::kCharWidth + 16;
        // Hot swap (Mode Shim): the shims in the games' folders follow the Target as soon as it changes.
        const float swapW = 132;
        const float swapX = canvasW() - 4 - swapW;
        // What Stop does with the shims once the user said "Don't ask again" (App: the question at Stop), and the way
        // back to the question; left of Hot swap.
        float noteEnd = swapX;
        if (shims_ != nullptr && shims_->atStop() != ShimGamesView::AtStop::Ask) {
            const float askW = 96;
            const float askX = swapX - 8 - askW;
            const std::string said = shims_->atStop() == ShimGamesView::AtStop::Keep ? "At Stop: the shims stay"
                                                                                     : "At Stop: shims deleted";
            const float saidX = askX - 8 - static_cast<float>(said.size()) * ui::kCharWidth;
            window_.text(saidX, 22, said, kDim);
            button(askX, 18, "Ask again", [this] { shims_->setAtStop(ShimGamesView::AtStop::Ask); }, false, true, askW);
            tip(askX, 18, askW, 16, "Stop asks again what to do with the shims in the games' folders.");
            noteEnd = saidX;
        }
        window_.text(noteX, 22,
                     fit(locked_ ? "fixed while running" : "change them on the Mapping tab (M)",
                         static_cast<std::size_t>(std::max(0.0F, noteEnd - noteX - 12) / ui::kCharWidth)),
                     locked_ ? kActive : kDim);
        if (shims_ != nullptr) {
            const bool on = shims_->hotSwap();
            style::checkbox(window_, swapX, 18, swapW, "Hot swap", on, !locked_);
            if (!locked_) hit(swapX, 18, swapW, 16, [this, on] { shims_->setHotSwap(!on); });
            tip(swapX, 18, swapW, 16,
                "On: when the Target changes (Mode Shim), every game folder with the shim in it gets the new Target at "
                "once. Off: they get it at Start.");
        }
        if (!loaded_) {
            window_.text(4, 60, fit(status_, 118 + extraChars()), kWarning);
            window_.text(4, 80, "Pick another Target on the Mapping tab (M).", kDim);
            return;
        }
        if (editable() && gameValuesChanged()) saveGameValues();
        drawGames();
        popupTips_ = tips_.size();
        if (mode_ == Mode::Pick) drawPicker();
        if (mode_ == Mode::AddButton) drawAddButton();
        return;
    }
    picker_.draw(4, 18, !locked_);
    // Saved as you change it: where Save was, whether it is ("saved", or "not saved" in red); Undo instead of Revert.
    button(defaultX, 2, "Default", [this] { dropOwnEdits(); }, false, !builtIn_ && !locked_, 104);
    const bool unsaved = loaded_ && dirty();
    const std::string saveState = unsaved ? "not saved" : "saved";
    window_.text(saveX + 80 - static_cast<float>(saveState.size()) * ui::kCharWidth, 6, saveState,
                 unsaved ? kWarning : kDim);
    tip(saveX, 2, 80, 16,
        unsaved ? "This change could not be saved: the line at the bottom says why. Undo drops it."
                : "Every change is saved at once; Start uses what you see. Accessories it uses must be plugged in to "
                  "start.");
    const bool canUndo = loaded_ && !locked_ && (unsaved || !undo_.empty());
    button(revertX, 2, "Undo", [this] { undo(); }, false, canUndo, 80);
    tip(revertX, 2, 80, 16, "Goes back one change, as many times as you like (until the Target changes).");
    tip(defaultX, 2, 104, 16, "Deletes your own mapping for this Target: WheelBri's default mapping again.");
    if (!loaded_) {
        window_.text(4, 60, fit(status_, 118 + extraChars()), kWarning);
        // A way out: another target with < > above, or straight back to the G920 (the one every build has).
        window_.text(4, 80, "Pick another Target with < > above, or:", kDim);
        if (picker_.target() != ConfigPicker::Target::G920) {
            button(4 + 42 * ui::kCharWidth, 76, "Back to the G920",
                   [this] { picker_.useTarget(ConfigPicker::Target::G920); }, false, !locked_, 152);
        }
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
    style::headingBand(window_, 0, y, canvasW());
    window_.text(8, y, "AXES", kHeading);
    window_.text(48, y, "click a source to pick another", kDim);
    tip(4, y - 2, 290, 12, "Sources are the rig's controls, named in the Rig tab: click one to pick what drives it.");
    // The axes' and the D-pad's source fields share what a wider window adds.
    const float half = extraW() / 2;
    window_.text(620 + half, y, "D-PAD", kHeading);
    for (std::size_t i = 0; i < kAxisNames.size(); ++i) {
        const float row = y + 16 + static_cast<float>(i) * 20;
        window_.text(4, row + 4, kAxisNames[i], kText);
        drawSource(80, row, 420 + half, Slot::Axis, i);
        button(508 + half, row, "Invert", [this, i] { edit_.invert[i] = !edit_.invert[i]; }, edit_.invert[i], true, 70);
        tip(508 + half, row, 70, 16, "The axis the other way round: for a pedal that reads pressed at rest.");
        window_.text(620 + half, row + 4, kHatNames[i], kText);
        drawSource(668 + half, row, 288 + half, Slot::Hat, i);
    }

    // The target's buttons (the ones its profile names; the others are not seen by games).
    y += 16 + 4 * 20 + 12;
    style::headingBand(window_, 0, y, canvasW());
    window_.text(8, y, "BUTTONS", kHeading);
    window_.text(76, y, target + " button <- your control", kDim);
    tip(4, y - 2, 340, 12,
        "Each " + target + " button and the control that presses it; \"or\": a second one (a button, or an axis past a "
        "point, such as a handbrake lever).");
    // Related buttons together (the profile's groups: FACE, PADDLES, STICKS, SYSTEM; the SHIFTER below both columns).
    // A group with nothing mapped is one line with Show (a G920 without a shifter: no Gear rows).
    const ButtonLayout layout = layoutButtons(*profile_, [this](const ButtonGroup& group) {
        if (std::find(shownGroups_.begin(), shownGroups_.end(), group.name) != shownGroups_.end()) return false;
        return std::none_of(group.buttons.begin(), group.buttons.end(), [this](const std::size_t b) {
            return b < edit_.buttons.size() && (edit_.buttons[b].mapped() || !edit_.secondaries[b].empty());
        });
    });
    const std::vector<ButtonGroup> groups = profile_->buttonGroups();
    const float gridTop = y + 16;
    for (const ButtonCell& cell : layout.cells) {
        const float x = 4 + static_cast<float>(cell.column) * (478 + half);   // two columns across the canvas
        const float row = gridTop + cell.y;
        if (cell.folded) {
            const bool below = std::any_of(groups.begin(), groups.end(), [&cell](const ButtonGroup& group) {
                return group.name == cell.text && group.place == ButtonGroup::Place::Below;
            });
            const float w = below ? canvasW() - 8 : 470 + half;
            window_.outline(x, row, w, 18, kPanel);
            window_.text(x + 6, row + 5, cell.text, kDim);
            window_.text(x + 6 + static_cast<float>(cell.text.size() + 2) * ui::kCharWidth, row + 5, "nothing mapped",
                         kDim);
            const std::string name = cell.text;
            button(x + w - 62, row + 1, "Show", [this, name] { shownGroups_.push_back(name); }, false, true, 58);
            continue;
        }
        if (cell.heading) {
            window_.text(x, row + 3, cell.text, kDim);
            continue;
        }
        const std::size_t index = cell.button;
        char label[32];
        std::snprintf(label, sizeof(label), "%2d %.10s", gameNumber(index),
                      std::string(profile_->buttonName(index)).c_str());
        window_.text(x + 8, row + 4, label, kText);
        // The first source (a button), "or" a second one (a button, or a one-sided axis past a threshold).
        const float quarter = half / 2;   // each column's fields share its extra width
        drawSource(x + 112, row, 206 + quarter, Slot::Button, index);
        window_.text(x + 322 + quarter, row + 4, "or", kDim);
        drawAltSource(x + 342 + quarter, row, 126 + quarter, index);
    }
    const float gridEnd = gridTop + layout.height - 4;

    drawRotation(gridEnd + 14);

    // One status line; the explanations are hover text (the headings, saved, Undo, Default, Invert).
    const std::string state = dirty() ? "not saved" : "saved";
    window_.text(4, contentH() - 24, fit(status_.empty() ? state : status_, 118 + extraChars()),
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
        HoverText::drawBox(window_, hoverX_, hoverY_, it->text, canvasW(), contentH());   // wrapped, a few lines
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
    style::headingBand(window_, 0, y, canvasW());
    window_.text(8, y, "STEERING ROTATION", kHeading);
    window_.text(160, y, "full lock to full lock, in degrees", kDim);

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

    // A game can have its own (NFS Payback: 180): the Games tab.
    y += 4;
    window_.text(4, y, "A game can steer with its own rotation (NFS Payback: 180): the Games tab (G) sets it.",
                 kDim);

    y += 18;
    if (profile_ != nullptr && profile_->identity().centringSpring) y = drawCentringSpring(y);
    if (profile_ != nullptr && profile_->hasNativeButtonOrder()) drawButtonOrder(y);
}

float MappingEditView::drawButtonOrder(const float top) {
    // The order the target's buttons reach the game in (ButtonOrder): as Logitech's driver numbered them, or the
    // wheel's own HID order, which newer games' presets read. This tab names them in the driver's order either way.
    float y = top;
    const std::string target(profile_->identity().shortName);
    style::headingBand(window_, 0, y, canvasW());
    window_.text(4, y, "BUTTON ORDER", kHeading);
    window_.text(140, y, "the order the game gets the " + target + "'s buttons in", kDim);
    tip(4, y - 2, 600, 12,
        "Logitech driver: as Logitech's Windows software numbered the " + target + "'s buttons, which games of its "
        "time read (Race Driver: GRID). Native: the wheel's own order, without Logitech's driver, which newer games' "
        "presets read (Assetto Corsa Competizione). The buttons keep their names here; their numbers are the game's: "
        "from 1 with Logitech driver (as joy.cpl and games show them), from 0 with Native. Takes effect at the next "
        "Start.");
    y += 16;
    window_.text(4, y + 4, "Order", kText);
    float x = kSliderX;
    for (const ButtonOrder order : {ButtonOrder::Driver, ButtonOrder::Native}) {
        const std::string label = order == ButtonOrder::Driver ? "Logitech driver" : "Native";
        x += button(x, y, label, [this, order] { edit_.buttonOrder = order; }, edit_.buttonOrder == order, !locked_,
                    136) + 4;
    }
    return y + 20;
}

float MappingEditView::drawCentringSpring(const float top) {
    // The target's own (Logitech's software for the G25 and G27): a spring to the centre in every game, on top of the
    // game's forces. 0 is off (the checkbox unticked).
    float y = top;
    const std::string target(profile_->identity().shortName);
    style::headingBand(window_, 0, y, canvasW());
    window_.text(8, y, "CENTRING SPRING", kHeading);
    window_.text(140, y, "the " + target + "'s own, in every game: pulls the wheel to the centre", kDim);
    tip(4, y - 2, 600, 12,
        "As Logitech's software for the " + target + " had it (\"Enable Centering Spring in Force Feedback Games\", "
        "0 to 100%): a spring around the centre on top of the game's forces, whatever the game sends (Race Driver: "
        "GRID sends none, so the wheel stays where it was left). 0 = off. Takes effect at the next Start.");
    y += 16;
    window_.text(4, y + 4, "Strength", kText);
    const bool changed = edit_.centringSpringPercent != saved_.centringSpringPercent;
    drawField(kSliderX, y, 64, Field::CentringSpring, tuning::formatNumber(edit_.centringSpringPercent), changed);
    window_.text(kSliderX + 68, y + 4, "%", kDim);
    float x = kSliderX + 92;
    for (const float preset : kSpringPresets) {
        const std::string label = preset == 0.0F ? "Off" : percent(preset);
        x += button(x, y, label, [this, preset] { edit_.centringSpringPercent = preset; },
                    std::fabs(edit_.centringSpringPercent - preset) < 0.5F, !locked_, 48) + 4;
    }
    return y + 20;
}

// --- the Games page ---------------------------------------------------------------------------------------------

namespace {

constexpr std::uint64_t kFileStateEveryMs = 2000;

std::string clockText(const std::uint64_t ms) {
    const std::uint64_t seconds = ms / 1000;
    char text[32];
    std::snprintf(text, sizeof(text), "%llu:%02llu:%02llu", static_cast<unsigned long long>(seconds / 3600),
                  static_cast<unsigned long long>(seconds / 60 % 60), static_cast<unsigned long long>(seconds % 60));
    return text;
}

}  // namespace

void MappingEditView::pressListBar() {
    const std::uint64_t now = nowMs();
    if (gripPressMs_ != 0 && now - gripPressMs_ < kDoubleClickMs) {   // the second press: fit the longest name
        gripPressMs_ = 0;
        listDragging_ = false;
        gameListW_ = std::clamp(std::ceil(fitListW_), kGameListMinW, maxGameListW());
        keepListWidth();
        return;
    }
    gripPressMs_ = now;
    listGrab_ = clickX_ - gameListW();
    gameListW_ = gameListW();   // what is shown, if the canvas made the kept width narrower
    listDragging_ = true;
}

void MappingEditView::keepListWidth() {
    const std::string value = std::to_string(static_cast<int>(std::lround(gameListW())));
    if (stateFile_.empty() || readAppSetting(stateFile_, "games_list_width") == value) return;
    if (!writeAppSetting(stateFile_, "games_list_width", value)) {
        status_ = "Could not keep the games list's width in " + pathToUtf8(stateFile_) + ".";
        statusError_ = true;
    }
}

void MappingEditView::selectGame(const std::string& id) {
    for (const GameProfile& game : games_) {
        if (game.id == id) {
            if (selectedGame_ != id) field_ = Field::None;
            selectedGame_ = id;
            selectedFolderOnly_.clear();
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

    // The list: the built-in profiles, the games the user added, then the folders of games WheelBri does not know; the
    // one that runs now in green. Each says what is in its folder.
    refreshGameFolders();
    if (!gameListRead_) {
        gameListRead_ = true;
        if (const std::string kept = readAppSetting(stateFile_, "games_list_width"); !kept.empty()) {
            gameListW_ = static_cast<float>(std::atoi(kept.c_str()));
        }
    }
    const float listW = gameListW();
    fitListW_ = kGameListMinW;
    // A row's name, cut only where its tag (`tagW` wide at the right) leaves no room; when cut, its hover text has it
    // whole. What the row needs to show it whole joins fitListW_ (a double-click on the bar).
    const auto drawName = [this, listW](const float rowY, const std::string& name, const float tagW,
                                        const std::string& tag, const ui::Colour colour) {
        const float room = listW - 6 - 8 - tagW - 6;
        const auto chars = static_cast<std::size_t>(std::max(4.0F, room / ui::kCharWidth));
        window_.text(10, rowY + 5, fit(name, chars), colour);
        fitListW_ = std::max(fitListW_, 6 + static_cast<float>(name.size()) * ui::kCharWidth + 8 + tagW + 6);
        if (name.size() > chars) tip(4, rowY, listW, 18, name + " (" + tag + ")");
    };
    style::headingBand(window_, 0, y, gameListW() + 4);
    window_.text(8, y, "GAMES", kHeading);
    y += 16;
    const auto folderTag = [this](const std::size_t folder, ui::Colour& colour) {
        const ShimState state = shims_->stateAt(folder);
        colour = state == ShimState::Foreign ? kWarning : state == ShimState::Missing ? style::kFaint : kActive;
        return state == ShimState::Foreign   ? std::string("other mod")
               : state == ShimState::Missing ? std::string("no shim")
                                             : "shim " + shims_->targetAt(folder);
    };
    for (std::size_t g = 0; g < games.size(); ++g) {
        if (y > contentH() - 150) break;   // keep the buttons and the hint clear
        const GameProfile& game = games[g];
        const bool on = g == picked && selectedFolderOnly_.empty();
        const bool runs = game.id == now_.id;
        const auto folder = gameFolders_.find(game.id);
        const bool hasFolder = shims_ != nullptr && folder != gameFolders_.end();
        // A game that does not support the target says so in the list too (amber).
        const bool unfit = targetAdvice(game, targetName_).has_value();
        window_.fill(4, y, listW, 18, on ? kHeading : kPanel);
        if (runs) {
            window_.fill(4 + listW - 64, y + 3, 58, 12, kActive);
            window_.text(4 + listW - 60, y + 5, "running", kDark);
            drawName(y, game.label, 58, "running", on ? kDark : kText);
        } else {
            ui::Colour colour = style::kFaint;
            std::string tag = userGame(game.id) ? "yours" : "built in";
            if (unfit) {
                tag = "! not " + upperName(targetName_);
                colour = kCaution;
            } else if (hasFolder) {
                tag = folderTag(folder->second, colour);
            }
            const float tagW = static_cast<float>(tag.size()) * ui::kCharWidth;
            window_.text(4 + listW - 6 - tagW, y + 5, tag, on ? kDark : colour);
            drawName(y, game.label, tagW, tag, on ? kDark : kText);
        }
        const std::string id = game.id;
        hit(4, y, listW, 18, [this, id] { selectGame(id); });
        y += 22;
    }
    for (const std::size_t folder : otherFolders_) {
        if (y > contentH() - 150 || shims_ == nullptr || folder >= shims_->folders().size()) break;
        const ShimGame& entry = shims_->folders()[folder];
        const bool on = selectedFolderOnly_ == entry.folder;
        window_.fill(4, y, listW, 18, on ? kHeading : kStripe);
        ui::Colour colour = style::kFaint;
        const std::string tag = folderTag(folder, colour);
        const float tagW = static_cast<float>(tag.size()) * ui::kCharWidth;
        window_.text(4 + listW - 6 - tagW, y + 5, tag, on ? kDark : colour);
        drawName(y, entry.name, tagW, tag, on ? kDark : kText);
        const std::filesystem::path path = entry.folder;
        hit(4, y, listW, 18, [this, path] {
            selectedFolderOnly_ = path;
            field_ = Field::None;
        });
        y += 22;
    }
    y += 4;
    button(4, y, "Add game...", [this] { addGame(); }, false, !locked_ && canPickFile(), 104);
    tip(4, y, 104, 16,
        canPickFile()
            ? "Pick a game's .exe: WheelBri finds the game by that name while it runs, and its folder joins the list "
              "(no shim put in)."
            : "No file window here yet: add games in games.ini next to app.ini.");
    const std::string pickedId = games[picked].id;
    button(4 + 112, y, "Remove", [this, pickedId] { removeGame(pickedId); }, false,
           !locked_ && userGame(pickedId) && selectedFolderOnly_.empty(), 84);
    tip(4 + 112, y, 84, 16, "Takes a game you added off the list, with its own values. Built-in games stay.");
    y += 22;
    // The bar between the list and the game: dragged, the list is wider or narrower; double-clicked, it fits.
    {
        const float barX = gameX() - 12;
        const float barH = contentH() - 100;
        const bool active = listDragging_ || (hoverInside_ && hoverX_ >= barX && hoverX_ < barX + 9 &&
                                              hoverY_ >= 42 && hoverY_ < 42 + barH);
        window_.fill(barX, 42, 9, barH, style::kSplit);
        window_.outline(barX, 42, 9, barH, active ? kHeading : style::kSplitEdge);
        const float midY = 42 + std::floor(barH / 2);
        for (const float dy : {-8.0F, 0.0F, 8.0F}) {
            window_.fill(barX + 4, midY + dy - 1, 2, 2, active ? kHeading : kDim);
        }
        hit(barX, 42, 9, barH, [this] { pressListBar(); });
        tip(barX, 42, 9, barH, "Drag to make the games list wider or narrower; double-click to fit its longest name.");
    }

    // A folder whose game WheelBri does not know: its folder and the shim there, and Add as your game for its values.
    std::optional<std::size_t> otherFolder;
    if (!selectedFolderOnly_.empty() && shims_ != nullptr) {
        for (const std::size_t folder : otherFolders_) {
            if (shims_->folders()[folder].folder == selectedFolderOnly_) otherFolder = folder;
        }
        if (!otherFolder) selectedFolderOnly_.clear();
    }
    if (otherFolder) {
        const ShimGame& entry = shims_->folders()[*otherFolder];
        window_.text(gameX(), 46, entry.name, kHeading);
        const std::string unknown =
            "WheelBri does not know the game in this folder: add it as yours to set its values.";
        window_.text(gameX(), 62, fit(unknown, gameChars()), kDim);
        float top = drawGameFolder(nullptr, otherFolder, 80);
        button(gameX(), top + 4, "Add as your game...", [this, entry] { addGame(entry.folder); }, false,
               !locked_ && canPickFile(), 168);
    } else {
        // The picked game: its name and whether it runs, its folder, its sub-tabs, then the one shown.
        float top = drawGameProfile(picked, 46);
        const GameProfile& game = games[picked];
        const auto folder = gameFolders_.find(game.id);
        top = drawGameFolder(&game, folder != gameFolders_.end() ? std::optional<std::size_t>(folder->second)
                                                                 : std::nullopt,
                             top);
        if (gameTab_ == GameTab::File && game.settings.empty()) gameTab_ = GameTab::Axes;
        float x = gameX();
        const auto tab = [&](const GameTab which, const char* label) {
            x += button(x, top + 4, label, [this, which] { gameTab_ = which; }, gameTab_ == which, true) + 6;
        };
        tab(GameTab::Axes, "Axes");
        tab(GameTab::Ffb, "Force feedback");
        tab(GameTab::Buttons, "Buttons");
        if (!game.settings.empty()) tab(GameTab::File, "Game file");
        top += 28;
        switch (gameTab_) {
            case GameTab::Axes: drawGameAxes(picked, top); break;
            case GameTab::Ffb: drawGameForces(picked, top); break;
            case GameTab::Buttons: drawGameButtons(picked, top); break;
            case GameTab::File: drawGameSettings(picked, top); break;
        }
    }

    // One status line: what was just done, else when the values apply.
    const float status = contentH() - 24;
    if (!status_.empty()) {
        window_.text(4, status, fit(status_, 118 + extraChars()), statusError_ ? kWarning : kActive);
    } else if (!locked_) {
        window_.text(4, status, "Saved as you change them: used from the next Start, or at once while WheelBri runs.",
                     kDim);
    } else if (path_ == sessionPath_) {
        window_.text(4, status,
                     fit("Running: changes apply at once (pedal axes in Mode Shim, and Add game: from the next Start).",
                         118 + extraChars()),
                     kActive);
    } else {
        window_.text(4, status,
                     fit("Running with WheelBri's default mapping: your changes are kept in your own mapping and used "
                         "from the next Start.",
                         118 + extraChars()),
                     kCaution);
    }
}

void MappingEditView::refreshGameFolders() {
    if (shims_ == nullptr) return;
    shims_->refresh();
    const std::uint64_t now = nowMs();
    if (now - gameFoldersReadMs_ < 2000 && gameFoldersCount_ == shims_->folders().size() + games_.size()) return;
    gameFoldersReadMs_ = now;
    gameFoldersCount_ = shims_->folders().size() + games_.size();
    gameFolders_.clear();
    otherFolders_.clear();
    for (std::size_t f = 0; f < shims_->folders().size(); ++f) {
        const GameProfile* const known = gameInFolder(shims_->folders()[f].folder, games_);
        if (known == nullptr) {
            otherFolders_.push_back(f);
        } else if (gameFolders_.count(known->id) == 0) {
            gameFolders_[known->id] = f;
        }
    }
}

float MappingEditView::drawGameFolder(const GameProfile* const game, const std::optional<std::size_t> folder, float y) {
    if (shims_ == nullptr) return y;
    const bool shimMode = picker_.way() == ConfigPicker::Way::Shim;
    style::headingBand(window_, gameX() - 4, y + 4, canvasW() - gameX());
    window_.text(gameX() + 4, y + 4, "FOLDER", kHeading);
    const float x = gameX() + 64;
    if (!folder || *folder >= shims_->folders().size()) {
        // No folder yet: only Mode Shim needs one (the shim goes there).
        window_.text(x, y + 4,
                     fit(shimMode ? "none yet: Mode Shim puts its shim in the game's folder"
                                  : "none needed in Mode Virtual (Mode Shim puts its shim there)",
                         gameChars() - 26),
                     shimMode ? kCaution : kDim);
        // The folder window starts next to the last listed game's folder (games are often side by side).
        const std::filesystem::path start =
            shims_->folders().empty() ? std::filesystem::path() : shims_->folders().back().folder.parent_path();
        button(canvasW() - 4 - 152, y, "Choose folder...", [this, game, start] { shims_->chooseFolder(game, start); },
               false, !locked_, 152);
        return y + 24;
    }
    const std::size_t index = *folder;
    const ShimGame& entry = shims_->folders()[index];
    const ShimState state = shims_->stateAt(index);
    const std::string stateText = state == ShimState::Installed ? "shim in place"
                                  : state == ShimState::Outdated ? "older shim (Start replaces it)"
                                  : state == ShimState::Foreign  ? "another mod's dinput8.dll: Start leaves it"
                                                                 : "no shim (Start puts it in)";
    const std::string target = shims_->targetAt(index);
    const std::string presents =
        (state == ShimState::Installed || state == ShimState::Outdated) && !target.empty()
            ? ", presents the " + upperName(target)
            : std::string();
    const std::size_t pathChars = gameChars() > 60 ? gameChars() - 52 : 8;
    window_.text(x, y + 4, fit(pathToUtf8(entry.folder), pathChars), kDim);
    const float stateX = x + static_cast<float>(pathChars + 2) * ui::kCharWidth;
    window_.text(stateX, y + 4, stateText + presents,
                 state == ShimState::Foreign ? kWarning : state == ShimState::Missing ? kDim : kActive);
    tip(x, y, static_cast<float>(pathChars) * ui::kCharWidth, 16, pathToUtf8(entry.folder));
    y += 20;
    // What to do with it: put the shim in or take it out, take the folder off the list.
    float bx = x;
    if (state == ShimState::Missing || state == ShimState::Outdated) {
        bx += button(bx, y, "Put shim", [this, index] { shims_->put(index); }, false, !locked_, 96) + 6;
    } else if (state == ShimState::Installed) {
        bx += button(bx, y, "Remove shim", [this, index] { shims_->remove(index); }, false, !locked_, 112) + 6;
    }
    button(bx, y, "Forget folder", [this, index] { shims_->forget(index); }, false, !locked_, 120);
    tip(bx, y, 120, 16, "Takes the folder off the list; its files stay as they are.");
    return y + 24;
}

float MappingEditView::drawGameProfile(const std::size_t index, float y) {
    const GameProfile& game = games_[index];
    window_.text(gameX(), y, game.label, kHeading);
    window_.text(gameX() + static_cast<float>(game.label.size() + 2) * ui::kCharWidth, y,
                 fit("program " + game.program, gameChars() - game.label.size() - 2),
                 kDim);
    y += 16;
    // A game that works one way only ([game] ways): a session run the other way tells the user when it starts.
    if (!game.virtualDevice || !game.shim) {
        const std::string only = game.shim ? "Mode Shim only" : "Mode Virtual only";
        for (const std::string& line : wrap(only + ": " + game.waysNote + ".", gameChars(), 2)) {
            window_.text(gameX(), y, line, kCaution);
            y += 12;
        }
    }

    // A game that does not support the target ([game] not_for): what goes wrong and what to use, in an amber box. The
    // target is changed on the Mapping tab only.
    if (const auto advice = targetAdvice(game, targetName_)) {
        const std::vector<std::string> lines = wrap("! " + *advice, gameChars() - 2, 3);
        const float boxH = 8 + 12 * static_cast<float>(lines.size());
        window_.fill(gameX(), y, canvasW() - gameX() - 4, boxH, style::kCautionBack);
        window_.outline(gameX(), y, canvasW() - gameX() - 4, boxH, kCaution);
        float line = y + 5;
        for (const std::string& text : lines) {
            window_.text(gameX() + 8, line, text, kCaution);
            line += 12;
        }
        y += boxH + 6;
    }

    // Now: the game that runs (while a session does), else how it is found.
    if (now_.id == game.id) {
        const std::string line =
            "RUNNING, pid " + std::to_string(now_.pid) + ", for " + clockText(now_.forMs) +
            (now_.profileApplies ? ": its profile and your values are in use" : ": your values are in use");
        window_.text(gameX(), y, fit(line, gameChars()), kActive);
        y += 12;
        std::string applied;
        if (!now_.rotation.empty()) applied += "steering " + now_.rotation;
        if (std::fabs(now_.ffbGain - 1.0F) > 1e-4F) {
            applied += (applied.empty() ? "" : ", ") + std::string("force feedback x") +
                       tuning::formatNumber(now_.ffbGain, 2);
        }
        if (!describeOverlay(game).empty()) applied += (applied.empty() ? "" : ", ") + std::string("pedals as below");
        if (!applied.empty()) {
            window_.text(gameX(), y, fit("now: " + applied, gameChars()), kText);
            y += 12;
        }
        if (!now_.warning.empty()) {
            window_.text(gameX(), y, fit(now_.warning, gameChars()), kWarning);
            y += 12;
        }
    } else if (now_.session) {
        window_.text(gameX(), y,
                     fit(now_.id.empty() ? std::string("not running: WheelBri looks for it every 2 s")
                                         : "not running (another game with a profile runs)",
                         gameChars()),
                     kDim);
        y += 12;
    } else {
        window_.text(gameX(), y,
                     fit("While WheelBri runs (Start) it finds the game when it starts, and uses this profile.",
                         gameChars()),
                     kDim);
        y += 12;
    }
    return y;
}

// A section's heading on the Games page: its name, and what it holds (dim).
void MappingEditView::gameSection(const float y, const std::string& name, const std::string& about) {
    style::headingBand(window_, gameX() - 4, y, canvasW() - gameX());
    window_.text(gameX() + 4, y, name, kHeading);
    window_.text(gameX() + 160, y, fit(about, gameChars() - 20), kDim);
}

float MappingEditView::drawGameSettings(const std::size_t index, float y) {
    const GameProfile& game = games_[index];
    const std::string id = game.id;
    if (game.settings.empty()) return y;   // nothing of its own to change: no Game config section at all
    gameSection(y, "GAME CONFIG", "the game's own settings: written into its file only by Save to file");
    y += 16;
    window_.text(gameX(), y, fit(game.settingsNote, gameChars()), kDim);
    y += 16;

    // What the file holds now against the values below (as shown, saved or not), read every 2 s.
    const GameFilePlace place = gameFilePlace(game);
    const std::vector<GameSettingText> wanted = gameValues(game);
    std::string key = id + "|" + pathToUtf8(place.root) + "|" + place.relative;
    for (const GameSettingText& value : wanted) key += "|" + value.name + "=" + value.text;
    const std::uint64_t readNow = nowMs();
    if (!fileState_ || key != fileStateKey_ || readNow - fileStateReadMs_ >= kFileStateEveryMs) {
        fileState_ = readGameSettingsState(place.root, place.relative, wanted);
        fileStateKey_ = key;
        fileStateReadMs_ = readNow;
    }
    const GameSettingsFileState& file = *fileState_;
    using Status = GameSettingsFileState::Status;
    window_.text(gameX(), y, "File", kText);
    const std::string where = place.located           ? pathToUtf8(place.root / pathFromUtf8(place.relative))
                              : !place.root.empty()   ? "Documents/" + game.settingsFile
                                                      : game.settingsFile;
    window_.text(gameValueX(), y, fit(where, valueChars()), place.located ? kText : kDim);
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
    window_.text(gameValueX(), y, fit(fileLine, valueChars()), fileColour);
    if (!place.confirmed) {
        y += 12;
        window_.text(gameValueX(), y,
                     fit("not seen written by " + game.label + " yet: play it once while WheelBri runs", valueChars()),
                     kCaution);
        tip(gameX(), y - 12, canvasW() - gameX() - 4, 24,
            "WheelBri knows a game's settings file when it sees the game write it (when the game ends). Another game "
            "can have a file of the same name with the same settings (NFS Heat's for NFS Unbound): until then Save to "
            "file asks first.");
    }
    y += 16;
    // The file found: Save to file. Not found yet: Locate it first (the button then says Save to file).
    const bool found = file.status == Status::Read || file.status == Status::Unreadable;
    if (found) {
        button(gameValueX(), y, "Save to file", [this, index] { saveGameFile(index); }, !file.asSet(),
               !locked_ && file.status == Status::Read, 120);
        button(gameValueX() + 128, y, "Locate...", [this, index] { locateGameFile(index); }, false,
               !locked_ && canPickFile(), 88);
        window_.text(gameValueX() + 224, y + 4, fit("keeps the original as .before-wheelbri", valueChars() - 28),
                     kDim);
    } else {
        button(gameValueX(), y, "Locate file", [this, index] { locateGameFile(index); }, true,
               !locked_ && canPickFile(), 120);
        const std::string name = pathToUtf8(pathFromUtf8(game.settingsFile).filename());
        window_.text(gameValueX() + 128, y + 4,
                     fit(canPickFile() ? "find the game's " + name + " file; then Save to file"
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
        window_.text(gameX(), y + 4, fit(setting.value ? gameSettingLabel(setting.id) : "steering rotation", 19),
                     kText);
        const std::optional<std::string> inFile =
            s < file.entries.size() ? file.entries[s].inFile : std::optional<std::string>();
        // What the file holds, right: amber while it is not what the next Start writes.
        if (file.status == Status::Read) {
            const bool asWanted = s < wanted.size() && inFile == wanted[s].text;
            window_.text(fileColumnX(), y + 4, fit(inFile.value_or(std::string("not found")), 13),
                         asWanted ? kDim : kCaution);
        }
        tip(gameX(), y, fileColumnX() - gameX(), 16,
            setting.name + " in the game's settings file" +
                (setting.value ? ": profile " + tuning::formatNumber(*setting.value, 2) + ", 0 to 1"
                               : ": the steering rotation, in degrees"));
        if (!setting.value) {
            // The same value as Mapping's steering rotation: changing either changes both.
            const bool ownRotation = edit_.gameRotations.count(id) != 0;
            const bool rotationChanged =
                ownRotation != (saved_.gameRotations.count(id) != 0) ||
                (ownRotation && !(edit_.gameRotations.at(id) == saved_.gameRotations.at(id)));
            const float deg = rotationWhileRunning(game, edit_.gameRotations, profileFitsTarget(game, targetName_))
                                   .value_or(edit_.steeringRotationDeg);
            drawField(gameValueX(), y, 80, Field::GameSteering, degrees(deg), rotationChanged, index);
            window_.text(gameValueX() + 84, y + 4, "deg", kDim);
            window_.text(gameValueX() + 248, y + 4, fit("same as in Mapping", 26), kDim);
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
        drawField(gameValueX(), y, 80, Field::GameSetting, tuning::formatNumber(own.value_or(*setting.value), 2),
                  changed, index * kMaxGameSettings + s);
        const std::string settingId = setting.id;
        button(gameValueX() + 168, y, "Default", [this, id, settingId] {
            if (const auto byGame = edit_.gameSettings.find(id); byGame != edit_.gameSettings.end()) {
                byGame->second.erase(settingId);
                if (byGame->second.empty()) edit_.gameSettings.erase(byGame);
            }
        }, false, editable() && own.has_value(), 72);
        window_.text(gameValueX() + 248, y + 4,
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
    // One in games.ini with a built-in profile's id (a hand edit) is that profile, not the user's (reloadGames).
    const auto same = [&id](const GameProfile& g) { return g.id == id; };
    return std::any_of(userGames_.begin(), userGames_.end(), same) &&
           std::none_of(gameProfiles().begin(), gameProfiles().end(), same);
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

void MappingEditView::addGame(const std::filesystem::path& start) {
    const std::optional<std::filesystem::path> picked =
        pickFile("Add a game: pick its program (.exe)", start, "");
    if (!picked) return;   // cancelled
    // Its folder joins the game folders (for Mode Shim; no shim put in until Put shim or Start).
    const auto addFolder = [this, &picked] {
        if (shims_ == nullptr) return;
        for (const ShimGame& entry : shims_->folders()) {
            std::error_code ec;
            if (std::filesystem::equivalent(entry.folder, picked->parent_path(), ec)) return;   // listed already
        }
        std::string problem;
        if (!shims_->addFolder(picked->parent_path(), problem)) {
            WE_LOG_WARN(log(), "games: {}'s folder not added: {}", pathToUtf8(picked->parent_path()), problem);
        }
        gameFoldersReadMs_ = 0;
    };
    const std::string program = programName(pathToUtf8(picked->filename()));
    if (program.size() < 5 || program.compare(program.size() - 4, 4, ".exe") != 0) {
        status_ = "Not added: pick the game's program, a .exe file.";
        statusError_ = true;
        return;
    }
    for (const GameProfile& game : games_) {
        if (game.program == program) {
            addFolder();
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
    // At most 20 bytes (a label's length, GameProfiles.hpp), never half a character (a name like "Pokémon").
    game.label = truncateUtf8(pathToUtf8(picked->stem()), 20);
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
    addFolder();
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
    // Its own values go first (saved as they change), so the mapping never names a game that is gone: the config
    // would then have a problem (a [game_rotation] line for no game). (Only this save's result stops it: an error the
    // page showed before, for something else, does not.)
    edit_.gameTunings.erase(id);
    edit_.gameRotations.erase(id);
    edit_.gameFfbGains.erase(id);
    edit_.gameSettings.erase(id);
    if (gameValuesChanged()) {
        saveGameValues();
        if (statusError_) {
            WE_LOG_WARN(log(), "games: {} not removed: its values could not be saved ({})", label, status_);
            return;
        }
    }
    if (!writeFileReplacing(userGamesFile(), formatUserGames(next))) {
        status_ = "Not removed: cannot write " + pathToUtf8(userGamesFile()) + ".";
        statusError_ = true;
        WE_LOG_WARN(log(), "games: {} not removed: {}", label, status_);
        return;
    }
    WE_LOG_INFO(log(), "games: removed {} ({})", label, id);
    status_ = label + " removed, with its own values.";
    statusError_ = false;
    // Add game put its folder on the list: it goes too (its files stay), unless WheelBri's shim is in it (the folder
    // stays listed, so Remove shim can delete it).
    if (const auto folder = gameFolders_.find(id); shims_ != nullptr && folder != gameFolders_.end() &&
                                                   folder->second < shims_->folders().size()) {
        const ShimState state = shims_->stateAt(folder->second);
        if (state == ShimState::Installed || state == ShimState::Outdated) {
            status_ = label + " removed, with its own values. Its folder stays listed: WheelBri's shim is in it " +
                      "(Remove shim, then Forget folder).";
        } else {
            shims_->forget(folder->second);
        }
    }
    reloadGames();
    gameFoldersCount_ = 0;   // the folders are matched to the games again at the next draw
    selectedGame_.clear();
    selectedFolderOnly_.clear();
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
        // Picked: the user's own from now on, written in the file, no longer the Rig's.
        AutoInputs& autos = edit_.autoInputs;
        if (pickKind_ == Slot::Axis) autos.axes[pickIndex_] = false;
        if (pickKind_ == Slot::Hat) autos.hat[pickIndex_] = false;
        if (pickKind_ == Slot::Button && pickIndex_ < autos.buttons.size()) autos.buttons[pickIndex_] = false;
        return;
    }
    const std::size_t picked = selectedGameIndex();
    if (picked >= games_.size()) return;
    const std::string id = games_[picked].id;
    if (source == edit_.buttons[pickIndex_]) {
        // The mapping's own: no value of the game's needed.
        const GameTuning* const own = tuningOf(id);
        if (own == nullptr || own->buttons.count(static_cast<int>(pickIndex_)) == 0) {
            status_ = "Button " + std::to_string(gameNumber(pickIndex_)) + " " +
                      std::string(profile_->buttonName(pickIndex_)) +
                      " already comes from that control in the Mapping tab: nothing added.";
            statusError_ = false;
        }
        tuningFor(id).buttons.erase(static_cast<int>(pickIndex_));
        dropEmptyTuning(id);
    } else {
        tuningFor(id).buttons[static_cast<int>(pickIndex_)] = source;
    }
}

float MappingEditView::drawGameAxes(const std::size_t index, float y) {
    const GameProfile& game = games_[index];
    const std::string id = game.id;
    const GameTuning* const own = tuningOf(id);
    const GameTuning* const saved = saved_.gameTunings.count(id) != 0 ? &saved_.gameTunings.at(id) : nullptr;
    const std::string target = profile_ != nullptr ? profile_->identity().shortName : targetName_;
    gameSection(y, "AXES", "what the game reads; starts from its profile and your Mapping tab");
    y += 16;
    // The columns: name, the axis the game reads, its direction, the degrees (steering), Default, where it comes from.
    const float axisX = gameValueX() - 52;
    const float directionX = axisX + 136;
    const float degreesX = directionX + 96;
    const float defaultX = degreesX + 72;
    const float fromX = defaultX + 80;
    window_.text(axisX, y, "game reads", kDim);
    window_.text(directionX, y, "direction", kDim);
    window_.text(degreesX, y, "degrees", kDim);
    window_.text(fromX, y, "comes from", kDim);
    y += 14;
    const auto from = [&](const float rowY, const bool mine, const std::string& source) {
        window_.text(fromX, rowY + 4, fit(mine ? "yours" : source, valueChars() - 40), mine ? kCaution : kDim);
    };

    // Steering: the wheel's axis (games read it there), and the rotation the game is played with ([game_rotation]):
    // yours, else the profile's, else the Mapping tab's. The wheel's own software must be set to the same.
    window_.text(gameX(), y + 4, "Steering", kText);
    window_.fill(axisX, y, 128, 16, kStripe);
    window_.text(axisX + 6, y + 4, "x, the wheel", kDim);
    const bool ownRotation = edit_.gameRotations.count(id) != 0;
    const bool fits = profileFitsTarget(game, targetName_);
    const std::optional<float> rotation = rotationWhileRunning(game, edit_.gameRotations, fits);
    const bool rotationChanged = ownRotation != (saved_.gameRotations.count(id) != 0) ||
                                 (ownRotation && !(edit_.gameRotations.at(id) == saved_.gameRotations.at(id)));
    const float degreesNow = rotation.value_or(edit_.steeringRotationDeg);
    drawField(degreesX, y, 64, Field::PerGame, degrees(degreesNow), rotationChanged, index);
    button(defaultX, y, "Default", [this, id] { edit_.gameRotations.erase(id); }, false, editable() && ownRotation,
           72);
    from(y, ownRotation, fits && game.rotationDeg ? "profile" : "Mapping tab");
    y += 20;

    // Pedals: the axis the game reads each one from (the profile's layout over the target's, then yours), and its
    // direction (the Mapping tab's invert, then yours).
    const JoystickLayout device = layoutFor(targetName_, 0);
    const JoystickLayout profileLayout = fits ? overlay(device, game) : device;
    const JoystickLayout shown = own != nullptr ? tunedLayout(profileLayout, *own) : profileLayout;
    const std::array<ObjectGuid, 3> axes = {shown.throttle, shown.brake, shown.clutch};
    const std::array<ObjectGuid, 3> deviceAxes = {device.throttle, device.brake, device.clutch};
    const std::array<ObjectGuid, 3> profileAxes = {profileLayout.throttle, profileLayout.brake, profileLayout.clutch};
    for (std::size_t p = 0; p < 3; ++p) {
        window_.text(gameX(), y + 4, kPedalNames[p], kText);
        const bool ownAxis = own != nullptr && own->pedalAxes[p].has_value();
        const bool axisChanged = ownAxis != (saved != nullptr && saved->pedalAxes[p].has_value()) ||
                                 (ownAxis && saved != nullptr && saved->pedalAxes[p] != own->pedalAxes[p]);
        // < and > go through the axes (the clutch also through none).
        const auto step = [this, id, p, axes](const int by) {
            std::vector<ObjectGuid> choices(kPedalAxes.begin(), kPedalAxes.end());
            if (p == 2) choices.push_back(ObjectGuid::Any);
            const auto at = std::find(choices.begin(), choices.end(), axes[p]);
            const auto i = at == choices.end() ? 0 : static_cast<int>(at - choices.begin());
            const auto n = static_cast<int>(choices.size());
            tuningFor(id).pedalAxes[p] = choices[static_cast<std::size_t>(((i + by) % n + n) % n)];
        };
        button(axisX, y, "<", [step] { step(-1); }, false, editable(), 24);
        window_.fill(axisX + 28, y, 72, 16, kStripe);
        window_.outline(axisX + 28, y, 72, 16, axisChanged ? kCaution : style::kFieldEdge);
        window_.text(axisX + 34, y + 4, std::string(wheelbri::axisName(axes[p])), kText);
        button(axisX + 104, y, ">", [step] { step(1); }, false, editable(), 24);
        const bool ownInvert = own != nullptr && own->pedalInvert[p].has_value();
        const bool invert = ownInvert ? *own->pedalInvert[p] : edit_.invert[p + 1];
        button(directionX, y, invert ? "Inverted" : "Normal", [this, id, p, invert] {
            tuningFor(id).pedalInvert[p] = !invert;
        }, invert, editable(), 88);
        button(defaultX, y, "Default", [this, id, p] {
            tuningFor(id).pedalAxes[p].reset();
            tuningFor(id).pedalInvert[p].reset();
            dropEmptyTuning(id);
        }, false, editable() && (ownAxis || ownInvert), 72);
        from(y, ownAxis || ownInvert, fits && profileAxes[p] != deviceAxes[p] ? "profile" : "Mapping tab");
        y += 20;
    }
    y += 6;
    window_.text(axisX, y, fit("Default: back to where it came from. Amber: yours, for this game.", valueChars() + 6),
                 kDim);
    y += 12;
    window_.text(axisX, y,
                 fit("How they feel (dead zones, curves): your rig's own software.",
                     valueChars() + 6),
                 kDim);
    y += 12;
    if (rotation && std::fabs(*rotation - edit_.steeringRotationDeg) >= 0.5F) {
        window_.text(axisX, y,
                     fit("Set your wheel's own software to " + degrees(*rotation) + " deg while you play this game.",
                         valueChars() + 6),
                     kCaution);
        y += 12;
    }
    // Two pedals on one axis, or (Mode Virtual) an axis the virtual wheel has not: said, and what happens then.
    if (!isValid(shown)) {
        window_.text(axisX, y, fit("Two pedals on one axis: pick another axis for one of them (not used until then).",
                                   valueChars() + 6),
                     kWarning);
        y += 12;
    } else if (picker_.virtualDevice() && !routePedals(device, shown)) {
        const std::string deviceAxesText = std::string(wheelbri::axisName(device.throttle)) + ", " +
                                           std::string(wheelbri::axisName(device.brake)) + " and " +
                                           std::string(wheelbri::axisName(device.clutch));
        for (const std::string& line : wrap("Mode Virtual: the virtual " + target + " has only " + deviceAxesText +
                                                " for pedals, each once; these are used in Mode Shim only.",
                                            valueChars() + 6, 2)) {
            window_.text(axisX, y, line, kCaution);
            y += 12;
        }
    }
    return y;
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
    // How strong is the wheel's own app's to say, for every game; WheelBri passes all the game asks.
    constexpr const char* kStrength = "How strong: set it in your wheel's own app (Moza Pit House, Fanatec, Simucube, "
                                      "Thrustmaster, Logitech G HUB). WheelBri passes all of what the game asks.";
    for (const std::string& line : wrap(kStrength, gameChars(), 2)) {
        window_.text(gameX(), y, line, kDim);
        y += 12;
    }
    y += 4;
    // What the game's own profile changes here (NFS Payback: its pushing forces x3, the Steering force and Vibration
    // rows), so it is not only a number below.
    if (game.ffbGain.has_value()) {
        const std::string times = tuning::formatNumber(*game.ffbGain, 2);   // e.g. "3"
        const bool overridden = own != nullptr && (own->ffbGains[static_cast<std::size_t>(FfbGroup::Constant)] ||
                                                   own->ffbGains[static_cast<std::size_t>(FfbGroup::Periodic)]);
        const std::string line =
            "Its game profile: Steering force and Vibration x" + times + " (" + game.ffbNote + ")" +
            (overridden ? "; your own values below replace it" : "; Default below goes back to it");
        for (const std::string& part : wrap(line, gameChars(), 2)) {
            window_.text(gameX(), y, part, kCaution);
            y += 12;
        }
        y += 4;
    }
    const auto row = [&](const std::string& label, const TuningField field, const std::string& value,
                         const bool mine, const std::string& about, std::function<void()> reset) {
        window_.text(gameX(), y + 4, label, kText);
        const bool changed = tuningChanged(id) && mine;
        drawField(gameValueX(), y, 72, Field::Tuning, value, changed, static_cast<std::size_t>(field));
        button(gameValueX() + 80, y, "Default", std::move(reset), false, editable() && mine, 72);
        window_.text(gameValueX() + 160, y + 4, fit(about, valueChars() - 20), kDim);
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
            const float barX = gameValueX() + 160;
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
    window_.text(gameValueX(), y, fit("0 turns that kind off, 1 as the game sends it, up to 4", valueChars()), kDim);
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
        window_.text(gameX(), y + 4, label, kText);
        button(gameValueX(), y, onOff(value), std::move(flip), mine, editable(), 72);
        button(gameValueX() + 80, y, "Default", std::move(reset), false, editable() && mine, 72);
        window_.text(gameValueX() + 160, y + 4, fit(about, valueChars() - 20), kDim);
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

int MappingEditView::moveOldLimits() {
    // Every one of the user's own mappings (my-*.ini, the older per-mode ones too), not only the shown target's.
    const std::filesystem::path folder = picker_.ownPath().parent_path();
    std::error_code ec;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec)) return 0;
    int moved = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        const std::filesystem::path file = entry.path();
        const std::string name = pathToUtf8(file.filename());
        if (!entry.is_regular_file(ec) || name.rfind("my-", 0) != 0 || file.extension() != ".ini") continue;
        const std::optional<std::string> text = readFileText(file);
        if (!text) continue;
        const std::optional<std::string> next = movedFromOldLimits(*text);
        if (!next) continue;
        if (!writeFileReplacing(file, *next)) {
            WE_LOG_WARN(log(), "mapping: cannot write {}: its force limits stay at the older 0.70 / 0.80",
                        pathToUtf8(file));
            continue;
        }
        WE_LOG_INFO(log(), "mapping: force limits in {} moved from the older strength 0.70, cap 0.80 to {:.2f}, "
                           "{:.2f}: the wheel's own app sets how strong",
                    pathToUtf8(file), kWheelAppMasterGain, kWheelAppMaxMagnitude);
        ++moved;
    }
    return moved;
}

float MappingEditView::drawGameButtons(const std::size_t index, float y) {
    // Only the buttons changed for this game, under the target's own groups (in the Mapping tab's order); Add button...
    // adds one. Each target keeps its own (my-<target>.ini), so this is the shown target's list.
    const GameProfile& game = games_[index];
    const std::string id = game.id;
    const GameTuning* const own = tuningOf(id);
    const std::string target = profile_ != nullptr ? profile_->identity().shortName : targetName_;
    gameSection(y, "BUTTONS", "only the ones you change for this game; the rest: Mapping tab");
    y += 16;
    if (profile_ == nullptr) return y;

    // The changed buttons, group by group; one in no group (or with no groups at all) at the end.
    struct Group {
        std::string name;
        std::vector<std::size_t> buttons;
    };
    std::vector<Group> groups;
    std::vector<bool> shown(profile_->buttonCount(), false);
    const auto changed = [own](const std::size_t b) {
        return own != nullptr && own->buttons.count(static_cast<int>(b)) != 0;
    };
    for (const ButtonGroup& group : profile_->buttonGroups()) {
        Group mine{group.name, {}};
        for (const std::size_t b : group.buttons) {
            if (b < shown.size() && !shown[b] && changed(b)) {
                mine.buttons.push_back(b);
                shown[b] = true;
            }
        }
        if (!mine.buttons.empty()) groups.push_back(std::move(mine));
    }
    Group rest{profile_->buttonGroups().empty() ? std::string() : std::string("OTHER"), {}};
    for (std::size_t b = 0; b < shown.size(); ++b) {
        if (!shown[b] && changed(b)) rest.buttons.push_back(b);
    }
    if (!rest.buttons.empty()) groups.push_back(std::move(rest));

    const float labelW = 120;
    const float removeW = 72;
    const float fieldW = std::floor((canvasW() - gameX() - 4 - labelW - removeW - 16) / 2);
    const float mappingX = gameX() + labelW;
    const float gameColumnX = mappingX + fieldW + 8;
    const auto chars = static_cast<std::size_t>((fieldW - 12) / ui::kCharWidth);
    if (groups.empty()) {
        for (const std::string& line : wrap("No button changed for this game: every " + target +
                                                " button comes from the Mapping tab.",
                                            gameChars(), 2)) {
            window_.text(gameX(), y + 4, line, kDim);
            y += 12;
        }
        y += 12;
    } else {
        window_.text(gameX(), y, target + " button", kDim);
        window_.text(mappingX, y, "Mapping tab", kDim);
        window_.text(gameColumnX, y, "this game", kDim);
        y += 14;
    }
    const float bottom = contentH() - 52 - 64;   // room for Add button... and the notes under it
    std::size_t hidden = 0;
    for (const Group& group : groups) {
        if (!group.name.empty()) {
            if (y > bottom) {
                hidden += group.buttons.size();
                continue;
            }
            window_.text(gameX(), y + 4, group.name, kDim);
            y += kButtonHeadingH;
        }
        for (const std::size_t b : group.buttons) {
            if (y > bottom) {
                ++hidden;
                continue;
            }
            char label[32];
            std::snprintf(label, sizeof(label), "%2d %.10s", gameNumber(b),
                          std::string(profile_->buttonName(b)).c_str());
            window_.text(gameX() + 4, y + 4, label, kText);
            // What the Mapping tab gives it: shown, not changed here.
            const SourceRef mapped = edit_.buttons[b];
            window_.fill(mappingX, y, fieldW, 16, kStripe);
            window_.text(mappingX + 6, y + 4, fit(describe(mapped, false), chars), kDim);
            if (mapped.mapped()) tip(mappingX, y, fieldW, 16, describeFull(mapped, false));
            // This game's: amber, click to change it.
            const SourceRef source = own->buttons.at(static_cast<int>(b));
            window_.fill(gameColumnX, y, fieldW, 16, kStripe);
            window_.outline(gameColumnX, y, fieldW, 16, kCaution);
            window_.text(gameColumnX + 6, y + 4, fit(source.mapped() ? describe(source, false) : "nothing", chars),
                         source.mapped() ? kText : kDim);
            if (source.mapped()) tip(gameColumnX, y, fieldW, 16, describeFull(source, false));
            if (editable()) {
                hit(gameColumnX, y, fieldW, 16, [this, b] {
                    mode_ = Mode::Pick;
                    pickKind_ = Slot::GameButton;
                    pickIndex_ = b;
                });
            }
            button(gameColumnX + fieldW + 8, y, "Remove", [this, id, b] {
                tuningFor(id).buttons.erase(static_cast<int>(b));
                dropEmptyTuning(id);
            }, false, editable(), removeW);
            y += kButtonRowH;
        }
    }
    if (hidden > 0) {
        window_.text(gameX(), y + 4, std::to_string(hidden) + " more not shown: a bigger window shows them.", kCaution);
        y += 16;
    }
    y += 8;
    const std::size_t named = layoutButtons(*profile_).cells.size();
    button(mappingX, y, "Add button...", [this] { mode_ = Mode::AddButton; }, false, editable() && named > 0, 120);
    const std::size_t helpChars = gameChars() - 15;   // from mappingX
    window_.text(mappingX + 132, y + 4, fit("pick a " + target + " button, then the control for it", helpChars - 16),
                 kDim);
    y += 28;
    window_.text(mappingX, y,
                 fit("Click a \"this game\" field to change it. Remove: back to the Mapping tab.", helpChars), kDim);
    window_.text(mappingX, y + 12,
                 fit("Grouped as in the Mapping tab. Each target keeps its own list.", helpChars), kDim);
    return y + 24;
}

void MappingEditView::drawAddButton() {
    hit(0, 0, canvasW(), contentH(), [] {});   // clicks outside the popup do nothing
    window_.fill(kPopupX, kPopupY, popupW(), popupH(), style::kBox);
    window_.outline(kPopupX, kPopupY, popupW(), popupH(), kHeading);
    const float left = kPopupX + 12;
    const std::size_t picked = selectedGameIndex();
    if (profile_ == nullptr || picked >= games_.size()) {
        mode_ = Mode::Idle;
        return;
    }
    const GameProfile& game = games_[picked];
    const GameTuning* const own = tuningOf(game.id);
    const std::string target = profile_->identity().shortName;
    window_.text(left, kPopupY + 10, "Add button: which " + target + " button should come from another control in " +
                                         game.label + "?", kHeading);
    button(kPopupX + popupW() - 100, kPopupY + 6, "Cancel", [this] { mode_ = Mode::Idle; }, false, true, 88);
    window_.text(left, kPopupY + 32, "Then pick its control (or nothing). Amber: already yours for this game.", kDim);

    // The target's buttons where the Mapping tab has them: two columns of groups, the shifter's gears below.
    const ButtonLayout layout = layoutButtons(*profile_);
    const float columnW = (popupW() - 24) / 2;
    const float top = kPopupY + 56;
    for (const ButtonCell& cell : layout.cells) {
        const float x = left + static_cast<float>(cell.column) * columnW;
        const float y = top + cell.y;
        if (y > kPopupY + popupH() - 24) break;
        if (cell.heading) {
            window_.text(x, y + 3, cell.text, kDim);
            continue;
        }
        const std::size_t b = cell.button;
        const bool mine = own != nullptr && own->buttons.count(static_cast<int>(b)) != 0;
        const float w = columnW - 12;
        window_.fill(x, y, w, 16, kPanel);
        if (mine) window_.outline(x, y, w, 16, kCaution);
        const std::string text =
            std::to_string(gameNumber(b)) + " " + std::string(profile_->buttonName(b)) + (mine ? "  yours" : "");
        window_.text(x + 4, y + 4, fit(text, static_cast<std::size_t>((w - 8) / ui::kCharWidth)),
                     mine ? kCaution : kText);
        tip(x, y, w, 16, "Mapping tab: " + (edit_.buttons[b].mapped() ? describeFull(edit_.buttons[b], false)
                                                                      : std::string("nothing")));
        hit(x, y, w, 16, [this, b] {
            mode_ = Mode::Pick;
            pickKind_ = Slot::GameButton;
            pickIndex_ = b;
        });
    }
}

MappingEditView::GameFilePlace MappingEditView::gameFilePlace(const GameProfile& game) const {
    // The one the user located (app state: game_file.<id>) unless the game was seen writing another, else the one
    // it was seen writing, else the profile's under Documents (Windows only).
    const std::string seen = readAppSetting(stateFile_, "game_file_seen." + game.id);
    if (const std::string located = readAppSetting(stateFile_, "game_file." + game.id); !located.empty()) {
        const std::filesystem::path file = pathFromUtf8(located);
        const bool confirmed = settingsFileFits(file, game.settingsFile) ||
                               (!seen.empty() && sameSettingsFile(file, pathFromUtf8(seen)));
        // One the game never wrote gives way to the one it did (NFS Heat's, located for NFS Unbound by mistake).
        if (confirmed || seen.empty()) return {file.parent_path(), pathToUtf8(file.filename()), true, confirmed};
    }
    const std::filesystem::path documents = documentsFolder();
    if (!seen.empty() && !(!documents.empty() && sameSettingsFile(pathFromUtf8(seen),
                                                                  documents / pathFromUtf8(game.settingsFile)))) {
        const std::filesystem::path file = pathFromUtf8(seen);
        return {file.parent_path(), pathToUtf8(file.filename()), true, true};
    }
    return {documents, game.settingsFile, false, true};
}

void MappingEditView::setGameNow(GameNow now) {
    if (now.id != watchedGame_) {   // a game ended (or the session did), or one started
        if (!watchedGame_.empty()) learnGameFile(watchedGame_, watchedSince_);
        watchedGame_ = now.id;
        // Seen every 2 s: from a little before it was first seen running.
        watchedSince_ = std::filesystem::file_time_type::clock::now() -
                        std::chrono::milliseconds(static_cast<std::int64_t>(now.forMs)) - std::chrono::seconds(5);
    }
    now_ = std::move(now);
}

void MappingEditView::learnGameFile(const std::string& id, const std::filesystem::file_time_type since) {
    const auto game = std::find_if(games_.begin(), games_.end(), [&id](const GameProfile& g) { return g.id == id; });
    if (game == games_.end() || game->settings.empty() || game->settingsFile.empty()) return;
    // Still running (the session stopped, not the game): it writes its file when it ends, not yet.
    if (const auto programs = runningPrograms()) {
        for (const RunningProgram& program : *programs) {
            if (programName(program.name) == game->program) return;
        }
    }
    const std::string located = readAppSetting(stateFile_, "game_file." + id);
    const std::filesystem::path locatedFile = located.empty() ? std::filesystem::path() : pathFromUtf8(located);
    const std::vector<std::filesystem::path> written =
        settingsFilesWrittenBetween(documentsFolder(), game->settingsFile, locatedFile, since,
                                    std::filesystem::file_time_type::clock::now() + std::chrono::seconds(5));
    if (written.size() != 1) {
        WE_LOG_INFO(log(), "game settings: {} ended; {} settings files written while it ran: none taken as its own",
                    game->label, written.size());
        return;
    }
    const std::string file = pathToUtf8(written.front());
    if (readAppSetting(stateFile_, "game_file_seen." + id) == file) return;
    if (!writeAppSetting(stateFile_, "game_file_seen." + id, file)) {
        WE_LOG_WARN(log(), "game settings: could not keep {}'s settings file in {}", game->label,
                    pathToUtf8(stateFile_));
        return;
    }
    fileState_.reset();
    WE_LOG_INFO(log(), "game settings: {} wrote {} while it ran: its settings file", game->label, file);
}

std::vector<GameSettingText> MappingEditView::gameValues(const GameProfile& game) const {
    // As shown (saved or not): the game's own rotation if it has one, else the Mapping tab's.
    const float steeringDeg = rotationWhileRunning(game, edit_.gameRotations, profileFitsTarget(game, targetName_))
                                  .value_or(edit_.steeringRotationDeg);
    return gameSettingTexts(game, edit_.gameSettings, steeringDeg);
}

void MappingEditView::locateGameFile(const std::size_t index) {
    const GameProfile& game = games_[index];
    const std::filesystem::path name = pathFromUtf8(game.settingsFile).filename();
    std::filesystem::path start = documentsFolder();
    if (!start.empty()) {
        // Its folder under Documents when that is there, else Documents.
        std::error_code ec;
        const std::filesystem::path folder = (start / pathFromUtf8(game.settingsFile)).parent_path();
        if (std::filesystem::is_directory(folder, ec)) start = folder;
    }
    const std::optional<std::filesystem::path> picked =
        pickFile("Locate " + game.label + "'s settings file (" + pathToUtf8(name) + ")", start,
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
    saveAnyway_.clear();
    // Its own for sure only if the game was seen writing it (or it is where its profile says): another game's file
    // may have the same name and settings (NFS Heat's for NFS Unbound).
    if (gameFilePlace(game).confirmed) {
        status_ = "Found " + game.label + "'s settings file: Save to file writes the values into it.";
        statusError_ = false;
    } else {
        status_ = "Kept, but " + game.label + " has not been seen writing this file (it may be another game's): play " +
                  game.label + " once while WheelBri runs, or Save to file asks first.";
        statusError_ = true;
    }
    WE_LOG_INFO(log(), "game settings: {}: settings file located: {} ({})", game.label, pathToUtf8(*picked),
                gameFilePlace(game).confirmed ? "its own" : "not seen written by the game yet");
}

void MappingEditView::saveGameFile(const std::size_t index) {
    const GameProfile& game = games_[index];
    // The game reads the file when it starts and writes its own values back when it ends: only with it closed.
    if (const auto programs = runningPrograms()) {
        for (const RunningProgram& program : *programs) {
            if (programName(program.name) == game.program) {
                status_ = game.label + " is running: close it first, then Save to file.";
                statusError_ = true;
                return;
            }
        }
    }
    const GameFilePlace place = gameFilePlace(game);
    const std::string where = pathToUtf8(place.root / pathFromUtf8(place.relative));
    if (!place.confirmed && saveAnyway_ != where) {   // maybe another game's: the second press writes it
        saveAnyway_ = where;
        status_ = game.label + " has not been seen writing this file: it may be another game's (NFS Heat's holds the " +
                  "same settings). Save to file again to write it anyway.";
        statusError_ = true;
        return;
    }
    saveAnyway_.clear();
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
    window_.outline(x, y, w, 16, twice ? kWarning : changed ? kCaution : style::kFieldEdge);
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
    window_.fill(kPopupX, kPopupY, popupW(), popupH(), style::kBox);
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
                     ". Click a control below to add or remove it; an axis' row sets where it presses.",
                 kDim);

    // The ones it has, as chips (at most 8, so two rows of 5 or one).
    const int kColumns = popupColumns();
    const float kChipW = entryW(kColumns);
    const auto chipsPerRow = static_cast<std::size_t>(kColumns);
    if (altFocus_ >= list.size()) altFocus_ = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const float x = left + static_cast<float>(i % chipsPerRow) * (kChipW + 4);
        const float y = kPopupY + 48 + static_cast<float>(i / chipsPerRow) * 18;
        window_.fill(x, y, kChipW, 16, kStripe);
        window_.outline(x, y, kChipW, 16, kHeading);
        window_.text(x + 4, y + 4, fit(shortAlt(list[i]), static_cast<std::size_t>((kChipW - 8) / ui::kCharWidth)),
                     kText);
        tip(x, y, kChipW, 16, list[i].axis ? describeAlt(list[i]) : describeFull(list[i].source, false));
    }
    const DeviceLabels* const wheel = wheelLabels();
    std::vector<const DeviceLabels*> order;
    if (wheel != nullptr) order.push_back(wheel);
    for (const DeviceLabels& device : devices_) {
        if (&device != wheel) order.push_back(&device);
    }
    float y = kPopupY + 88;
    const float kEntryW = kChipW;
    const float bottom = kPopupY + popupH() - 40;   // the last row ends above the note at the bottom
    if (devices_.empty()) {
        window_.text(left, y, "The rig has no devices yet: define them in the Rig tab.", kCaution);
    }
    const ControlUse self{ControlUse::Kind::Secondary, target};
    // A control's entry: pressed, it is added (an axis: its row's slider comes alive) or, when it is one already,
    // taken off.
    const auto entryAt = [&](const float x, const float entryY, const RigDevice& rigDevice, const bool isWheel,
                             const bool axis, const std::size_t index) {
        const SourceRef source{isWheel ? DeviceId{} : rigDevice.id, static_cast<int>(index)};
        EntryState state = entryState({source, axis}, self);
        if (!state.mine && !state.taken && list.size() >= max) {
            state.taken = true;   // full: nothing more is added
            state.tip = "At most " + std::to_string(max) + " secondary assignments per button";
        }
        const RigControl& control = axis ? rigDevice.axes[index] : rigDevice.buttons[index];
        const ButtonRef ref{source, axis, false, 0.5F};
        drawEntry(x, entryY, kEntryW, (axis ? "axis " : "") + std::to_string(index) + " " + controlName(control), state,
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
                      if (ref.axis) altFocus_ = list.size() - 1;
                  });
        return source;
    };
    // Each device's name line inside a section.
    const auto deviceLine = [&](const DeviceLabels& device, const bool isWheel) {
        window_.text(left + 8, y + 2,
                     fit(deviceName(device), 40) + "  " + deviceId(device.vendorId, device.productId) + "  " +
                         (isWheel ? "(the wheel)" : deviceKindName(device.kind)),
                     kText);
        y += 14;
    };

    // AXES: every device's named axes, one a row: its entry, then the slider of the travel it presses past (0% as
    // soon as it moves, 100% only at its end), the number typed in, and which way round it reads. Live once the axis
    // is added; greyed before.
    window_.text(left, y + 2, "AXES", kHeading);
    y += 16;
    thresholdSliderX_ = left + kEntryW + 12 + 24;
    thresholdSliderW_ = std::max(100.0F, popupW() - 24 - (kEntryW + 12) - 24 - 38 - 56 - 8 - 88 - 8);
    for (const DeviceLabels* const entry : order) {
        const RigDevice* const rigDevice = rig_.find(idOf(*entry));
        if (rigDevice == nullptr) continue;
        if (std::none_of(rigDevice->axes.begin(), rigDevice->axes.end(),
                         [](const RigControl& c) { return c.defined(); })) {
            continue;
        }
        if (y > bottom) break;
        const bool isWheel = wheel != nullptr && idOf(*entry) == idOf(*wheel);
        deviceLine(*entry, isWheel);
        for (std::size_t a = 0; a < rigDevice->axes.size(); ++a) {
            if (!rigDevice->axes[a].defined() || y > bottom) continue;
            const SourceRef source = entryAt(left, y, *rigDevice, isWheel, true, a);
            std::size_t mine = list.size();
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (list[i].axis && list[i].source == source) mine = i;
            }
            const bool live = mine < list.size();
            const float sliderX = thresholdSliderX_;
            const float sliderW = thresholdSliderW_;
            const float threshold = live ? std::clamp(list[mine].threshold, 0.0F, 1.0F) : 0.0F;
            window_.text(sliderX - 24, y + 4, "0%", kDim);
            window_.fill(sliderX, y + 6, sliderW, 4, live ? kStripe : kPanel);
            if (live) {
                window_.fill(sliderX, y + 6, threshold * sliderW, 4, kHeading);
                window_.fill(sliderX + threshold * sliderW - 3, y + 1, 6, 14, kText);
                hit(sliderX - 4, y, sliderW + 8, 16, [this, mine] {
                    altFocus_ = mine;
                    setThresholdFromX(clickX_);   // the press sets it; holding the button drags it (onPointer)
                    thresholdDragging_ = true;
                });
                tip(sliderX - 4, y, sliderW + 8, 16,
                    "0%: pressed as soon as it moves; 100%: only at its end. Released 5% below (at rest near 0%).");
            } else {
                tip(sliderX - 4, y, sliderW + 8, 16, "Press the axis on the left to add it, then set where it presses");
            }
            const float valueX = sliderX + sliderW + 6;
            window_.text(valueX, y + 4, "100%", kDim);
            if (live) {
                drawField(valueX + 38, y, 56, Field::Threshold, tuning::formatPercent(list[mine].threshold), false,
                          mine);
            } else {
                window_.fill(valueX + 38, y, 56, 16, kPanel);
                window_.text(valueX + 44, y + 4, "--", kDim);
            }
            // Which way round it reads (a lever released at the top): here, not in the rig.
            const float invertX = valueX + 38 + 56 + 8;
            button(invertX, y, "Inverted", [&list, mine] {
                if (mine < list.size()) list[mine].invert = !list[mine].invert;
            }, live && list[mine].invert, live, 88);
            if (live) tip(invertX, y, 88, 16, "On: the axis rests at the top (+1) and is pressed towards the bottom.");
            y += 18;
        }
        y += 4;
    }

    // BUTTONS: every device's named buttons, a grid each.
    y += 4;
    window_.text(left, y + 2, "BUTTONS", kHeading);
    y += 16;
    for (const DeviceLabels* const entry : order) {
        const RigDevice* const rigDevice = rig_.find(idOf(*entry));
        if (rigDevice == nullptr) continue;
        if (std::none_of(rigDevice->buttons.begin(), rigDevice->buttons.end(),
                         [](const RigControl& c) { return c.defined(); })) {
            continue;
        }
        if (y > bottom) break;
        const bool isWheel = wheel != nullptr && idOf(*entry) == idOf(*wheel);
        deviceLine(*entry, isWheel);
        std::size_t shown = 0;
        for (std::size_t b = 0; b < rigDevice->buttons.size(); ++b) {
            if (!rigDevice->buttons[b].defined()) continue;
            const float entryY = y + static_cast<float>(shown / static_cast<std::size_t>(kColumns)) * 18;
            if (entryY <= bottom) {
                (void)entryAt(left + static_cast<float>(shown % static_cast<std::size_t>(kColumns)) * (kEntryW + 4),
                              entryY, *rigDevice, isWheel, false, b);
            }
            ++shown;
        }
        y += static_cast<float>((shown + static_cast<std::size_t>(kColumns) - 1) / static_cast<std::size_t>(kColumns)) *
                 18 + 4;
    }
    window_.text(left, kPopupY + popupH() - 20,
                 "Greyed: used elsewhere (hover to see where). Axes: a pedal or a lever, pressed past the threshold.",
                 kDim);
}

void MappingEditView::drawPicker() {
    hit(0, 0, canvasW(), contentH(), [] {});   // clicks outside the popup do nothing
    window_.fill(kPopupX, kPopupY, popupW(), popupH(), style::kBox);
    window_.outline(kPopupX, kPopupY, popupW(), popupH(), kHeading);
    const bool axis = pickKind_ == Slot::Axis;
    const bool forGame = pickKind_ == Slot::GameButton;
    const std::string what = pickKind_ == Slot::Axis ? kAxisNames[pickIndex_]
                             : pickKind_ == Slot::Hat ? std::string("D-pad ") + kHatNames[pickIndex_]
                                                      : "button " + std::to_string(gameNumber(pickIndex_)) + " " +
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
        window_.text(left, y, "The rig has no devices yet: define them in the Rig tab.", kCaution);
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
            drawEntry(x, y, kEntryW, text, state, [this, source] {
                setPicked(source);   // which way round stays the row's Invert
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
