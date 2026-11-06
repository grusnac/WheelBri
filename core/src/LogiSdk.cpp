// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/LogiSdk.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstring>
#include <format>

namespace wheelbri::logi {

namespace {

constexpr std::array<Arg, 4> kNone{Arg::Int, Arg::Int, Arg::Int, Arg::Int};

constexpr Function fn(std::string_view name, std::uint8_t args, std::uint8_t keyArgs, std::array<Arg, 4> kinds,
                      std::array<std::string_view, 4> argNames, Ret ret, Kind kind, Extra extra = Extra::None) {
    return Function{name, args, keyArgs, kinds, argNames, ret, kind, extra};
}

using A = Arg;
constexpr std::string_view kIdx = "index";

// The SDK's functions with the arguments its documentation gives them (LogitechSteeringWheelLib.h, SDK 8.x).
constexpr std::array kFunctions{
    fn("LogiSteeringInitializeWithWindow", 2, 0, {A::Bool, A::Window, A::Int, A::Int}, {"ignore XInput", "window"},
       Ret::Bool, Kind::Lifecycle),
    fn("LogiSteeringInitialize", 1, 0, {A::Bool, A::Int, A::Int, A::Int}, {"ignore XInput"}, Ret::Bool,
       Kind::Lifecycle),
    fn("LogiSteeringShutdown", 0, 0, kNone, {}, Ret::Void, Kind::Lifecycle),
    fn("LogiUpdate", 0, 0, kNone, {}, Ret::Bool, Kind::Input),
    fn("LogiGetStateENGINES", 1, 1, kNone, {kIdx}, Ret::State, Kind::Input, Extra::State),
    fn("LogiGetState", 1, 1, kNone, {kIdx}, Ret::State, Kind::Input, Extra::State),
    fn("LogiGetDevicePath", 3, 1, {A::Int, A::Pointer, A::Int, A::Int}, {kIdx, "buffer", "size"}, Ret::Bool,
       Kind::Query, Extra::TextBuffer),
    fn("LogiGetFriendlyProductName", 3, 1, {A::Int, A::Pointer, A::Int, A::Int}, {kIdx, "buffer", "size"},
       Ret::Bool, Kind::Query, Extra::TextBuffer),
    fn("LogiIsConnected", 1, 1, kNone, {kIdx}, Ret::Bool, Kind::Query),
    fn("LogiIsDeviceConnected", 2, 2, {A::Int, A::DeviceType, A::Int, A::Int}, {kIdx, "type"}, Ret::Bool,
       Kind::Query),
    fn("LogiIsManufacturerConnected", 2, 2, {A::Int, A::Manufacturer, A::Int, A::Int}, {kIdx, "manufacturer"},
       Ret::Bool, Kind::Query),
    fn("LogiIsModelConnected", 2, 2, {A::Int, A::Model, A::Int, A::Int}, {kIdx, "model"}, Ret::Bool, Kind::Query),
    fn("LogiButtonTriggered", 2, 2, kNone, {kIdx, "button"}, Ret::Bool, Kind::Input),
    fn("LogiButtonReleased", 2, 2, kNone, {kIdx, "button"}, Ret::Bool, Kind::Input),
    fn("LogiButtonIsPressed", 2, 2, kNone, {kIdx, "button"}, Ret::Bool, Kind::Input),
    fn("LogiGenerateNonLinearValues", 2, 1, kNone, {kIdx, "coefficient"}, Ret::Bool, Kind::Setup),
    fn("LogiGetNonLinearValue", 2, 1, kNone, {kIdx, "input"}, Ret::Int, Kind::Input),
    fn("LogiHasForceFeedback", 1, 1, kNone, {kIdx}, Ret::Bool, Kind::Query),
    fn("LogiIsPlaying", 2, 2, {A::Int, A::ForceType, A::Int, A::Int}, {kIdx, "force"}, Ret::Bool, Kind::Force),
    fn("LogiPlaySpringForce", 4, 1, {A::Int, A::Percent, A::Percent, A::Percent},
       {kIdx, "offset", "saturation", "coefficient"}, Ret::Bool, Kind::Force),
    fn("LogiStopSpringForce", 1, 1, kNone, {kIdx}, Ret::Bool, Kind::Force),
    fn("LogiPlayConstantForce", 2, 1, {A::Int, A::Percent, A::Int, A::Int}, {kIdx, "magnitude"}, Ret::Bool,
       Kind::Force),
    fn("LogiStopConstantForce", 1, 1, kNone, {kIdx}, Ret::Bool, Kind::Force),
    fn("LogiPlayDamperForce", 2, 1, {A::Int, A::Percent, A::Int, A::Int}, {kIdx, "coefficient"}, Ret::Bool,
       Kind::Force),
    fn("LogiStopDamperForce", 1, 1, kNone, {kIdx}, Ret::Bool, Kind::Force),
    fn("LogiPlaySideCollisionForce", 2, 1, {A::Int, A::Percent, A::Int, A::Int}, {kIdx, "magnitude"}, Ret::Bool,
       Kind::Force),
    fn("LogiPlayFrontalCollisionForce", 2, 1, {A::Int, A::Percent, A::Int, A::Int}, {kIdx, "magnitude"},
       Ret::Bool, Kind::Force),
    fn("LogiPlayDirtRoadEffect", 2, 1, {A::Int, A::Percent, A::Int, A::Int}, {kIdx, "magnitude"}, Ret::Bool,
       Kind::Force),
    fn("LogiStopDirtRoadEffect", 1, 1, kNone, {kIdx}, Ret::Bool, Kind::Force),
    fn("LogiPlayBumpyRoadEffect", 2, 1, {A::Int, A::Percent, A::Int, A::Int}, {kIdx, "magnitude"}, Ret::Bool,
       Kind::Force),
    fn("LogiStopBumpyRoadEffect", 1, 1, kNone, {kIdx}, Ret::Bool, Kind::Force),
    fn("LogiPlaySlipperyRoadEffect", 2, 1, {A::Int, A::Percent, A::Int, A::Int}, {kIdx, "magnitude"}, Ret::Bool,
       Kind::Force),
    fn("LogiStopSlipperyRoadEffect", 1, 1, kNone, {kIdx}, Ret::Bool, Kind::Force),
    fn("LogiPlaySurfaceEffect", 4, 1, {A::Int, A::PeriodicType, A::Percent, A::Milliseconds},
       {kIdx, "type", "magnitude", "period"}, Ret::Bool, Kind::Force),
    fn("LogiStopSurfaceEffect", 1, 1, kNone, {kIdx}, Ret::Bool, Kind::Force),
    fn("LogiPlayCarAirborne", 1, 1, kNone, {kIdx}, Ret::Bool, Kind::Force),
    fn("LogiStopCarAirborne", 1, 1, kNone, {kIdx}, Ret::Bool, Kind::Force),
    fn("LogiPlaySoftstopForce", 2, 1, {A::Int, A::Percent, A::Int, A::Int}, {kIdx, "usable range"}, Ret::Bool,
       Kind::Force),
    fn("LogiStopSoftstopForce", 1, 1, kNone, {kIdx}, Ret::Bool, Kind::Force),
    fn("LogiSetPreferredControllerProperties", 1, 0, {A::Pointer, A::Int, A::Int, A::Int}, {"properties"},
       Ret::Bool, Kind::Setup, Extra::PropertiesIn),
    fn("LogiGetCurrentControllerProperties", 2, 1, {A::Int, A::Pointer, A::Int, A::Int}, {kIdx, "properties"},
       Ret::Bool, Kind::Query, Extra::PropertiesOut),
    fn("LogiGetShifterMode", 1, 1, kNone, {kIdx}, Ret::Int, Kind::Query),
    fn("LogiSetOperatingRange", 2, 1, kNone, {kIdx, "range"}, Ret::Bool, Kind::Setup),
    fn("LogiGetOperatingRange", 2, 1, {A::Int, A::Pointer, A::Int, A::Int}, {kIdx, "range"}, Ret::Bool,
       Kind::Query, Extra::RangeOut),
    fn("LogiPlayLeds", 4, 1, {A::Int, A::Float, A::Float, A::Float}, {kIdx, "rpm", "first led", "redline"},
       Ret::Bool, Kind::Lights),
    fn("LogiSetOperatingRangeDInput", 2, 1, {A::Handle, A::Int, A::Int, A::Int}, {"device", "range"}, Ret::Bool,
       Kind::Setup),
    fn("LogiGetOperatingRangeDInput", 2, 1, {A::Handle, A::Pointer, A::Int, A::Int}, {"device", "range"},
       Ret::Bool, Kind::Query, Extra::RangeOut),
    fn("LogiPlayLedsDInput", 4, 1, {A::Handle, A::Float, A::Float, A::Float}, {"device", "rpm", "first led",
                                                                              "redline"},
       Ret::Bool, Kind::Lights),
};
static_assert(kFunctions.size() == 48, "the 48 names NFS Payback looks up");

std::int32_t low32(const std::int64_t raw) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(raw)); }
bool lowBool(const std::int64_t raw) { return (raw & 0xFF) != 0; }

