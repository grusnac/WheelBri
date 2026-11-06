// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace wheelbri {

// NFS 2015's own wheel bindings, from its settings file (Documents\Need For Speed\settings\PROFILEOPTIONS_profile, a
// text file of "key value" lines). WheelBri gives NFS 2015 its pedals where the game reads a G920's (its game
// profile, config/games/nfs2015.ini). A player who worked around the game's G920 table before, by binding accelerate
// to the brake input in the game's wheel controls, would get it wrong twice: this check finds that, so the log and
// the window can say to reset the game's wheel controls. It only reads; WheelBri never writes the game's files.
//
// The format (2026-10-09, NFS 2015 on Steam): "GstInput.CustomControlsWheel 0|1" (1: the player's wheel bindings are
// used, else the game's defaults) and per action "GstKeyBinding.racevehicle.<Concept>.<slot>.<field> <n>"; the wheel's
// slot has type 3 (slot 2 in every profile seen; 0 is the pad, 1 the keyboard) and its button is the game's wheel input
// (IDWB_*, numbered as in NFS16.exe: 24 Throttle, 25 Brake, 26 Clutch). Accelerate is ConceptMoveFB, brake
// ConceptBrake.
struct Nfs2015WheelBindings {
    enum class Verdict {
        Unreadable,   // not such a file, or the wheel bindings are missing: nothing is said
        Defaults,     // the game's own wheel bindings (CustomControlsWheel 0): right with WheelBri
        AsExpected,   // the player's, with accelerate on Throttle and brake on Brake: right with WheelBri
        PedalsMoved,  // the player's, with accelerate or brake elsewhere: wrong with WheelBri's pedal profile
    };
    Verdict verdict = Verdict::Unreadable;
    std::optional<int> accelerate;   // the wheel input accelerate is bound to (-1: none), when custom
    std::optional<int> brake;
};

[[nodiscard]] Nfs2015WheelBindings checkNfs2015Settings(std::string_view profileText);

// "Throttle", "Brake", "Clutch", "Ltrigger"...: an NFS 2015 wheel input's name (IDWB_ without the prefix), or
// "input N".
[[nodiscard]] std::string nfs2015WheelInputName(int input);

}  // namespace wheelbri
