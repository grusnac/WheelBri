// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace wheelbri {

// The DirectInput force feedback driver a virtual wheel's OEMForceFeedback entry (OEM\VID_046D&PID_xxxx, HKCU and
// HKLM) names. Logitech's software writes that entry for every wheel of the model it knows, and the same key serves a
// virtual one: G HUB's hidpp_forcefeedback_x64.dll (HID++ 0x8123) for the G920/G923, Logitech Gaming Software's own
// for the G25/G27. Without an entry DirectInput uses Windows' HID PID driver.
//
// WheelBri never writes the entry but for the G25's and G27's classic identities (below). Until 2026-11-03 it wrote its own copy of G HUB's for the virtual G923 (marked with
// a WheelBri value); that copy was Windows' HID PID entry with G HUB's CLSID (effect ids 0x000F0026..., the 2026-11-01
// registry exports in captures/g923), which G HUB's driver refuses, so it is always removed.
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

// Which wheels keep another program's driver: those Logitech's drivers can drive, the ones with HID++ (the G920, G923;
// G HUB's driver when it is installed, Windows' HID PID driver when not) and the G25's and G27's classic identities
// (VirtualWheel::classic: G HUB's older-wheel driver). The G25's and G27's other identity has HID PID only: another
// program's driver goes.
[[nodiscard]] constexpr bool keepsOtherDrivers(const bool hidppOrClassic) { return hidppOrClassic; }

// Whether an entry names a driver other than Windows' HID PID driver (and is not WheelBri's old copy).
[[nodiscard]] constexpr bool isOtherDriver(const std::wstring_view registered, const bool wheelBris) {
    return !wheelBris && !registered.empty() && !sameClsid(registered, kPidClsid);
}

enum class Action {
    Leave,    // no entry, or one that stays
    Remove,   // goes: DirectInput then finds the driver that should drive the wheel
};

// `registered`: the CLSID the entry names (empty: no entry). `wheelBris`: it has WheelBri's value (WheelBri's old
// copy). `keepOthers`: keepsOtherDrivers for the wheel. `otherInstalled`: another program's driver is in either hive.
[[nodiscard]] constexpr Action action(const std::wstring_view registered, const bool wheelBris, const bool keepOthers,
                                      const bool otherInstalled) {
    if (wheelBris) return Action::Remove;
    if (registered.empty()) return Action::Leave;
    if (sameClsid(registered, kPidClsid)) {
        // Windows' own entry would hide the other driver (DirectInput reads HKCU first); Windows writes it again.
        return keepOthers && otherInstalled ? Action::Remove : Action::Leave;
    }
    return keepOthers ? Action::Leave : Action::Remove;
}

// --- The G25's and G27's classic identities (VirtualWheel::classic) ----------------------------------------------
// G HUB's older-wheel driver (jerry_forcefeedback_x64.dll) has a class per wheel but never writes their entries (G HUB
// does not support the G25 or G27), so WheelBri writes them, in HKCU, while a classic wheel runs, marked with its
// WheelBri value, and removes them at Stop (a crash: the next Start's removal of WheelBri's entries). The values are
// G HUB's own for the G923 (its HKLM entry, captures/g923/2026-11-01/S0-check), which the older-wheel driver takes
// (ACC, 2026-11-03), and the wheel axis' entry with it, which tells DirectInput that X takes forces: without it every
// effect was DIERR_INVALIDPARAM.
inline constexpr std::wstring_view kG25Clsid = L"{920B260D-BE85-4D30-8797-8BA0A23A5D53}";   // "Logitech G25 Racing ..."
inline constexpr std::wstring_view kG27Clsid = L"{920B260F-BE85-4D30-8797-8BA0A23A5D53}";   // "Logitech G27 Racing ..."

// The class for a wheel's product id (046D:C299 the G25, C29B the G27); empty for any other.
[[nodiscard]] constexpr std::wstring_view classicClsid(const std::uint16_t productId) {
    return productId == 0xC299 ? kG25Clsid : productId == 0xC29B ? kG27Clsid : std::wstring_view{};
}

