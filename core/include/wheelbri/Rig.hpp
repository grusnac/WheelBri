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
// and is for. The Wheel setup tab makes it (one file per rig, <name>.ini in rigsFolder()); the Mapping tab maps its
// controls onto the emulated wheel (the mapping is the config, another file). A control's line:
//
//   [device_1]
//   name = R3 Racing Wheel
//   id = 346e:0005
//   kind = wheel
//   axis_0 = Wheel | steering
//   axis_2 = Gas | throttle | invert       ; invert: released at the top of the axis
//   button_13 = R paddle | right paddle
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
};

inline constexpr std::array<ControlRole, 6> kAxisRoles = {ControlRole::Steering, ControlRole::Throttle,
                                                          ControlRole::Brake,    ControlRole::Clutch,
                                                          ControlRole::Handbrake, ControlRole::OtherAxis};
// In the order the Wheel setup tab offers them.
inline constexpr std::array<ControlRole, 17> kButtonRoles = {
    ControlRole::Button,     ControlRole::DpadUp,      ControlRole::DpadRight, ControlRole::DpadDown, ControlRole::DpadLeft,
    ControlRole::LeftPaddle, ControlRole::RightPaddle, ControlRole::ShiftUp,   ControlRole::ShiftDown,
    ControlRole::Gear1,      ControlRole::Gear2,       ControlRole::Gear3,
    ControlRole::Gear4,   ControlRole::Gear5,   ControlRole::Gear6,     ControlRole::Gear7,    ControlRole::GearReverse};

[[nodiscard]] const char* roleName(ControlRole role);   // "throttle", "d-pad up", "gear 1", "" for None
[[nodiscard]] std::optional<ControlRole> parseRole(std::string_view text);
[[nodiscard]] bool isAxisRole(ControlRole role);

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
// Writes it (making the folder); false if it cannot be written.
bool saveRig(const std::filesystem::path& folder, const std::string& name, const Rig& rig);
// Renames <old>.ini to <name>.ini; the error, or empty when done.
[[nodiscard]] std::string renameRig(const std::filesystem::path& folder, const std::string& oldName,
                                    const std::string& name);
// Why `name` cannot be a new rig's name (as configNameProblem), or empty.
[[nodiscard]] std::string rigNameProblem(const std::string& name, const std::vector<std::string>& existing);

}  // namespace wheelbri
