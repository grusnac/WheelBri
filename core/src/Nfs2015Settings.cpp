#include "wheelbri/Nfs2015Settings.hpp"

#include <array>
#include <charconv>

namespace wheelbri {

namespace {

constexpr int kThrottle = 24;
constexpr int kBrake = 25;
constexpr int kWheelType = 3;
constexpr int kSlots = 8;   // bindings per action: the profiles seen have 3 to 6

// The value of the line "<key> <value>" (the whole key, a space, the value up to the line's end without '\r').
std::optional<int> intValue(const std::string_view text, const std::string_view key) {
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.size() > key.size() && line.substr(0, key.size()) == key && line[key.size()] == ' ') {
            const std::string_view value = line.substr(key.size() + 1);
            int number = 0;
            const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), number);
            if (ec == std::errc{} && ptr == value.data() + value.size()) return number;
            return std::nullopt;   // the key with something else than a number: unreadable
        }
        at = end + 1;
    }
    return std::nullopt;
}

}  // namespace

Nfs2015WheelBindings checkNfs2015Settings(const std::string_view profileText) {
    Nfs2015WheelBindings result;
    const std::optional<int> custom = intValue(profileText, "GstInput.CustomControlsWheel");
    if (!custom) return result;
    if (*custom == 0) {
        result.verdict = Nfs2015WheelBindings::Verdict::Defaults;
        return result;
    }
    // The slot holding the wheel binding (type 3; slot 2 in every profile seen, 0 the pad, 1 the keyboard).
    const auto wheelInput = [&](const std::string_view action) -> std::optional<int> {
        bool any = false;
        for (int slot = 0; slot < kSlots; ++slot) {
            const std::string prefix =
                "GstKeyBinding.racevehicle." + std::string(action) + "." + std::to_string(slot) + ".";
            const std::optional<int> type = intValue(profileText, prefix + "type");
            if (!type) continue;
            any = true;
            if (*type == kWheelType) {
                const std::optional<int> button = intValue(profileText, prefix + "button");
                if (!button) return std::nullopt;
                return *button;
            }
        }
        if (!any) return std::nullopt;
        return -1;   // bound, but to no wheel input
    };
    result.accelerate = wheelInput("ConceptMoveFB");
    result.brake = wheelInput("ConceptBrake");
    if (!result.accelerate || !result.brake) {
        result.verdict = Nfs2015WheelBindings::Verdict::Unreadable;
        return result;
    }
    result.verdict = *result.accelerate == kThrottle && *result.brake == kBrake
                         ? Nfs2015WheelBindings::Verdict::AsExpected
                         : Nfs2015WheelBindings::Verdict::PedalsMoved;
    return result;
}

std::string nfs2015WheelInputName(const int input) {
    // IDWB_* in the order NFS16.exe lists them (its strings, 2026-10-09); the paddles at 14 and 15 confirmed by a
    // binding.
    static constexpr std::array<std::string_view, 39> kNames{
        "Lup",      "Ldown",    "Lleft",    "Lright",   "Rup",       "Rdown",       "Rleft",      "Rright",
        "Rtopleft", "Rtopright", "Lthumb",  "Rthumb",   "start",     "alt",         "Ltrigger",   "Rtrigger",
        "Ltrigger2", "Rtrigger2", "Ltrigger3", "Rtrigger3", "Ltrigger4", "Rtrigger4", "Ltrigger5", "Rtrigger5",
        "Throttle", "Brake",    "Clutch",   "Horn",     "ShiftUp",   "ShiftDown",   "Enter",      "FirstGear",
        "SecondGear", "ThirdGear", "FourthGear", "FifthGear", "SixthGear", "ReverseGear", "Undefined"};
    if (input >= 0 && static_cast<std::size_t>(input) < kNames.size()) {
        return std::string(kNames[static_cast<std::size_t>(input)]);
    }
    return input < 0 ? std::string("not a wheel input") : "input " + std::to_string(input);
}

}  // namespace wheelbri
