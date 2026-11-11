// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/LiveMapping.hpp"

#include <cmath>

namespace wheelbri {

LiveMappingChange liveMappingChange(const EmulatorConfig& running, const EmulatorConfig& next) {
    LiveMappingChange change;
    if (next.mapper != running.mapper) {
        if (next.mapper.accessories != running.mapper.accessories) {
            change.waiting.emplace_back("the accessories the mapping reads (the whole mapping)");
        } else {
            change.mapper = next.mapper;
            change.rotationChanged =
                std::fabs(next.mapper.steering.targetRotationDeg - running.mapper.steering.targetRotationDeg) >= 0.5F;
        }
    }
    if (next.buttonOrder != running.buttonOrder) change.buttonOrder = next.buttonOrder;
    if (next.combinedPedals != running.combinedPedals) change.combinedPedals = next.combinedPedals;
    if (next.antiDeadZonePercent != running.antiDeadZonePercent) change.antiDeadZonePercent = next.antiDeadZonePercent;

    const auto waits = [&change](const bool differs, const char* what) {
        if (differs) change.waiting.emplace_back(what);
    };
    waits(next.targetProfile != running.targetProfile, "the target");
    waits(next.sourceDeviceMatch != running.sourceDeviceMatch, "the wheel ([emulator] source_device)");
    waits(next.statePort != running.statePort || next.ffbPort != running.ffbPort, "the ports");
    waits(next.stateRateHz != running.stateRateHz, "[emulator] state_rate_hz");
    waits(next.trueForce != running.trueForce, "[emulator] trueforce");
    waits(next.gameFfb != running.gameFfb || next.ffbInvert != running.ffbInvert || next.ffb != running.ffb ||
              std::fabs(next.centringSpringPercent - running.centringSpringPercent) >= 0.5F,
          "[ffb]");
    return change;
}

void applyLiveMappingChange(EmulatorConfig& running, const LiveMappingChange& change) {
    if (change.mapper) running.mapper = *change.mapper;
    if (change.buttonOrder) running.buttonOrder = *change.buttonOrder;
    if (change.combinedPedals) running.combinedPedals = *change.combinedPedals;
    if (change.antiDeadZonePercent) running.antiDeadZonePercent = *change.antiDeadZonePercent;
}

}  // namespace wheelbri
