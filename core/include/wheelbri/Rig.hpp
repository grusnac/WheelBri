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

// The rig: every device the user has (wheel, pedals, shifter, button box) and what each of their controls is called
// and is for. The Rig tab makes it (one file, <name>.ini in rigsFolder(), "My rig" at first); the Mapping tab maps its
// controls onto the emulated wheel (the mapping is the config, another file). A control's line:
//
//   [device_1]
//   name = My wheel
//   id = 346e:0005
//   kind = wheel
//   axis_0 = Wheel | steering
//   axis_2 = Gas | throttle | invert       ; invert: released at the top of the axis
//   axis_5 = Brake | brake
//   button_13 = R paddle | right paddle
// Rigs written before 2026-10-13 may have "| deadzone 3% | full 90%" on an axis: read and ignored (WheelBri has no
// dead zones: the device's own software sets them), and left out when the rig is saved again.
namespace wheelbri {

enum class ControlRole {
    None,
    // Axes.
    Steering, Throttle, Brake, Clutch, Handbrake, OtherAxis,
    // Buttons.
    Button, DpadUp, DpadRight, DpadDown, DpadLeft, ShiftUp, ShiftDown,
    Gear1, Gear2, Gear3, Gear4, Gear5, Gear6, Gear7, GearReverse,
    // Added later, so they come last (the values above stay as they were; files store the names anyway).
    LeftPaddle, RightPaddle,
    // The G920's own buttons by name (its profile's names: A, B, X, Y, Menu, View, RSB, LSB, Xbox): Auto-map puts each
    // on that G920 button.
    ButtonA, ButtonB, ButtonX, ButtonY, Menu, View, Rsb, Lsb, Xbox,
};

inline constexpr std::array<ControlRole, 6> kAxisRoles = {ControlRole::Steering, ControlRole::Throttle,
                                                          ControlRole::Brake,    ControlRole::Clutch,
                                                          ControlRole::Handbrake, ControlRole::OtherAxis};
// In the order the Wheel setup tab offers them.
// Handbrake is both: a lever that reports an axis, or one switched to report a button (Moza's HBP can do either).
inline constexpr std::array<ControlRole, 27> kButtonRoles = {
    ControlRole::Button,      ControlRole::Handbrake,     ControlRole::ButtonA,     ControlRole::ButtonB,   ControlRole::ButtonX,  ControlRole::ButtonY,
    ControlRole::Menu,       ControlRole::View,        ControlRole::Rsb,       ControlRole::Lsb,      ControlRole::Xbox,
    ControlRole::DpadUp,     ControlRole::DpadRight,   ControlRole::DpadDown,  ControlRole::DpadLeft,
    ControlRole::LeftPaddle, ControlRole::RightPaddle, ControlRole::ShiftUp,   ControlRole::ShiftDown,
    ControlRole::Gear1,      ControlRole::Gear2,       ControlRole::Gear3,
    ControlRole::Gear4,   ControlRole::Gear5,   ControlRole::Gear6,     ControlRole::Gear7,    ControlRole::GearReverse};

[[nodiscard]] const char* roleName(ControlRole role);   // "throttle", "d-pad up", "gear 1", "" for None
// As the app shows it, and the name a control gets when the role is picked: "Throttle", "D-pad Up", "Gear 1", the
// G920's own spelling for its buttons ("A", "RSB", "Xbox"); "" for None. At most DeviceLabels::kMaxLabel characters.
[[nodiscard]] std::string roleLabel(ControlRole role);
// The G920 button a role stands for, as the G920 profile names it ("A", "R Paddle", "Gear 1"), or "" (not a G920
// button: Button, the D-pad, the axes).
[[nodiscard]] std::string g920ButtonFor(ControlRole role);
[[nodiscard]] std::optional<ControlRole> parseRole(std::string_view text);
[[nodiscard]] bool isAxisRole(ControlRole role);
// Whether a control with `role` may have it: an axis role on an axis, a button role on a button (Handbrake: both).
[[nodiscard]] bool roleFits(ControlRole role, bool axis);
// Throttle, brake, clutch, handbrake: one-sided axes (released at one end, pressed toward the other), the only ones
// that may press a target button past a threshold (MapperConfig::buttonSecondary). Steering goes both ways: never.
[[nodiscard]] bool axisDrivesButtons(ControlRole role);
[[nodiscard]] bool isPedal(ControlRole role);   // throttle, brake, clutch

struct RigControl {
    std::string label;                  // what it says on the device (DeviceLabels::kMaxLabel characters at most)
    ControlRole role = ControlRole::None;
    bool invert = false;                // an axis released (or centred) at +1 instead of -1

    [[nodiscard]] bool defined() const { return !label.empty() || role != ControlRole::None; }
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
    // The device with a Steering axis (else the first of kind wheel): the one that plays force feedback.
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
// Another control of the rig that Auto-map would put in the same place as `role` on control `index` of `device`
// (the same role, or another role for the same G920 button: Shift up and Right paddle), as "button 7 ('Y') of Moza
// R3"; empty when there is none, or the role may repeat (None, Button, Axis). The first in the rig's order wins.
[[nodiscard]] std::string sameRoleElsewhere(const Rig& rig, const DeviceId& device, bool axis, std::size_t index,
                                            ControlRole role);

// The rig's device that mapping device `number` reads (0: the wheel, found by `sourceDeviceMatch` as
// [emulator] source_device takes it, empty = the rig's wheel; 1..3: mapper.accessories), or null.
[[nodiscard]] const RigDevice* rigDeviceFor(const MapperConfig& mapper, const Rig& rig,
                                            const std::string& sourceDeviceMatch, int number);
// At Start: an axis pressing a target button (buttonSecondary) that the rig does not name as throttle, brake, clutch or
// handbrake, one line each for the log (the mapping is used as it is: nothing is taken away).
[[nodiscard]] std::vector<std::string> buttonAxisProblems(const MapperConfig& mapper, const Rig& rig,
                                                          const std::string& sourceDeviceMatch);

// Writes it (making the folder); false if it cannot be written.
bool saveRig(const std::filesystem::path& folder, const std::string& name, const Rig& rig);

}  // namespace wheelbri
