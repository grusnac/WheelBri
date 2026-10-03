#pragma once

#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/MapperConfig.hpp"
#include "wheelbri/Rig.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>

// The mapping by device ids, as the Mapping tab edits it, and how a source is written in the config:
//   axis = 2               the wheel's ([emulator] source_device) axis 2
//   axis = 044f:b696/1     axis 1 of the accessory with USB id 044f:b696 (pedals, a shifter, a button box)
//   target_4 = -1          nothing
// A session opens every accessory its sources name (MapperConfig::accessories, at most kMaxAccessories).
namespace wheelbri {

// A control on a device: `device` empty = the wheel. `index` is an axis or a button, kUnmapped for none.
struct SourceRef {
    DeviceId device;
    int index = kUnmapped;

    [[nodiscard]] bool mapped() const { return index >= 0; }
    friend bool operator==(const SourceRef&, const SourceRef&) = default;
};

// "2", "-1" or "044f:b696/2" (hex ids, 1 to 4 digits each); nullopt if it is none of those.
[[nodiscard]] std::optional<SourceRef> parseSource(std::string_view text);
// The other way: "-1" for nothing, "2" on the wheel, "044f:b696/2" on an accessory.
[[nodiscard]] std::string formatSource(const SourceRef& source);
// "vvvv:pppp" as source_device takes it, or nullopt if `text` is not exactly a USB id (e.g. "346e", "Moza").
[[nodiscard]] std::optional<DeviceId> parseDeviceId(std::string_view text);

// The device number of `device` in `mapper` (0 for the wheel, i.e. empty), adding it to the accessories when it is
// not there yet; nullopt when all kMaxAccessories slots are taken by others.
[[nodiscard]] std::optional<int> deviceNumber(MapperConfig& mapper, const DeviceId& device);
// A device number and index back to a SourceRef (an unknown device number reads as nothing).
[[nodiscard]] SourceRef sourceOf(const MapperConfig& mapper, int device, int index);

// What the Mapping tab edits: the wheel, where each axis, button and D-pad direction of the emulated wheel comes
// from, and whether an axis is inverted. Everything else in the config stays as it is in the file.
struct MappingModel {
    static constexpr std::size_t kSteering = 0, kThrottle = 1, kBrake = 2, kClutch = 3;

    std::string sourceDevice;                 // [emulator] source_device: the wheel (it plays force feedback)
    std::array<SourceRef, 4> axes;            // steering, throttle, brake, clutch
    std::array<bool, 4> invert{};             // the same order
    std::array<SourceRef, MapperConfig::kMaxTargetButtons> buttons;
    std::array<SourceRef, 4> hat;             // up, right, down, left

    friend bool operator==(const MappingModel&, const MappingModel&) = default;
};

[[nodiscard]] MappingModel mappingModel(const EmulatorConfig& config);

struct MappingWriteResult {
    std::string text;    // the config with the mapping written in (comments and other settings kept)
    std::string error;   // why it could not be written (then `text` is empty)
};

// Fills `model` from the rig's roles: the wheel (the device with the Steering axis) becomes source_device; Steering,
// Throttle, Brake and Clutch axes (with their invert) the emulated wheel's axes; D-pad roles its D-pad; Shift up /
// down its R / L paddle and gears its "Gear N" / "Gear R" buttons, where `profile` has them. Plain buttons are left
// alone (only the user knows which G920 button they should be). Returns how many sources were set.
int autoMap(MappingModel& model, const Rig& rig, const IDeviceProfile& profile);

// Writes `model` into the config text `base`. A source on the wheel's own id (when source_device is a USB id) is
// written as the wheel's; a button left at its default (target N <- the wheel's button N) loses its line.
[[nodiscard]] MappingWriteResult writeMapping(std::string_view base, const MappingModel& model);

}  // namespace wheelbri