std::string hex(const std::int64_t raw) { return std::format("0x{:x}", static_cast<std::uint64_t>(raw)); }

std::string nameOr(const int value, std::span<const std::string_view> names, const int first) {
    // In 64 bits: value - first overflows int for a value near INT_MAX when first is negative (UBSan).
    const std::int64_t i = std::int64_t{value} - first;
    if (i >= 0 && i < static_cast<std::int64_t>(names.size()) && !names[static_cast<std::size_t>(i)].empty()) {
        return std::string(names[static_cast<std::size_t>(i)]);
    }
    return std::format("unknown {}", value);
}

std::string formatArg(const Arg kind, const std::int64_t raw) {
    switch (kind) {
        case Arg::Int: return std::to_string(low32(raw));
        case Arg::Bool: return lowBool(raw) ? "true" : "false";
        case Arg::Percent: return std::format("{}%", low32(raw));
        case Arg::Pointer:
        case Arg::Window:
        case Arg::Handle: return hex(raw);
        case Arg::DeviceType: return deviceTypeName(low32(raw));
        case Arg::Manufacturer: return manufacturerName(low32(raw));
        case Arg::Model: return modelName(low32(raw));
        case Arg::ForceType: return forceTypeName(low32(raw));
        case Arg::PeriodicType: return periodicTypeName(low32(raw));
        case Arg::Milliseconds: return std::format("{} ms", low32(raw));
        case Arg::Float: return std::format("{:.3f}", std::bit_cast<float>(static_cast<std::uint32_t>(raw)));
    }
    return std::to_string(raw);
}

