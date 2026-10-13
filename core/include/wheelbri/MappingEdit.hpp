#pragma once

#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/GameRotation.hpp"
#include "wheelbri/MapperConfig.hpp"
#include "wheelbri/Rig.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
// One of a target button's secondary sources ([buttons] target_N_or, MapperConfig::buttonSecondary): a button as
// above, or an axis
//   axis 5 > 50%                       the wheel's axis 5, pressed past half its travel
//   30b7:1001/axis 3 inverted > 40%    that accessory's axis 3, resting at +1, pressed past 40%
// ("> P%" left out: 50%). Only throttle, brake, clutch and handbrake axes make sense (one-sided); the Mapping tab
// offers only those (axisDrivesButtons, Rig.hpp). The line is a comma-separated list of them:
//   target_2_or = 20, 30b7:1001/4, 30b7:1001/axis 3 > 50%
struct ButtonRef {
    SourceRef source;
    bool axis = false;
    bool invert = false;
    float threshold = 0.5F;

    [[nodiscard]] bool mapped() const { return source.mapped(); }
    friend bool operator==(const ButtonRef&, const ButtonRef&) = default;
};
[[nodiscard]] std::optional<ButtonRef> parseButtonRef(std::string_view text);
[[nodiscard]] std::string formatButtonRef(const ButtonRef& ref);
// The list: "-1" or empty is none; nullopt when any item is not a source (or is -1 among others).
[[nodiscard]] std::optional<std::vector<ButtonRef>> parseButtonRefs(std::string_view text);
[[nodiscard]] std::string formatButtonRefs(const std::vector<ButtonRef>& refs);
inline constexpr float kMinButtonThreshold = 0.05F;
inline constexpr float kMaxButtonThreshold = 0.95F;

// "vvvv:pppp" as source_device takes it, or nullopt if `text` is not exactly a USB id (e.g. "346e", "Racing Wheel").
[[nodiscard]] std::optional<DeviceId> parseDeviceId(std::string_view text);

// The device number of `device` in `mapper` (0 for the wheel, i.e. empty), adding it to the accessories when it is
// not there yet; nullopt when all kMaxAccessories slots are taken by others.
[[nodiscard]] std::optional<int> deviceNumber(MapperConfig& mapper, const DeviceId& device);
// A device number and index back to a SourceRef (an unknown device number reads as nothing).
[[nodiscard]] SourceRef sourceOf(const MapperConfig& mapper, int device, int index);

// What the Mapping tab edits: the wheel, where each axis, button and D-pad direction of the emulated wheel comes
// from, whether an axis is inverted, and the steering rotation (AxisTuning.hpp). Everything else in the config stays
// as it is in the file. There are no dead zones: they belong in the devices' own software (an older config's
// deadzone, deadzone_low and saturation_high lines are dropped by writeMapping).
struct MappingModel {
    static constexpr std::size_t kSteering = 0, kThrottle = 1, kBrake = 2, kClutch = 3;

    std::string sourceDevice;                 // [emulator] source_device: the wheel (it plays force feedback)
    std::array<SourceRef, 4> axes;            // steering, throttle, brake, clutch
    std::array<bool, 4> invert{};             // the same order
    std::array<SourceRef, MapperConfig::kMaxTargetButtons> buttons;
    // Each button's secondary sources (at most MapperConfig::kMaxSecondaries), in the order they were added.
    std::array<std::vector<ButtonRef>, MapperConfig::kMaxTargetButtons> secondaries;
    std::array<SourceRef, 4> hat;             // up, right, down, left
    // [steering] source_rotation_deg: what the wheel's own software is set to; target_rotation_deg: the game's full
    // lock (the emulated wheel reports full lock when the wheel has turned that far).
    float wheelRotationDeg = 900.0F;
    float gameRotationDeg = 900.0F;
    // [game_rotation]: the user's own rotation per game profile (GameRotation.hpp); a game left out uses its profile's.
    GameRotations gameRotations;
    // [game_settings]: the user's own values for games' own settings (GameSettings.hpp); a setting left out uses its
    // game profile's.
    GameSettingOverrides gameSettings;
    // [game_ffb_gain]: the user's own force feedback gain per game; a game left out uses its profile's.
    GameFfbGains gameFfbGains;

