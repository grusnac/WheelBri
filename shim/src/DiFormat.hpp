#pragma once

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <string>

namespace wheelbri::shim {

// Human-readable renderings of DirectInput values for the Phase 0 log.

// At most `maxChars` characters (game strings may not be terminated inside their MAX_PATH buffers).
[[nodiscard]] std::string toUtf8(const WCHAR* text, std::size_t maxChars = MAX_PATH);
[[nodiscard]] std::string toUtf8(const CHAR* text);

[[nodiscard]] std::string guidToString(const GUID& guid);
[[nodiscard]] std::string describeGuid(const GUID& guid);          // "GUID_ConstantForce {...}" when known
[[nodiscard]] std::string describeProperty(REFGUID property);        // DIPROP_* are small integers cast to GUID refs
[[nodiscard]] std::string describePropertyValue(REFGUID property, LPCDIPROPHEADER header);
[[nodiscard]] std::string describeHresult(HRESULT result);
[[nodiscard]] std::string describeDevType(DWORD devType);
[[nodiscard]] std::string describeEnumFlags(DWORD flags);
[[nodiscard]] std::string describeCooperativeLevel(DWORD flags);
[[nodiscard]] std::string describeCaps(const DIDEVCAPS& caps);
[[nodiscard]] std::string describeDataFormat(LPCDIDATAFORMAT format);
[[nodiscard]] std::string describeDeviceState(DWORD size, const void* data);
[[nodiscard]] std::string describeObjectType(DWORD type);
[[nodiscard]] std::string describeEffect(const GUID& effectType, LPCDIEFFECT effect, DWORD flags);
[[nodiscard]] std::string describeEffectFlags(DWORD flags);          // DIEP_* flags of SetParameters

}  // namespace wheelbri::shim
