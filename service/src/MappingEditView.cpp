#include "MappingEditView.hpp"

#include "Platform.hpp"

#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/DeviceMatch.hpp"
#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/IniDocument.hpp"
#include "wheelbri/Logging.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
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

std::string fit(std::string text, const std::size_t chars) {
    if (text.size() > chars) text = text.substr(0, chars - 2) + "..";
    return text;
}

std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

DeviceId idOf(const DeviceLabels& labels) { return {labels.vendorId, labels.productId}; }

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
    }
    if (devicesReadMs_ == 0 || platform::nowMs() - devicesReadMs_ >= kDevicesEveryMs) refreshDevices();
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
    path_ = picker_.selected();
    loaded_ = false;
    profile_.reset();
    const ConfigEntry* const entry = picker_.current();
    builtIn_ = entry != nullptr && entry->builtIn;
    if (path_.empty()) {
        status_ = "No config to edit: there is none next to the program.";
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
        status_ = builtIn_ ? "Built-in config: Save keeps your changes as a config of your own." : "";
        statusError_ = false;
    }
    WE_LOG_INFO(log(), "mapping: editing {}", path_);
}

void MappingEditView::save() {
    if (!loaded_) return;
    if (builtIn_) {
        mode_ = Mode::SaveAs;
        saveName_.clear();
        status_.clear();
        return;
    }
    const MappingWriteResult written = writeMapping(fileText_, edit_);
    if (!written.error.empty()) {
        status_ = "Not saved: " + written.error + ".";
        statusError_ = true;
        return;
    }
    std::ofstream file(path_, std::ios::binary | std::ios::trunc);
    file << written.text;
    file.close();
    if (!file) {
        status_ = "Could not write " + path_;
        statusError_ = true;
        WE_LOG_ERROR(log(), "mapping: {}", status_);
        return;
    }
    fileText_ = written.text;
    saved_ = edit_;
    status_ = "Saved. The next Start uses it.";
    statusError_ = false;
    WE_LOG_INFO(log(), "mapping: saved {}", path_);
}

void MappingEditView::saveAs() {
    const ConfigEntry* const from = picker_.current();
    if (from == nullptr || !from->builtIn) return;
    const MappingWriteResult written = writeMapping(fileText_, edit_);
    if (!written.error.empty()) {
        status_ = "Not saved: " + written.error + ".";
        statusError_ = true;
        return;
    }
    std::string name = saveName_;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    const ConfigResult made = createConfigWithText(picker_.userFolder(), name, *from, written.text, picker_.configs());
    if (!made.ok) {
        status_ = "Not saved: " + made.error + ".";
        statusError_ = true;
        return;
    }
    WE_LOG_INFO(log(), "mapping: saved as '{}' (made from {}): {}", name, from->name, made.path.string());
    picker_.adopt(made.path, "Made '" + name + "' with your mapping and picked it.");
    update();   // reads the new config: nothing unsaved any more
    status_ = "Saved as '" + name + "' (picked on the Status page). The next Start uses it.";
    statusError_ = false;
}

// --- devices and sources ---------------------------------------------------------------------------------------

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
    const int set = autoMap(edit_, rig_, *profile_);
    status_ = set == 0 ? "Nothing to auto-map: give the controls roles (steering, throttle, d-pad ...) in the Rig tab."
                       : "Auto-mapped " + std::to_string(set) +
                             " source(s) from the rig's roles; pick the plain buttons yourself, then Save.";
    statusError_ = set == 0;
}

std::string MappingEditView::describe(const SourceRef& source, const bool axis) const {
    if (!source.mapped()) return "(nothing)";
    const DeviceLabels* const labels = labelsOf(source);
    const std::string device = labels != nullptr       ? deviceName(*labels)
                               : source.device.empty() ? std::string("the wheel")
                                                       : deviceId(source.device.vendorId, source.device.productId);
    std::string name;
    std::string role;
    if (const RigControl* const control = rigControl(source, axis)) {
        name = control->label;
        role = roleName(control->role);
    }
    const std::string number = (axis ? "axis " : "button ") + std::to_string(source.index);
    // "Wheel: Gas  (axis 2, throttle)", "Wheel: axis 6", "Wheel: button 4  (d-pad up)".
    std::string about = name.empty() ? std::string() : number;
    if (!role.empty()) about += (about.empty() ? "" : ", ") + role;
    return fit(device, 20) + ": " + (name.empty() ? number : name) + (about.empty() ? "" : "  (" + about + ")");
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
    edit_.sourceDevice = deviceId(wheel.vendorId, wheel.productId);
    mode_ = Mode::Idle;
}

// --- input -------------------------------------------------------------------------------------------------------

void MappingEditView::onClick(const float x, const float y) {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) {
            const auto action = it->action;   // a copy: the list belongs to the last draw
            action();
            return;
        }
    }
}

void MappingEditView::onText(const std::string& text) {
    if (mode_ != Mode::SaveAs) return;
    for (const char c : text) {
        if (c == ui::kBackspace) {
            if (!saveName_.empty()) saveName_.pop_back();
        } else if (c == ui::kEnter) {
            saveAs();
            return;
        } else if (saveName_.size() < kMaxConfigName && !(saveName_.empty() && c == ' ') &&
                   static_cast<unsigned char>(c) >= ' ' && static_cast<unsigned char>(c) < 0x7F) {
            saveName_ += c;
        }
    }
}

