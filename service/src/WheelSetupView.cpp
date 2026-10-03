#include "WheelSetupView.hpp"

#include "Platform.hpp"

#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/DeviceLabels.hpp"
#include "wheelbri/Logging.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <utility>

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.setup");
    return logger;
}

using Colour = ui::Colour;

// The same colours as the rest of the window (MappingView.cpp, App.cpp).
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
constexpr Colour kCentre{160, 164, 172};

constexpr int kAxes = static_cast<int>(RawInputState::kMaxAxes);
constexpr int kButtons = static_cast<int>(MapperConfig::kMaxSourceButtons);
constexpr std::size_t kMaxDeviceName = 40;
constexpr std::uint64_t kListEveryMs = 2000;   // looks for devices plugged in or out this often
constexpr float kTrigger = 0.5F;               // Detect: an axis this far from where it rested has been moved
constexpr float kBack = 0.15F;                 // ...and is armed again once back this close
constexpr int kSettleFrames = 20;              // about a third of a second of reads before the rest is taken
const std::string kFirstRig = "My rig";

// Layout: the header, the axes rows, the buttons grid, the hints.
constexpr float kAxesTop = 80;
constexpr float kAxisRow = 17;
constexpr float kButtonsTop = kAxesTop + 16 + kAxes * kAxisRow + 8;
constexpr int kColumns = 8;
constexpr float kTileW = 114;
constexpr float kTileH = 30;
constexpr float kTileStepX = 118;
constexpr float kTileStepY = 33;
constexpr float kHintTop = kButtonsTop + 16 + 8 * kTileStepY + 4;

std::string fit(std::string text, const std::size_t chars) {
    if (text.size() > chars) text.resize(chars);
    return text;
}

std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(' ') - first + 1);
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string capitalised(std::string text) {
    if (!text.empty()) text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    return text;
}

DeviceId idOf(const WheelInfo& info) { return {info.vendorId, info.productId}; }

std::string idText(const DeviceId& id) { return deviceId(id.vendorId, id.productId); }

// What a device probably is, from its name: only the starting point, the user picks.
DeviceKind guessKind(const std::string& name) {
    const std::string text = lower(name);
    if (text.find("pedal") != std::string::npos && text.find("wheel") == std::string::npos) return DeviceKind::Pedals;
    if (text.find("shifter") != std::string::npos) return DeviceKind::Shifter;
    if (text.find("handbrake") != std::string::npos) return DeviceKind::Handbrake;
    if (text.find("wheel") != std::string::npos || text.find("racing") != std::string::npos) return DeviceKind::Wheel;
    return DeviceKind::Other;
}

bool isPedalRole(const ControlRole role) {
    return role == ControlRole::Throttle || role == ControlRole::Brake || role == ControlRole::Clutch ||
           role == ControlRole::Handbrake;
}

// "Gas  (throttle, inverted)": the label, then what it is for.
std::string describe(const RigControl& control) {
    std::string text = control.label;
    std::string about = roleName(control.role);
    if (control.invert) about += about.empty() ? "inverted" : ", inverted";
    if (!about.empty()) text += (text.empty() ? "(" : "  (") + about + ")";
    return text;
}

}  // namespace

WheelSetupView::WheelSetupView(ui::UiWindow& window, Settings settings)
    : window_(window), settings_(std::move(settings)) {
    loadRigs();
}

WheelSetupView::~WheelSetupView() = default;

bool WheelSetupView::anyRig(const std::filesystem::path& rigsFolder) { return !listRigs(rigsFolder).empty(); }

// --- the devices -----------------------------------------------------------------------------------------

void WheelSetupView::releaseWheel() {
    open_.clear();
    backend_.reset();
    connected_.clear();
    devicesListedMs_ = 0;
    listLogged_ = false;
}

bool WheelSetupView::refreshDevices(const bool force) {
    if (!force && devicesListedMs_ != 0 && platform::nowMs() - devicesListedMs_ < kListEveryMs) return false;
    if (backend_ == nullptr) {
        backend_ = startWheelBackend();
        if (backend_ == nullptr) {
            devicesListedMs_ = platform::nowMs();
            status_ = "Cannot read game controllers on this computer (the log says why).";
            statusError_ = true;
            return false;
        }
        WE_LOG_INFO(log(), "wheel setup: looking for game controllers through {}", backend_->describe());
    }
    std::vector<WheelInfo> found = backend_->list();
    // The time is taken after listing: DirectInput can take a good part of a second to list.
    devicesListedMs_ = platform::nowMs();
    if (found.size() != connected_.size() || !listLogged_) {
        listLogged_ = true;
        std::string names;
        for (const WheelInfo& info : found) {
            names += (names.empty() ? "" : ", ") + info.name + " (" + idText(idOf(info)) + ")";
        }
        WE_LOG_INFO(log(), "wheel setup: {} game controller(s) connected{}{}", found.size(), found.empty() ? "" : ": ",
                    names);
    }
    connected_ = std::move(found);
    return true;
}

