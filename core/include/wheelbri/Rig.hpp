#pragma once

#include "wheelbri/DeviceLabels.hpp"
#include "wheelbri/IniDocument.hpp"
#include "wheelbri/MapperConfig.hpp"
#include "wheelbri/WheelState.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The rig: every device the user has (wheel, pedals, shifter, button box) and what each of their controls is called.
// The Rig tab makes it (one file, <name>.ini in rigsFolder(), "My rig" at first); the Mapping tab maps its controls
// onto the emulated wheel by hand (the mapping is the config, another file). A control's line:
//
//   [device_1]
//   name = My wheel
//   id = 346e:0005
//   kind = wheel
//   axis_0 = Wheel
//   axis_2 = Gas | invert       ; invert: released at the top of the axis
//   button_13 = R Paddle
//   button_14 = Horn | button   ; marked a button ("It is": Button), named Horn
//   button_15 = | button        ; marked a button, no name yet (shown as "Button")
// A control has a name and nothing else: WheelBri does not know what a control is for (every wheel is different), so
// nothing is mapped from the names. The one mark is Button: it is kept whatever the name becomes (typed, deleted). Rigs
// written before 2026-10-13 may have a role after the name ("Gas | throttle") or "| deadzone 3% | full 90%" on an
// axis: read and ignored (a role-only control gets the role's name; a Steering axis makes its device the wheel; an
// older button role is the Button mark), and left out when the rig is saved again.
namespace wheelbri {

// One-click names the Rig tab's control popup offers ("It is"): pressing one fills in the name when the control has
// none (a name it has is never replaced); Button (kButtonMark) also marks the control a button.
inline constexpr std::array<std::string_view, 6> kAxisNames = {
    "Steering", "Throttle", "Brake", "Clutch", "Handbrake", "Axis"};
inline constexpr std::array<std::string_view, 27> kButtonNames = {
    "Button",   "Handbrake",   "A",          "B",          "X",        "Y",        "Menu",     "View",     "RSB",
    "LSB",      "Xbox",        "D Up",       "D Right",    "D Down",     "D Left",   "L Paddle", "R Paddle",
    "Shift up", "Shift down",  "Gear 1",     "Gear 2",     "Gear 3",   "Gear 4",   "Gear 5",   "Gear 6",   "Gear 7",
    "Reverse"};

inline constexpr std::string_view kButtonMark = "Button";

struct RigControl {
    std::string label;                  // what it says on the device (DeviceLabels::kMaxLabel characters at most)
    bool invert = false;                // an axis released (or centred) at +1 instead of -1
    bool button = false;                // a button marked "Button" ("It is"), named or not

    [[nodiscard]] bool defined() const { return !label.empty() || button; }
    // What the app shows for it: its name, or "Button" for a button with none yet.
    [[nodiscard]] std::string displayName() const {
        return label.empty() && button ? std::string(kButtonMark) : label;
    }
    friend bool operator==(const RigControl&, const RigControl&) = default;
};

struct RigDevice {
    std::string name;
    DeviceId id;
    DeviceKind kind = DeviceKind::Other;
    std::array<RigControl, RawInputState::kMaxAxes> axes{};
    std::array<RigControl, MapperConfig::kMaxSourceButtons> buttons{};

    friend bool operator==(const RigDevice&, const RigDevice&) = default;
};

struct Rig {
    static constexpr std::size_t kMaxDevices = kMaxInputDevices;   // the wheel and three accessories

    std::vector<RigDevice> devices;

    [[nodiscard]] const RigDevice* find(const DeviceId& id) const;
    [[nodiscard]] RigDevice* find(const DeviceId& id);
    // The wheel when the mapping does not name one ([emulator] source_device): the first device of kind wheel, else the
    // first device; null for an empty rig. It plays force feedback.
    [[nodiscard]] const RigDevice* wheel() const;
    friend bool operator==(const Rig&, const Rig&) = default;
};

struct RigResult {
    Rig rig;
    std::vector<std::string> errors;   // bad values, unknown keys and sections; what could be read is kept
};

[[nodiscard]] RigResult loadRig(const IniDocument& ini);
// The whole file (the app owns it): a header comment, then one [device_N] section per device.
[[nodiscard]] std::string writeRig(const Rig& rig, const std::string& name);

// The device as the older per-device names (DeviceLabels), and back (for importing those files).
[[nodiscard]] DeviceLabels labelsOf(const RigDevice& device);
[[nodiscard]] RigDevice rigDeviceFrom(const DeviceLabels& labels);

// The rigs in `folder`: their names (file names without .ini), sorted.
[[nodiscard]] std::vector<std::string> listRigs(const std::filesystem::path& folder);
// Reads <folder>/<name>.ini; nullopt if it cannot be read. `errors` gets what was wrong in it.
[[nodiscard]] std::optional<Rig> readRig(const std::filesystem::path& folder, const std::string& name,
                                         std::vector<std::string>& errors);
// The rig's device that mapping device `number` reads (0: the wheel, found by `sourceDeviceMatch` as
// [emulator] source_device takes it, empty = the rig's wheel; 1..3: mapper.accessories), or null.
[[nodiscard]] const RigDevice* rigDeviceFor(const MapperConfig& mapper, const Rig& rig,
                                            const std::string& sourceDeviceMatch, int number);
// Writes it (making the folder); false if it cannot be written.
bool saveRig(const std::filesystem::path& folder, const std::string& name, const Rig& rig);

}  // namespace wheelbri
