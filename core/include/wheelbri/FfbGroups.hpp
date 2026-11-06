// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/Ffb.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// A running game's force feedback by kind of effect, as the app's Live tab shows it: per kind, the strongest the game
// asks and the strongest that reaches the wheel (after the safety limits). Nothing here is set by the user: how strong
// the forces are is the game's and the wheel's own app's to set.
namespace wheelbri {

class FfbController;

// The kinds of effect, in the window's order.
enum class FfbGroup : std::uint8_t { Constant, Periodic, Spring, Damper, Friction, Inertia };
inline constexpr std::size_t kFfbGroupCount = 6;
// The group an effect type belongs to (sine, square, triangle and the saws are periodic).
[[nodiscard]] FfbGroup ffbGroupOf(FfbEffectType type);

// What the window calls each kind (labels, at most 20 characters) and what it is, in a few words.
inline constexpr std::array<std::string_view, kFfbGroupCount> kFfbGroupNames = {
    "Steering force", "Vibration", "Centring spring", "Damper", "Friction", "Inertia"};
inline constexpr std::array<std::string_view, kFfbGroupCount> kFfbGroupAbout = {
    "constant force: the road, the tyres", "sine, square, ...: kerbs, engine, rumble", "spring: pulls to the centre",
    "damper: resists fast turning",        "friction: resists any turning",           "inertia: a heavier wheel"};

// Per kind, 0..1: the strongest playing effect the game asks for, and the strongest the wheel gets for it. A slot
// that is not playing, or has sent nothing yet, counts for neither.
struct FfbGroupLevels {
    std::array<float, kFfbGroupCount> asked{};
    std::array<float, kFfbGroupCount> sent{};
};
[[nodiscard]] FfbGroupLevels ffbGroupLevels(const FfbController& controller);

}  // namespace wheelbri