void WheelSetupView::openConnected() {
    // Gone: closed. New: opened (a device that cannot be opened is tried again after the next listing).
    open_.erase(std::remove_if(open_.begin(), open_.end(),
                               [this](const Open& device) {
                                   return std::none_of(connected_.begin(), connected_.end(), [&](const WheelInfo& info) {
                                       return info.path == device.info.path;
                                   });
                               }),
                open_.end());
    for (const WheelInfo& info : connected_) {
        if (std::any_of(open_.begin(), open_.end(), [&](const Open& d) { return d.info.path == info.path; })) continue;
        if (std::any_of(open_.begin(), open_.end(), [&](const Open& d) { return idOf(d.info) == idOf(info); })) {
            continue;   // a second device with the same id: a rig tells devices apart by id
        }
        auto wheel = backend_->open(info);
        if (wheel == nullptr) {
            WE_LOG_WARN(log(), "wheel setup: cannot open '{}' (the line above says why); trying again in a moment",
                        info.name);
            continue;
        }
        WE_LOG_INFO(log(), "wheel setup: reading '{}' ({})", info.name, idText(idOf(info)));
        Open device;
        device.info = wheel->info();
        device.wheel = std::move(wheel);
        open_.push_back(std::move(device));
    }
}

WheelSetupView::Open* WheelSetupView::openFor(const DeviceId& id) {
    for (Open& device : open_) {
        if (idOf(device.info) == id) return &device;
    }
    return nullptr;
}

const WheelSetupView::Open* WheelSetupView::openFor(const DeviceId& id) const {
    for (const Open& device : open_) {
        if (idOf(device.info) == id) return &device;
    }
    return nullptr;
}

void WheelSetupView::update(const SessionWheel* const session) {
    if (session != nullptr) {
        if (!fromSession_) releaseWheel();   // the session has the devices now
        fromSession_ = true;
        sessionInfo_ = session->info;
        sessionAxes_ = session->axes;
        sessionButtons_ = session->buttons;
        if (open_.empty() || idOf(open_[0].info) != idOf(session->info)) {
            open_.clear();
            Open device;
            device.info = session->info;
            open_.push_back(std::move(device));
        }
        open_[0].raw = session->raw;
        open_[0].haveRaw = true;
        shown_ = idOf(session->info);
        detect(open_[0]);
        return;
    }
    if (fromSession_) {
        fromSession_ = false;
        open_.clear();
    }
    if (refreshDevices(false)) openConnected();
    bool lost = false;
    for (auto it = open_.begin(); it != open_.end();) {
        if (it->wheel != nullptr && it->wheel->poll(it->raw)) {
            it->haveRaw = true;
            ++it;
        } else {
            WE_LOG_WARN(log(), "wheel setup: '{}' stopped answering (unplugged?)", it->info.name);
            it = open_.erase(it);
            lost = true;
        }
    }
    if (lost) refreshDevices(true);
    const std::vector<DeviceId> shown = shownList();
    if (!shown.empty() && std::find(shown.begin(), shown.end(), shown_) == shown.end()) {
        const RigDevice* const wheel = edit_.wheel();
        shown_ = wheel != nullptr && std::find(shown.begin(), shown.end(), wheel->id) != shown.end() ? wheel->id
                                                                                                    : shown.front();
    }
    for (Open& device : open_) detect(device);
}

void WheelSetupView::detect(Open& device) {
    if (!device.haveRaw) return;
    const RawInputState& raw = device.raw;
    if (!device.restTaken) {
        if (++device.settleFrames < kSettleFrames) return;
        device.settleFrames = 0;
        device.rest = raw;
        device.armed.fill(true);
        device.previousButtons = raw.buttons;
        device.restTaken = true;
        return;
    }
    const DeviceId id = idOf(device.info);
    // Controls already defined in the rig (a label or a role) never open the popup by themselves: pressing or moving
    // them again only shows them (click one to change it).
    const RigDevice* const known = edit_.find(id);
    const auto defined = [known](const bool axis, const int index) {
        if (known == nullptr) return false;
        const auto i = static_cast<std::size_t>(index);
        return axis ? known->axes[i].defined() : known->buttons[i].defined();
    };
    const std::uint64_t pressed = raw.buttons & ~device.previousButtons;
    device.previousButtons = raw.buttons;
    int first = -1;      // the lowest button pressed now
    int newButton = -1;  // the lowest one of those not defined yet
    for (int source = 0; pressed != 0 && source < kButtons; ++source) {
        if (((pressed >> source) & 1U) == 0) continue;
        if (first < 0) first = source;
        if (newButton < 0 && !defined(false, source)) newButton = source;
    }
    if (first >= 0 && id == shown_) lastPressed_ = first;
    // An axis that triggered is armed again once it is back where it rested.
    int moved = -1;
    for (int axis = 0; axis < kAxes; ++axis) {
        const auto a = static_cast<std::size_t>(axis);
        const float distance = std::abs(raw.axes[a] - device.rest.axes[a]);
        if (!device.armed[a] && distance < kBack) device.armed[a] = true;
        if (device.armed[a] && distance > kTrigger && moved < 0 && !defined(true, axis)) moved = axis;
    }
    if (!detect_ || popup_ != Popup::None) return;
    if (newButton >= 0) {
        shown_ = id;
        lastPressed_ = newButton;
        openControl(false, newButton, std::nullopt);
    } else if (moved >= 0) {
        device.armed[static_cast<std::size_t>(moved)] = false;
        shown_ = id;
        openControl(true, moved, device.rest.axes[static_cast<std::size_t>(moved)]);
    }
}