inline constexpr std::uint32_t kClassicCreatedBy = 0x0800;   // DirectX 8
// DIFFDEVICEATTRIBUTES: flags, sample period and minimum time resolution (us).
inline constexpr std::array<std::uint32_t, 3> kClassicAttributes = {0, 1000, 1000};

// DIEFFECTATTRIBUTES: the driver's id for the effect, its DIEFT_ flags, its static and dynamic parameters, and its
// coordinate systems (0x30: Cartesian and polar).
struct ClassicEffect {
    std::wstring_view guid;
    std::wstring_view name;
    std::array<std::uint32_t, 5> attributes;
};
inline constexpr std::array<ClassicEffect, 11> kClassicEffects = {{
    {L"{13541C20-8E33-11D0-9AD0-00A0C9A06E35}", L"Constant", {0, 0x8601, 0x3ED, 0x3ED, 0x30}},
    {L"{13541C21-8E33-11D0-9AD0-00A0C9A06E35}", L"Ramp Force", {1, 0x8602, 0x3EF, 0x3EF, 0x30}},
    {L"{13541C22-8E33-11D0-9AD0-00A0C9A06E35}", L"Square Wave", {2, 0x8603, 0x3EF, 0x3EF, 0x30}},
    {L"{13541C23-8E33-11D0-9AD0-00A0C9A06E35}", L"Sine Wave", {3, 0x8603, 0x3EF, 0x3EF, 0x30}},
    {L"{13541C24-8E33-11D0-9AD0-00A0C9A06E35}", L"Triangle Wave", {4, 0x8603, 0x3EF, 0x3EF, 0x30}},
    {L"{13541C25-8E33-11D0-9AD0-00A0C9A06E35}", L"Sawtooth Up Wave", {5, 0x8603, 0x3EF, 0x3EF, 0x30}},
    {L"{13541C26-8E33-11D0-9AD0-00A0C9A06E35}", L"Sawtooth Down Wave", {6, 0x8603, 0x3EF, 0x3EF, 0x30}},
    {L"{13541C27-8E33-11D0-9AD0-00A0C9A06E35}", L"Spring", {7, 0xD804, 0x36D, 0x36D, 0x30}},
    {L"{13541C28-8E33-11D0-9AD0-00A0C9A06E35}", L"Damper", {8, 0xD804, 0x36D, 0x36D, 0x30}},
    {L"{13541C29-8E33-11D0-9AD0-00A0C9A06E35}", L"Inertia", {9, 0xD804, 0x36D, 0x36D, 0x30}},
    {L"{13541C2A-8E33-11D0-9AD0-00A0C9A06E35}", L"Friction", {10, 0xD804, 0x36D, 0x36D, 0x30}},
}};

// The wheel axis' entry (OEM\VID_xxxx&PID_xxxx\Axes\0): DIOBJECTATTRIBUTES (flags 0x8101: a force actuator) and
// DIFFOBJECTATTRIBUTES (maximum force 10, force resolution 256), as G HUB writes them for the G923.
inline constexpr std::wstring_view kWheelAxisName = L"Wheel axis";
inline constexpr std::array<std::uint8_t, 4> kWheelAxisAttributes = {0x01, 0x81, 0x00, 0x00};
inline constexpr std::array<std::uint8_t, 8> kWheelAxisFfAttributes = {0x0A, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};

// The values as the registry holds them (REG_BINARY): each one little-endian, one after another.
template <std::size_t N>
constexpr std::array<std::uint8_t, N * 4> bytes(const std::array<std::uint32_t, N>& values) {
    std::array<std::uint8_t, N * 4> out{};
    for (std::size_t i = 0; i < N; ++i) {
        for (std::size_t b = 0; b < 4; ++b) out[i * 4 + b] = static_cast<std::uint8_t>(values[i] >> (8 * b));
    }
    return out;
}

}  // namespace logitech_ffb

}  // namespace wheelbri
