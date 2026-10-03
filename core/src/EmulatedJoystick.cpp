#include "wheelbri/EmulatedJoystick.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wheelbri {

namespace {

// Offsets in DIJOYSTATE / DIJOYSTATE2 (what an object reports before any data format is set).
constexpr std::uint32_t kOffsetPov0 = 32;
constexpr std::uint32_t kOffsetButtons = 48;

// HID usages (Generic Desktop page 0x01, Button page 0x09).
constexpr std::uint16_t kGenericDesktop = 0x01;
constexpr std::uint16_t kButtonPage = 0x09;

constexpr double kReportSpan = 65535.0;   // profile ranges are 0..65535 (see DeviceProfile)

struct AxisInfo {
    ObjectGuid guid;
    const char* name;           // config spelling
    std::uint32_t offset;       // in DIJOYSTATE
    std::uint16_t usage;        // HID Generic Desktop usage
};

constexpr AxisInfo kAxes[] = {
    {ObjectGuid::XAxis, "x", 0, 0x30},   {ObjectGuid::YAxis, "y", 4, 0x31},   {ObjectGuid::ZAxis, "z", 8, 0x32},
    {ObjectGuid::RxAxis, "rx", 12, 0x33}, {ObjectGuid::RyAxis, "ry", 16, 0x34}, {ObjectGuid::RzAxis, "rz", 20, 0x35},
    {ObjectGuid::Slider, "slider", 24, 0x36},
};

const AxisInfo* axisInfo(const ObjectGuid guid) {
    for (const AxisInfo& info : kAxes) {
        if (info.guid == guid) {
            return &info;
        }
    }
    return nullptr;
}

void putU32(std::span<std::uint8_t> out, const std::uint32_t offset, const std::uint32_t value) {
    for (std::uint32_t i = 0; i < 4; ++i) {
        out[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));   // little-endian, as on x86/x64
    }
}

}  // namespace

JoystickLayout layoutFor(const std::string_view profile, const std::size_t buttonCount) {
    JoystickLayout layout;
    layout.buttonCount = buttonCount;
    static_cast<void>(profile);   // one built-in layout: the G920's
    return layout;
}

bool isValid(const JoystickLayout& layout) {
    const ObjectGuid pedals[] = {layout.throttle, layout.brake, layout.clutch};
    for (std::size_t i = 0; i < 3; ++i) {
        const bool optional = i == 2;   // only the clutch may be absent
        if (pedals[i] == ObjectGuid::Any ? !optional : axisInfo(pedals[i]) == nullptr) {
            return false;
        }
        if (pedals[i] == ObjectGuid::XAxis) {
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (pedals[i] != ObjectGuid::Any && pedals[i] == pedals[j]) {
                return false;
            }
        }
    }
    return true;
}

std::optional<ObjectGuid> parseAxisName(const std::string_view name) {
    if (name == "none") {
        return ObjectGuid::Any;
    }
    for (const AxisInfo& info : kAxes) {
        if (name == info.name) {
            return info.guid;
        }
    }
    return std::nullopt;
}

std::string_view axisName(const ObjectGuid guid) {
    const AxisInfo* const info = axisInfo(guid);
    return info != nullptr ? std::string_view(info->name) : std::string_view("none");
}