std::vector<DeviceId> WheelSetupView::shownList() const {
    std::vector<DeviceId> list;
    for (const Open& device : open_) list.push_back(idOf(device.info));
    if (!fromSession_) {
        for (const WheelInfo& info : connected_) {
            if (std::find(list.begin(), list.end(), idOf(info)) == list.end()) list.push_back(idOf(info));
        }
        for (const RigDevice& device : edit_.devices) {
            if (std::find(list.begin(), list.end(), device.id) == list.end()) list.push_back(device.id);
        }
    }
    return list;
}

std::string WheelSetupView::systemName(const DeviceId& id) const {
    if (const Open* const device = openFor(id)) return device->info.name;
    for (const WheelInfo& info : connected_) {
        if (idOf(info) == id) return info.name;
    }
    if (const RigDevice* const device = edit_.find(id)) return device->name;
    return idText(id);
}

int WheelSetupView::axisCount() const {
    if (fromSession_) return sessionAxes_;
    const Open* const device = openFor(shown_);
    return device != nullptr && device->wheel != nullptr ? device->wheel->axes() : 0;
}

int WheelSetupView::buttonCount() const {
    if (fromSession_) return sessionButtons_;
    const Open* const device = openFor(shown_);
    return device != nullptr && device->wheel != nullptr ? device->wheel->buttons() : 0;
}

const RawInputState* WheelSetupView::liveRaw() const {
    const Open* const device = openFor(shown_);
    return device != nullptr && device->haveRaw ? &device->raw : nullptr;
}

// --- the rig ----------------------------------------------------------------------------------------------------

void WheelSetupView::loadRigs() {
    rigNames_ = listRigs(settings_.rigsFolder);
    const std::string active = readAppSetting(settings_.stateFile, "rig");
    if (std::find(rigNames_.begin(), rigNames_.end(), active) != rigNames_.end()) {
        activate(active);
    } else if (!rigNames_.empty()) {
        activate(rigNames_.front());
    } else {
        // The first rig: what was named per device before (older Wheel setup) is its starting point.
        rigName_ = kFirstRig;
        rigSaved_ = false;
        saved_ = Rig{};
        edit_ = Rig{};
        for (const DeviceLabels& labels : listDeviceLabels(settings_.devicesFolder)) {
            if (edit_.devices.size() < Rig::kMaxDevices) edit_.devices.push_back(rigDeviceFrom(labels));
        }
        edit_.devices.reserve(Rig::kMaxDevices);
        status_ = "No rig yet: touch each control and say what it is, then Save to keep it as '" + kFirstRig + "'.";
        statusError_ = false;
        WE_LOG_INFO(log(), "wheel setup: no rig yet ({} device(s) named before)", edit_.devices.size());
    }
}

void WheelSetupView::activate(const std::string& name) {
    std::vector<std::string> errors;
    const std::optional<Rig> rig = readRig(settings_.rigsFolder, name, errors);
    popup_ = Popup::None;
    editing_ = nullptr;
    if (!rig) {
        status_ = "Cannot read the rig '" + name + "'.";
        statusError_ = true;
        WE_LOG_WARN(log(), "wheel setup: cannot read rig '{}'", name);
        return;
    }
    for (const std::string& problem : errors) WE_LOG_WARN(log(), "rig '{}': {}", name, problem);
    rigName_ = name;
    rigSaved_ = true;
    saved_ = *rig;
    edit_ = *rig;
    edit_.devices.reserve(Rig::kMaxDevices);   // names being typed point into it
    status_ = errors.empty() ? "Rig '" + name + "'." : "Rig '" + name + "': " + errors.front();
    statusError_ = !errors.empty();
    if (!writeAppSetting(settings_.stateFile, "rig", name)) {
        WE_LOG_WARN(log(), "cannot save the active rig in {}", settings_.stateFile.string());
    }
    WE_LOG_INFO(log(), "wheel setup: rig '{}' ({} device(s))", name, rig->devices.size());
}

