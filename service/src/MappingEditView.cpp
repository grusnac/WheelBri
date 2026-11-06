// SPDX-License-Identifier: GPL-3.0-or-later
#include "MappingEditView.hpp"
#include "MappingEditViewParts.hpp"
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
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <utility>

namespace wheelbri::service {

using namespace editview;

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
    // The order is the top row's (each has its own file; WheelBri's default serves both): saved with the mapping.
    if (profile_->hasNativeButtonOrder()) saved_.buttonOrder = picker_.buttonOrder();
    rigNotices_ = followRig(saved_, rig_, *profile_);
    edit_ = saved_;
    readRenumbered();
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
               (picker_.buttonOrder() == ButtonOrder::Native ? " with Native button order" : "") +
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
        dropChangedRenumbered(saved_, edit_);
        saved_ = edit_;
    }
    status_ = "Saved. The next Start uses it.";
    statusError_ = false;
    toast_ = page_ == Page::Games ? std::string() : "Mapping saved";   // the Games page says its own
}

void MappingEditView::autoSave() {
    if (!loaded_ || !dirty()) return;   // while a session runs too: it takes the mapping at once (LiveMapping.hpp)
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
    status_ = undoing        ? (inUseNow() ? "Undone and saved: in use now." : "Undone and saved.")
              : inUseNow()   ? "Saved: in use now."
              : locked_      ? "Saved. The next Start uses it."
                             : "Saved. Start uses it.";
    if (!undoing) {
        undo_.push_back(before);
        if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    }
}

void MappingEditView::undo() {
    if (!loaded_) return;
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
           edit_.gameTunings != saved_.gameTunings;
}

// The Games page has no Save: its values are saved as they change, and only they (the Mapping tab's unsaved
// changes stay unsaved, still there to Save or Revert).
void MappingEditView::saveGameValues() {
    MappingModel pending = edit_;
    edit_ = saved_;
    edit_.gameRotations = pending.gameRotations;
    edit_.gameSettings = pending.gameSettings;
    edit_.gameTunings = pending.gameTunings;
    save();   // saved_ is then this (or read again from the user's own file, made from the default)
    if (statusError_) {
        // Not saved: the value goes back to the saved one (not tried again every frame), and the page says why.
        pending.gameRotations = saved_.gameRotations;
        pending.gameSettings = saved_.gameSettings;
        pending.gameTunings = saved_.gameTunings;
    } else {
        status_ = inUseNow() ? "Saved: in use now." : "Saved. The next Start uses it.";
        toast_ = inUseNow() ? "Saved: in use now" : "Saved";
    }
    edit_ = std::move(pending);
}

void MappingEditView::readRenumbered() {
    renumbered_.clear();
    if (profile_ == nullptr || !profile_->hasNativeButtonOrder() || picker_.buttonOrder() != ButtonOrder::Native) {
        return;
    }
    const std::string key = ConfigPicker::renumberedKey(picker_.target());
    const std::string kept = readAppSetting(stateFile_, key);
    if (kept == "pending") {
        // Just copied: every button the copy maps, whose number the game sees changed with the order.
        for (std::size_t b = 0; b < profile_->buttonCount() && b < edit_.buttons.size(); ++b) {
            const bool mapped = edit_.buttons[b].mapped() || !edit_.secondaries[b].empty();
            if (mapped && gameButtonNumber(*profile_, ButtonOrder::Driver, b) !=
                              gameButtonNumber(*profile_, ButtonOrder::Native, b)) {
                renumbered_.push_back(b);
            }
        }
        writeRenumbered();
        return;
    }
    std::size_t at = 0;
    while (at < kept.size()) {
        const std::size_t end = std::min(kept.find(',', at), kept.size());
        std::size_t b = 0;
        const char* const first = kept.data() + at;
        const char* const last = kept.data() + end;
        const auto [stop, error] = std::from_chars(first, last, b);
        if (error == std::errc() && stop == last && b < profile_->buttonCount() && !renumbered(b)) {
            renumbered_.push_back(b);
        }
        at = end + 1;
    }
    std::sort(renumbered_.begin(), renumbered_.end());
}

void MappingEditView::writeRenumbered() {
    std::string list;
    for (const std::size_t b : renumbered_) list += (list.empty() ? "" : ",") + std::to_string(b);
    if (!writeAppSetting(stateFile_, ConfigPicker::renumberedKey(picker_.target()), list)) {
        WE_LOG_WARN(log(), "mapping: cannot save the marked buttons in {}", pathToUtf8(stateFile_));
    }
}

