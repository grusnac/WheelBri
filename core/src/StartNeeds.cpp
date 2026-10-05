#include "wheelbri/StartNeeds.hpp"

#include "wheelbri/DeviceLabels.hpp"
#include "wheelbri/DeviceMatch.hpp"

#include <algorithm>

namespace wheelbri {

namespace {

std::string idText(const DeviceId& id) { return deviceId(id.vendorId, id.productId); }

}  // namespace

StartNeeds startNeeds(const EmulatorConfig& config, const Rig* const rig) {
    StartNeeds needs;
    NeededDevice wheel;
    if (!config.sourceDeviceMatch.empty()) {
        wheel.match = config.sourceDeviceMatch;
        wheel.label = config.sourceDeviceMatch;
        if (rig != nullptr) {   // the rig's name for it, when the rig has it
            for (const RigDevice& device : rig->devices) {
                if (!device.id.empty() &&
                    deviceMatches(device.name, device.id.vendorId, device.id.productId, config.sourceDeviceMatch)) {
                    if (!device.name.empty()) wheel.label = device.name;
                    wheel.id = device.id;
                    break;
                }
            }
        }
    } else {
        const RigDevice* device = rig != nullptr ? rig->wheel() : nullptr;
        if (device == nullptr && rig != nullptr && !rig->devices.empty()) device = &rig->devices.front();
        if (device == nullptr || device->id.empty()) {
            needs.problem = rig == nullptr ? "NO RIG: set it up in the Rig tab" : "NO WHEEL IN THE RIG (Rig tab)";
            return needs;
        }
        wheel.id = device->id;
        wheel.label = device->name.empty() ? idText(device->id) : device->name;
    }
    needs.devices.push_back(wheel);

    for (std::size_t i = 0; i < config.mapper.accessoryCount(); ++i) {
        const DeviceId& id = config.mapper.accessories[i];
        if (!wheel.id.empty() && id == wheel.id) continue;   // the wheel itself (a session reads it as the wheel)
        NeededDevice accessory;
        accessory.id = id;
        const RigDevice* device = rig != nullptr ? rig->find(id) : nullptr;
        accessory.label = device != nullptr && !device->name.empty() ? device->name : idText(id);
        needs.devices.push_back(accessory);
    }
    return needs;
}

std::vector<std::string> missingDevices(const StartNeeds& needs, const std::vector<PresentDevice>& present) {
    std::vector<std::string> missing;
    for (const NeededDevice& needed : needs.devices) {
        const bool there = std::any_of(present.begin(), present.end(), [&](const PresentDevice& p) {
            if (!needed.match.empty()) return deviceMatches(p.name, p.vendorId, p.productId, needed.match);
            return p.vendorId == needed.id.vendorId && p.productId == needed.id.productId;
        });
        if (!there) missing.push_back(needed.label);
    }
    return missing;
}

}  // namespace wheelbri
