// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/FfbGroups.hpp"

#include "wheelbri/FfbController.hpp"

#include <algorithm>
#include <cmath>

namespace wheelbri {

FfbGroup ffbGroupOf(const FfbEffectType type) {
    switch (type) {
        case FfbEffectType::Constant: return FfbGroup::Constant;
        case FfbEffectType::Spring: return FfbGroup::Spring;
        case FfbEffectType::Damper: return FfbGroup::Damper;
        case FfbEffectType::Friction: return FfbGroup::Friction;
        case FfbEffectType::Inertia: return FfbGroup::Inertia;
        case FfbEffectType::Sine:
        case FfbEffectType::Square:
        case FfbEffectType::Triangle:
        case FfbEffectType::SawtoothUp:
        case FfbEffectType::SawtoothDown: return FfbGroup::Periodic;
    }
    return FfbGroup::Constant;
}

FfbGroupLevels ffbGroupLevels(const FfbController& controller) {
    FfbGroupLevels levels;
    for (std::size_t i = 0; i < controller.slotCount(); ++i) {
        const FfbController::SlotView slot = controller.slot(i);
        if (!slot.playing || !slot.sent) continue;
        const auto g = static_cast<std::size_t>(ffbGroupOf(slot.requested.type));
        levels.asked[g] = std::max(levels.asked[g], std::min(1.0F, std::fabs(ffbStrength(slot.requested))));
        levels.sent[g] = std::max(levels.sent[g], std::min(1.0F, std::fabs(ffbStrength(*slot.sent))));
    }
    return levels;
}

}  // namespace wheelbri