void MappingEditView::dropChangedRenumbered(const MappingModel& before, const MappingModel& after) {
    const auto changed = [&](const std::size_t b) {
        return before.buttons[b] != after.buttons[b] || before.secondaries[b] != after.secondaries[b];
    };
    const auto kept = std::remove_if(renumbered_.begin(), renumbered_.end(), changed);
    if (kept == renumbered_.end()) return;
    renumbered_.erase(kept, renumbered_.end());
    writeRenumbered();
}

bool MappingEditView::renumbered(const std::size_t b) const {
    return std::find(renumbered_.begin(), renumbered_.end(), b) != renumbered_.end();
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

bool MappingEditView::inUseNow() const { return locked_ && path_ == sessionPath_; }

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
    (void)hits_.dispatch(x, y);
}

void MappingEditView::onKey(const char key) {
    if (key != ui::kEscape) return;
    if (field_ != Field::None) {
        field_ = Field::None;   // Escape while typing: drop what was typed
        return;
    }
    if ((mode_ == Mode::PickAlt || mode_ == Mode::Pick) && !findText_.empty()) {
        findText_.clear();   // the find box first, then the popup
        listScroll_ = 0;
        return;
    }
    if (mode_ == Mode::PickAlt) {
        closeAltPicker();
        return;
    }
    mode_ = Mode::Idle;
}

void MappingEditView::onText(const std::string& text) {
    if (field_ == Field::None && (mode_ == Mode::PickAlt || mode_ == Mode::Pick)) {
        // The secondary popup's find box: printable characters, Backspace; Enter does nothing.
        for (const char c : text) {
            if (c == ui::kBackspace) {
                if (!findText_.empty()) findText_.pop_back();
            } else if (c >= ' ' && c <= '~' && findText_.size() < 24) {
                findText_ += c;
            }
        }
        listScroll_ = 0;
        return;
    }
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
        if (!leftDown || mode_ != Mode::PickAlt) {
            thresholdDragging_ = false;
            return;
        }
        setThresholdFromX(x);
        return;
    }
    if (!dragging_) return;
    if (!leftDown || mode_ != Mode::Idle) {
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
    std::vector<ButtonRef>& list = altList();
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
    hits_.add(x, y, w, h, std::move(action));
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
    window_.outline(x, y, w, 16, typing ? kHeading : changed ? kMine : style::kFieldEdge);
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
    window_.outline(x, y, w, 16, state.twice ? kWarning : changed ? kMine : style::kFieldEdge);
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
    const float room = std::max(0.0F, w - 12 - tagW);
    const std::string text = describe(source, kind == Slot::Axis);
    // A button's secondaries share the field: its own text first, the chips what is left (at least a "+N" chip).
    const float chipsW =
        kind == Slot::Button
            ? std::min(chipsWidth(edit_.secondaries[index]), std::max(room - textWidth(text) - 8, 5 * ui::kCharWidth))
            : 0.0F;
    const auto chars = static_cast<std::size_t>(std::max(0.0F, room - (chipsW > 0 ? chipsW + 8 : 0.0F)) /
                                                ui::kCharWidth);
    const std::string shown = fit(text, chars);
    window_.text(x + 6, y + 4, shown, missing(source, kind == Slot::Axis) ? kCaution : source.mapped() ? kText : kDim);
    hit(x, y, w, 16, [this, kind, index] { openPicker(kind, index); });
    if (kind == Slot::Button) {
        const bool changed = edit_.secondaries[index] != saved_.secondaries[index];
        if (changed && !state.twice) window_.outline(x, y, w, 16, kMine);
        drawChips(x + 6 + textWidth(shown) + 8, y, chipsW, edit_.secondaries[index], index, false,
                  [this, index] { openAltPicker(index); });
    }
}

float MappingEditView::textWidth(const std::string& text) {
    return static_cast<float>(text.size()) * ui::kCharWidth;
}

float MappingEditView::chipsWidth(const std::vector<ButtonRef>& list) const {
    float w = 0;
    for (const ButtonRef& ref : list) w += textWidth("or " + shortAlt(ref)) + 8 + kChipGap;
    return w > 0 ? w - kChipGap : 0.0F;
}