EmulatedJoystick::EmulatedJoystick(const JoystickLayout& layout) : invertPedals_(layout.invertPedals) {
    struct Axis {
        ObjectGuid guid;
        Control control;
        const char* name;
    };
    std::vector<Axis> axes = {{ObjectGuid::XAxis, Control::Steering, "Wheel axis"},
                              {layout.throttle, Control::Throttle, "Accelerator"},
                              {layout.brake, Control::Brake, "Brake"}};
    if (layout.clutch != ObjectGuid::Any) {
        axes.push_back({layout.clutch, Control::Clutch, "Clutch"});
    }
    // HID devices list (and number) their axes in usage order: X, Y, Z, Rx, Ry, Rz, Slider.
    std::sort(axes.begin(), axes.end(),
              [](const Axis& a, const Axis& b) { return axisInfo(a.guid)->usage < axisInfo(b.guid)->usage; });
    for (std::size_t i = 0; i < axes.size(); ++i) {
        const AxisInfo* const info = axisInfo(axes[i].guid);
        EmulatedObject object;
        object.guid = axes[i].guid;
        object.control = axes[i].control;
        object.typeId = di::kAbsAxis | di::makeInstance(static_cast<std::uint32_t>(i)) |
                        (axes[i].control == Control::Steering ? di::kFfActuator : 0);
        object.nativeOffset = info->offset;
        object.name = axes[i].name;
        object.usagePage = kGenericDesktop;
        object.usage = info->usage;
        objects_.push_back(object);
    }

    const std::size_t buttons = std::min<std::size_t>(layout.buttonCount, 32);
    for (std::size_t i = 0; i < buttons; ++i) {
        EmulatedObject button;
        button.guid = ObjectGuid::Button;
        button.control = Control::Button;
        button.typeId = di::kPushButton | di::makeInstance(static_cast<std::uint32_t>(i));
        button.nativeOffset = kOffsetButtons + static_cast<std::uint32_t>(i);
        button.buttonIndex = static_cast<std::uint32_t>(i);
        button.name = "Button " + std::to_string(i + 1);
        button.usagePage = kButtonPage;
        button.usage = static_cast<std::uint16_t>(i + 1);
        objects_.push_back(button);
    }

    EmulatedObject pov;
    pov.guid = ObjectGuid::Pov;
    pov.control = Control::Pov;
    pov.typeId = di::kPov | di::makeInstance(0);
    pov.nativeOffset = kOffsetPov0;
    pov.name = "Hat Switch";
    pov.usagePage = kGenericDesktop;
    pov.usage = 0x39;
    objects_.push_back(pov);

    ranges_.assign(objects_.size(), LogicalRange{});
    assignedOffset_.assign(objects_.size(), std::nullopt);
}

std::size_t EmulatedJoystick::axisCount() const {
    return static_cast<std::size_t>(
        std::count_if(objects_.begin(), objects_.end(), [](const auto& o) { return o.isAxis(); }));
}

std::size_t EmulatedJoystick::buttonCount() const {
    return static_cast<std::size_t>(
        std::count_if(objects_.begin(), objects_.end(), [](const auto& o) { return o.isButton(); }));
}

std::size_t EmulatedJoystick::povCount() const {
    return static_cast<std::size_t>(
        std::count_if(objects_.begin(), objects_.end(), [](const auto& o) { return o.isPov(); }));
}

std::uint32_t EmulatedJoystick::sizeOf(const EmulatedObject& object) { return object.isButton() ? 1 : 4; }

bool EmulatedJoystick::matches(const FormatEntry& entry, const EmulatedObject& object) const {
    if (entry.guid != ObjectGuid::Any && entry.guid != object.guid) {
        return false;
    }
    const std::uint32_t kinds = entry.type & (di::kAxis | di::kButton | di::kPov);
    if (kinds != 0 && (kinds & object.typeId) == 0) {
        return false;
    }
    if ((entry.type & di::kInstanceMask) != di::kAnyInstance &&
        di::instanceOf(entry.type) != di::instanceOf(object.typeId)) {
        return false;
    }
    if ((entry.type & di::kFfActuator) != 0 && (object.typeId & di::kFfActuator) == 0) {
        return false;
    }
    // Velocity / acceleration / force aspects exist in c_dfDIJoystick2 but not on this device.
    const std::uint32_t aspect = entry.flags & di::kAspectMask;
    return !object.isAxis() || aspect == 0 || aspect == di::kAspectPosition;
}

EmulatedJoystick::FormatResult EmulatedJoystick::setDataFormat(const std::vector<FormatEntry>& entries,
                                                               const std::uint32_t dataSize) {
    std::vector<std::optional<std::uint32_t>> assigned(objects_.size());
    std::vector<std::uint32_t> unassignedPovs;

    for (const FormatEntry& entry : entries) {
        const bool optional = (entry.type & di::kOptional) != 0;
        std::optional<std::size_t> chosen;
        for (std::size_t i = 0; i < objects_.size(); ++i) {
            if (!assigned[i] && matches(entry, objects_[i])) {
                chosen = i;
                break;
            }
        }
        if (!chosen) {
            if (!optional) {
                return FormatResult::InvalidParam;
            }
            const bool povEntry = entry.guid == ObjectGuid::Pov || (entry.type & di::kPov) != 0;
            if (povEntry && entry.offset % 4 == 0 && entry.offset <= dataSize && 4 <= dataSize - entry.offset) {
                unassignedPovs.push_back(entry.offset);
            }
            continue;
        }
        const EmulatedObject& object = objects_[*chosen];
        const std::uint32_t size = sizeOf(object);
        // Written so a huge offset from the game cannot wrap around and pass.
        if (entry.offset > dataSize || size > dataSize - entry.offset || (size == 4 && entry.offset % 4 != 0)) {
            return FormatResult::InvalidParam;
        }
        assigned[*chosen] = entry.offset;
    }

    assignedOffset_ = std::move(assigned);
    unassignedPovOffsets_ = std::move(unassignedPovs);
    dataSize_ = dataSize;
    hasFormat_ = true;
    return FormatResult::Ok;
}

