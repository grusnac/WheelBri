#pragma once

#include "wheelbri/IniDocument.hpp"
#include "wheelbri/MapperConfig.hpp"
#include "wheelbri/WheelState.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What a physical device is and what its controls are called: the wheel, the pedals, a shifter, a button box.
// The Wheel setup tab identifies each connected device and names its axes and buttons; nothing here decides what
// a control does in the game (that is the mapping, in the config). One file per device, <vid>-<pid>.ini in the
// devices folder:
//
//   [device]
//   name = My wheel
//   id = 346e:0005
//   kind = wheel                ; wheel | pedals | shifter | handbrake | button box | other
//   [axis_labels]
//   axis_0 = Wheel
//   [button_labels]
//   button_13 = R paddle
namespace wheelbri {

enum class DeviceKind { Wheel, Pedals, Shifter, Handbrake, ButtonBox, Other };

inline constexpr std::array<DeviceKind, 6> kDeviceKinds = {DeviceKind::Wheel,     DeviceKind::Pedals,
                                                          DeviceKind::Shifter,   DeviceKind::Handbrake,
                                                          DeviceKind::ButtonBox, DeviceKind::Other};

[[nodiscard]] const char* deviceKindName(DeviceKind kind);           // "wheel", "button box"
[[nodiscard]] std::optional<DeviceKind> parseDeviceKind(std::string_view text);

struct DeviceLabels {
    static constexpr std::size_t kMaxLabel = 12;   // characters, as [button_labels] allows

    std::string name;                 // what the user calls it (the system's name to begin with)
    std::uint16_t vendorId = 0;
    std::uint16_t productId = 0;
    DeviceKind kind = DeviceKind::Other;
    std::array<std::string, RawInputState::kMaxAxes> axes{};
    std::array<std::string, MapperConfig::kMaxSourceButtons> buttons{};

    friend bool operator==(const DeviceLabels&, const DeviceLabels&) = default;
};

// "346e-0005.ini" and "346e:0005".
[[nodiscard]] std::string deviceLabelsFileName(std::uint16_t vendorId, std::uint16_t productId);
[[nodiscard]] std::string deviceId(std::uint16_t vendorId, std::uint16_t productId);

struct DeviceLabelsResult {
    DeviceLabels labels;
    std::vector<std::string> errors;   // unknown keys, bad values, names too long (cut)
};

[[nodiscard]] DeviceLabelsResult loadDeviceLabels(const IniDocument& ini);
// Every device named in `folder` (its <vid>-<pid>.ini files that read without errors and have an id), sorted by
// name: what the Mapping tab offers as sources.
[[nodiscard]] std::vector<DeviceLabels> listDeviceLabels(const std::filesystem::path& folder);

// Puts the device's names into the mapping's labels (what the live view shows next to the numbers).
void applyDeviceLabels(const DeviceLabels& labels, MapperConfig& mapper);

}  // namespace wheelbri
