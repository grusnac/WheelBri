#pragma once

#include <optional>
#include <string>
#include <string_view>

// The steering rotation as the app edits it (Mapping tab) and the config reads it (EmulatorConfig): the allowed
// range, typed numbers, and how they are written. The mapper applies it (Mapper.cpp), before the state goes to the
// shim or the virtual device, so both ways to the game get the same.
//   steering: source_rotation_deg (what the wheel's own software is set to), target_rotation_deg (the game's full
//             lock: the emulated wheel reports full lock there).
// There are no dead zones in WheelBri (since 2026-10-13): every axis is used as the device reports it; a dead zone is
// set in the device's own software (Pit House for Moza).
namespace wheelbri::tuning {

inline constexpr float kMinRotationDeg = 90.0F;
inline constexpr float kMaxRotationDeg = 2520.0F;

// A typed number: "180", " 2.5 ", "5%", "180 deg"; nullopt for anything else (empty, "abc", "1e3", "-", "5%%").
// A leading '-' is accepted (the range checks below refuse it where it does not belong).
[[nodiscard]] std::optional<float> parseNumber(std::string_view text);

// Without trailing zeros: 180 -> "180", 0.025 -> "0.025", 2.5 -> "2.5"; at most `decimals` decimals.
[[nodiscard]] std::string formatNumber(float value, int decimals = 4);
// A fraction as a percentage: 0.02 -> "2%", 0.025 -> "2.5%".
[[nodiscard]] std::string formatPercent(float fraction);

// Why a value cannot be used, or empty. Fractions, not percentages.
[[nodiscard]] std::string rotationProblem(float degrees);

}  // namespace wheelbri::tuning