std::optional<std::size_t> EmulatedJoystick::objectAtOffset(const std::uint32_t offset) const {
    for (std::size_t i = 0; i < objects_.size(); ++i) {
        if (reportedOffset(i) == offset && (hasFormat_ ? assignedOffset_[i].has_value() : true)) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> EmulatedJoystick::objectById(const std::uint32_t typeId) const {
    const std::uint32_t kinds = typeId & (di::kAxis | di::kButton | di::kPov);
    for (std::size_t i = 0; i < objects_.size(); ++i) {
        const EmulatedObject& object = objects_[i];
        if ((kinds & object.typeId) != 0 && di::instanceOf(typeId) == di::instanceOf(object.typeId)) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> EmulatedJoystick::objectByUsage(const std::uint16_t page, const std::uint16_t usage) const {
    for (std::size_t i = 0; i < objects_.size(); ++i) {
        if (objects_[i].usagePage == page && objects_[i].usage == usage) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> EmulatedJoystick::formatOffset(const std::size_t object) const {
    return hasFormat_ ? assignedOffset_[object] : std::nullopt;
}

std::uint32_t EmulatedJoystick::reportedOffset(const std::size_t object) const {
    if (hasFormat_ && assignedOffset_[object]) {
        return *assignedOffset_[object];
    }
    return objects_[object].nativeOffset;
}

void EmulatedJoystick::setRange(const std::size_t object, const LogicalRange range) { ranges_[object] = range; }

std::uint32_t EmulatedJoystick::valueOf(const std::size_t index, const TargetReport& report) const {
    const EmulatedObject& object = objects_[index];
    const auto pedal = [&](const std::int32_t raw) {
        const std::int32_t clamped = std::clamp<std::int32_t>(raw, 0, 65535);
        return invertPedals_ ? 65535 - clamped : clamped;
    };
    const auto scaled = [&](const std::int32_t raw) {
        const LogicalRange range = ranges_[index];
        const double fraction = std::clamp(static_cast<double>(raw) / kReportSpan, 0.0, 1.0);
        const double span = static_cast<double>(range.max) - static_cast<double>(range.min);
        return static_cast<std::uint32_t>(static_cast<std::int32_t>(std::llround(range.min + span * fraction)));
    };
    switch (object.control) {
        case Control::Steering: return scaled(report.steering);
        case Control::Throttle: return scaled(pedal(report.throttle));
        case Control::Brake: return scaled(pedal(report.brake));
        case Control::Clutch: return scaled(pedal(report.clutch));
        case Control::Button: return ((report.buttons >> object.buttonIndex) & 1U) != 0 ? di::kButtonDown : 0;
        case Control::Pov:
            return report.hat >= 0 && report.hat < 8 ? static_cast<std::uint32_t>(report.hat) * 4500U
                                                     : di::kPovCentered;
    }
    return 0;
}

bool EmulatedJoystick::writeState(const TargetReport& report, const std::span<std::uint8_t> out) const {
    if (!hasFormat_ || out.size() != dataSize_) {
        return false;
    }
    std::fill(out.begin(), out.end(), std::uint8_t{0});
    for (const std::uint32_t offset : unassignedPovOffsets_) {
        putU32(out, offset, di::kPovCentered);
    }
    for (std::size_t i = 0; i < objects_.size(); ++i) {
        if (!assignedOffset_[i]) {
            continue;
        }
        const std::uint32_t value = valueOf(i, report);
        if (objects_[i].isButton()) {
            out[*assignedOffset_[i]] = static_cast<std::uint8_t>(value);
        } else {
            putU32(out, *assignedOffset_[i], value);
        }
    }
    return true;
}

std::vector<EmulatedJoystick::Change> EmulatedJoystick::diff(const TargetReport& before,
                                                             const TargetReport& after) const {
    std::vector<Change> changes;
    for (std::size_t i = 0; i < objects_.size(); ++i) {
        if (!assignedOffset_[i]) {
            continue;
        }
        const std::uint32_t now = valueOf(i, after);
        if (now != valueOf(i, before)) {
            changes.push_back({*assignedOffset_[i], now});
        }
    }
    std::sort(changes.begin(), changes.end(), [](const Change& a, const Change& b) { return a.offset < b.offset; });
    return changes;
}

}  // namespace wheelbri