void WheelSetupView::save() {
    for (RigDevice& device : edit_.devices) {
        device.name = trimmed(device.name);
        if (device.name.empty()) device.name = fit(systemName(device.id), kMaxDeviceName);
    }
    if (!saveRig(settings_.rigsFolder, rigName_, edit_)) {
        status_ = "Could not save " + (settings_.rigsFolder / (rigName_ + ".ini")).string();
        statusError_ = true;
        WE_LOG_ERROR(log(), "wheel setup: {}", status_);
        return;
    }
    saved_ = edit_;
    rigSaved_ = true;
    rigNames_ = listRigs(settings_.rigsFolder);
    writeAppSetting(settings_.stateFile, "rig", rigName_);
    status_ = "Saved the rig '" + rigName_ + "' (" + std::to_string(edit_.devices.size()) +
              " device(s)). The Mapping tab maps from it.";
    statusError_ = false;
    WE_LOG_INFO(log(), "wheel setup: saved rig '{}' ({} device(s))", rigName_, edit_.devices.size());
}

void WheelSetupView::revert() {
    edit_ = saved_;
    edit_.devices.reserve(Rig::kMaxDevices);
    popup_ = Popup::None;
    editing_ = nullptr;
    status_ = "Changes dropped.";
    statusError_ = false;
}

void WheelSetupView::newRig() {
    const std::string name = trimmed(popupName_);
    if (const std::string problem = rigNameProblem(name, rigNames_); !problem.empty()) {
        status_ = "Not made: " + problem + ".";
        statusError_ = true;
        return;
    }
    if (!saveRig(settings_.rigsFolder, name, Rig{})) {
        status_ = "Could not write the rig '" + name + "'.";
        statusError_ = true;
        return;
    }
    rigNames_ = listRigs(settings_.rigsFolder);
    activate(name);
    status_ = "New rig '" + name + "': touch each control and say what it is, then Save.";
}

void WheelSetupView::renameRig() {
    const std::string name = trimmed(popupName_);
    if (name == rigName_) {
        popup_ = Popup::None;
        editing_ = nullptr;
        return;
    }
    std::vector<std::string> others = rigNames_;
    others.erase(std::remove(others.begin(), others.end(), rigName_), others.end());
    std::string problem = rigNameProblem(name, others);
    if (problem.empty() && rigSaved_) problem = wheelbri::renameRig(settings_.rigsFolder, rigName_, name);
    if (!problem.empty()) {
        status_ = "Not renamed: " + problem + ".";
        statusError_ = true;
        return;
    }
    WE_LOG_INFO(log(), "wheel setup: rig '{}' renamed '{}'", rigName_, name);
    rigName_ = name;
    rigNames_ = listRigs(settings_.rigsFolder);
    if (rigSaved_) writeAppSetting(settings_.stateFile, "rig", name);
    popup_ = Popup::None;
    editing_ = nullptr;
    status_ = "Renamed to '" + name + "'.";
    statusError_ = false;
}

RigDevice* WheelSetupView::shownDevice() { return shown_.empty() ? nullptr : edit_.find(shown_); }

const RigDevice* WheelSetupView::shownDevice() const { return shown_.empty() ? nullptr : edit_.find(shown_); }

RigDevice* WheelSetupView::addShownDevice() {
    if (RigDevice* const device = shownDevice()) return device;
    if (shown_.empty()) return nullptr;
    if (edit_.devices.size() >= Rig::kMaxDevices) {
        status_ = "A rig has at most " + std::to_string(Rig::kMaxDevices) + " devices: remove one first.";
        statusError_ = true;
        return nullptr;
    }
    RigDevice device;
    device.id = shown_;
    device.name = fit(systemName(shown_), kMaxDeviceName);
    device.kind = guessKind(device.name);
    editing_ = nullptr;   // a name being typed may point into the list
    edit_.devices.push_back(std::move(device));
    return &edit_.devices.back();
}

void WheelSetupView::removeShownDevice() {
    editing_ = nullptr;
    edit_.devices.erase(std::remove_if(edit_.devices.begin(), edit_.devices.end(),
                                       [this](const RigDevice& device) { return device.id == shown_; }),
                        edit_.devices.end());
}

// --- the control popup --------------------------------------------------------------------------------------------

void WheelSetupView::openControl(const bool axis, const int index, const std::optional<float> rest) {
    popup_ = Popup::Control;
    popupAxis_ = axis;
    popupIndex_ = index;
    popupRest_ = rest;
    const RigDevice* const device = shownDevice();
    popupControl_ = device == nullptr ? RigControl{}
                    : axis           ? device->axes[static_cast<std::size_t>(index)]
                                     : device->buttons[static_cast<std::size_t>(index)];
    if (axis && rest && !popupControl_.defined()) popupControl_.invert = *rest > 0.5F;   // a pedal resting at the top
    editing_ = &popupControl_.label;   // ready to type
    editingMax_ = DeviceLabels::kMaxLabel;
}

