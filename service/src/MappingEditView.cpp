#include "MappingEditView.hpp"

#include "Platform.hpp"

#include "wheelbri/AxisTuning.hpp"
#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/DeviceMatch.hpp"
#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/FileText.hpp"
#include "wheelbri/GameProfiles.hpp"
#include "wheelbri/GameRotation.hpp"
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

// The popups.
constexpr float kPopupX = 24;
constexpr float kPopupY = 56;
constexpr float kPopupW = 912;
constexpr float kPopupH = 640;


// The game full lock slider: 90 to 1080 degrees, in steps of 10 (typed values go up to tuning::kMaxRotationDeg).
constexpr float kSliderX = 132;
constexpr float kSliderW = 400;
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

// What a rig control is called in this tab: "Throttle/Gas" (its role / its name), one of them when they say the same
// ("A", "D-pad Up" named "Up") or it has only one; empty when it has neither (the caller says its number).
std::string controlName(const RigControl& control) {
    const bool plain = control.role == ControlRole::None || control.role == ControlRole::Button ||
                       control.role == ControlRole::OtherAxis;
    const std::string role = plain ? std::string() : roleLabel(control.role);
    const auto lower = [](std::string text) {
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    if (role.empty()) return control.label;
    // The same, or the name is the role's last word ("D-pad Up" named "Up"): the role alone.
    const std::string label = lower(control.label);
    const std::string full = lower(role);
    if (label.empty() || full == label ||
        (full.size() > label.size() && full.compare(full.size() - label.size(), label.size(), label) == 0 &&
         full[full.size() - label.size() - 1] == ' ')) {
        return role;
    }
    return role + "/" + control.label;
}

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
        status_ = std::string("No config: config/") + ConfigPicker::builtInName(picker_.way()) +
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
        text = std::string("; WheelBri: your own mapping for the ") + ConfigPicker::name(picker_.way()) +
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
    // shows it (pick another, or Auto-map).
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

void MappingEditView::autoMapFromRig() {
    if (profile_ == nullptr) return;
    const AutoMapResult result = autoMap(edit_, rig_, *profile_);
    WE_LOG_INFO(log(), "mapping: auto-mapped {} source(s) from the rig '{}'", result.set, rigName_);
    for (const std::string& note : result.notes) WE_LOG_INFO(log(), "mapping: auto-map: {}", note);
    // What still reads a control the rig has not named (amber in the tab): the user's to pick or name.
    std::size_t unnamed = 0;
    for (std::size_t i = 0; i < edit_.axes.size(); ++i) unnamed += missing(edit_.axes[i], true) ? 1 : 0;
    for (std::size_t i = 0; i < edit_.hat.size(); ++i) unnamed += missing(edit_.hat[i], false) ? 1 : 0;
    for (std::size_t i = 0; i < edit_.buttons.size(); ++i) {
        if (!profile_->buttonName(i).empty()) unnamed += missing(edit_.buttons[i], false) ? 1 : 0;
    }
    if (result.set == 0) {
        if (rigUnsaved_) {
            status_ = "Nothing to auto-map in the saved rig, and the Rig tab has unsaved changes: Save it there first.";
            statusError_ = true;
            return;
        }
        status_ = "Nothing to auto-map: give the controls roles (steering, throttle, Y, d-pad up ...) in the Rig tab.";
        statusError_ = true;
        return;
    }
    status_ = "Auto-mapped " + std::to_string(result.set) + " from the rig's roles";
    if (!result.notes.empty()) {
        status_ += "; " + result.notes.front() +
                   (result.notes.size() > 1 ? " (+" + std::to_string(result.notes.size() - 1) + " more, Logs)" : "");
    }
    if (unnamed > 0) status_ += "; " + std::to_string(unnamed) + " amber: not in the rig";
    status_ += ". Then Save.";
    statusError_ = false;
    if (rigUnsaved_) {
        status_ = "The Rig tab has unsaved changes: this used the saved rig. Save it there, then Auto-map again.";
        statusError_ = true;
    }
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
    // "Pedals: Throttle/Gas": the control's role (what it is for) / its name; one of them when they say the same
    // ("Moza R3: A") or it has only one; its number when it has neither.
    const RigControl* const control = rigControl(source, axis);
    std::string name = control != nullptr ? controlName(*control) : std::string();
    if (name.empty()) name = number;
    if (missing(source, axis)) name += "  (not in the rig)";
    return fit(deviceText(source), 20) + ": " + name;
}

std::string MappingEditView::describeFull(const SourceRef& source, const bool axis) const {
    if (!source.mapped()) return {};
    // "Moza R3: A  (button 0, role A)": the hover text, with what the short form leaves out.
    std::string text = deviceText(source) + ": ";
    const std::string number = (axis ? "axis " : "button ") + std::to_string(source.index);
    const RigControl* const control = rigControl(source, axis);
    text += control != nullptr && !control->label.empty() ? "'" + control->label + "', " + number : number;
    if (control != nullptr && control->role != ControlRole::None) text += ", role " + roleLabel(control->role);
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

void MappingEditView::changeWheel(const DeviceLabels& wheel) {
    const DeviceLabels* const old = wheelLabels();
    const DeviceId newId = idOf(wheel);
    // Sources on the old wheel keep pointing at it (now as an accessory); the new wheel's become the wheel's.
    const auto move = [&](SourceRef& source) {
        if (source.mapped() && source.device.empty() && old != nullptr) source.device = idOf(*old);
        if (source.device == newId) source.device = {};
    };
    for (SourceRef& s : edit_.axes) move(s);
    for (SourceRef& s : edit_.buttons) move(s);
    for (SourceRef& s : edit_.hat) move(s);
    for (auto& list : edit_.secondaries) {
        for (ButtonRef& b : list) move(b.source);
    }
    edit_.sourceDevice = deviceId(wheel.vendorId, wheel.productId);
    mode_ = Mode::Idle;
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
                    (field_ == Field::PerGame && (c == '/' || c == 'o' || c == 'f' || c == 'O' || c == 'F'))) &&
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
    setGameRotationFromX(x);
}

void MappingEditView::setGameRotationFromX(const float x) {
    const float t = std::clamp((x - kSliderX) / kSliderW, 0.0F, 1.0F);
    const float value = kSliderMinDeg + t * (kSliderMaxDeg - kSliderMinDeg);
    edit_.gameRotationDeg =
        std::clamp(std::round(value / kSliderStepDeg) * kSliderStepDeg, kSliderMinDeg, kSliderMaxDeg);
}

std::string MappingEditView::fieldText(const Field field, const std::size_t index) const {
    switch (field) {
        case Field::PerGame: {
            const std::span<const GameProfile> games = gameProfiles();
            if (index >= games.size()) return {};
            const std::optional<SteeringRotation> rotation = rotationWhileRunning(games[index], edit_.gameRotations);
            return rotation ? formatGameRotation({false, *rotation}) : std::string("off");
        }
        case Field::GameRotation: return degrees(edit_.gameRotationDeg);
        case Field::WheelRotation: return degrees(edit_.wheelRotationDeg);
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
    if (field_ == Field::PerGame) {
        // "180", "360/180" or "off" for one game.
        const std::span<const GameProfile> games = gameProfiles();
        const std::optional<GameRotationOverride> parsed = parseGameRotation(typed_);
        if (!parsed) {
            problem = "'" + typed_ + "': expected degrees (180), wheel/game (360/180) or off; each " +
                      tuning::formatNumber(tuning::kMinRotationDeg) + " to " +
                      tuning::formatNumber(tuning::kMaxRotationDeg);
        } else if (fieldIndex_ < games.size()) {
            edit_.gameRotations[games[fieldIndex_].id] = *parsed;
        }
    } else if (!number) {
        problem = "'" + typed_ + "' is not a number";
    } else {
        const float value = *number;
        switch (field_) {
            case Field::GameRotation:
            case Field::WheelRotation:
                problem = tuning::rotationProblem(value);
                if (problem.empty()) {
                    (field_ == Field::GameRotation ? edit_.gameRotationDeg : edit_.wheelRotationDeg) = value;
                }
                break;
            case Field::PerGame:
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
    const std::string target = profile_ != nullptr ? profile_->identity().shortName : std::string("G920");
    window_.text(4, 4, "MAPPING", kHeading);
    // With the shim, the app's games row is where this would be.
    if (picker_.virtualDevice()) window_.text(68, 4, "what your controls do on the " + target, kDim);

    // The target (fixed inputs, picked with < >), then whose mapping onto it this is: WheelBri's default, or the
    // user's own saved one.
    picker_.draw(4, 18, !locked_);
    window_.text(372, 22, builtIn_ ? "mapping: default" : "mapping: your own", builtIn_ ? kDim : kCaution);
    const bool canSave = loaded_ && dirty() && !locked_;
    button(580, 18, "Default", [this] { dropOwnEdits(); }, false, !builtIn_ && !locked_, 104);
    button(692, 18, "Save", [this] { save(); }, canSave, canSave, 80);
    button(780, 18, "Revert", [this] {
        edit_ = saved_;
        status_ = "Changes dropped.";
        statusError_ = false;
    }, false, canSave, 80);
    if (!loaded_) {
        window_.text(4, 60, fit(status_, 118), kWarning);
        return;
    }

    if (rig_.devices.empty()) {
        window_.text(4, 42,
                     rigName_.empty() ? "No rig yet: define your devices and their controls in the Rig tab first."
                                      : "The rig '" + rigName_ + "' has no devices yet: define them in the Rig tab.",
                     kCaution);
    } else {
        // The wheel: device 0, the one that plays force feedback.
        drawWheelRow();
    }

    // Axes, left; D-pad, right.
    float y = 66;
    window_.text(4, y, "AXES", kHeading);
    window_.text(48, y, "click a source to pick another", kDim);
    window_.text(620, y, "D-PAD", kHeading);
    for (std::size_t i = 0; i < kAxisNames.size(); ++i) {
        const float row = y + 16 + static_cast<float>(i) * 20;
        window_.text(4, row + 4, kAxisNames[i], kText);
        drawSource(80, row, 420, Slot::Axis, i);
        button(508, row, "Invert", [this, i] { edit_.invert[i] = !edit_.invert[i]; }, edit_.invert[i], true, 70);
        window_.text(620, row + 4, kHatNames[i], kText);
        drawSource(668, row, 288, Slot::Hat, i);
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
        const float x = 4 + static_cast<float>(cell % 2) * 478;
        const float row = y + 16 + static_cast<float>(cell / 2) * 20;
        ++cell;
        gridEnd = row + 16;
        char label[32];
        std::snprintf(label, sizeof(label), "%2zu %.10s", index, std::string(name).c_str());
        window_.text(x, row + 4, label, kText);
        // The first source (a button), "or" a second one (a button, or a one-sided axis past a threshold).
        drawSource(x + 112, row, 206, Slot::Button, index);
        window_.text(x + 322, row + 4, "or", kDim);
        drawAltSource(x + 342, row, 126, index);
    }

    drawRotation(gridEnd + 14);

    const float hint = kContentHeight - 52;
    window_.text(4, hint, "Sources are the rig's controls (defined in the Rig tab); Auto-map fills what their roles say.",
                 kDim);
    window_.text(4, hint + 12, "Save writes the config; Start uses it. Accessories must be plugged in to start.",
                 kDim);
    const std::string state = dirty() ? "unsaved changes" : "saved";
    window_.text(4, hint + 28, fit(status_.empty() ? state : status_, 118),
                 statusError_ ? kWarning : status_.empty() ? (dirty() ? kCaution : kDim) : kActive);

    popupTips_ = tips_.size();   // a popup's own tips come after the tab's (which it covers)
    if (mode_ == Mode::Pick) drawPicker();
    if (mode_ == Mode::PickWheel) drawWheelPicker();
    if (mode_ == Mode::PickAlt) drawAltPicker();
    drawTooltip();   // over everything
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
        constexpr float kMaxW = 940;
        const std::string text = fit(it->text, static_cast<std::size_t>((kMaxW - 12) / ui::kCharWidth));
        const float w = static_cast<float>(text.size()) * ui::kCharWidth + 12;
        constexpr float h = 18;
        // Below and right of the pointer; moved in at the window's right edge, above the pointer at the bottom.
        float x = std::min(hoverX_ + 12, 960 - w - 2);
        float y = hoverY_ + 18;
        if (y + h > kContentHeight - 2) y = hoverY_ - h - 6;
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

    // The game's full lock: the slider (90 to 1080), the typed number (up to 2520) and the usual values.
    y += 16;
    window_.text(4, y + 4, "Game full lock", kText);
    const float shown = std::clamp(edit_.gameRotationDeg, kSliderMinDeg, kSliderMaxDeg);
    const float filled = (shown - kSliderMinDeg) / (kSliderMaxDeg - kSliderMinDeg) * kSliderW;
    window_.fill(kSliderX, y + 6, kSliderW, 4, kStripe);
    window_.fill(kSliderX, y + 6, filled, 4, locked_ ? kDim : kHeading);
    window_.fill(kSliderX + filled - 3, y + 1, 6, 14, locked_ ? kDim : kText);
    window_.outline(kSliderX, y + 6, kSliderW, 4, edit_.gameRotationDeg != saved_.gameRotationDeg ? kCaution : kPanel);
    if (!locked_) {
        hit(kSliderX - 4, y, kSliderW + 8, 16, [this] {
            setGameRotationFromX(clickX_);   // the press sets it; holding the button drags it (onPointer)
            dragging_ = true;
        });
    }
    drawField(kSliderX + kSliderW + 12, y, 64, Field::GameRotation, degrees(edit_.gameRotationDeg),
              edit_.gameRotationDeg != saved_.gameRotationDeg);
    window_.text(kSliderX + kSliderW + 80, y + 4, "deg", kDim);
    float x = kSliderX + kSliderW + 112;
    for (const float preset : kPresetsDeg) {
        x += button(x, y, degrees(preset), [this, preset] { edit_.gameRotationDeg = preset; },
                    std::fabs(edit_.gameRotationDeg - preset) < 0.5F, !locked_, 46) + 4;
    }
    window_.text(kSliderX, y + 20, "90", kDim);
    window_.text(kSliderX + kSliderW - 32, y + 20, "1080", kDim);

    // What the wheel's own software is set to: the mapper turns that into the game's full lock.
    y += 34;
    window_.text(4, y + 4, "Wheel rotation", kText);
    drawField(kSliderX, y, 64, Field::WheelRotation, degrees(edit_.wheelRotationDeg),
              edit_.wheelRotationDeg != saved_.wheelRotationDeg);
    window_.text(kSliderX + 68, y + 4, "deg: what your wheel's own software (e.g. Moza Pit House) is set to", kDim);
    const bool same = std::fabs(edit_.wheelRotationDeg - edit_.gameRotationDeg) < 0.5F;
    if (!same) {
        button(852, y, "Same as game", [this] { edit_.wheelRotationDeg = edit_.gameRotationDeg; }, false, !locked_,
               104);
    }
    y += 20;
    if (same) {
        window_.text(4, y, fit("Both match: the wheel reaches its end stop where the game reaches full lock. Keep them "
                               "the same when you change one.", 118),
                     kDim);
        y += 12;
    } else {
        const std::string game = degrees(edit_.gameRotationDeg);
        for (const std::string& line : wrap("Wheel " + degrees(edit_.wheelRotationDeg) + " deg, game " + game +
                                                " deg: set your wheel's own software to " + game +
                                                " too (or press Same as game).",
                                            118, 2)) {
            window_.text(4, y, line, kCaution);
            y += 12;
        }
        const char* const why = edit_.wheelRotationDeg > edit_.gameRotationDeg
                                    ? "Otherwise the wheel turns on past the game's full lock and the force feedback "
                                      "drops there."
                                    : "Otherwise the game's full lock is never reached: the wheel stops first.";
        window_.text(4, y, why, kDim);
        y += 12;
    }

    // Per game (game profiles with a rotation for this target): it steers with that rotation while the game runs,
    // instead of the one above. The user's own number, or off, goes in [game_rotation]; Default drops it.
    y += 16;
    bool any = false;
    const std::span<const GameProfile> games = gameProfiles();
    for (std::size_t g = 0; g < games.size(); ++g) {
        const GameProfile& game = games[g];
        const bool own = edit_.gameRotations.count(game.id) != 0;
        if ((!game.rotationDeg && !own) || !profileFitsTarget(game, targetName_)) continue;
        if (!any) {
            window_.text(4, y, "PER GAME", kHeading);
            window_.text(76, y, "applied while that game runs, instead of the rotation above", kDim);
            y += 16;
            any = true;
        }
        if (y > kContentHeight - 52 - 30) break;   // keep clear of the hint
        const std::optional<SteeringRotation> rotation = rotationWhileRunning(game, edit_.gameRotations);
        const bool changed = own != (saved_.gameRotations.count(game.id) != 0) ||
                             (own && !(edit_.gameRotations.at(game.id) == saved_.gameRotations.at(game.id)));
        window_.text(4, y + 4, fit(game.label, 14), kText);
        const std::string value = rotation ? formatGameRotation({false, *rotation}) : std::string("off");
        drawField(132, y, 80, Field::PerGame, value, changed, g);
        window_.text(216, y + 4, rotation ? "deg" : "", kDim);
        const std::string id = game.id;
        button(252, y, rotation ? "On" : "Off", [this, id, rotation, &game] {
            if (rotation) {
                edit_.gameRotations[id] = GameRotationOverride{true, {}};   // off: the rotation above in this game too
            } else if (game.rotationDeg) {
                edit_.gameRotations.erase(id);                               // back to the profile's
            } else {
                edit_.gameRotations[id] = GameRotationOverride{false, {edit_.wheelRotationDeg, edit_.gameRotationDeg}};
            }
        }, rotation.has_value(), !locked_, 48);
        button(304, y, "Default", [this, id] { edit_.gameRotations.erase(id); }, false, !locked_ && own, 72);
        std::string about = game.rotationDeg ? "profile: " + degrees(*game.rotationDeg) + ". " : std::string();
        if (rotation) {
            about += "Set your wheel's own software to " + degrees(rotation->wheelDeg) + " deg for this game.";
        }
        window_.text(384, y + 4, fit(about, 71), kDim);
        y += 20;
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
    // "Hand > 50%": the axis' name (or role), and where it presses the button.
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
    hit(0, 0, 960, kContentHeight, [] {});   // clicks outside the popup do nothing
    window_.fill(kPopupX, kPopupY, kPopupW, kPopupH, kPopup);
    window_.outline(kPopupX, kPopupY, kPopupW, kPopupH, kHeading);
    const float left = kPopupX + 12;
    const std::size_t target = pickIndex_;
    const std::string targetName = profile_->identity().shortName + " " + std::string(profile_->buttonName(target));
    window_.text(left, kPopupY + 10, "What else presses " + targetName + "? Its secondary assignments:", kHeading);
    button(kPopupX + kPopupW - 100, kPopupY + 6, "Done", [this] { mode_ = Mode::Idle; }, false, true, 88);
    std::vector<ButtonRef>& list = edit_.secondaries[target];
    const std::size_t max = MapperConfig::kMaxSecondaries;
    button(left, kPopupY + 28, "(clear all)", [&list] { list.clear(); }, false, !list.empty(), 100);
    window_.text(left + 112, kPopupY + 32,
                 std::to_string(list.size()) + " of " + std::to_string(max) +
                     ". Click a control below to add or remove it; click an axis here for its threshold.",
                 kDim);

    // The ones it has, as chips (an axis' chip picks it for the threshold row).
    constexpr float kChipW = 174;
    if (altFocus_ >= list.size()) altFocus_ = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const float x = left + static_cast<float>(i % 5) * (kChipW + 4);
        const float y = kPopupY + 48 + static_cast<float>(i / 5) * 18;
        const bool focused = list[i].axis && i == altFocus_;
        window_.fill(x, y, kChipW, 16, focused ? kHeading : kStripe);
        window_.outline(x, y, kChipW, 16, kHeading);
        window_.text(x + 4, y + 4, fit(shortAlt(list[i]), 20), focused ? kDark : kText);
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
    constexpr int kColumns = 5;
    constexpr float kEntryW = 174;
    if (devices_.empty()) {
        window_.text(left, y, "The rig has no devices yet: define them in the Rig tab and Save.", kCaution);
    }
    const ControlUse self{ControlUse::Kind::Secondary, target};
    for (const DeviceLabels* const entry : order) {
        const DeviceLabels& device = *entry;
        if (y > kPopupY + kPopupH - 30) break;
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
        // Only one-sided axes: steering (or an unknown axis) has no "pressed" direction.
        for (std::size_t a = 0; a < rigDevice->axes.size(); ++a) {
            if (axisDrivesButtons(rigDevice->axes[a].role)) entryAt(true, a, rigDevice->axes[a]);
        }
        if (column != 0) y += 18;
        y += 8;
    }
    window_.text(left, kPopupY + kPopupH - 20,
                 "Greyed: used elsewhere (hover to see where). Axes: only throttle, brake, clutch, handbrake. Save writes it.",
                 kDim);
}

void MappingEditView::drawWheelRow() {
    window_.text(4, 42, "Wheel", kDim);
    const DeviceLabels* const wheel = wheelLabels();
    window_.text(68, 42,
                 wheel != nullptr ? fit(deviceName(*wheel), 40) + "  " + deviceId(wheel->vendorId, wheel->productId)
                                  : "'" + edit_.sourceDevice + "' (not named in the Rig tab yet)",
                 wheel != nullptr ? kText : kCaution);
    window_.text(500, 42, "plays force feedback", kDim);
    button(680, 38, "Change", [this] { mode_ = Mode::PickWheel; }, false, !devices_.empty(), 70);
    button(760, 38, "Auto-map", [this] { autoMapFromRig(); }, false, !rig_.devices.empty(), 80);
    window_.text(848, 42, fit("rig " + rigName_, 14), kDim);
}

void MappingEditView::drawPicker() {
    hit(0, 0, 960, kContentHeight, [] {});   // clicks outside the popup do nothing
    window_.fill(kPopupX, kPopupY, kPopupW, kPopupH, kPopup);
    window_.outline(kPopupX, kPopupY, kPopupW, kPopupH, kHeading);
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
    button(kPopupX + kPopupW - 100, kPopupY + 6, "Cancel", [this] { mode_ = Mode::Idle; }, false, true, 88);
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
    constexpr int kColumns = 5;
    constexpr float kEntryW = 174;
    if (devices_.empty()) {
        window_.text(left, y, "The rig has no devices yet: define them in the Rig tab and Save.", kCaution);
    }
    for (const DeviceLabels* const entry : order) {
        const DeviceLabels& device = *entry;
        if (y > kPopupY + kPopupH - 30) break;
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

void MappingEditView::drawWheelPicker() {
    hit(0, 0, 960, kContentHeight, [] {});
    window_.fill(kPopupX, kPopupY, kPopupW, kPopupH, kPopup);
    window_.outline(kPopupX, kPopupY, kPopupW, kPopupH, kHeading);
    const float left = kPopupX + 12;
    window_.text(left, kPopupY + 10, "Which device is the wheel? It plays the game's force feedback.", kHeading);
    button(kPopupX + kPopupW - 100, kPopupY + 6, "Cancel", [this] { mode_ = Mode::Idle; }, false, true, 88);
    const DeviceLabels* const wheel = wheelLabels();
    float y = kPopupY + 34;
    for (const DeviceLabels& device : devices_) {
        const bool on = wheel != nullptr && idOf(device) == idOf(*wheel);
        window_.fill(left, y, kPopupW - 24, 18, on ? kHeading : kPanel);
        window_.text(left + 6, y + 5,
                     fit(deviceName(device), 40) + "  " + deviceId(device.vendorId, device.productId) + "  " +
                         deviceKindName(device.kind),
                     on ? kDark : kText);
        hit(left, y, kPopupW - 24, 18, [this, device] { changeWheel(device); });
        y += 22;
    }
    window_.text(left, y + 8, "The rig's devices (Rig tab). All of them are hidden from games while WheelBri runs.",
                 kDim);
}

}  // namespace wheelbri::service
