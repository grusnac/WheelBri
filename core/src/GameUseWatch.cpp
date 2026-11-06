// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/GameUseWatch.hpp"

#include <algorithm>

namespace wheelbri {

std::optional<bool> GameUseWatch::update(const std::uint64_t nowMs, const bool pedal, const std::uint64_t gamePackets) {
    if (gamePackets > 0) {
        if (!warning_) return std::nullopt;
        warning_ = false;   // the game opened the wheel after all
        return false;
    }
    if (started_ && pedal && nowMs > lastMs_) pedalMs_ += std::min<std::uint64_t>(nowMs - lastMs_, 100);
    started_ = true;
    lastMs_ = nowMs;
    if (!warning_ && pedalMs_ >= kPedalMs) {
        warning_ = true;
        return true;
    }
    return std::nullopt;
}

}  // namespace wheelbri