void MappingEditView::onKey(const char key) {
    if (key == ui::kEscape) mode_ = Mode::Idle;
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

void MappingEditView::drawSource(const float x, const float y, const float w, const Slot kind,
                                 const std::size_t index) {
    const SourceRef& source = slot(kind, index);
    const bool changed = kind == Slot::Axis     ? source != saved_.axes[index]
                         : kind == Slot::Hat    ? source != saved_.hat[index]
                                                : source != saved_.buttons[index];
    window_.fill(x, y, w, 16, kStripe);
    window_.outline(x, y, w, 16, changed ? kCaution : kPanel);
    const auto chars = static_cast<std::size_t>((w - 12) / ui::kCharWidth);
    window_.text(x + 6, y + 4, fit(describe(source, kind == Slot::Axis), chars), source.mapped() ? kText : kDim);
    hit(x, y, w, 16, [this, kind, index] {
        mode_ = Mode::Pick;
        pickKind_ = kind;
        pickIndex_ = index;
    });
}

void MappingEditView::draw() {
    hits_.clear();
    const std::string target = profile_ != nullptr ? profile_->identity().shortName : std::string("G920");
    window_.text(4, 4, "MAPPING", kHeading);
    window_.text(68, 4, "what your controls do on the " + target, kDim);

    const ConfigEntry* const entry = picker_.current();
    window_.text(4, 22, "Config", kDim);
    window_.text(68, 22,
                 entry == nullptr ? std::string("(none)")
                                  : fit(entry->name, 40) + (entry->builtIn ? "  (built-in)" : "  (yours)") +
                                        "  - pick another on the Status page",
                 kText);
    const bool canSave = loaded_ && dirty();
    button(800, 18, builtIn_ ? "Save as" : "Save", [this] { save(); }, canSave, canSave, 70);
    button(876, 18, "Revert", [this] {
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
    }
    // The wheel: device 0, the one that plays force feedback.
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
    for (std::size_t index = 0; index < edit_.buttons.size(); ++index) {
        const std::string_view name = profile_->buttonName(index);
        if (name.empty()) continue;
        const float x = 4 + static_cast<float>(cell % 2) * 478;
        const float row = y + 16 + static_cast<float>(cell / 2) * 20;
        ++cell;
        char label[32];
        std::snprintf(label, sizeof(label), "%2zu %.10s", index, std::string(name).c_str());
        window_.text(x, row + 4, label, kText);
        drawSource(x + 112, row, 356, Slot::Button, index);
    }

    const float hint = kContentHeight - 52;
    window_.text(4, hint, "Sources are the rig's controls (defined in the Rig tab); Auto-map fills what their roles say.",
                 kDim);
    window_.text(4, hint + 12, "Save writes the config; Start uses it. Accessories must be plugged in to start.",
                 kDim);
    const std::string state = dirty() ? "unsaved changes" : "saved";
    window_.text(4, hint + 28, fit(status_.empty() ? state : status_, 118),
                 statusError_ ? kWarning : status_.empty() ? (dirty() ? kCaution : kDim) : kActive);

    if (mode_ == Mode::Pick) drawPicker();
    if (mode_ == Mode::PickWheel) drawWheelPicker();
    if (mode_ == Mode::SaveAs) drawSaveAs();
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
    button(kPopupX + kPopupW - 100, kPopupY + 6, "Cancel", [this] { mode_ = Mode::Idle; }, false, true, 88);
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
            const bool on = source == current;
            window_.fill(x, y, kEntryW, 16, on ? kHeading : kPanel);
            const std::string text = std::to_string(index) + " " +
                                     (control->label.empty() ? std::string(roleName(control->role)) : control->label);
            window_.text(x + 4, y + 4, fit(text, 21), on ? kDark : kText);
            const bool invert = control->invert;
            hit(x, y, kEntryW, 16, [this, source, invert] {
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

void MappingEditView::drawSaveAs() {
    hit(0, 0, 960, kContentHeight, [] {});
    constexpr float kX = 160;
    constexpr float kY = 180;
    constexpr float kW = 640;
    constexpr float kH = 130;
    window_.fill(kX, kY, kW, kH, kPopup);
    window_.outline(kX, kY, kW, kH, kHeading);
    window_.text(kX + 16, kY + 14, "Built-in configs stay as they are: save your mapping as a config of your own.",
                 kHeading);
    window_.text(kX + 16, kY + 42, "Name", kDim);
    const float field = kX + 64;
    const float width = static_cast<float>(kMaxConfigName) * ui::kCharWidth + 16;
    window_.fill(field, kY + 38, width, 16, kDark);
    window_.outline(field, kY + 38, width, 16, kHeading);
    window_.text(field + 6, kY + 42, saveName_ + ((platform::nowMs() / 500) % 2 == 0 ? "_" : ""), kText);
    window_.text(kX + 16, kY + 66,
                 statusError_ && !status_.empty() ? fit(status_, 76) : "Enter saves it, Esc goes back.",
                 statusError_ && !status_.empty() ? kWarning : kDim);
    button(kX + 16, kY + kH - 28, "Save", [this] { saveAs(); }, true, !saveName_.empty(), 80);
    button(kX + 104, kY + kH - 28, "Cancel", [this] { mode_ = Mode::Idle; }, false, true, 80);
}

}  // namespace wheelbri::service
