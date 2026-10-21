#include "WheelSetupView.hpp"
#include "UiStyle.hpp"

#include "Platform.hpp"
#include "Version.hpp"

#include "wheelbri/AxisTuning.hpp"
#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/DeviceLabels.hpp"
#include "wheelbri/Logging.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <span>
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
constexpr Colour kPanel = style::kPanel;
constexpr Colour kStripe = style::kField;
constexpr Colour kText = style::kText;
constexpr Colour kDim = style::kDim;
constexpr Colour kHeading = style::kHeading;
constexpr Colour kActive = style::kActive;
constexpr Colour kWarning = style::kWarning;
constexpr Colour kCaution = style::kCaution;
constexpr Colour kDark = style::kDark;
constexpr Colour kCentre{160, 164, 172};

constexpr int kAxes = static_cast<int>(RawInputState::kMaxAxes);
constexpr int kButtons = static_cast<int>(MapperConfig::kMaxSourceButtons);
constexpr std::size_t kMaxDeviceName = 40;
constexpr float kTrigger = 0.5F;               // Detect: an axis this far from where it rested has been moved
constexpr float kBack = 0.15F;                 // ...and is armed again once back this close
constexpr int kSettleFrames = 20;              // about a third of a second of reads before the rest is taken
const std::string kFirstRig = "My rig";

// Layout: the header (two rows), the devices' sections (scrolled), and under them the list of devices (left) and the
// hints (right). A section: its name row, its axes, its buttons as tiles.
constexpr float kDevicesTop = 46;
constexpr float kSectionHead = 22;
constexpr float kAxisRow = 17;
constexpr float kButtonsHead = 16;
constexpr float kTileW = 114;                  // at least: more columns in a wider window (tileColumns)
constexpr float kTileH = 30;
constexpr float kTileStepY = 33;
constexpr float kSectionGap = 14;
constexpr float kFoldRow = 24;                 // a line for the unnamed controls (Show all / Named only)
constexpr float kListRow = 18;                 // a line of the bottom-left list
constexpr float kListW = 470;                  // the list's width (the hints right of it)
constexpr float kAxisBarX = 440;               // an axis row: the description left of it, the value right of it
constexpr float kAxisBarW = 400;               // (both in the design: a wider window moves and widens the bar)
constexpr float kScrollStep = 40;              // pixels per mouse wheel notch (and per Up / Down)
constexpr std::size_t kListRows = 6;           // the bottom-left list's lines; more scroll
constexpr float kBarW = 8;                     // the devices' scroll bar

std::string fit(std::string text, const std::size_t chars) {
    if (text.size() > chars) text.resize(chars);
    return text;
}

// `text` in lines of at most `chars`, broken at spaces; at most `maxLines` (the last one cut).
std::vector<std::string> wrapped(const std::string& text, const std::size_t chars, const std::size_t maxLines) {
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

std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(' ') - first + 1);
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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

// What a control is called (its name, or what it is).
std::string describe(const RigControl& control) { return control.displayName(); }

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
    relist_ = RelistTimer(relistSettings());
    listLogged_ = false;
}

RelistTimer::Settings WheelSetupView::relistSettings() {
    return platform::DeviceChanges::shared().available() ? RelistTimer::Settings{}
                                                         : RelistTimer::Settings::withoutNotifications();
}

bool WheelSetupView::refreshDevices(const bool force) {
    // Only when a device came or went (a quick look at a counter on every frame), not every 2 s: listing through
    // DirectInput can hold this window's thread for a good part of a second.
    const std::uint64_t changes = platform::DeviceChanges::shared().counter().count();
    if (!force && !relist_.due(platform::nowMs(), changes)) return false;
    if (backend_ == nullptr) {
        backend_ = startWheelBackend();
        if (backend_ == nullptr) {
            relist_.listed(platform::nowMs(), changes);   // tried again at the next change, or 30 s on
            status_ = "Cannot read game controllers on this computer (the log says why).";
            statusError_ = true;
            return false;
        }
        WE_LOG_INFO(log(), "wheel setup: looking for game controllers through {}", backend_->describe());
    }
    std::vector<WheelInfo> found = backend_->list();
    // The time is taken after listing: DirectInput can take a good part of a second to list.
    relist_.listed(platform::nowMs(), changes);
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
    // Controls already named in the rig never open the popup by themselves: pressing or moving
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
    if (first >= 0) {
        lastPressed_ = first;
        lastPressedDevice_ = id;
    }
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
        lastPressed_ = newButton;
        lastPressedDevice_ = id;
        reveal_ = id;   // its section comes into view behind the popup
        openControl(id, false, newButton, std::nullopt);
    } else if (moved >= 0) {
        device.armed[static_cast<std::size_t>(moved)] = false;
        reveal_ = id;
        openControl(id, true, moved, device.rest.axes[static_cast<std::size_t>(moved)]);
    }
}

std::vector<DeviceId> WheelSetupView::shownDevices() const {
    std::vector<DeviceId> list;
    const auto add = [&list](const DeviceId& id) {
        if (std::find(list.begin(), list.end(), id) == list.end()) list.push_back(id);
    };
    for (const RigDevice& device : edit_.devices) add(device.id);   // saved ones too, connected or not
    for (const Open& device : open_) add(idOf(device.info));
    return list;
}

