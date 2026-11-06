// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/MapperConfig.hpp"

#include <optional>
#include <string>
#include <vector>

// The Mapping tab's values while a session runs: the app saves the mapping as it changes, and the session reads the
// file again and uses what it can at once (service runLoop), as it does the Games tab's per-game values. What a
// running session can take:
//   the mapping (MapperConfig): steering axis, invert and rotation; the pedals' axes, invert and curve; every target
//     button and its secondaries; the D-pad's buttons. Only while it reads the same accessories: the session opened
//     those at Start, so a mapping that reads another device waits, whole, for the next Start.
//   [buttons] order (Pipeline::setButtonOrder).
// Everything else waits for the next Start and is said once per change: the target, the wheel, the ports and state
// rate, TrueForce, and [ffb] (force feedback is set in the wheel's own app; the few [ffb] keys are read at Start).
// [logging], [telemetry] and [retention] are read at Start only and are not compared. The per-game sections
// ([game_tuning], [game_rotation]) have their own path (GameWatch::setValues) and are not compared here either.
// The rotation is still 1:1 (MapperConfig.hpp): a new one changes what the game is told (HID++ aperture, TrueForce,
// a game's own settings file at its next Save to file) and what the app asks the wheel's own software to be set to.
namespace wheelbri {

struct LiveMappingChange {
    std::optional<MapperConfig> mapper;           // the mapping to use from now on, when it changed and can be used
    std::optional<ButtonOrder> buttonOrder;       // the button order to use from now on, when it changed
    bool rotationChanged = false;                 // mapper's steering rotation differs from the running one
    std::vector<std::string> waiting;             // what changed but waits for the next Start, one line each

    [[nodiscard]] bool any() const { return mapper || buttonOrder || !waiting.empty(); }
};

// What of `next` (the file as read again, prepared as Start prepares it: the rig's inputs filled in, the wheel's
// labels applied) a session running with `running` uses now. Pure; `running` is what the session uses at the moment
// (Start's config with the earlier live changes in it).
[[nodiscard]] LiveMappingChange liveMappingChange(const EmulatorConfig& running, const EmulatorConfig& next);

// `running` with `change` taken in: what the session compares the next read with.
void applyLiveMappingChange(EmulatorConfig& running, const LiveMappingChange& change);

}  // namespace wheelbri
