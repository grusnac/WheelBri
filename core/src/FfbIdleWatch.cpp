#include "wheelbri/FfbIdleWatch.hpp"

#include <algorithm>

namespace wheelbri {

std::optional<bool> FfbIdleWatch::update(const std::uint64_t nowMs, const bool pedal, const std::size_t lastingLoaded,
                                         const std::size_t lastingWanted) {
    const std::uint64_t step = started_ && nowMs > lastMs_ ? std::min<std::uint64_t>(nowMs - lastMs_, 100) : 0;
    started_ = true;
    lastMs_ = nowMs;
    if (lastingLoaded == 0 || lastingWanted > 0) {
        pedalMs_ = 0;   // nothing loaded (menus, another controller) or the game plays it: nothing forgotten
        if (!warning_) return std::nullopt;
        warning_ = false;
        return false;
    }
    if (pedal) pedalMs_ += step;
    if (!warning_ && pedalMs_ >= kPedalMs) {
        warning_ = true;
        return true;
    }
    return std::nullopt;
}

}  // namespace wheelbri
