// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace wheelbri {

// Which of a device's controls the Rig tab lists (the rest fold into one line, "4 axes not named (1, 3, 4, 7)"):
// `named[i]` says whether control i has a name. With `fold` false every control is shown, except the unnamed ones
// from `foldFrom` on, which fold anyway (a device's buttons past 63: a Moza base reports 128, most never used).
struct ControlFold {
    std::vector<int> shown;    // in order
    std::vector<int> hidden;   // the unnamed ones, in order (empty without folding)
};
[[nodiscard]] ControlFold foldUnnamed(const std::vector<bool>& named, bool fold,
                                      std::size_t foldFrom = static_cast<std::size_t>(-1));

// Indexes as short text: runs of three or more as "8-11", the others one by one: "1, 3, 4, 7", "8-11, 14-17, 25".
[[nodiscard]] std::string indexRanges(const std::vector<int>& indexes);

}  // namespace wheelbri
