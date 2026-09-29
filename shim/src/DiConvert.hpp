#pragma once

// Conversions between DirectInput structures and the portable types in wheelemu/EmulatedJoystick.hpp.

#include "DiFormat.hpp"

#include "wheelemu/DeviceProfile.hpp"
#include "wheelemu/EmulatedJoystick.hpp"

#include <cstdint>
#include <vector>

namespace wheelemu::shim {

// The portable constants must equal DirectInput's.
static_assert(di::kAbsAxis == DIDFT_ABSAXIS);
static_assert(di::kAxis == DIDFT_AXIS);
static_assert(di::kPushButton == DIDFT_PSHBUTTON);
static_assert(di::kButton == DIDFT_BUTTON);
static_assert(di::kPov == DIDFT_POV);
static_assert(di::kAnyInstance == DIDFT_ANYINSTANCE);
static_assert(di::kInstanceMask == DIDFT_INSTANCEMASK);
static_assert(di::kFfActuator == DIDFT_FFACTUATOR);
static_assert(di::kOptional == DIDFT_OPTIONAL);
static_assert(di::kAspectMask == DIDOI_ASPECTMASK);
static_assert(di::kAspectPosition == DIDOI_ASPECTPOSITION);
static_assert(di::kObjectFfActuator == DIDOI_FFACTUATOR);
static_assert(di::makeInstance(5) == DIDFT_MAKEINSTANCE(5));

[[nodiscard]] ObjectGuid objectGuidOf(const GUID* guid);
[[nodiscard]] const GUID& guidOf(ObjectGuid guid);
[[nodiscard]] std::vector<FormatEntry> toEntries(const DIDATAFORMAT& format);
[[nodiscard]] GUID toGuid(const Guid& guid);

// Numeric ids of the DIPROP_* values handled by the emulation (MAKEDIPROP(n) == n).
namespace prop {
inline constexpr std::uintptr_t kBufferSize = 1;
inline constexpr std::uintptr_t kAxisMode = 2;
inline constexpr std::uintptr_t kGranularity = 3;
inline constexpr std::uintptr_t kRange = 4;
inline constexpr std::uintptr_t kDeadZone = 5;
inline constexpr std::uintptr_t kSaturation = 6;
inline constexpr std::uintptr_t kCalibrationMode = 10;
inline constexpr std::uintptr_t kGuidAndPath = 12;
inline constexpr std::uintptr_t kInstanceName = 13;
inline constexpr std::uintptr_t kProductName = 14;
inline constexpr std::uintptr_t kPhysicalRange = 18;
inline constexpr std::uintptr_t kLogicalRange = 19;
inline constexpr std::uintptr_t kVidPid = 24;
inline constexpr std::uintptr_t kTypeName = 26;
}  // namespace prop

// DIPROP_* are small integers cast to GUID references (MAKEDIPROP); anything else is a real GUID.
[[nodiscard]] inline std::uintptr_t propertyId(REFGUID property) {
    return reinterpret_cast<std::uintptr_t>(&property);
}
[[nodiscard]] inline std::uintptr_t propertyId(const GUID* property) {
    return reinterpret_cast<std::uintptr_t>(property);
}

// ASCII text into a DirectInput CHAR / WCHAR buffer, always terminated.
template <std::size_t N>
void copyText(CHAR (&out)[N], const std::string& text) {
    std::size_t i = 0;
    for (; i + 1 < N && i < text.size(); ++i) {
        out[i] = text[i];
    }
    out[i] = '\0';
}

template <std::size_t N>
void copyText(WCHAR (&out)[N], const std::string& text) {
    std::size_t i = 0;
    for (; i + 1 < N && i < text.size(); ++i) {
        out[i] = static_cast<WCHAR>(static_cast<unsigned char>(text[i]));
    }
    out[i] = L'\0';
}

}  // namespace wheelemu::shim