std::int32_t readInt(std::span<const std::uint8_t> bytes, const std::size_t offset) {
    std::int32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof value);
    return value;
}

}  // namespace

std::span<const Function> functions() { return kFunctions; }

const Function* findFunction(const std::string_view name) {
    const std::size_t i = functionIndex(name);
    return i < kFunctions.size() ? &kFunctions[i] : nullptr;
}

std::size_t functionIndex(const std::string_view name) {
    for (std::size_t i = 0; i < kFunctions.size(); ++i) {
        if (kFunctions[i].name == name) return i;
    }
    return kFunctions.size();
}

std::string formatCall(const Function& f, const std::span<const std::int64_t> args) {
    std::string out(f.name);
    out += '(';
    for (std::size_t i = 0; i < f.args && i < args.size(); ++i) {
        if (i > 0) out += ", ";
        out += f.argNames[i];
        out += ' ';
        out += formatArg(f.kinds[i], args[i]);
    }
    out += ')';
    return out;
}

std::string formatResult(const Function& f, const std::int64_t raw) {
    switch (f.ret) {
        case Ret::Void: return "void";
        case Ret::Bool: return lowBool(raw) ? "true" : "false";
        case Ret::Int: return std::to_string(low32(raw));
        case Ret::State: return raw == 0 ? "null" : hex(raw);
    }
    return std::to_string(raw);
}

std::string modelName(const int model) {
    static constexpr std::string_view kNames[] = {
        "G27", "Driving Force GT", "G25", "MOMO Racing", "MOMO Force", "Driving Force Pro", "Driving Force",
        "NASCAR Racing Wheel", "Formula Force", "Formula Force GP", "Force 3D Pro", "Extreme 3D Pro", "Freedom 2.4",
        "Attack 3", "Force 3D", "Strike Force 3D", "G940 joystick", "G940 throttle", "G940 pedals", "RumblePad",
        "RumblePad 2", "Cordless RumblePad 2", "Cordless Gamepad", "Dual Action Gamepad", "Precision Gamepad 2",
        "ChillStream", "G29", "G920"};
    return nameOr(model, kNames, 0);
}

std::string manufacturerName(const int manufacturer) {
    static constexpr std::string_view kNames[] = {"none", "Logitech", "Microsoft", "other"};
    return nameOr(manufacturer, kNames, -1);
}

