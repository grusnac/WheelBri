// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/AutoMap.hpp"

#include <memory>

namespace wheelbri {

namespace {

// Calls take(kind, axis, into) for every input the Rig maps, `into` being where its source goes.
template <typename Take>
void forEachAutoInput(AutoMapping& out, const IDeviceProfile& profile, const Take& take) {
    for (std::size_t i = 0; i < kAutoAxisKinds.size(); ++i) take(kAutoAxisKinds[i], true, out.axes[i]);
    for (std::size_t i = 0; i < kAutoHatKinds.size(); ++i) take(kAutoHatKinds[i], false, out.hat[i]);
    for (std::size_t c = 0; c < kCoreButtonCount; ++c) {
        const int target = profile.coreButton(static_cast<CoreButton>(c));
        if (target < 0 || static_cast<std::size_t>(target) >= out.buttons.size()) continue;
        take(kCoreButtonKinds[c], false, out.buttons[static_cast<std::size_t>(target)]);
    }
}

}  // namespace

bool isAutoKind(const std::string_view kind) {
    const auto in = [kind](const auto& kinds) {
        for (const std::string_view k : kinds) {
            if (k == kind) return true;
        }
        return false;
    };
    return !kind.empty() && (in(kAutoAxisKinds) || in(kAutoHatKinds) || in(kCoreButtonKinds));
}

AutoMapping autoMapping(const Rig& rig, const IDeviceProfile& profile, const DeviceId& wheel) {
    AutoMapping out;
    forEachAutoInput(out, profile, [&](const std::string_view kind, const bool axis, SourceRef& into) {
        int count = 0;
        for (const RigDevice& device : rig.devices) {
            if (device.id.empty()) continue;
            const auto take = [&](const auto& controls) {
                for (std::size_t i = 0; i < controls.size(); ++i) {
                    if (controls[i].kind != kind) continue;
                    if (++count == 1) into = {device.id == wheel ? DeviceId{} : device.id, static_cast<int>(i)};
                }
            };
            if (axis) {
                take(device.axes);
            } else {
                take(device.buttons);
            }
        }
        if (count > 1) {
            into = {};
            out.notices.push_back(std::to_string(count) + " controls in the Rig are " + std::string(kind) +
                                  ": none is used");
        }
    });
    return out;
}

DeviceId mappingWheel(const std::string& sourceDevice, const Rig& rig) {
    if (const std::optional<DeviceId> id = parseDeviceId(sourceDevice)) return *id;
    if (sourceDevice.empty()) {
        if (const RigDevice* const device = rig.wheel()) return device->id;
    }
    return {};
}

std::vector<std::string> applyRig(EmulatorConfig& config, const Rig& rig) {
    if (!config.fromRig) return {};
    const std::unique_ptr<IDeviceProfile> profile = makeProfile(config.targetProfile);
    if (profile == nullptr) return {};
    AutoMapping found = autoMapping(rig, *profile, mappingWheel(config.sourceDeviceMatch, rig));
    std::vector<std::string> notices = std::move(found.notices);
    MapperConfig& mapper = config.mapper;
    const auto place = [&](const bool isAuto, const SourceRef& source, int& index, int& device) {
        if (!isAuto) return;
        index = kUnmapped;
        device = 0;
        if (!source.mapped()) return;
        const std::optional<int> number = deviceNumber(mapper, source.device);
        if (!number) {
            notices.push_back(formatSource(source) + ": at most " + std::to_string(MapperConfig::kMaxAccessories) +
                              " accessories besides the wheel can be read at once");
            return;
        }
        index = source.index;
        device = *number;
    };
    const AutoInputs& autos = config.autoInputs;
    place(autos.axes[0], found.axes[0], mapper.steering.sourceAxis, mapper.steering.sourceDevice);
    place(autos.axes[1], found.axes[1], mapper.throttle.sourceAxis, mapper.throttle.sourceDevice);
    place(autos.axes[2], found.axes[2], mapper.brake.sourceAxis, mapper.brake.sourceDevice);
    place(autos.axes[3], found.axes[3], mapper.clutch.sourceAxis, mapper.clutch.sourceDevice);
    HatButtons& hat = mapper.hatButtons;
    place(autos.hat[0], found.hat[0], hat.up, hat.devices[0]);
    place(autos.hat[1], found.hat[1], hat.right, hat.devices[1]);
    place(autos.hat[2], found.hat[2], hat.down, hat.devices[2]);
    place(autos.hat[3], found.hat[3], hat.left, hat.devices[3]);
    for (std::size_t t = 0; t < mapper.buttonMap.size(); ++t) {
        place(autos.buttons[t], found.buttons[t], mapper.buttonMap[t], mapper.buttonDevice[t]);
    }
    return notices;
}

std::vector<std::string> followRig(MappingModel& model, const Rig& rig, const IDeviceProfile& profile) {
    if (!model.fromRig) return {};
    AutoMapping found = autoMapping(rig, profile, mappingWheel(model.sourceDevice, rig));
    const AutoInputs& autos = model.autoInputs;
    for (std::size_t i = 0; i < model.axes.size(); ++i) {
        if (autos.axes[i]) model.axes[i] = found.axes[i];
    }
    for (std::size_t i = 0; i < model.hat.size(); ++i) {
        if (autos.hat[i]) model.hat[i] = found.hat[i];
    }
    for (std::size_t t = 0; t < model.buttons.size(); ++t) {
        if (autos.buttons[t]) model.buttons[t] = found.buttons[t];
    }
    return std::move(found.notices);
}

}  // namespace wheelbri