void WheelSetupView::closeControl(const bool keep) {
    popup_ = Popup::None;
    editing_ = nullptr;
    if (!keep) return;
    RigDevice* const device = addShownDevice();
    if (device == nullptr) return;
    popupControl_.label = trimmed(popupControl_.label);
    RigControl& control = popupAxis_ ? device->axes[static_cast<std::size_t>(popupIndex_)]
                                     : device->buttons[static_cast<std::size_t>(popupIndex_)];
    control = popupControl_;
    if (!popupAxis_) control.invert = false;
}

// --- input ----------------------------------------------------------------------------------------------------------

float WheelSetupView::axisValue(const int axis) const {
    const RawInputState* const raw = liveRaw();
    if (raw == nullptr || axis < 0 || axis >= kAxes) return 0.0F;
    return raw->axes[static_cast<std::size_t>(axis)];
}

bool WheelSetupView::buttonDown(const int source) const {
    const RawInputState* const raw = liveRaw();
    return raw != nullptr && source >= 0 && source < kButtons && ((raw->buttons >> source) & 1U) != 0;
}

void WheelSetupView::onClick(const float x, const float y) {
    if (popup_ == Popup::None) editing_ = nullptr;   // a click ends typing; a name field's own click starts it again
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) {
            const auto action = it->action;   // a copy: the list belongs to the last draw
            action();
            return;
        }
    }
}

void WheelSetupView::onText(const std::string& text) {
    if (editing_ == nullptr) return;
    for (const char c : text) {
        if (c == ui::kBackspace) {
            if (!editing_->empty()) editing_->pop_back();
        } else if (c == ui::kEnter) {
            switch (popup_) {
                case Popup::Control: closeControl(true); break;
                case Popup::NewRig: newRig(); break;
                case Popup::RenameRig: renameRig(); break;
                case Popup::None: editing_ = nullptr; break;
            }
            return;
        } else if (c != ';' && c != '#' && c != '=' && c != '[' && c != ']' && c != '|' &&
                   editing_->size() < editingMax_ && !(editing_->empty() && c == ' ') &&
                   static_cast<unsigned char>(c) >= ' ' && static_cast<unsigned char>(c) < 0x7F) {
            *editing_ += c;   // nothing that would read as a comment, a section or a role in the file
        }
    }
}

void WheelSetupView::onKey(const char key) {
    if (key != ui::kEscape) return;
    if (popup_ == Popup::Control) {
        closeControl(false);   // Escape in the popup: drop what was typed
    } else {
        popup_ = Popup::None;
    }
    editing_ = nullptr;
}

// --- drawing helpers -------------------------------------------------------------------------------------------------

void WheelSetupView::hit(const float x, const float y, const float w, const float h, std::function<void()> action) {
    hits_.push_back({x, y, w, h, std::move(action)});
}

float WheelSetupView::button(const float x, const float y, const std::string& label, std::function<void()> action,
                             const bool on, const bool enabled, const float width) {
    const float w = width > 0 ? width : static_cast<float>(label.size()) * ui::kCharWidth + 14;
    window_.fill(x, y, w, 16, !enabled ? kDark : on ? kHeading : kPanel);
    window_.outline(x, y, w, 16, enabled ? (on ? kHeading : kDim) : kPanel);
    const float textX = x + (w - static_cast<float>(label.size()) * ui::kCharWidth) / 2;
    window_.text(textX, y + 4, label, !enabled ? kDim : on ? kDark : kText);
    if (enabled) hit(x, y, w, 16, std::move(action));
    return w;
}

void WheelSetupView::drawNameField(const float x, const float y, std::string& name, const std::size_t maxChars) {
    const float width = static_cast<float>(maxChars) * ui::kCharWidth + 16;
    const bool editing = editing_ == &name;
    window_.fill(x, y, width, 16, editing ? kDark : kPanel);
    window_.outline(x, y, width, 16, editing ? kHeading : kDim);
    const std::string shown = name + (editing && (platform::nowMs() / 500) % 2 == 0 ? "_" : "");
    window_.text(x + 6, y + 4, shown.empty() ? "(none)" : shown, name.empty() && !editing ? kDim : kText);
    hit(x, y, width, 16, [this, &name, maxChars] {
        editing_ = &name;
        editingMax_ = maxChars;
    });
}

// --- drawing ----------------------------------------------------------------------------------------------------------