int WheelSetupView::tileCount(const DeviceId& id) const {
    const int buttons = buttonCount(id);
    if (buttons >= 0) return std::min(buttons, kButtons);
    // Not read now (unplugged, or the session has it): the rig's named buttons, up to the last one.
    int last = -1;
    if (const RigDevice* const device = edit_.find(id)) {
        for (int b = 0; b < kButtons; ++b) {
            if (device->buttons[static_cast<std::size_t>(b)].defined()) last = b;
        }
    }
    return last + 1;
}

std::string WheelSetupView::statusText(const DeviceId& id) const {
    if (connected(id)) return "Connected";
    return fromSession_ ? "-" : "Not connected";
}

bool WheelSetupView::connected(const DeviceId& id) const {
    return openFor(id) != nullptr ||
           std::any_of(connected_.begin(), connected_.end(), [&id](const WheelInfo& info) { return idOf(info) == id; });
}

std::vector<DeviceId> WheelSetupView::listedDevices() const {
    std::vector<DeviceId> list = shownDevices();
    if (!fromSession_) {
        for (const WheelInfo& info : connected_) {   // connected, not opened (yet)
            if (std::find(list.begin(), list.end(), idOf(info)) == list.end()) list.push_back(idOf(info));
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

int WheelSetupView::axisCount(const DeviceId& id) const {
    if (fromSession_) return idOf(sessionInfo_) == id ? sessionAxes_ : -1;
    const Open* const device = openFor(id);
    return device != nullptr && device->wheel != nullptr ? device->wheel->axes() : -1;
}

int WheelSetupView::buttonCount(const DeviceId& id) const {
    if (fromSession_) return idOf(sessionInfo_) == id ? sessionButtons_ : -1;
    const Open* const device = openFor(id);
    return device != nullptr && device->wheel != nullptr ? device->wheel->buttons() : -1;
}

const RawInputState* WheelSetupView::liveRaw(const DeviceId& id) const {
    const Open* const device = openFor(id);
    return device != nullptr && device->haveRaw ? &device->raw : nullptr;
}

// --- the rig ----------------------------------------------------------------------------------------------------

void WheelSetupView::loadRigs() {
    // One rig: the one the app remembers ([app] rig), else the first file in the folder. Other files there (from when
    // the tab managed several rigs) are left as they are.
    const std::vector<std::string> names = listRigs(settings_.rigsFolder);
    const std::string active = readAppSetting(settings_.stateFile, "rig");
    if (std::find(names.begin(), names.end(), active) != names.end()) {
        activate(active);
    } else if (!names.empty()) {
        activate(names.front());
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
        status_ = "No rig yet: touch each control and say what it is (saved as you go).";
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
        status_ = "Cannot read the rig (" + name + ".ini).";
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
    status_ = errors.empty() ? std::string() : "The rig: " + errors.front();
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
    writeAppSetting(settings_.stateFile, "rig", rigName_);
    status_ = "Saved the rig (" + std::to_string(edit_.devices.size()) + " device(s)). The Mapping tab maps from it.";
    statusError_ = false;
    WE_LOG_INFO(log(), "wheel setup: saved rig '{}' ({} device(s))", rigName_, edit_.devices.size());
}

void WheelSetupView::autoSave() {
    if (edit_ == saved_ || popup_ != Popup::None || editing_ != nullptr || fromSession_) return;
    if (notSaved_ && *notSaved_ == edit_) return;   // said why already: not again
    const Rig before = saved_;
    const bool wasSaved = rigSaved_;
    const bool undoing = std::exchange(undoing_, false);
    save();
    if (statusError_ || edit_ != saved_) {
        notSaved_ = edit_;
        afterSave_.clear();   // the status says why it was not saved
        return;
    }
    notSaved_.reset();
    const std::string said = std::exchange(afterSave_, {});
    if (undoing) {
        status_ = "Undone and saved.";
    } else if (!said.empty()) {
        status_ = said;   // what the change was (Forget), rather than the usual "Saved the rig"
    }
    if (!undoing && wasSaved) {
        undo_.push_back(before);
        if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    }
}

void WheelSetupView::undo() {
    popup_ = Popup::None;
    editing_ = nullptr;
    if (edit_ != saved_) {   // a change that could not be saved: dropped
        edit_ = saved_;
        edit_.devices.reserve(Rig::kMaxDevices);
        notSaved_.reset();
        status_ = "Change dropped.";
        statusError_ = false;
        return;
    }
    if (undo_.empty()) return;
    edit_ = undo_.back();
    edit_.devices.reserve(Rig::kMaxDevices);
    undo_.pop_back();
    undoing_ = true;   // autoSave writes it, without putting it on the list again
    WE_LOG_INFO(log(), "wheel setup: undo ({} more)", undo_.size());
}

RigDevice* WheelSetupView::focusDevice() { return focus_.empty() ? nullptr : edit_.find(focus_); }

const RigDevice* WheelSetupView::focusDevice() const { return focus_.empty() ? nullptr : edit_.find(focus_); }

RigDevice* WheelSetupView::addFocusDevice() {
    if (RigDevice* const device = focusDevice()) return device;
    if (focus_.empty()) return nullptr;
    if (edit_.devices.size() >= Rig::kMaxDevices) {
        status_ = "A rig has at most " + std::to_string(Rig::kMaxDevices) + " devices: Forget one first (bottom left).";
        statusError_ = true;
        return nullptr;
    }
    RigDevice device;
    device.id = focus_;
    device.name = fit(systemName(focus_), kMaxDeviceName);
    device.kind = guessKind(device.name);
    editing_ = nullptr;   // a name being typed may point into the list
    edit_.devices.push_back(std::move(device));
    WE_LOG_INFO(log(), "wheel setup: '{}' ({}) joins the rig", edit_.devices.back().name, idText(focus_));
    return &edit_.devices.back();
}

void WheelSetupView::forget(const DeviceId& id) {
    editing_ = nullptr;
    const auto before = edit_.devices.size();
    edit_.devices.erase(std::remove_if(edit_.devices.begin(), edit_.devices.end(),
                                       [&id](const RigDevice& device) { return device.id == id; }),
                        edit_.devices.end());
    if (edit_.devices.size() != before) {
        // Said once it is saved (autoSave), so the save's own line does not replace it.
        afterSave_ = "Forgot " + idText(id) + " and its names (saved): Undo brings it back.";
        status_ = afterSave_;
        statusError_ = false;
        WE_LOG_INFO(log(), "wheel setup: {} taken out of the rig", idText(id));
    }
}

// --- the control popup --------------------------------------------------------------------------------------------

void WheelSetupView::openControl(const DeviceId& id, const bool axis, const int index,
                                 const std::optional<float> rest) {
    focus_ = id;
    popup_ = Popup::Control;
    popupAxis_ = axis;
    popupIndex_ = index;
    popupRest_ = rest;
    const RigDevice* const device = focusDevice();
    popupControl_ = device == nullptr ? RigControl{}
                    : axis           ? device->axes[static_cast<std::size_t>(index)]
                                     : device->buttons[static_cast<std::size_t>(index)];
    editing_ = &popupControl_.label;   // ready to type
    editingMax_ = DeviceLabels::kMaxLabel;
}

void WheelSetupView::closeControl(const bool keep) {
    popup_ = Popup::None;
    editing_ = nullptr;
    if (!keep) return;
    RigDevice* const device = addFocusDevice();   // naming a control puts its device in the rig
    if (device == nullptr) return;
    popupControl_.label = trimmed(popupControl_.label);
    RigControl& control = popupAxis_ ? device->axes[static_cast<std::size_t>(popupIndex_)]
                                     : device->buttons[static_cast<std::size_t>(popupIndex_)];
    control = popupControl_;
}

// --- input ----------------------------------------------------------------------------------------------------------

float WheelSetupView::axisValue(const DeviceId& id, const int axis) const {
    const RawInputState* const raw = liveRaw(id);
    if (raw == nullptr || axis < 0 || axis >= kAxes) return 0.0F;
    return raw->axes[static_cast<std::size_t>(axis)];
}

bool WheelSetupView::buttonDown(const DeviceId& id, const int source) const {
    const RawInputState* const raw = liveRaw(id);
    return raw != nullptr && source >= 0 && source < kButtons && ((raw->buttons >> source) & 1U) != 0;
}

void WheelSetupView::onClick(const float x, const float y) {
    if (popup_ == Popup::None) editing_ = nullptr;   // a click ends typing; a name field's own click starts it again
    // A press on the scroll bar: on the thumb it is taken where pressed; elsewhere on the track the thumb jumps there
    // (centred on the mouse). Either way the drag goes on while the button is held (onPointer).
    if (popup_ == Popup::None && bar_.shown && x >= bar_.x - 3 && x < bar_.x + bar_.w + 3 && y >= bar_.y &&
        y < bar_.y + bar_.h) {
        const float offset = y - (bar_.y + bar_.thumbStart);
        dragGrab_ = offset >= 0 && offset < bar_.thumbSize ? offset : bar_.thumbSize / 2;
        dragging_ = true;
        dragTo(y);
        return;
    }
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) {
            const auto action = it->action;   // a copy: the list belongs to the last draw
            hits_.clear();                    // stale until the next draw: a second click before it does nothing
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
                case Popup::None: editing_ = nullptr; break;
            }
            return;
        } else if (c != ';' && c != '#' && c != '=' && c != '[' && c != ']' && c != '|' &&
                   editing_->size() < editingMax_ && !(editing_->empty() && c == ' ') &&
                   static_cast<unsigned char>(c) >= ' ' && static_cast<unsigned char>(c) < 0x7F) {
            *editing_ += c;   // nothing that would read as a comment, a section or a second part in the file
        }
    }
}

void WheelSetupView::onKey(const char key) {
    if (key != ui::kEscape) {
        if (popup_ != Popup::None || editing_ != nullptr) return;
        if (key == ui::kPageDown) scrollBy(scrollRoom_ * 0.9F);
        if (key == ui::kPageUp) scrollBy(-scrollRoom_ * 0.9F);
        if (key == ui::kDown) scrollBy(kScrollStep);
        if (key == ui::kUp) scrollBy(-kScrollStep);
        if (key == ui::kHome) scrollBy(-scrollMost_ - 1);
        if (key == ui::kEnd) scrollBy(scrollMost_ + 1);
        return;
    }
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
    style::button(window_, x, y, w, 16, label, on, enabled);
    if (enabled) hit(x, y, w, 16, std::move(action));
    return w;
}

void WheelSetupView::drawNameField(const float x, const float y, std::string& name, const std::size_t maxChars) {
    const float width = static_cast<float>(maxChars) * ui::kCharWidth + 16;
    const bool editing = editing_ == &name;
    window_.fill(x, y, width, 16, kStripe);
    window_.outline(x, y, width, 16, editing ? kHeading : style::kFieldEdge);
    const std::string shown = name + (editing && (platform::nowMs() / 500) % 2 == 0 ? "_" : "");
    window_.text(x + 6, y + 4, shown.empty() ? "(none)" : shown, name.empty() && !editing ? kDim : kText);
    hit(x, y, width, 16, [this, &name, maxChars] {
        editing_ = &name;
        editingMax_ = maxChars;
    });
}

// --- drawing ----------------------------------------------------------------------------------------------------------

void WheelSetupView::onScroll(const int notches) {
    if (popup_ != Popup::None) return;
    // Over the device list (when it has more lines than it shows): the list.
    const std::size_t listed = listedDevices().size();
    if (pointerY_ >= listTop_ && pointerY_ < listBottom_ && pointerX_ < kListW && listed > kListRows) {
        const auto most = static_cast<long long>(listed - kListRows);
        listScroll_ = static_cast<std::size_t>(
            std::clamp(static_cast<long long>(listScroll_) - static_cast<long long>(notches), 0LL, most));
        return;
    }
    scrollBy(-static_cast<float>(notches) * kScrollStep);
}

void WheelSetupView::scrollBy(const float pixels) { scroll_ = std::clamp(scroll_ + pixels, 0.0F, scrollMost_); }

void WheelSetupView::onPointer(const float x, const float y, const bool leftDown) {
    pointerX_ = x;
    pointerY_ = y;
    if (!leftDown) {
        dragging_ = false;
        return;
    }
    if (dragging_) dragTo(y);
}

void WheelSetupView::dragTo(const float y) {
    // The thumb's start follows the mouse (less where it was taken); its place along the free track is the scroll.
    if (!bar_.shown) return;
    const float free = std::max(1.0F, bar_.h - bar_.thumbSize);
    const float at = std::clamp((y - dragGrab_ - bar_.y) / free, 0.0F, 1.0F);
    scroll_ = at * scrollMost_;
}

int WheelSetupView::tileColumns() const {
    // As many tiles of at least kTileW as fit (8 in the design's 960).
    return std::max(8, static_cast<int>((window_.width() - 8 + 4) / (kTileW + 4)));
}

float WheelSetupView::bottomPanelHeight() const {
    // The list: its heading and a line per device (at least one, at most kListRows: more scroll), or the hints' lines,
    // whichever is taller.
    const auto rows = static_cast<float>(std::clamp<std::size_t>(listedDevices().size(), 1, kListRows));
    return std::max(16 + rows * kListRow, 7 * 13.0F) + 8;
}

void WheelSetupView::draw() {
    hits_.clear();
    hover_.clear();
    autoSave();
    drawHeader();
    const float bottomTop = contentHeight() - bottomPanelHeight();
    window_.fill(0, bottomTop - 6, window_.width(), 1, kPanel);
    drawDevices(kDevicesTop, bottomTop - 10);
    drawDeviceList(bottomTop);
    drawHints(std::max(kListW + 24, window_.width() / 2), bottomTop);
    if (popup_ == Popup::Control) drawControlPopup();
}

void WheelSetupView::drawHeader() {
    window_.text(4, 6, "RIG", kHeading);
    // Save and Revert end left of the version in the top-right corner (Version.hpp); the state right before them.
    constexpr float kRevertWidth = 64;
    constexpr float kSaveWidth = 56;
    const float kRevertX = window_.width() - kVersionReserve - kRevertWidth;
    const float kSaveX = kRevertX - 6 - kSaveWidth;
    // Saved as you change it: where Save was, whether it is; Undo instead of Revert.
    const bool unsaved = edit_ != saved_;
    const std::string state = unsaved ? "not saved" : !rigSaved_ ? "nothing named yet" : "saved";
    window_.text(kSaveX + kSaveWidth - static_cast<float>(state.size()) * ui::kCharWidth, 6, state,
                 unsaved ? kWarning : kDim);
    hover_.add(kSaveX + kSaveWidth - static_cast<float>(state.size()) * ui::kCharWidth, 2,
               static_cast<float>(state.size()) * ui::kCharWidth, 16,
               unsaved ? "This change could not be saved: the line at the bottom right says why. Undo drops it."
                       : "Every change is saved at once; the Mapping tab maps the rig's controls onto the Target. No "
                         "dead zones here: set them in the device's own software (Pit House for Moza).");
    button(kRevertX, 2, "Undo", [this] { undo(); }, false, unsaved || !undo_.empty(), kRevertWidth);
    hover_.add(kRevertX, 2, kRevertWidth, 16, "Goes back one change, as many times as you like.");

    // The second row: what the tab shows, and Detect (at the right).
    const std::vector<DeviceId> shown = shownDevices();
    const auto connected = static_cast<std::size_t>(
        std::count_if(shown.begin(), shown.end(), [this](const DeviceId& id) { return this->connected(id); }));
    window_.text(4, 26,
                 fromSession_ ? std::string("The running session's wheel (stop to see every device).")
                 : connected == 0
                     ? std::string("No game controller found: plug one in.")
                     : std::to_string(connected) +
                           " connected; touch a control (or click it) to name it. Scroll for more devices.",
                 connected == 0 && !fromSession_ ? kCaution : kDim);
    // Detect: a switch, drawn as a checkbox.
    style::checkbox(window_, 800 + extraW(), 22, 156, "Detect", detect_, true);
    hit(800 + extraW(), 22, 156, 16, [this] {
        detect_ = !detect_;
        for (Open& d : open_) d.restTaken = false;   // where everything rests now
    });
    hover_.add(800 + extraW(), 22, 156, 16,
               detect_ ? "On: touch a control on any device (a button, an axis, a pedal all the way) and its popup "
                         "opens; named ones only light up. Off: click a control to name it."
                       : "Off: click a control to name it. On: touching a control opens its popup.");
}

WheelSetupView::Listed WheelSetupView::listed(const DeviceId& id) const {
    const RigDevice* const device = edit_.find(id);
    // The axes the device has (all of them when that is not known: unplugged, or not the session's wheel), and any
    // named beyond those: a button box shows no axes.
    const int reported = axisCount(id);
    std::size_t axisRows = reported >= 0 ? static_cast<std::size_t>(std::min(reported, kAxes))
                                         : static_cast<std::size_t>(kAxes);
    if (device != nullptr) {
        for (std::size_t a = axisRows; a < static_cast<std::size_t>(kAxes); ++a) {
            if (device->axes[a].defined()) axisRows = a + 1;
        }
    }
    std::vector<bool> axes(axisRows, false);
    std::vector<bool> buttons(static_cast<std::size_t>(std::max(0, tileCount(id))), false);
    bool any = false;
    if (device != nullptr) {
        for (std::size_t a = 0; a < axes.size(); ++a) any = (axes[a] = device->axes[a].defined()) || any;
        for (std::size_t b = 0; b < buttons.size(); ++b) any = (buttons[b] = device->buttons[b].defined()) || any;
    }
    Listed list;
    list.folds = any;
    list.axes = foldUnnamed(axes, any && !showAll_);
    list.buttons = foldUnnamed(buttons, any && !showAll_);
    return list;
}

float WheelSetupView::sectionHeight(const DeviceId& id) const {
    const Listed list = listed(id);
    const auto tiles = static_cast<int>(list.buttons.shown.size());
    const int columns = tileColumns();
    const float buttonRows = static_cast<float>((tiles + columns - 1) / columns);
    const int lines = (list.axes.hidden.empty() ? 0 : 1) + (list.buttons.hidden.empty() ? 0 : 1) +
                      (list.folds && showAll_ ? 1 : 0);
    const auto foldLines = static_cast<float>(lines);
    return kSectionHead + static_cast<float>(list.axes.shown.size()) * kAxisRow + kButtonsHead +
           (tiles == 0 ? 12.0F : buttonRows * kTileStepY) + foldLines * kFoldRow + kSectionGap;
}

void WheelSetupView::drawFoldLine(const float y, const std::string& text, const bool showAll) {
    const float w = window_.width() - kBarW - 8;
    window_.outline(2, y + 1, w, kFoldRow - 4, kPanel);
    window_.text(8, y + 7, fit(text, static_cast<std::size_t>((w - 120) / ui::kCharWidth)), kDim);
    button(w - 100, y + 3, showAll ? "Show all" : "Named only", [this, showAll] { showAll_ = showAll; }, false, true,
           96);
    hover_.add(w - 100, y + 3, 96, 16,
               showAll ? "Lists every axis and button of every device, named or not."
                       : "Lists only the named axes and buttons of a device that has any.");
}

void WheelSetupView::drawDevices(const float top, const float bottom) {
    const std::vector<DeviceId> devices = shownDevices();
    bar_.shown = false;
    if (devices.empty()) {
        window_.text(4, top + 4, "Plug in your wheel, pedals, shifter or button box: they show up here at once.",
                     kText);
        scroll_ = 0;
        scrollMost_ = 0;
        return;
    }
    // Where each section starts (unscrolled), and what the scrolling can reach.
    std::vector<float> starts;
    float total = 0;
    for (const DeviceId& id : devices) {
        starts.push_back(total);
        total += sectionHeight(id);
    }
    const float room = std::max(1.0F, bottom - top);
    scrollRoom_ = room;
    scrollMost_ = std::max(0.0F, total - room);
    if (reveal_) {
        // Detect found a control: its device's section into view.
        const auto at = std::find(devices.begin(), devices.end(), *reveal_);
        if (at != devices.end()) {
            const float start = starts[static_cast<std::size_t>(at - devices.begin())];
            const float height = sectionHeight(*at);
            if (start < scroll_ || start + height > scroll_ + room) scroll_ = start;
        }
        reveal_.reset();
    }
    scroll_ = std::clamp(scroll_, 0.0F, scrollMost_);
    for (std::size_t i = 0; i < devices.size(); ++i) {
        const float y = top + starts[i] - scroll_;
        if (y > bottom || y + sectionHeight(devices[i]) < top) continue;   // not in view
        drawSection(devices[i], y, top, bottom);
    }
    if (scrollMost_ > 0) {
        // Where the view is in all the devices: dragged or clicked (onClick, onPointer), the mouse wheel, PageUp /
        // PageDown, Home / End.
        bar_ = {window_.width() - kBarW - 2, top, kBarW, room, 0, 0, true};
        bar_.thumbSize = std::max(16.0F, room * room / total);
        bar_.thumbStart = (room - bar_.thumbSize) * (scroll_ / scrollMost_);
        window_.fill(bar_.x, bar_.y, bar_.w, bar_.h, kStripe);
        window_.fill(bar_.x, bar_.y + bar_.thumbStart, bar_.w, bar_.thumbSize, dragging_ ? kText : kHeading);
    }
}

void WheelSetupView::drawSection(const DeviceId& id, const float y0, const float top, const float bottom) {
    const auto visible = [top, bottom](const float y, const float h) { return y >= top - 2 && y + h <= bottom + 2; };
    RigDevice* const device = edit_.find(id);
    const bool live = liveRaw(id) != nullptr;
    float y = y0;
    // Its name: the rig's (typed here), or the system's until one of its controls is named.
    if (visible(y, 18)) {
        window_.fill(0, y, window_.width() - kBarW - 4, 18, kPanel);
        if (device != nullptr) {
            drawNameField(4, y + 1, device->name, kMaxDeviceName);
        } else {
            window_.text(10, y + 5, fit(systemName(id), kMaxDeviceName), kText);
        }
        // Its id and whether it is connected: nothing else.
        const float x = 4 + static_cast<float>(kMaxDeviceName) * ui::kCharWidth + 28;
        const bool online = connected(id);
        window_.text(x, y + 5, idText(id), kDim);
        window_.text(x + 12 * ui::kCharWidth, y + 5, statusText(id), online ? kActive : kCaution);
    }
    y += kSectionHead;

    // The axes: -1..1 from the centre, as the system reports them; click one to say what it is.
    const int axes = axisCount(id);
    const float barX = kAxisBarX + extraW() / 3;
    const float barW = kAxisBarW + extraW() * 2 / 3;
    const auto textChars = static_cast<std::size_t>((barX - 52) / ui::kCharWidth);
    const Listed list = listed(id);
    for (std::size_t row = 0; row < list.axes.shown.size(); ++row, y += kAxisRow) {
        const int axis = list.axes.shown[row];
        if (!visible(y - 3, kAxisRow)) continue;
        if (row % 2 == 0) window_.fill(0, y - 3, window_.width() - kBarW - 4, kAxisRow - 1, kStripe);
        const RigControl control = device != nullptr ? device->axes[static_cast<std::size_t>(axis)] : RigControl{};
        const std::string text = describe(control);
        window_.text(4, y, "Axis " + std::to_string(axis), kDim);
        window_.text(60, y, text.empty() ? "(not named)" : fit(text, textChars - 2), text.empty() ? kDim : kText);
        hit(0, y - 3, window_.width() - kBarW - 4, kAxisRow - 1,
            [this, id, axis] { openControl(id, true, axis, std::nullopt); });
        if (live && axes >= 0 && axis >= axes) {
            window_.text(barX, y, "(this device has no axis " + std::to_string(axis) + ")", kDim);
            continue;
        }
        const float v = std::clamp(axisValue(id, axis), -1.0F, 1.0F);
        const float half = barW / 2;
        window_.fill(barX, y - 1, barW, 10, kPanel);
        window_.fill(v >= 0 ? barX + half : barX + half + half * v, y - 1, half * std::abs(v), 10,
                     control.defined() ? kActive : kCentre);
        window_.fill(barX + half, y - 2, 1, 12, kCentre);
        char value[16];
        std::snprintf(value, sizeof(value), "%+.3f", static_cast<double>(v));
        window_.text(barX + barW + 12, y, live ? value : "-", kDim);   // offline: no value
    }
    if (!list.axes.hidden.empty()) {
        const std::size_t n = list.axes.hidden.size();
        if (visible(y - 3, kFoldRow)) {
            drawFoldLine(y - 3, std::to_string(n) + (n == 1 ? " axis" : " axes") + " not named (" +
                                    indexRanges(list.axes.hidden) + ")", true);
        }
        y += kFoldRow;
    }

    // The buttons it has, as tiles (as many per row as fit); click one to say what it is. Not read now: the named
    // ones, as saved.
    const auto tiles = static_cast<int>(list.buttons.shown.size());
    if (visible(y, 12)) {
        style::headingBand(window_, 0, y + 2, window_.width() - kBarW - 4);
        window_.text(8, y + 2, "BUTTONS", kHeading);
        window_.text(76, y + 2,
                     tiles == 0 ? (live && list.buttons.hidden.empty() ? "none on this device" : "none named")
                     : live     ? "press one (it lights up); click one to say what it is"
                                : "click one to say what it is",
                     kDim);
    }
    y += kButtonsHead;
    const int columns = tileColumns();
    const float stepX = (window_.width() - 8 - kBarW - 4) / static_cast<float>(columns);
    const float tileW = stepX - 4;
    for (int tile = 0; tile < tiles; ++tile) {
        const int source = list.buttons.shown[static_cast<std::size_t>(tile)];
        const float x = 4 + static_cast<float>(tile % columns) * stepX;
        const float ty = y + static_cast<float>(tile / columns) * kTileStepY;
        if (!visible(ty, kTileH)) continue;
        const bool down = buttonDown(id, source);
        const RigControl control = device != nullptr ? device->buttons[static_cast<std::size_t>(source)] : RigControl{};
        window_.fill(x, ty, tileW, kTileH, down ? kActive : control.defined() ? kPanel : kStripe);
        if (source == lastPressed_ && id == lastPressedDevice_) window_.outline(x, ty, tileW, kTileH, kHeading);
        char number[16];
        std::snprintf(number, sizeof(number), "%2d", source);
        window_.text(x + 4, ty + 5, number, down ? kDark : kDim);
        const auto chars = static_cast<std::size_t>((tileW - 26) / ui::kCharWidth);
        window_.text(x + 26, ty + 5, fit(control.displayName(), chars),
                     down ? kDark : kText);
        hit(x, ty, tileW, kTileH, [this, id, source] { openControl(id, false, source, std::nullopt); });
    }
    y += tiles == 0 ? 12.0F : static_cast<float>((tiles + columns - 1) / columns) * kTileStepY;
    if (!list.buttons.hidden.empty()) {
        const std::size_t n = list.buttons.hidden.size();
        if (visible(y, kFoldRow)) {
            drawFoldLine(y, std::to_string(n) + (n == 1 ? " button" : " buttons") + " not named (" +
                                indexRanges(list.buttons.hidden) + ")" +
                                (detect_ ? ": press one to name it" : ": turn Detect on and press one to name it"),
                         true);
        }
        y += kFoldRow;
    }
    if (list.folds && showAll_ && visible(y, kFoldRow)) {
        drawFoldLine(y, "Every axis and button, named or not", false);
    }
}

void WheelSetupView::drawDeviceList(const float top) {
    style::headingBand(window_, 0, top, kListW);
    window_.text(8, top, "DEVICES", kHeading);
    window_.text(16 + 35 * ui::kCharWidth, top, "Status", kDim);
    float y = top + 16;
    const std::vector<DeviceId> listed = listedDevices();
    if (listed.empty()) window_.text(4, y + 4, "none: plug in a game controller", kCaution);
    // More than kListRows: scrolled with the mouse wheel over it, and it says how many are above and below.
    listScroll_ = std::min(listScroll_, listed.size() > kListRows ? listed.size() - kListRows : 0);
    listTop_ = y;
    listBottom_ = y + static_cast<float>(std::min(listed.size(), kListRows)) * kListRow;
    if (listed.size() > kListRows) {
        const std::size_t below = listed.size() - kListRows - listScroll_;
        window_.text(76, top,
                     std::to_string(listScroll_) + " up, " + std::to_string(below) + " down", kCaution);
    }
    const std::vector<DeviceId> shown = shownDevices();
    for (std::size_t row = listScroll_; row < listed.size() && row < listScroll_ + kListRows; ++row) {
        const DeviceId& id = listed[row];
        const RigDevice* const device = edit_.find(id);
        const bool connected = this->connected(id);
        window_.fill(4, y + 5, 6, 6, connected ? kActive : kDim);   // green: connected
        const std::string name = device != nullptr ? device->name : systemName(id);
        window_.text(16, y + 4, fit(name, 24), connected ? kText : kDim);
        window_.text(16 + 25 * ui::kCharWidth, y + 4, idText(id), kDim);
        window_.text(16 + 35 * ui::kCharWidth, y + 4, statusText(id), connected ? kActive : kCaution);
        // Clicking its line brings its section into view.
        if (std::find(shown.begin(), shown.end(), id) != shown.end()) {
            hit(4, y, kListW - 72, kListRow, [this, id] { reveal_ = id; });
        }
        if (device != nullptr && !fromSession_) {
            button(kListW - 64, y, "Forget", [this, id] { forget(id); }, false, true, 64);
            hover_.add(kListW - 64, y, 64, 16,
                       "Takes the device out of the rig, with its names (Undo brings it back).");
        }
        y += kListRow;
    }
}

void WheelSetupView::drawHints(const float x, const float top) {
    const auto chars = static_cast<std::size_t>(std::max(20.0F, (window_.width() - x - 8) / ui::kCharWidth));
    const float bottom = contentHeight() - 4;
    float y = top;
    const auto say = [&](const std::string& text, const ui::Colour colour, const std::size_t maxLines) {
        for (const std::string& line : wrapped(text, chars, maxLines)) {
            if (y + 10 > bottom) return;
            window_.text(x, y, line, colour);
            y += 12;
        }
        y += 2;
    };
    // One line of what to do; the rest is hover text (Detect, Save, Show all, Forget).
    say(detect_ ? "Touch a control to name it, or click it." : "Click a control to name it.", kDim, 1);
    if (lastPressed_ >= 0) {
        const RigDevice* const device = edit_.find(lastPressedDevice_);
        const std::string label =
            device != nullptr ? describe(device->buttons[static_cast<std::size_t>(lastPressed_)]) : std::string();
        say("Last pressed: " + (device != nullptr ? device->name : systemName(lastPressedDevice_)) + " button " +
                std::to_string(lastPressed_) + (label.empty() ? "" : "  " + label),
            kHeading, 1);
    }
    if (!status_.empty()) say(status_, statusError_ ? kWarning : kActive, 2);
}

void WheelSetupView::drawHoverText() const {
    if (popup_ != Popup::None) return;   // the popup covers the tab
    hover_.draw(window_, window_.width(), contentHeight());
}

void WheelSetupView::drawControlPopup() {
    constexpr float kW = 760;
    constexpr float kRoleW = 100;      // a name button, and the gap after it
    constexpr float kRoleGap = 4;
    constexpr float kRoleRow = 20;
    // The name buttons wrap at the popup's right edge: the popup is as tall as their rows (there are more button names
    // than fit on two rows), so OK / Cancel / Clear always sit below them.
    const float kX = std::floor((window_.width() - kW) / 2);   // centred
    const float rolesLeft = kX + 76;
    const float rolesRight = kX + kW - 12;
    const std::size_t roleCount = popupAxis_ ? kAxisNames.size() : kButtonNames.size();
    const auto perRow = static_cast<std::size_t>(std::max(1.0F, std::floor((rolesRight - rolesLeft + kRoleGap) /
                                                                             (kRoleW + kRoleGap))));
    const auto roleRows = static_cast<float>((roleCount + perRow - 1) / perRow);
    const float height = 58 + roleRows * kRoleRow + 6 + (popupAxis_ ? 24.0F : 0.0F) + 30;
    // Below the header when there is room; never past the tab bar.
    const float kY = std::floor(std::max(40.0F, std::min(130 + (window_.height() - kHeight) / 2,
                                                         contentHeight() - height - 4)));
    hit(0, 0, window_.width(), contentHeight(), [] {});   // clicks outside the popup do nothing
    window_.fill(kX, kY, kW, height, style::kBox);
    window_.outline(kX, kY, kW, height, kHeading);
    const float left = kX + 16;
    const RigDevice* const device = focusDevice();
    const std::string deviceName = device != nullptr ? device->name : systemName(focus_);
    window_.text(left, kY + 12,
                 (popupAxis_ ? "Axis " : "Button ") + std::to_string(popupIndex_) + " on " + fit(deviceName, 40), kHeading);
    if (popupAxis_) {
        drawPopupAxisBar(left + 480, kY + 11, 240);
    } else if (buttonDown(focus_, popupIndex_)) {
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
    // A choice picks what the control is (lit while picked, whatever the name becomes; again: unpicked) and fills in
    // the name while there is none (a name is never replaced), except Button, whose name the user types.
    const auto nameButton = [&](const std::string& name, const float width) {
        if (x + width > rolesRight) {
            x = rolesLeft;
            y += kRoleRow;
        }
        x += button(x, y, name, [this, name] {
            if (popupControl_.kind == name) {   // again: unpicked (the name stays)
                popupControl_.kind.clear();
                return;
            }
            popupControl_.kind = name;
            // The name too while there is none; never Button's (that name is the user's to type).
            if (name != kButtonMark && trimmed(popupControl_.label).empty()) popupControl_.label = name;
        }, popupControl_.kind == name, true, width) + kRoleGap;
    };
    for (const std::string_view name : popupAxis_ ? std::span<const std::string_view>(kAxisNames)
                                                  : std::span<const std::string_view>(kButtonNames)) {
        nameButton(std::string(name), kRoleW);
    }

    y += kRoleRow + 6;
    if (popupAxis_) {
        // Which way round it reads is the Mapping tab's (its Invert); here only where it rested, for a pedal.
        std::string note = "Which way round: the Mapping tab's Invert for the axis this one drives.";
        if (popupRest_) {
            char rest[64];
            std::snprintf(rest, sizeof(rest), "It rested at %+.2f when it was found", static_cast<double>(*popupRest_));
            note = std::string(rest) + (*popupRest_ > 0.5F ? ": released at the top, so Invert it on the Mapping tab."
                                                           : ".");
        }
        window_.text(left, y + 4, note, kDim);
    }
    const float by = kY + height - 26;
    button(left, by, "OK", [this] { closeControl(true); }, true, true, 80);
    button(left + 88, by, "Cancel", [this] { closeControl(false); }, false, true, 80);
    // Clear empties the control and stays open: OK keeps it cleared (the control unnamed), Cancel keeps it as it was.
    button(left + 176, by, "Clear", [this] {
        popupControl_ = RigControl{};
        editing_ = &popupControl_.label;   // ready to type a new name
    }, false, true, 80);
    window_.text(left + 270, by + 4, "Enter keeps it, Esc drops it.", kDim);
}

// The live value: steering from the centre, a pedal's travel from its released end (where it rested when found).
void WheelSetupView::drawPopupAxisBar(const float x, const float y, const float width) {
    const float v = std::clamp(axisValue(focus_, popupIndex_), -1.0F, 1.0F);
    const float half = width / 2;
    const float centre = x + half;
    window_.fill(x, y, width, 10, kPanel);
    // A pedal or lever (found resting at an end): its travel; anything else from the centre.
    const bool pedal = popupRest_ && std::fabs(*popupRest_) > 0.5F;
    if (!pedal) {
        window_.fill(v >= 0 ? centre : centre + half * v, y, half * std::abs(v), 10, kActive);
        window_.fill(centre, y - 1, 1, 12, kCentre);
        return;
    }
    const bool inverted = *popupRest_ > 0.5F;   // released at the top
    const float travel = inverted ? (1.0F - v) / 2 : (v + 1.0F) / 2;
    window_.fill(inverted ? x + width * (1.0F - travel) : x, y, width * travel, 10, kActive);
}

}  // namespace wheelbri::service
