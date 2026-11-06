// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/SourceRef.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wheelbri {

// One of a target button's secondary sources ([buttons] target_N_or, MapperConfig::buttonSecondary; a game's
// [game_tuning] <game>.button_N_or, GameTuning.hpp): a button as a source is written (SourceRef.hpp: 5,
// 044f:b696/5), or an axis
//   axis 5 > 50%                       the wheel's axis 5, pressed past half its travel
//   30b7:1001/axis 3 inverted > 40%    that accessory's axis 3, resting at +1, pressed past 40%
// ("> P%" left out: 50%). A pedal or handbrake lever (one-sided) is what makes sense here; the Mapping tab offers
// every named axis and leaves the choice to the user. The line is a comma-separated list of them:
//   target_2_or = 20, 30b7:1001/4, 30b7:1001/axis 3 > 50%
struct ButtonRef {
    SourceRef source;
    bool axis = false;
    bool invert = false;
    float threshold = 0.5F;

    [[nodiscard]] bool mapped() const { return source.mapped(); }
    friend bool operator==(const ButtonRef&, const ButtonRef&) = default;
};
// Defined in MappingEdit.cpp, next to parseSource.
[[nodiscard]] std::optional<ButtonRef> parseButtonRef(std::string_view text);
[[nodiscard]] std::string formatButtonRef(const ButtonRef& ref);
// The list: "-1" or empty is none; nullopt when any item is not a source (or is -1 among others).
[[nodiscard]] std::optional<std::vector<ButtonRef>> parseButtonRefs(std::string_view text);
[[nodiscard]] std::string formatButtonRefs(const std::vector<ButtonRef>& refs);
// The whole travel: 0% presses the button as soon as the axis leaves where it rests, 100% only at its end.
inline constexpr float kMinButtonThreshold = 0.0F;
inline constexpr float kMaxButtonThreshold = 1.0F;

}  // namespace wheelbri