void WheelSetupView::draw() {
    hits_.clear();
    drawHeader();
    if (shown_.empty()) {
        window_.text(4, kAxesTop, "Plug in your wheel, pedals, shifter or button box: they show up here within two seconds.",
                     kText);
        if (!status_.empty()) window_.text(4, kAxesTop + 20, fit(status_, 118), statusError_ ? kWarning : kActive);
        if (popup_ == Popup::NewRig || popup_ == Popup::RenameRig) drawNamePopup();
        return;
    }
    drawAxes(kAxesTop);
    drawButtons(kButtonsTop);

    window_.text(4, kHintTop,
                 detect_ ? "Detect is on: touch a new button or axis (a pedal all the way); defined ones only light up (click to change)."
                         : "Click an axis or a button to say what it is (or turn Detect on and just touch it).",
                 kDim);
    window_.text(4, kHintTop + 12, "Save keeps the rig; the Mapping tab maps its controls onto the G920.", kDim);
    if (lastPressed_ >= 0) {
        const RigDevice* const device = shownDevice();
        const std::string label =
            device != nullptr ? describe(device->buttons[static_cast<std::size_t>(lastPressed_)]) : std::string();
        window_.text(4, kHintTop + 26,
                     "Last pressed: button " + std::to_string(lastPressed_) + (label.empty() ? "" : "  " + label), kHeading);
    }
    if (!status_.empty()) window_.text(4, kHintTop + 40, fit(status_, 118), statusError_ ? kWarning : kActive);
    if (popup_ == Popup::Control) drawControlPopup();
    if (popup_ == Popup::NewRig || popup_ == Popup::RenameRig) drawNamePopup();
}

void WheelSetupView::drawHeader() {
    // The rig.
    window_.text(4, 6, "WHEEL SETUP", kHeading);
    window_.text(108, 6, "Rig", kDim);
    const std::size_t rigs = rigNames_.size();
    const auto rigIndex = static_cast<std::size_t>(
        std::find(rigNames_.begin(), rigNames_.end(), rigName_) - rigNames_.begin());
    const bool canSwitch = rigs > 1 || (rigs == 1 && !rigSaved_);
    float x = 140;
    x += button(x, 2, "<", [this, rigs, rigIndex] {
        if (rigs == 0) return;
        activate(rigNames_[rigIndex < rigs ? (rigIndex + rigs - 1) % rigs : 0]);
    }, false, canSwitch, 20) + 2;
    window_.fill(x, 2, 260, 16, kStripe);
    window_.text(x + 6, 6, fit(rigName_, 22) + (rigSaved_ ? "" : " (not saved)"), rigSaved_ ? kText : kCaution);
    x += 262;
    x += button(x, 2, ">", [this, rigs, rigIndex] {
        if (rigs == 0) return;
        activate(rigNames_[rigIndex < rigs ? (rigIndex + 1) % rigs : 0]);
    }, false, canSwitch, 20) + 6;
    x += button(x, 2, "New", [this] {
        popup_ = Popup::NewRig;
        popupName_.clear();
        editing_ = &popupName_;
        editingMax_ = kMaxConfigName;
    }, false, true, 48) + 4;
    button(x, 2, "Rename", [this] {
        popup_ = Popup::RenameRig;
        popupName_ = rigName_;
        editing_ = &popupName_;
        editingMax_ = kMaxConfigName;
    }, false, true, 64);
    const std::string state = !rigSaved_ ? "not saved yet" : dirty() ? "unsaved changes" : "saved";
    window_.text(650, 6, state, dirty() ? kCaution : kDim);
    button(800, 2, "Save", [this] { save(); }, dirty(), dirty(), 70);
    button(876, 2, "Revert", [this] { revert(); }, false, rigSaved_ && dirty(), 80);

    // The device shown.
    window_.text(4, 26, "Device", kDim);
    const std::vector<DeviceId> list = shownList();
    x = 68;
    if (list.empty()) {
        window_.text(x, 26, "no game controller found: plug one in", kCaution);
        return;
    }
    const auto at = static_cast<std::size_t>(std::find(list.begin(), list.end(), shown_) - list.begin());
    const std::size_t count = list.size();
    x += button(x, 22, "<", [this, list, at, count] { shown_ = list[(at + count - 1) % count]; }, false, count > 1, 24) + 4;
    const RigDevice* const device = shownDevice();
    const bool connected = openFor(shown_) != nullptr;
    window_.fill(x, 22, 420, 16, kStripe);
    window_.text(x + 6, 26, fit(device != nullptr ? device->name : systemName(shown_), 34) + "  " + idText(shown_),
                 connected ? kText : kDim);
    x += 424;
    button(x, 22, ">", [this, list, at, count] { shown_ = list[(at + 1) % count]; }, false, count > 1, 24);
    window_.text(x + 30, 26, std::to_string(std::min(at, count - 1) + 1) + " of " + std::to_string(count), kDim);
    if (fromSession_) {
        window_.text(620, 26, "(the session's)", kDim);
    } else if (device != nullptr) {
        button(620, 22, "Remove from rig", [this] { removeShownDevice(); }, false, true, 140);
        if (!connected) window_.text(766, 26, "unplugged", kCaution);
    } else {
        button(620, 22, "Add to rig", [this] { addShownDevice(); }, false, true, 140);
    }
    button(800, 22, detect_ ? "Detect: on" : "Detect: off", [this] {
        detect_ = !detect_;
        for (Open& d : open_) d.restTaken = false;   // where everything rests now
    }, detect_, true, 156);

    if (device == nullptr) {
        window_.text(4, 46, "Not in the rig yet: touch one of its controls (or Add to rig) to add it.", kDim);
        return;
    }
    RigDevice* const editable = shownDevice();
    window_.text(4, 46, "Name", kDim);
    drawNameField(68, 42, editable->name, kMaxDeviceName);
    window_.text(4, 64, "It is a", kDim);
    x = 68;
    for (const DeviceKind kind : kDeviceKinds) {
        x += button(x, 60, capitalised(deviceKindName(kind)), [this, kind] {
            if (RigDevice* const d = shownDevice()) d->kind = kind;
        }, editable->kind == kind, true, 100) + 6;
    }
}

