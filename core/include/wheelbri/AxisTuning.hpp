#pragma once

#include <optional>
#include <string>
#include <string_view>

// The steering rotation and the axes' dead zones as the app edits them (Rig tab, Mapping tab) and the config reads
// them (EmulatorConfig): the allowed ranges, typed numbers, and how they are written. The mapper applies them
// (Mapper.cpp), before the state goes to the shim or the virtual device, so both ways to the game get the same.
//   steering: source_rotation_deg (what the wheel's own software is set to), target_rotation_deg (the game's full
//             lock: the emulated wheel reports full lock there), deadzone (around the centre, of half the range);
//   pedals:   deadzone_low (ignored at the released end), saturation_high (from there on: fully pressed).
namespace wheelbri::tuning {

inline constexpr float kMinRotationDeg = 90.0F;
inline constexpr float kMaxRotationDeg = 2520.0F;
inline constexpr float kMaxSteeringDeadzone = 0.5F;
inline constexpr float kMaxPedalDeadzone = 0.99F;
inline constexpr float kMinPedalFull = 0.01F;

// A typed number: "180", " 2.5 ", "5%", "180 deg"; nullopt for anything else (empty, "abc", "1e3", "-", "5%%").
// A leading '-' is accepted (the range checks below refuse it where it does not belong).
[[nodiscard]] std::optional<float> parseNumber(std::string_view text);

// Without trailing zeros: 180 -> "180", 0.025 -> "0.025", 2.5 -> "2.5"; at most `decimals` decimals.
[[nodiscard]] std::string formatNumber(float value, int decimals = 4);
// A fraction as a percentage: 0.02 -> "2%", 0.025 -> "2.5%".
[[nodiscard]] std::string formatPercent(float fraction);

// Why a value cannot be used, or empty. Fractions, not percentages.
[[nodiscard]] std::string rotationProblem(float degrees);
[[nodiscard]] std::string steeringDeadzoneProblem(float deadzone);
[[nodiscard]] std::string pedalProblem(float deadzone, float full);

}  // namespace wheelbri::tuning