std::string deviceTypeName(const int type) {
    static constexpr std::string_view kNames[] = {"none", "wheel", "joystick", "gamepad", "other"};
    return nameOr(type, kNames, -1);
}

std::string forceTypeName(const int type) {
    static constexpr std::string_view kNames[] = {"none",           "spring",      "constant",   "damper",
                                                  "side collision", "frontal collision", "dirt road", "bumpy road",
                                                  "slippery road",  "surface effect", "",       "soft stop",
                                                  "car airborne"};
    return nameOr(type, kNames, -1);
}

std::string periodicTypeName(const int type) {
    static constexpr std::string_view kNames[] = {"none", "sine", "square", "triangle"};
    return nameOr(type, kNames, -1);
}

std::optional<ControllerProperties> decodeProperties(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kPropertiesSize) return std::nullopt;
    ControllerProperties p;
    p.forceEnable = bytes[0] != 0;
    p.overallGain = readInt(bytes, 4);
    p.springGain = readInt(bytes, 8);
    p.damperGain = readInt(bytes, 12);
    p.defaultSpringEnabled = bytes[16] != 0;
    p.defaultSpringGain = readInt(bytes, 20);
    p.combinePedals = bytes[24] != 0;
    p.wheelRange = readInt(bytes, 28);
    p.gameSettingsEnabled = bytes[32] != 0;
    p.allowGameSettings = bytes[33] != 0;
    return p;
}

std::string describe(const ControllerProperties& p) {
    return std::format("force {}, overall gain {}%, spring gain {}%, damper gain {}%, default spring {} at {}%, "
                       "combined pedals {}, wheel range {} deg, game settings {} (allowed {})",
                       p.forceEnable ? "on" : "off", p.overallGain, p.springGain, p.damperGain,
                       p.defaultSpringEnabled ? "on" : "off", p.defaultSpringGain, p.combinePedals ? "yes" : "no",
                       p.wheelRange, p.gameSettingsEnabled ? "on" : "off", p.allowGameSettings ? "yes" : "no");
}

std::optional<StateSummary> decodeState(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kStatePrefixSize) return std::nullopt;
    StateSummary s;
    s.x = readInt(bytes, 0);
    s.y = readInt(bytes, 4);
    s.z = readInt(bytes, 8);
    s.rx = readInt(bytes, 12);
    s.ry = readInt(bytes, 16);
    s.rz = readInt(bytes, 20);
    s.sliders = {readInt(bytes, 24), readInt(bytes, 28)};
    s.pov0 = static_cast<std::uint32_t>(readInt(bytes, 32));
    for (std::size_t b = 0; b < 32; ++b) {
        if ((bytes[48 + b] & 0x80) != 0) s.buttons |= 1U << b;
    }
    return s;
}

std::string describe(const StateSummary& s) {
    return std::format("x={} y={} z={} rx={} ry={} rz={} s0={} s1={} pov0={} buttons={:08x}", s.x, s.y, s.z, s.rx,
                       s.ry, s.rz, s.sliders[0], s.sliders[1], static_cast<std::int32_t>(s.pov0), s.buttons);
}

ChangeLog::Seen ChangeLog::observe(const std::size_t function, const std::int64_t key0, const std::int64_t key1,
                                   const std::string_view value) {
    const std::lock_guard lock(mutex_);
    const auto [it, inserted] = last_.try_emplace(std::tuple{function, key0, key1}, value);
    if (inserted) return Seen::First;
    if (it->second == value) return Seen::Same;
    it->second.assign(value);
    return Seen::Changed;
}

std::string newestVersionFolder(const std::span<const std::string> names) {
    std::string best;
    std::vector<unsigned> bestParts;
    for (const std::string& name : names) {
        std::vector<unsigned> parts;
        bool ok = !name.empty();
        std::size_t start = 0;
        while (ok && start <= name.size()) {
            const std::size_t end = std::min(name.find('_', start), name.size());
            unsigned value = 0;
            const auto [ptr, ec] = std::from_chars(name.data() + start, name.data() + end, value);
            ok = ec == std::errc() && ptr == name.data() + end && end > start;
            parts.push_back(value);
            start = end + 1;
        }
        if (ok && (best.empty() || parts > bestParts)) {
            best = name;
            bestParts = std::move(parts);
        }
    }
    return best;
}

}  // namespace wheelbri::logi