void WheelSetupView::drawAxes(const float top) {
    window_.text(4, top, "AXES", kHeading);
    window_.text(48, top, "move them; click one to say what it is", kDim);
    const RigDevice* const device = shownDevice();
    const int axes = axisCount();
    const bool live = liveRaw() != nullptr;
    for (int axis = 0; axis < kAxes; ++axis) {
        const float y = top + 16 + static_cast<float>(axis) * kAxisRow;
        if (axis % 2 == 0) window_.fill(0, y - 3, kWidth, kAxisRow - 1, kStripe);
        const RigControl control = device != nullptr ? device->axes[static_cast<std::size_t>(axis)] : RigControl{};
        const std::string text = describe(control);
        window_.text(4, y, std::to_string(axis), kDim);
        window_.text(20, y, text.empty() ? "(not defined)" : fit(text, 34), text.empty() ? kDim : kText);
        hit(0, y - 3, kWidth, kAxisRow - 1, [this, axis] { openControl(true, axis, std::nullopt); });
        if (live && axes > 0 && axis >= axes) {
            window_.text(300, y, "(this device has no axis " + std::to_string(axis) + ")", kDim);
            continue;
        }
        // -1..1 from the centre, as the system reports it.
        constexpr float kBarX = 300;
        constexpr float kBarW = 420;
        const float v = std::clamp(axisValue(axis), -1.0F, 1.0F);
        const float half = kBarW / 2;
        window_.fill(kBarX, y - 1, kBarW, 10, kPanel);
        window_.fill(v >= 0 ? kBarX + half : kBarX + half + half * v, y - 1, half * std::abs(v), 10,
                     control.defined() ? kActive : kCentre);
        window_.fill(kBarX + half, y - 2, 1, 12, kCentre);
        char value[16];
        std::snprintf(value, sizeof(value), "%+.3f", static_cast<double>(v));
        window_.text(732, y, live ? value : "unplugged", kDim);
    }
}

void WheelSetupView::drawButtons(const float top) {
    window_.text(4, top, "BUTTONS", kHeading);
    window_.text(68, top, "press them (the one pressed lights up); click one to say what it is", kDim);
    const RigDevice* const device = shownDevice();
    const int buttons = buttonCount();
    const bool live = liveRaw() != nullptr;
    for (int source = 0; source < kButtons; ++source) {
        const float x = 4 + static_cast<float>(source % kColumns) * kTileStepX;
        const float y = top + 16 + static_cast<float>(source / kColumns) * kTileStepY;
        const bool present = !live || buttons == 0 || source < buttons;
        const bool down = buttonDown(source);
        const RigControl control = device != nullptr ? device->buttons[static_cast<std::size_t>(source)] : RigControl{};
        window_.fill(x, y, kTileW, kTileH, !present ? kDark : down ? kActive : control.defined() ? kPanel : kStripe);
        if (source == lastPressed_) window_.outline(x, y, kTileW, kTileH, kHeading);
        char number[8];
        std::snprintf(number, sizeof(number), "%2d", source);
        window_.text(x + 4, y + 5, number, down ? kDark : kDim);
        window_.text(x + 26, y + 5, fit(control.label, 11), down ? kDark : kText);
        window_.text(x + 4, y + 18, fit(roleName(control.role), 13), down ? kDark : kHeading);
        hit(x, y, kTileW, kTileH, [this, source] { openControl(false, source, std::nullopt); });
    }
}

