// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/MapperConfig.hpp"
#include "wheelbri/MappingEdit.hpp"
#include "wheelbri/Rig.hpp"
#include "wheelbri/SourceRef.hpp"

#include <array>
#include <string>
#include <vector>

// The inputs every target has, mapped from the Rig by what each control "is" (the Rig tab's "It is"): the steering,
// the throttle, brake and clutch, the D-pad and the core buttons (CoreButton: A, B, X, Y and the paddles; on the G25
// and G27 A, B, X, Y go on the black diamond). Nothing else maps itself: every other button is the user's to map on
// the Mapping tab. An input follows the Rig only in a mapping with [emulator] from_rig = true and no line of its own
// (EmulatorConfig::autoInputs); a line, -1 included, always wins. Which way a pedal reads (Invert) is never taken from
// the Rig: the Mapping tab sets it.
namespace wheelbri {

// What the Rig gives each input, for one target.
struct AutoMapping {
    std::array<SourceRef, 4> axes;   // steering, throttle, brake, clutch (MappingModel::kSteering..)
    std::array<SourceRef, 4> hat;    // up, right, down, left
    std::array<SourceRef, MapperConfig::kMaxTargetButtons> buttons;   // the target's core buttons only
    // What the Rig could not give, for the Mapping tab: "2 controls in the Rig are A: none is used".
    std::vector<std::string> notices;
};

// What each "It is" maps to: the axis kinds (kAxisNames) for steering .. clutch, the button kinds for the D-pad.
inline constexpr std::array<std::string_view, 4> kAutoAxisKinds = {"Steering", "Throttle", "Brake", "Clutch"};
inline constexpr std::array<std::string_view, 4> kAutoHatKinds = {"D Up", "D Right", "D Down", "D Left"};

// Whether `kind` is one the Rig maps by itself (an axis kind, a D-pad one, a core button's): only one control of a rig
// may be it (on two, neither is used), so the Rig tab moves it rather than giving it to a second control.
[[nodiscard]] bool isAutoKind(std::string_view kind);

// A kind on exactly one control of the rig maps; on two or more, none of them does (a notice says so). A source on
// `wheel` (the device the mapping calls the wheel) has an empty device; one on a device without an id is not used.
[[nodiscard]] AutoMapping autoMapping(const Rig& rig, const IDeviceProfile& profile, const DeviceId& wheel);

// The device a mapping calls the wheel: its source_device when that is a vid:pid, else (empty) the rig's wheel
// (Rig::wheel). Empty when neither (a source_device that is part of a name).
[[nodiscard]] DeviceId mappingWheel(const std::string& sourceDevice, const Rig& rig);

// Fills `config`'s auto inputs (fromRig only) from `rig` for its target; returns the notices (autoMapping's, and an
// input left out because a fourth accessory cannot be read). Called before a session starts and before Start's
// checks (StartNeeds).
std::vector<std::string> applyRig(EmulatorConfig& config, const Rig& rig);

// The same for the Mapping tab's model: its auto inputs (MappingModel::autoInputs) get the Rig's sources.
std::vector<std::string> followRig(MappingModel& model, const Rig& rig, const IDeviceProfile& profile);

}  // namespace wheelbri
