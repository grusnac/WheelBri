// The value types compared with == (change detection: an unchanged mapping writes the same text, an effect sent
// again only when it changed, a rig saved only when edited): every field on its own makes two values different, and
// two values with every field alike are equal. A field left out of a comparison would hide a change.

#include "wheelbri/DeviceLabels.hpp"
#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/DrawList.hpp"
#include "wheelbri/Ffb.hpp"
#include "wheelbri/GameProfiles.hpp"
#include "wheelbri/GameRotation.hpp"
#include "wheelbri/GameSettings.hpp"
#include "wheelbri/GameTuning.hpp"
#include "wheelbri/HidWheelDevice.hpp"
#include "wheelbri/MapperConfig.hpp"
#include "wheelbri/MappingEdit.hpp"
#include "wheelbri/Rig.hpp"
#include "wheelbri/WheelOutput.hpp"
#include "wheelbri/WheelState.hpp"

#include <gtest/gtest.h>

namespace wheelbri {
namespace {

// `base` equals itself; each change, applied to a copy on its own, makes the copy differ from `base`.
template <class T, class... Changes>
void eachFieldCounts(const T& base, const Changes&... changes) {
    EXPECT_TRUE(base == base);
    int field = 0;
    const auto differs = [&](const auto& change) {
        T other = base;
        change(other);
        EXPECT_FALSE(other == base) << "field " << field;
        EXPECT_FALSE(base == other) << "field " << field;
        ++field;
    };
    (differs(changes), ...);
}

TEST(Equality, SourcesAndDevices) {
    eachFieldCounts(DeviceId{1, 2}, [](DeviceId& v) { v.vendorId = 9; }, [](DeviceId& v) { v.productId = 9; });
    eachFieldCounts(SourceRef{{1, 2}, 3}, [](SourceRef& v) { v.device.vendorId = 9; }, [](SourceRef& v) { v.index = 9; });
    eachFieldCounts(ButtonSource{}, [](ButtonSource& v) { v.device = 1; }, [](ButtonSource& v) { v.index = 1; },
                    [](ButtonSource& v) { v.axis = true; }, [](ButtonSource& v) { v.invert = true; },
                    [](ButtonSource& v) { v.threshold = 0.7F; });
    eachFieldCounts(ButtonRef{}, [](ButtonRef& v) { v.source.index = 1; }, [](ButtonRef& v) { v.axis = true; },
                    [](ButtonRef& v) { v.invert = true; }, [](ButtonRef& v) { v.threshold = 0.7F; });
    eachFieldCounts(ControlRef{}, [](ControlRef& v) { v.source.index = 1; }, [](ControlRef& v) { v.axis = true; });
    eachFieldCounts(ControlUse{}, [](ControlUse& v) { v.kind = ControlUse::Kind::Hat; },
                    [](ControlUse& v) { v.index = 1; });
}

TEST(Equality, MappingModel) {
    eachFieldCounts(
        MappingModel{}, [](MappingModel& v) { v.sourceDevice = "346e"; }, [](MappingModel& v) { v.axes[1].index = 4; },
        [](MappingModel& v) { v.invert[2] = true; }, [](MappingModel& v) { v.buttons[3].index = 7; },
        [](MappingModel& v) { v.secondaries[3].push_back(ButtonRef{}); }, [](MappingModel& v) { v.hat[0].index = 4; },
        [](MappingModel& v) { v.steeringRotationDeg = 540.0F; }, [](MappingModel& v) { v.centringSpringPercent = 10.0F; },
        [](MappingModel& v) { v.gameRotations["x"] = {}; }, [](MappingModel& v) { v.gameSettings["x"]["y"] = 1.0F; },
        [](MappingModel& v) { v.gameFfbGains["x"] = 1.0F; }, [](MappingModel& v) { v.gameTunings["x"] = {}; });
}

TEST(Equality, GameValues) {
    eachFieldCounts(
        GameTuning{}, [](GameTuning& v) { v.pedalAxes[0] = ObjectGuid::Slider; },
        [](GameTuning& v) { v.pedalInvert[1] = true; }, [](GameTuning& v) { v.ffbStrength = 0.5F; },
        [](GameTuning& v) { v.ffbGains[2] = 0.5F; }, [](GameTuning& v) { v.ffbCap = 0.5F; },
        [](GameTuning& v) { v.ffbSlew = 5.0F; }, [](GameTuning& v) { v.ffbFadeSeconds = 1.0F; },
        [](GameTuning& v) { v.ffbInvert = true; }, [](GameTuning& v) { v.ffbEffects = false; },
        [](GameTuning& v) { v.buttons[1] = {}; });
    eachFieldCounts(GameRotationOverride{}, [](GameRotationOverride& v) { v.off = true; },
                    [](GameRotationOverride& v) { v.deg = 180.0F; });
    eachFieldCounts(GameSettingText{"a", "1"}, [](GameSettingText& v) { v.name = "b"; },
                    [](GameSettingText& v) { v.text = "2"; });
    eachFieldCounts(PedalRoute{}, [](PedalRoute& v) { v.from[0] = 1; }, [](PedalRoute& v) { v.from[2] = 0; });
}

TEST(Equality, ForceFeedback) {
    eachFieldCounts(FfbCondition{}, [](FfbCondition& v) { v.center = 0.1F; }, [](FfbCondition& v) { v.deadband = 0.1F; },
                    [](FfbCondition& v) { v.positiveCoefficient = 0.1F; },
                    [](FfbCondition& v) { v.negativeCoefficient = 0.1F; },
                    [](FfbCondition& v) { v.positiveSaturation = 0.1F; },
                    [](FfbCondition& v) { v.negativeSaturation = 0.1F; });
    eachFieldCounts(FfbPeriodic{}, [](FfbPeriodic& v) { v.magnitude = 0.1F; }, [](FfbPeriodic& v) { v.offset = 0.1F; },
                    [](FfbPeriodic& v) { v.periodMs = 1.0F; }, [](FfbPeriodic& v) { v.phaseDeg = 1.0F; });
    eachFieldCounts(FfbEffect{}, [](FfbEffect& v) { v.type = FfbEffectType::Spring; },
                    [](FfbEffect& v) { v.constantLevel = 0.1F; }, [](FfbEffect& v) { v.condition.center = 0.1F; },
                    [](FfbEffect& v) { v.periodic.offset = 0.1F; }, [](FfbEffect& v) { v.gain = 0.5F; });
    eachFieldCounts(
        WheelEffect{}, [](WheelEffect& v) { v.type = FfbEffectType::Sine; }, [](WheelEffect& v) { v.level = 1; },
        [](WheelEffect& v) { v.rightCoefficient = 1; }, [](WheelEffect& v) { v.leftCoefficient = 1; },
        [](WheelEffect& v) { v.rightSaturation = 1; }, [](WheelEffect& v) { v.leftSaturation = 1; },
        [](WheelEffect& v) { v.deadband = 1; }, [](WheelEffect& v) { v.center = 1; },
        [](WheelEffect& v) { v.periodMs = 1; }, [](WheelEffect& v) { v.magnitude = 1; },
        [](WheelEffect& v) { v.offset = 1; }, [](WheelEffect& v) { v.phase = 1; });
    eachFieldCounts(
        di::Output{}, [](di::Output& v) { v.block = di::Block::Periodic; }, [](di::Output& v) { v.magnitude = 1; },
        [](di::Output& v) { v.offset = 1; }, [](di::Output& v) { v.positiveCoefficient = 1; },
        [](di::Output& v) { v.negativeCoefficient = 1; }, [](di::Output& v) { v.positiveSaturation = 1; },
        [](di::Output& v) { v.negativeSaturation = 1; }, [](di::Output& v) { v.deadBand = 1; },
        [](di::Output& v) { v.periodicMagnitude = 1; }, [](di::Output& v) { v.periodicOffset = 1; },
        [](di::Output& v) { v.phase = 1; }, [](di::Output& v) { v.periodUs = 1; });
    eachFieldCounts(
        evdev::Output{}, [](evdev::Output& v) { v.type = 0; }, [](evdev::Output& v) { v.direction = 0; },
        [](evdev::Output& v) { v.level = 1; }, [](evdev::Output& v) { v.rightSaturation = 1; },
        [](evdev::Output& v) { v.leftSaturation = 1; }, [](evdev::Output& v) { v.rightCoefficient = 1; },
        [](evdev::Output& v) { v.leftCoefficient = 1; }, [](evdev::Output& v) { v.deadband = 1; },
        [](evdev::Output& v) { v.center = 1; }, [](evdev::Output& v) { v.waveform = 1; },
        [](evdev::Output& v) { v.periodMs = 1; }, [](evdev::Output& v) { v.magnitude = 1; },
        [](evdev::Output& v) { v.offset = 1; }, [](evdev::Output& v) { v.phase = 1; });
}

TEST(Equality, DevicesAndReports) {
    eachFieldCounts(RawInputState{}, [](RawInputState& v) { v.axes[3] = 0.5F; },
                    [](RawInputState& v) { v.buttons = 1; }, [](RawInputState& v) { v.hat = 2; });
    eachFieldCounts(TargetReport{}, [](TargetReport& v) { v.steering = 1; }, [](TargetReport& v) { v.throttle = 1; },
                    [](TargetReport& v) { v.brake = 1; }, [](TargetReport& v) { v.clutch = 1; },
                    [](TargetReport& v) { v.buttons = 1; }, [](TargetReport& v) { v.hat = 2; });
    eachFieldCounts(Guid{}, [](Guid& v) { v.data1 = 1; }, [](Guid& v) { v.data2 = 1; }, [](Guid& v) { v.data3 = 1; },
                    [](Guid& v) { v.data4[7] = 1; });
    eachFieldCounts(ButtonCell{}, [](ButtonCell& v) { v.column = 1; }, [](ButtonCell& v) { v.y = 1.0F; },
                    [](ButtonCell& v) { v.heading = true; }, [](ButtonCell& v) { v.text = "x"; },
                    [](ButtonCell& v) { v.button = 1; }, [](ButtonCell& v) { v.folded = true; });
    eachFieldCounts(InputReport{}, [](InputReport& v) { v.bytes[0] = 1; }, [](InputReport& v) { v.size = 1; });
    eachFieldCounts(DeviceLabels{}, [](DeviceLabels& v) { v.name = "x"; }, [](DeviceLabels& v) { v.vendorId = 1; },
                    [](DeviceLabels& v) { v.productId = 1; }, [](DeviceLabels& v) { v.kind = DeviceKind::Wheel; },
                    [](DeviceLabels& v) { v.axes[0] = "x"; }, [](DeviceLabels& v) { v.buttons[0] = "x"; });
    eachFieldCounts(RigControl{}, [](RigControl& v) { v.label = "x"; }, [](RigControl& v) { v.kind = "x"; });
    eachFieldCounts(RigDevice{}, [](RigDevice& v) { v.name = "x"; }, [](RigDevice& v) { v.id.vendorId = 1; },
                    [](RigDevice& v) { v.kind = DeviceKind::Pedals; }, [](RigDevice& v) { v.axes[0].label = "x"; },
                    [](RigDevice& v) { v.buttons[0].kind = "x"; });
    eachFieldCounts(Rig{}, [](Rig& v) { v.devices.push_back(RigDevice{}); });
    eachFieldCounts(DrawList::Command{}, [](DrawList::Command& v) { v.kind = DrawList::Command::Kind::Text; },
                    [](DrawList::Command& v) { v.x = 1.0F; }, [](DrawList::Command& v) { v.y = 1.0F; },
                    [](DrawList::Command& v) { v.w = 1.0F; }, [](DrawList::Command& v) { v.h = 1.0F; },
                    [](DrawList::Command& v) { v.colour[0] = 1; }, [](DrawList::Command& v) { v.text = "x"; });
}

}  // namespace
}  // namespace wheelbri