    friend bool operator==(const MappingModel&, const MappingModel&) = default;
};

[[nodiscard]] MappingModel mappingModel(const EmulatorConfig& config);

// Where a rig control is used in the mapping. Each control is used at most once: the role assignment of one emulated
// input (an axis, a D-pad direction, a button: the main source) or a secondary of one button. The Mapping tab offers
// a used control nowhere else (greyed, its use in a tooltip); Save refuses a mapping that uses one twice. Only the
// target's named buttons count (the others reach no game).
struct ControlRef {
    SourceRef source;   // device empty = the wheel
    bool axis = false;
    friend bool operator==(const ControlRef&, const ControlRef&) = default;
};
struct ControlUse {
    enum class Kind { Axis, Hat, Button, Secondary };
    Kind kind = Kind::Button;
    std::size_t index = 0;   // MappingModel::kSteering.., the D-pad direction (up, right, down, left), the button
    friend bool operator==(const ControlUse&, const ControlUse&) = default;
};
// Every use of `control`, in the order: axes, D-pad, buttons, then secondaries by button. A source on the wheel's own
// id (source_device a vid:pid) counts as the wheel's.
[[nodiscard]] std::vector<ControlUse> usesOf(const MappingModel& model, const IDeviceProfile& profile,
                                             const ControlRef& control);
// "Role: G920 A", "Role: G920 throttle", "Role: G920 D-pad Up", "Secondary of G920 X".
[[nodiscard]] std::string describeUse(const ControlUse& use, const IDeviceProfile& profile);
// One line per control used more than once ("button 4 of 30b7:1001: role: G920 A, secondary of G920 X").
[[nodiscard]] std::vector<std::string> controlsUsedTwice(const MappingModel& model, const IDeviceProfile& profile);

struct MappingWriteResult {
    std::string text;    // the config with the mapping written in (comments and other settings kept)
    std::string error;   // why it could not be written (then `text` is empty)
};

struct AutoMapResult {
    int set = 0;                      // sources placed from the rig's roles
    std::vector<std::string> notes;   // what was left out or changed besides, one line each (the app's log, status)
};

// Fills `model` from the rig's roles: the wheel (the device with the Steering axis, else the first of kind wheel)
// becomes source_device (what read another wheel keeps reading it, as an accessory); Steering, Throttle, Brake and
// Clutch axes (with their invert) the emulated wheel's axes; D-pad roles its D-pad; the G920's button roles (A, B, X,
// Y, Menu, View, RSB, LSB, Xbox, the paddles, Shift up / down, gears; g920ButtonFor, Rig.hpp) that button, where
// `profile` has it. Plain buttons are left alone (only the user knows which G920 button they should be), and so is
// everything no role names. The secondaries are the user's and stay as they are. Edge cases, each said in `notes`: a
// role given to two controls (the first in the rig's order keeps it); a role the target lacks (handbrake axis, Gear
// 7); a control that is a secondary (it stays one: its role's input is left empty); a control placed now that an
// older line also maps elsewhere (that line goes: a control is used once).
AutoMapResult autoMap(MappingModel& model, const Rig& rig, const IDeviceProfile& profile);

// Why the model's rotations cannot be saved (AxisTuning.hpp's ranges), or empty.
[[nodiscard]] std::string tuningProblem(const MappingModel& model);

// Writes `model` into the config text `base` (refused with tuningProblem's reason). A number is written only when it
// differs from what the file says (or, with no line, from the default), so an unchanged mapping keeps its text. A
// source on the wheel's own id (when source_device is a USB id) is written as the wheel's; a button left at its
// default (target N <- the wheel's button N) loses its line.
[[nodiscard]] MappingWriteResult writeMapping(std::string_view base, const MappingModel& model);

}  // namespace wheelbri