void MappingEditView::drawChips(const float x, const float y, const float w, const std::vector<ButtonRef>& list,
                                const std::optional<std::size_t> twiceFor, const bool dim,
                                const std::function<void()>& action) {
    if (list.empty() || w <= 0) return;
    std::string all;
    for (const ButtonRef& ref : list) all += (all.empty() ? "" : ", ") + describeAlt(ref);
    const std::string tipText = "Also pressed by: " + all + (action ? ". Click to change." : ".");
    const auto chip = [&](const float at, const float chipW, const std::string& text, const ui::Colour colour,
                          const bool twice) {
        window_.fill(at, y + 2, chipW, 12, kPanel);
        if (twice) window_.outline(at, y + 2, chipW, 12, kWarning);
        window_.text(at + 4, y + 4, text, colour);
        tip(at, y, chipW, 16, (twice ? "Used twice (red in the picker). " : "") + tipText);
        if (action) hit(at, y, chipW, 16, action);
    };
    float at = x;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const ButtonRef& ref = list[i];
        std::string text = "or " + shortAlt(ref);
        // What is left after this one: a "+N" chip for the rest when they do not all fit.
        const std::string rest = "+" + std::to_string(list.size() - i - 1);
        const float restW = i + 1 < list.size() ? textWidth(rest) + 8 + kChipGap : 0.0F;
        float chipW = textWidth(text) + 8;
        if (at + chipW + restW > x + w) {
            // Not whole: shortened if a few letters fit, else "+N" for this one and the rest.
            const float left = x + w - at - restW - 8;
            if (left >= 6 * ui::kCharWidth) {
                text = fit(text, static_cast<std::size_t>(left / ui::kCharWidth));
                chipW = textWidth(text) + 8;
            } else {
                const std::string more = "+" + std::to_string(list.size() - i);
                chip(at, textWidth(more) + 8, more, dim ? kDim : kText, false);
                return;
            }
        }
        const bool twice =
            twiceFor && entryState({ref.source, ref.axis}, ControlUse{ControlUse::Kind::Secondary, *twiceFor}).twice;
        chip(at, chipW, text, missing(ref.source, ref.axis) ? kCaution : dim ? kDim : kText, twice);
        at += chipW + kChipGap;
    }
}