void WheelSetupView::drawControlPopup() {
    constexpr float kX = 100;
    constexpr float kY = 130;
    constexpr float kW = 760;
    constexpr float kH = 210;
    hit(0, 0, kWidth, kContentHeight, [] {});   // clicks outside the popup do nothing
    window_.fill(kX, kY, kW, kH, kPopup);
    window_.outline(kX, kY, kW, kH, kHeading);
    const float left = kX + 16;
    const RigDevice* const device = shownDevice();
    const std::string deviceName = device != nullptr ? device->name : systemName(shown_);
    window_.text(left, kY + 12,
                 (popupAxis_ ? "Axis " : "Button ") + std::to_string(popupIndex_) + " on " + fit(deviceName, 40), kHeading);
    if (popupAxis_) {
        const float v = std::clamp(axisValue(popupIndex_), -1.0F, 1.0F);
        window_.fill(left + 480, kY + 11, 240, 10, kPanel);
        window_.fill(v >= 0 ? left + 600 : left + 600 + 120 * v, kY + 11, 120 * std::abs(v), 10, kActive);
        window_.fill(left + 600, kY + 10, 1, 12, kCentre);
    } else if (buttonDown(popupIndex_)) {
        window_.fill(left + 480, kY + 9, 80, 14, kActive);
        window_.text(left + 488, kY + 12, "pressed", kDark);
    }

    window_.text(left, kY + 38, "Name", kDim);
    drawNameField(left + 60, kY + 34, popupControl_.label, DeviceLabels::kMaxLabel);
    window_.text(left + 60 + DeviceLabels::kMaxLabel * ui::kCharWidth + 28, kY + 38,
                 "what it says on the device (12 characters at most)", kDim);

    window_.text(left, kY + 62, "It is", kDim);
    float x = left + 60;
    float y = kY + 58;
    const auto roleButton = [&](const ControlRole role, const std::string& label, const float width) {
        if (x + width > kX + kW - 12) {
            x = left + 60;
            y += 20;
        }
        x += button(x, y, label, [this, role] {
            popupControl_.role = role;
            if (isPedalRole(role) && popupRest_) popupControl_.invert = *popupRest_ > 0.5F;
        }, popupControl_.role == role, true, width) + 4;
    };
    roleButton(ControlRole::None, "nothing yet", 100);
    if (popupAxis_) {
        for (const ControlRole role : kAxisRoles) roleButton(role, capitalised(roleName(role)), 100);
    } else {
        for (const ControlRole role : kButtonRoles) roleButton(role, capitalised(roleName(role)), 100);
    }

    y += 26;
    if (popupAxis_) {
        button(left, y, "Inverted", [this] { popupControl_.invert = !popupControl_.invert; }, popupControl_.invert, true,
               90);
        std::string note = "inverted: released (or centred) at +1 instead of -1";
        if (popupRest_) {
            char rest[64];
            std::snprintf(rest, sizeof(rest), "it rested at %+.2f when it was found", static_cast<double>(*popupRest_));
            note = std::string(rest) + (*popupRest_ > 0.5F ? ": a pedal released at the top, so inverted" : "");
        }
        window_.text(left + 100, y + 4, note, kDim);
    }
    const float by = kY + kH - 26;
    button(left, by, "OK", [this] { closeControl(true); }, true, true, 80);
    button(left + 88, by, "Cancel", [this] { closeControl(false); }, false, true, 80);
    button(left + 176, by, "Clear", [this] {
        popupControl_ = RigControl{};
        closeControl(true);
    }, false, true, 80);
    window_.text(left + 270, by + 4, "Enter keeps it, Esc drops it.", kDim);
}

void WheelSetupView::drawNamePopup() {
    constexpr float kX = 200;
    constexpr float kY = 160;
    constexpr float kW = 560;
    constexpr float kH = 120;
    hit(0, 0, kWidth, kContentHeight, [] {});
    window_.fill(kX, kY, kW, kH, kPopup);
    window_.outline(kX, kY, kW, kH, kHeading);
    const bool isNew = popup_ == Popup::NewRig;
    window_.text(kX + 16, kY + 14, isNew ? "New rig" : "Rename the rig '" + fit(rigName_, 30) + "'", kHeading);
    window_.text(kX + 16, kY + 42, "Name", kDim);
    drawNameField(kX + 64, kY + 38, popupName_, kMaxConfigName);
    window_.text(kX + 16, kY + 66,
                 statusError_ && !status_.empty() ? fit(status_, 64)
                 : isNew && dirty()               ? std::string("Unsaved changes to this rig are dropped.")
                                                  : std::string("Letters, digits, spaces, - _ . ( ). Enter keeps it."),
                 statusError_ && !status_.empty() ? kWarning : kDim);
    const float by = kY + kH - 26;
    button(kX + 16, by, isNew ? "Create" : "Rename", [this, isNew] { isNew ? newRig() : renameRig(); }, true,
           !trimmed(popupName_).empty(), 80);
    button(kX + 104, by, "Cancel", [this] {
        popup_ = Popup::None;
        editing_ = nullptr;
    }, false, true, 80);
}

}  // namespace wheelbri::service
