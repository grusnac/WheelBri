#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace wheelbri {

// Logitech's DirectInput force feedback driver for HID++ wheels (hidpp_forcefeedback_x64.dll, installed with G HUB):
// the OEMForceFeedback entry Logitech's software writes for a G923 (OEM\VID_046D&PID_C26E), as read from the
// registry (HKCU and HKLM, the same in both). DirectInput hands a wheel's effects and Escape calls (ACC reads and sets
// the range with them) to the driver this entry names. With the TrueForce probe the service writes it for the
// virtual G923 when it is missing (VirtualWheelDevice.cpp).
namespace logitech_ffb {

inline constexpr std::wstring_view kClsid = L"{62B43F0E-E7DB-4329-8C13-A966D84A289F}";
inline constexpr std::uint32_t kCreatedBy = 0x0800;   // DirectX 8

// DIFFDEVICEATTRIBUTES: flags, sample period and minimum time resolution (us).
inline constexpr std::array<std::uint32_t, 3> kAttributes = {0, 1'000'000, 1'000'000};

// DIEFFECTATTRIBUTES: the driver's id for the effect, its DIEFT_ type, the static and dynamic parameters it takes,
// and its coordinate systems.
struct Effect {
    std::wstring_view guid;
    std::wstring_view name;
    std::array<std::uint32_t, 5> attributes;
};

inline constexpr std::array<Effect, 11> kEffects = {{
    {L"{13541C20-8E33-11D0-9AD0-00A0C9A06E35}", L"GUID_ConstantForce", {0x000F0026, 0x8601, 0x3FD, 0x3FD, 0x20}},
    {L"{13541C21-8E33-11D0-9AD0-00A0C9A06E35}", L"GUID_RampForce", {0x000F0027, 0x8602, 0x3FD, 0x3FD, 0x20}},
    {L"{13541C22-8E33-11D0-9AD0-00A0C9A06E35}", L"GUID_Square", {0x000F0030, 0x8603, 0x3FD, 0x3FD, 0x20}},
    {L"{13541C23-8E33-11D0-9AD0-00A0C9A06E35}", L"GUID_Sine", {0x000F0031, 0x8603, 0x3FD, 0x3FD, 0x20}},
    {L"{13541C24-8E33-11D0-9AD0-00A0C9A06E35}", L"GUID_Triangle", {0x000F0032, 0x8603, 0x3FD, 0x3FD, 0x20}},
    {L"{13541C25-8E33-11D0-9AD0-00A0C9A06E35}", L"GUID_SawtoothUp", {0x000F0033, 0x8603, 0x3FD, 0x3FD, 0x20}},
    {L"{13541C26-8E33-11D0-9AD0-00A0C9A06E35}", L"GUID_SawtoothDown", {0x000F0034, 0x8603, 0x3FD, 0x3FD, 0x20}},
    {L"{13541C27-8E33-11D0-9AD0-00A0C9A06E35}", L"GUID_Spring", {0x000F0040, 0xC804, 0x365, 0x365, 0x20}},
    {L"{13541C28-8E33-11D0-9AD0-00A0C9A06E35}", L"GUID_Damper", {0x000F0041, 0xC804, 0x365, 0x365, 0x20}},
    {L"{13541C29-8E33-11D0-9AD0-00A0C9A06E35}", L"GUID_Inertia", {0x000F0042, 0xC804, 0x365, 0x365, 0x20}},
    {L"{13541C2A-8E33-11D0-9AD0-00A0C9A06E35}", L"GUID_Friction", {0x000F0043, 0xC804, 0x365, 0x365, 0x20}},
}};

// The values as the registry holds them (REG_BINARY): each one little-endian, one after another.
template <std::size_t N>
constexpr std::array<std::uint8_t, N * 4> bytes(const std::array<std::uint32_t, N>& values) {
    std::array<std::uint8_t, N * 4> out{};
    for (std::size_t i = 0; i < N; ++i) {
        for (std::size_t b = 0; b < 4; ++b) out[i * 4 + b] = static_cast<std::uint8_t>(values[i] >> (8 * b));
    }
    return out;
}

enum class Action {
    Keep,       // the entry names Logitech's driver
    Write,      // missing or another driver, and Logitech's is installed
    NoDriver,   // missing or another driver, and Logitech's is not installed: nothing to write
};

// `registered`: the CLSID the entry names now (empty: no entry).
constexpr Action action(const std::wstring_view registered, const bool installed) {
    const auto lower = [](const wchar_t c) { return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + 32) : c; };
    bool same = registered.size() == kClsid.size();
    for (std::size_t i = 0; same && i < kClsid.size(); ++i) same = lower(registered[i]) == lower(kClsid[i]);
    if (same) return Action::Keep;
    return installed ? Action::Write : Action::NoDriver;
}

}  // namespace logitech_ffb

}  // namespace wheelbri
