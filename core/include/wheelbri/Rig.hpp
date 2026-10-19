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
//   axis_2 = Gas | throttle           ; "It is": Throttle, named Gas
//   button_13 = R Paddle | r paddle   ; "It is": R Paddle, named R Paddle
//   button_14 = Horn | button         ; "It is": Button, named Horn
//   button_15 = | a                   ; "It is": A, no name yet (shown as "A")
// A control has a name and what it is ("It is", one of the choices the Rig tab offers: kAxisNames, kButtonNames), each
// kept whatever the other becomes. WheelBri still maps nothing from either (every wheel is different): the Mapping tab
// is where the user says what drives what. A control named exactly like a choice, with none picked (a rig saved before
// 2026-10-15), reads as that choice. Rigs written before 2026-10-13 may have a role after the name ("Gas | throttle",
// read as the choice it names now) or "| deadzone 3% | full 90%" on an axis (ignored, left out when saved again); a
// Steering axis makes its device the wheel. Rigs written before 2026-10-19 may say "| invert" on an axis: ignored (the
// Mapping tab's Invert is the one that counts), left out when saved again.
namespace wheelbri {

// What a control is: the Rig tab's control popup offers these ("It is"). Pressing one picks it (again: unpicks it),
// kept while the name is typed or deleted; it also fills in the name when there is none, except Button (kButtonMark),
// whose name the user types.
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
    std::string kind;                   // what it is: one of kAxisNames / kButtonNames as written there, or empty

    [[nodiscard]] bool defined() const { return !label.empty() || !kind.empty(); }
    // What the app shows for it: its name, or what it is when it has no name yet.
    [[nodiscard]] std::string displayName() const { return label.empty() ? kind : label; }
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