void MappingEditView::drawPlus(const float x, const float y, const std::size_t index) {
    const bool on = hoverInside_ && hoverX_ >= x && hoverX_ < x + kPlusW && hoverY_ >= y && hoverY_ < y + 16;
    window_.fill(x, y, kPlusW, 16, on ? kHeading : style::kHeadingTint);
    window_.outline(x, y, kPlusW, 16, kHeading);
    window_.heading(x + (kPlusW - ui::kCharWidth) / 2, y + 4, "+", on ? kDark : kHeading);   // bold
    const std::size_t count = edit_.secondaries[index].size();
    tip(x, y, kPlusW, 16,
        count == 0 ? std::string("Add a second control that activates it too")
                   : "Add another control that activates it too (" + std::to_string(count) + " of " +
                         std::to_string(MapperConfig::kMaxSecondaries) + ")");
    hit(x, y, kPlusW, 16, [this, index] { openAltPicker(index); });
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
        window_.text(customX, 6, std::string(kCustom), kMine);
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
        // EDIT: this page changes the mapping, also while a session runs (each change in use at once,
        // LiveMapping.hpp); the Live tab shows it at work.
        window_.fill(66, 2, 40, 12, kPanel);
        window_.text(70, 4, "EDIT", kText);
        subtitle(114, !locked_     ? "what your controls do on the " + target
                      : inUseNow() ? "running: each change is in use at once (see it on Live, S)"
                                   : "running: changes are kept for the next Start");
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
        if (mode_ == Mode::PickAlt && altForGame()) drawAltPicker();
        if (mode_ == Mode::AddButton) drawAddButton();
        return;
    }
    picker_.draw(4, 18, !locked_, [this](const float x, const float y, const float w, const float h, std::string text) {
        tip(x, y, w, h, std::move(text));
    });
    // Saved as you change it: where Save was, whether it is ("saved", or "not saved" in red); Undo instead of Revert.
    // Not while a session runs: it deletes the file the session reads.
    button(defaultX, 2, "Default", [this] { dropOwnEdits(); }, false, !builtIn_ && !locked_, 104);
    const bool unsaved = loaded_ && dirty();
    const std::string saveState = unsaved ? "not saved" : "saved";
    window_.text(saveX + 80 - static_cast<float>(saveState.size()) * ui::kCharWidth, 6, saveState,
                 unsaved ? kWarning : kDim);
    tip(saveX, 2, 80, 16,
        unsaved ? "This change could not be saved: the line at the bottom says why. Undo drops it."
                : "Every change is saved at once; Start uses what you see. Accessories it uses must be plugged in to "
                  "start.");
    const bool canUndo = loaded_ && (unsaved || !undo_.empty());
    button(revertX, 2, "Undo", [this] { undo(); }, false, canUndo, 80);
    tip(revertX, 2, 80, 16, "Goes back one change, as many times as you like (until the Target changes).");
    tip(defaultX, 2, 104, 16,
        locked_ ? "Deletes your own mapping for this Target: once stopped (the running session reads it)."
                : "Deletes your own mapping for this Target: WheelBri's default mapping again.");
    if (!loaded_) {
        window_.text(4, 60, fit(status_, 118 + extraChars()), kWarning);
        // A way out: another target above, or straight back to the G920 (the one every build has).
        window_.text(4, 80, "Pick another Target above, or:", kDim);
        if (picker_.target() != ConfigPicker::Target::G920) {
            button(4 + 32 * ui::kCharWidth, 76, "Back to the G920",
                   [this] { picker_.useTarget(ConfigPicker::Target::G920); }, false, !locked_, 152);
        }
        return;
    }

    // No wheel line: there is one rig, and its wheel is device 0 (Rig::wheel, or the config's source_device).
    float y = 28 + picker_.rowsHeight();   // under the top rows (Target and Mode; Order for the G25 and G27)
    if (rig_.devices.empty()) {
        window_.text(4, y,
                     rigName_.empty() ? "No rig yet: define your devices and their controls in the Rig tab first."
                                      : "The rig has no devices yet: name their controls in the Rig tab.",
                     kCaution);
        y += 22;
    }

    // Axes, left; D-pad, right.
    style::headingBand(window_, 0, y, canvasW());
    style::heading(window_, 8, y, "AXES");
    window_.text(48, y, "click a source to pick another", kDim);
    tip(4, y - 2, 290, 12, "Sources are the rig's controls, named in the Rig tab: click one to pick what drives it.");
    // The axes' and the D-pad's source fields share what a wider window adds.
    const float half = extraW() / 2;
    window_.text(620 + half, y, "D-PAD", kHeading);
    for (std::size_t i = 0; i < kAxisNames.size(); ++i) {
        const float row = y + 16 + static_cast<float>(i) * 20;
        window_.text(4, row + 4, kAxisNames[i], kText);
        drawSource(4 + 8 * ui::kCharWidth + kLabelGap, row, 420 + half, Slot::Axis, i);   // "Steering"
        button(508 + half, row, "Invert", [this, i] { edit_.invert[i] = !edit_.invert[i]; }, edit_.invert[i], true, 70);
        tip(508 + half, row, 70, 16, "The axis the other way round: for a pedal that reads pressed at rest.");
        window_.text(620 + half, row + 4, kHatNames[i], kText);
        drawSource(620 + half + 5 * ui::kCharWidth + kLabelGap, row, 284 + half, Slot::Hat, i);   // "Right"
    }

    // The target's buttons (the ones its profile names; the others are not seen by games).
    y += 16 + 4 * 20 + 12;
    style::headingBand(window_, 0, y, canvasW());
    style::heading(window_, 8, y, "BUTTONS");
    window_.text(76, y, target + " button <- your control", kDim);
    tip(4, y - 2, 340, 12,
        "Each " + target + " button and the control that presses it; \"or\": a second one (a button, or an axis past a "
        "point, such as a handbrake lever).");
    if (!renumbered_.empty()) {
        // Copied from the Logitech driver mapping: the game numbers the amber rows by another number now.
        const float gotItW = 72;
        const float gotItX = canvasW() - 4 - gotItW;
        const float noteX = 76 + static_cast<float>(target.size() + 24) * ui::kCharWidth;
        window_.text(noteX, y,
                     fit("copied from Logitech: amber numbers are new, check them",
                         static_cast<std::size_t>(std::max(0.0F, gotItX - 8 - noteX) / ui::kCharWidth)),
                     kCaution);
        button(gotItX, y - 4, "Got it", [this] {
            renumbered_.clear();
            writeRenumbered();
        }, false, !locked_, gotItW);
        tip(gotItX, y - 4, gotItW, 16, "The numbers are checked: the amber marks go.");
    }
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
        const bool marked = renumbered(index);
        window_.text(x + 8, row + 4, label, marked ? kCaution : kText);
        if (marked) {
            tip(x + 8, row, static_cast<float>(kButtonLabelChars) * ui::kCharWidth, 16,
                "Button " + std::to_string(gameButtonNumber(*profile_, ButtonOrder::Driver, index)) +
                    " with Logitech, " + std::to_string(gameNumber(index)) +
                    " with Native: check the game's control for it. Changing this row takes the mark off.");
        }
        const float fieldX = x + 8 + static_cast<float>(kButtonLabelChars) * ui::kCharWidth + kLabelGap;
        // The source (a button), its secondaries as "or ..." chips inside the same field, and a "+" button at the row's
        // end that opens the secondary popup: on every row, tinted blue so a newcomer sees a button can have more.
        const float end = x + 468 + half;   // the row's right end
        drawSource(fieldX, row, end - kPlusW - 4 - fieldX, Slot::Button, index);
        drawPlus(end - kPlusW, row, index);
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
                                const EntryState& state, std::function<void()> action, const bool takenClicks) {
    const bool greyed = state.taken && !state.mine;
    window_.fill(x, y, w, 16, state.mine ? kHeading : greyed ? kDark : kPanel);
    if (state.twice) window_.outline(x, y, w, 16, kWarning);
    const auto chars = static_cast<std::size_t>((w - 8) / ui::kCharWidth);
    window_.text(x + 4, y + 4, fit(text, chars), state.mine ? kDark : greyed ? kDim : kText);
    if (!state.tip.empty()) tip(x, y, w, 16, state.tip);
    if ((!greyed || takenClicks) && action) hit(x, y, w, 16, std::move(action));
}

