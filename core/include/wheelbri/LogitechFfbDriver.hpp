#pragma once

#include <string_view>

namespace wheelbri {

// The DirectInput force feedback driver a virtual wheel's OEMForceFeedback entry (OEM\VID_046D&PID_xxxx, HKCU and
// HKLM) names. Logitech's software writes that entry for every wheel of the model it knows, and the same key serves a
// virtual one: G HUB's hidpp_forcefeedback_x64.dll (HID++ 0x8123) for the G920/G923, Logitech Gaming Software's own
// for the G25/G27. Without an entry DirectInput uses Windows' HID PID driver.
//
// WheelBri never writes the entry. Until 2026-11-03 it wrote its own copy of G HUB's for the virtual G923 (marked with
// a WheelBri value); that copy was not what G HUB writes (effect ids, flags, timings: the 2026-11-01 registry exports
// in captures/g923), so it is always removed.
namespace logitech_ffb {

inline constexpr std::wstring_view kGHubClsid = L"{62B43F0E-E7DB-4329-8C13-A966D84A289F}";
inline constexpr std::wstring_view kPidClsid = L"{EEC6993A-B3FD-11D2-A916-00C04FB98638}";   // Windows' HID PID driver

[[nodiscard]] constexpr bool sameClsid(const std::wstring_view a, const std::wstring_view b) {
    const auto lower = [](const wchar_t c) { return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + 32) : c; };
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

enum class Action {
    Leave,    // no entry, Windows' PID driver, or another program's driver kept on purpose
    Remove,   // goes, so DirectInput uses Windows' PID driver
};

// `registered`: the CLSID the entry names (empty: no entry). `wheelBris`: it has WheelBri's value (WheelBri's old
// copy). `keepOthers`: [emulator] logitech_ffb_driver = true, another program's driver stays.
[[nodiscard]] constexpr Action action(const std::wstring_view registered, const bool wheelBris, const bool keepOthers) {
    if (registered.empty() && !wheelBris) return Action::Leave;
    if (wheelBris) return Action::Remove;
    if (sameClsid(registered, kPidClsid)) return Action::Leave;
    return keepOthers ? Action::Leave : Action::Remove;
}

}  // namespace logitech_ffb

}  // namespace wheelbri
