#include "wheelbri/StartNeeds.hpp"

#include "wheelbri/DeviceLabels.hpp"
#include "wheelbri/DeviceMatch.hpp"
#include "wheelbri/DeviceProfile.hpp"

#include <algorithm>

namespace wheelbri {

namespace {

std::string idText(const DeviceId& id) { return deviceId(id.vendorId, id.productId); }

}  // namespace

bool mappingUsesRig(const MapperConfig& mapper, const Rig& rig, const bool wheelInRig) {
    // A source on device 0 is the wheel's, on 1..3 that accessory's.
    const auto fromRig = [&](const int source, const int device) {
        if (source < 0) return false;
        if (device == 0) return wheelInRig;
        const auto slot = static_cast<std::size_t>(device - 1);
        return device > 0 && slot < mapper.accessories.size() && !mapper.accessories[slot].empty() &&
               rig.find(mapper.accessories[slot]) != nullptr;
    };
    if (fromRig(mapper.steering.sourceAxis, mapper.steering.sourceDevice) ||
        fromRig(mapper.throttle.sourceAxis, mapper.throttle.sourceDevice) ||
        fromRig(mapper.brake.sourceAxis, mapper.brake.sourceDevice) ||
        fromRig(mapper.clutch.sourceAxis, mapper.clutch.sourceDevice)) {
        return true;
    }
    for (std::size_t i = 0; i < mapper.buttonMap.size(); ++i) {
        if (fromRig(mapper.buttonMap[i], mapper.buttonDevice[i])) return true;
        for (const ButtonSource& alt : mapper.buttonSecondary[i]) {
            if (fromRig(alt.index, alt.device)) return true;
        }
    }
    const HatButtons& hat = mapper.hatButtons;
    return fromRig(hat.up, hat.devices[0]) || fromRig(hat.right, hat.devices[1]) || fromRig(hat.down, hat.devices[2]) ||
           fromRig(hat.left, hat.devices[3]);
}

std::string targetProblem(const EmulatorConfig& config, const std::string_view target) {
    const auto profile = makeProfile(config.targetProfile);
    if (profile == nullptr) {
        return "UNKNOWN TARGET '" + config.targetProfile + "' in the mapping: Default, or target_profile = " +
               std::string(target);
    }
    if (config.targetProfile != target) {
        return "THE MAPPING IS FOR THE " + profile->identity().shortName + ": Default, or target_profile = " +
               std::string(target) + " in it";
    }
    return {};
}

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
            needs.problem = rig == nullptr ? kNoRig : "NO WHEEL IN THE RIG (Rig tab)";
            return needs;
        }
        wheel.id = device->id;
        wheel.label = device->name.empty() ? idText(device->id) : device->name;
    }
    needs.devices.push_back(wheel);
    // A rig and a target, but nothing of the rig mapped onto the target.
    if (rig != nullptr && !mappingUsesRig(config.mapper, *rig, !wheel.id.empty() && rig->find(wheel.id) != nullptr)) {
        needs.problem = kNoMapping;
        needs.devices.clear();
        return needs;
    }

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