void MappingEditView::drawRotation(const float top) {
    float y = top;
    style::headingBand(window_, 0, y, canvasW());
    style::heading(window_, 8, y, "STEERING ROTATION");
    window_.text(160, y, "full lock to full lock, in degrees", kDim);

    // The steering rotation: the slider (90 to 1080), the typed number (up to 2520) and the usual values.
    y += 16;
    window_.text(4, y + 4, "Rotation", kText);
    const float shown = std::clamp(edit_.steeringRotationDeg, kSliderMinDeg, kSliderMaxDeg);
    const float filled = (shown - kSliderMinDeg) / (kSliderMaxDeg - kSliderMinDeg) * sliderW();
    window_.fill(kSliderX, y + 6, sliderW(), 4, kStripe);
    window_.fill(kSliderX, y + 6, filled, 4, kHeading);
    window_.fill(kSliderX + filled - 3, y + 1, 6, 14, kText);
    window_.outline(kSliderX, y + 6, sliderW(), 4,
                    edit_.steeringRotationDeg != saved_.steeringRotationDeg ? kMine : kPanel);
    hit(kSliderX - 4, y, sliderW() + 8, 16, [this] {
        setRotationFromX(clickX_);   // the press sets it; holding the button drags it (onPointer)
        dragging_ = true;
    });
    drawField(kSliderX + sliderW() + 12, y, 64, Field::Rotation, degrees(edit_.steeringRotationDeg),
              edit_.steeringRotationDeg != saved_.steeringRotationDeg);
    window_.text(kSliderX + sliderW() + 80, y + 4, "deg", kDim);
    float x = kSliderX + sliderW() + 112;
    for (const float preset : kPresetsDeg) {
        x += button(x, y, degrees(preset), [this, preset] { edit_.steeringRotationDeg = preset; },
                    std::fabs(edit_.steeringRotationDeg - preset) < 0.5F, true, 40) + 4;   // 7 fit in 960
    }
    window_.text(kSliderX, y + 20, "90", kDim);
    window_.text(kSliderX + sliderW() - 32, y + 20, "1080", kDim);

    // WheelBri passes the wheel through 1:1: the wheel's own software sets where its end stop is. One line; why, and
    // that a game can have its own rotation (the Games tab), in its hover text.
    y += 34;
    const std::string reminder = "Set " + degrees(edit_.steeringRotationDeg) +
                                 " deg in your wheel's own software too (Pit House for Moza).";
    window_.text(4, y, fit(reminder, 118 + extraChars()), kCaution);
    tip(0, y - 2, static_cast<float>(reminder.size()) * ui::kCharWidth + 8, 12,
        "WheelBri passes the wheel through 1:1 and never changes that setting, so the wheel's end stop is the game's "
        "full lock. A game can steer with its own rotation (NFS Payback: 180): the Games tab (G) sets it.");

    y += 30;
    if (profile_ != nullptr && profile_->identity().centringSpring) drawCentringSpring(y);
}

float MappingEditView::drawCentringSpring(const float top) {
    // The target's own (Logitech's software for the G25 and G27): a spring to the centre in every game, on top of the
    // game's forces. 0 is off (the checkbox unticked).
    float y = top;
    const std::string target(profile_->identity().shortName);
    style::headingBand(window_, 0, y, canvasW());
    style::heading(window_, 8, y, "CENTRING SPRING");
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

}  // namespace wheelbri::service
