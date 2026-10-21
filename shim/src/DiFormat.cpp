#include "DiFormat.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace wheelbri::shim {

namespace {

std::string format(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

std::string format(const char* fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return buffer;
}

struct NamedGuid {
    const GUID* guid;
    const char* name;
};

const NamedGuid kKnownGuids[] = {
    {&GUID_SysKeyboard, "GUID_SysKeyboard"}, {&GUID_SysMouse, "GUID_SysMouse"},
    {&GUID_Joystick, "GUID_Joystick"},
    {&GUID_XAxis, "GUID_XAxis"}, {&GUID_YAxis, "GUID_YAxis"}, {&GUID_ZAxis, "GUID_ZAxis"},
    {&GUID_RxAxis, "GUID_RxAxis"}, {&GUID_RyAxis, "GUID_RyAxis"}, {&GUID_RzAxis, "GUID_RzAxis"},
    {&GUID_Slider, "GUID_Slider"}, {&GUID_Button, "GUID_Button"}, {&GUID_Key, "GUID_Key"},
    {&GUID_POV, "GUID_POV"}, {&GUID_Unknown, "GUID_Unknown"},
    {&GUID_ConstantForce, "GUID_ConstantForce"}, {&GUID_RampForce, "GUID_RampForce"},
    {&GUID_Square, "GUID_Square"}, {&GUID_Sine, "GUID_Sine"}, {&GUID_Triangle, "GUID_Triangle"},
    {&GUID_SawtoothUp, "GUID_SawtoothUp"}, {&GUID_SawtoothDown, "GUID_SawtoothDown"},
    {&GUID_Spring, "GUID_Spring"}, {&GUID_Damper, "GUID_Damper"}, {&GUID_Inertia, "GUID_Inertia"},
    {&GUID_Friction, "GUID_Friction"}, {&GUID_CustomForce, "GUID_CustomForce"},
    {&IID_IUnknown, "IID_IUnknown"},
    {&IID_IDirectInput8A, "IID_IDirectInput8A"}, {&IID_IDirectInput8W, "IID_IDirectInput8W"},
    {&IID_IDirectInputDevice8A, "IID_IDirectInputDevice8A"},
    {&IID_IDirectInputDevice8W, "IID_IDirectInputDevice8W"},
    {&IID_IDirectInputEffect, "IID_IDirectInputEffect"},
};

bool isPeriodic(const GUID& type) {
    return type == GUID_Square || type == GUID_Sine || type == GUID_Triangle || type == GUID_SawtoothUp ||
           type == GUID_SawtoothDown;
}

bool isCondition(const GUID& type) {
    return type == GUID_Spring || type == GUID_Damper || type == GUID_Inertia || type == GUID_Friction;
}

// The same limits EmulatedEffect::SetParameters puts on what the game passes: describeEffect is also called on
// parameters a call rejected, so it must not read further than a valid call could point to.
constexpr DWORD kMaxAxes = 8;
constexpr DWORD kMaxTypeSpecificBytes = 1024;

void appendFlag(std::string& out, const DWORD flags, const DWORD bit, const char* name) {
    if ((flags & bit) != 0) {
        out += out.empty() ? "" : "|";
        out += name;
    }
}

std::string describeTypeSpecific(const GUID& type, const DIEFFECT& effect) {
    const void* const params = effect.lpvTypeSpecificParams;
    const DWORD size = effect.cbTypeSpecificParams;
    if (params == nullptr || size == 0) {
        return " params=none";
    }
    if (size > kMaxTypeSpecificBytes) {
        return format(" params=%lu bytes (too large, not read)", size);
    }
    if (type == GUID_ConstantForce && size >= sizeof(DICONSTANTFORCE)) {
        return format(" constant{magnitude=%ld}", static_cast<const DICONSTANTFORCE*>(params)->lMagnitude);
    }
    if (type == GUID_RampForce && size >= sizeof(DIRAMPFORCE)) {
        const auto* ramp = static_cast<const DIRAMPFORCE*>(params);
        return format(" ramp{start=%ld end=%ld}", ramp->lStart, ramp->lEnd);
    }
    if (isPeriodic(type) && size >= sizeof(DIPERIODIC)) {
        const auto* periodic = static_cast<const DIPERIODIC*>(params);
        return format(" periodic{magnitude=%lu offset=%ld phase=%lu period=%luus}", periodic->dwMagnitude,
                      periodic->lOffset, periodic->dwPhase, periodic->dwPeriod);
    }
    if (isCondition(type) && size >= sizeof(DICONDITION)) {
        std::string out;
        const auto* conditions = static_cast<const DICONDITION*>(params);
        const DWORD count = std::min<DWORD>(size / sizeof(DICONDITION), kMaxAxes);
        for (DWORD i = 0; i < count; ++i) {
            const DICONDITION& c = conditions[i];
            out += format(" condition[%lu]{offset=%ld +coef=%ld -coef=%ld +sat=%lu -sat=%lu deadband=%ld}", i,
                          c.lOffset, c.lPositiveCoefficient, c.lNegativeCoefficient, c.dwPositiveSaturation,
                          c.dwNegativeSaturation, c.lDeadBand);
        }
        return out;
    }
    if (type == GUID_CustomForce && size >= sizeof(DICUSTOMFORCE)) {
        const auto* custom = static_cast<const DICUSTOMFORCE*>(params);
        return format(" custom{channels=%lu samplePeriod=%lu samples=%lu}", custom->cChannels,
                      custom->dwSamplePeriod, custom->cSamples);
    }
    return format(" params=%lu bytes (not decoded)", size);
}

}  // namespace

std::string toUtf8(const WCHAR* text, const std::size_t maxChars) {
    if (text == nullptr) {
        return "(null)";
    }
    // At most `maxChars`: the strings come from the game, which may not end them inside its buffer.
    const int chars = static_cast<int>(wcsnlen(text, maxChars));
    if (chars == 0) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, chars, nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, chars, result.data(), length, nullptr, nullptr);
    return result;
}

std::string toUtf8(const CHAR* text, const std::size_t maxChars) {
    // Bounded as the wide one: an ANSI name is a MAX_PATH array the game may not have ended.
    return text == nullptr ? "(null)" : std::string(text, strnlen(text, maxChars));
}

std::string guidToString(const GUID& guid) {
    return format("{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", guid.Data1, guid.Data2, guid.Data3,
                  guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
                  guid.Data4[6], guid.Data4[7]);
}

std::string describeGuid(const GUID& guid) {
    for (const NamedGuid& known : kKnownGuids) {
        if (*known.guid == guid) {
            return std::string(known.name) + " " + guidToString(guid);
        }
    }
    // DirectInput product GUIDs of HID devices end in "PIDVID"; decode them because that is what games match on.
    static constexpr unsigned char kPidVid[] = {0, 0, 'P', 'I', 'D', 'V', 'I', 'D'};
    if (std::equal(std::begin(kPidVid), std::end(kPidVid), guid.Data4) && guid.Data2 == 0 && guid.Data3 == 0) {
        return format("product VID_%04lX&PID_%04lX ", guid.Data1 & 0xFFFF, guid.Data1 >> 16) + guidToString(guid);
    }
    return guidToString(guid);
}

std::string describeProperty(REFGUID property) {
    const auto id = reinterpret_cast<ULONG_PTR>(&property);
    if (id > 0xFFFF) {
        return "property " + guidToString(property);
    }
    static const char* const kNames[] = {
        "?", "DIPROP_BUFFERSIZE", "DIPROP_AXISMODE", "DIPROP_GRANULARITY", "DIPROP_RANGE", "DIPROP_DEADZONE",
        "DIPROP_SATURATION", "DIPROP_FFGAIN", "DIPROP_FFLOAD", "DIPROP_AUTOCENTER", "DIPROP_CALIBRATIONMODE",
        "DIPROP_CALIBRATION", "DIPROP_GUIDANDPATH", "DIPROP_INSTANCENAME", "DIPROP_PRODUCTNAME",
        "DIPROP_JOYSTICKID", "DIPROP_GETPORTDISPLAYNAME", "DIPROP_17", "DIPROP_PHYSICALRANGE",
        "DIPROP_LOGICALRANGE", "DIPROP_KEYNAME", "DIPROP_CPOINTS", "DIPROP_APPDATA", "DIPROP_SCANCODE",
        "DIPROP_VIDPID", "DIPROP_USERNAME", "DIPROP_TYPENAME"};
    return id < std::size(kNames) ? kNames[id] : format("DIPROP_%lu", static_cast<unsigned long>(id));
}

std::string describePropertyValue(REFGUID property, LPCDIPROPHEADER header) {
    if (header == nullptr) {
        return "header=null";
    }
    std::string out = format("obj=0x%lx how=%lu", header->dwObj, header->dwHow);
    const auto id = reinterpret_cast<ULONG_PTR>(&property);
    const bool isRange = id == 4 || id == 18 || id == 19;
    const bool isString = id == 13 || id == 14 || id == 16 || id == 20 || id == 25 || id == 26;

    if (isRange && header->dwSize >= sizeof(DIPROPRANGE)) {
        const auto* range = reinterpret_cast<const DIPROPRANGE*>(header);
        out += format(" range=[%ld, %ld]", range->lMin, range->lMax);
    } else if (id == 12 && header->dwSize >= sizeof(DIPROPGUIDANDPATH)) {
        const auto* path = reinterpret_cast<const DIPROPGUIDANDPATH*>(header);
        out += " class=" + guidToString(path->guidClass) + " path=" + toUtf8(path->wszPath);
    } else if (isString && header->dwSize >= sizeof(DIPROPSTRING)) {
        out += " string=\"" + toUtf8(reinterpret_cast<const DIPROPSTRING*>(header)->wsz) + "\"";
    } else if (id == 24 && header->dwSize >= sizeof(DIPROPDWORD)) {
        const DWORD value = reinterpret_cast<const DIPROPDWORD*>(header)->dwData;
        out += format(" vid=%04lX pid=%04lX", value & 0xFFFF, value >> 16);
    } else if (header->dwSize == sizeof(DIPROPDWORD)) {
        out += format(" dword=%lu", reinterpret_cast<const DIPROPDWORD*>(header)->dwData);
    } else {
        out += format(" size=%lu", header->dwSize);
    }
    return out;
}

std::string describeHresult(const HRESULT result) {
    struct Named {
        unsigned long value;   // DIERR_* are unsigned literals; compare bit patterns
        const char* name;
    };
    static const Named kNames[] = {
        {static_cast<unsigned long>(DI_OK), "DI_OK"}, {static_cast<unsigned long>(S_FALSE), "S_FALSE/DI_NOEFFECT/DI_BUFFEROVERFLOW"}, {static_cast<unsigned long>(DI_POLLEDDEVICE), "DI_POLLEDDEVICE"},
        {static_cast<unsigned long>(DI_DOWNLOADSKIPPED), "DI_DOWNLOADSKIPPED"}, {static_cast<unsigned long>(DI_EFFECTRESTARTED), "DI_EFFECTRESTARTED"},
        {static_cast<unsigned long>(DI_TRUNCATED), "DI_TRUNCATED"}, {static_cast<unsigned long>(DIERR_INPUTLOST), "DIERR_INPUTLOST"},
        {static_cast<unsigned long>(DIERR_NOTACQUIRED), "DIERR_NOTACQUIRED"}, {static_cast<unsigned long>(DIERR_INVALIDPARAM), "DIERR_INVALIDPARAM"},
        {static_cast<unsigned long>(DIERR_DEVICENOTREG), "DIERR_DEVICENOTREG"}, {static_cast<unsigned long>(DIERR_UNSUPPORTED), "DIERR_UNSUPPORTED"},
        {static_cast<unsigned long>(DIERR_NOTINITIALIZED), "DIERR_NOTINITIALIZED"}, {static_cast<unsigned long>(DIERR_ACQUIRED), "DIERR_ACQUIRED"},
        {static_cast<unsigned long>(DIERR_OTHERAPPHASPRIO), "DIERR_OTHERAPPHASPRIO"}, {static_cast<unsigned long>(DIERR_NOTEXCLUSIVEACQUIRED), "DIERR_NOTEXCLUSIVEACQUIRED"},
        {static_cast<unsigned long>(DIERR_OBJECTNOTFOUND), "DIERR_OBJECTNOTFOUND"}, {static_cast<unsigned long>(DIERR_DEVICEFULL), "DIERR_DEVICEFULL"},
        {static_cast<unsigned long>(DIERR_EFFECTPLAYING), "DIERR_EFFECTPLAYING"}, {static_cast<unsigned long>(DIERR_INCOMPLETEEFFECT), "DIERR_INCOMPLETEEFFECT"},
        {static_cast<unsigned long>(DIERR_NOTDOWNLOADED), "DIERR_NOTDOWNLOADED"}, {static_cast<unsigned long>(E_POINTER), "E_POINTER"},
        {static_cast<unsigned long>(E_NOINTERFACE), "E_NOINTERFACE"}, {static_cast<unsigned long>(E_OUTOFMEMORY), "E_OUTOFMEMORY"},
    };
    for (const Named& named : kNames) {
        if (named.value == static_cast<unsigned long>(result)) {
            return named.name;
        }
    }
    return format("HRESULT 0x%08lX", static_cast<unsigned long>(result));
}

std::string describeDevType(const DWORD devType) {
    static const char* const kTypes[] = {"DEVICE", "MOUSE", "KEYBOARD", "JOYSTICK", "GAMEPAD", "DRIVING",
                                         "FLIGHT", "1STPERSON", "DEVICECTRL", "SCREENPOINTER", "REMOTE",
                                         "SUPPLEMENTAL"};
    const DWORD type = GET_DIDEVICE_TYPE(devType);
    const DWORD subtype = GET_DIDEVICE_SUBTYPE(devType);
    const char* name = type >= DI8DEVTYPE_DEVICE && type <= DI8DEVTYPE_SUPPLEMENTAL
                           ? kTypes[type - DI8DEVTYPE_DEVICE]
                           : "?";
    return format("0x%08lx (type=%s subtype=%lu%s)", devType, name, subtype,
                  (devType & DIDEVTYPE_HID) != 0 ? " HID" : "");
}

std::string describeEnumFlags(const DWORD flags) {
    std::string out;
    appendFlag(out, flags, DIEDFL_ATTACHEDONLY, "ATTACHEDONLY");
    appendFlag(out, flags, DIEDFL_FORCEFEEDBACK, "FORCEFEEDBACK");
    appendFlag(out, flags, DIEDFL_INCLUDEALIASES, "INCLUDEALIASES");
    appendFlag(out, flags, DIEDFL_INCLUDEPHANTOMS, "INCLUDEPHANTOMS");
    appendFlag(out, flags, DIEDFL_INCLUDEHIDDEN, "INCLUDEHIDDEN");
    return out.empty() ? "ALLDEVICES" : out;
}

std::string describeCooperativeLevel(const DWORD flags) {
    std::string out;
    appendFlag(out, flags, DISCL_EXCLUSIVE, "EXCLUSIVE");
    appendFlag(out, flags, DISCL_NONEXCLUSIVE, "NONEXCLUSIVE");
    appendFlag(out, flags, DISCL_FOREGROUND, "FOREGROUND");
    appendFlag(out, flags, DISCL_BACKGROUND, "BACKGROUND");
    appendFlag(out, flags, DISCL_NOWINKEY, "NOWINKEY");
    return out;
}

std::string describeCaps(const DIDEVCAPS& caps) {
    std::string flags;
    appendFlag(flags, caps.dwFlags, DIDC_ATTACHED, "ATTACHED");
    appendFlag(flags, caps.dwFlags, DIDC_POLLEDDEVICE, "POLLEDDEVICE");
    appendFlag(flags, caps.dwFlags, DIDC_EMULATED, "EMULATED");
    appendFlag(flags, caps.dwFlags, DIDC_POLLEDDATAFORMAT, "POLLEDDATAFORMAT");
    appendFlag(flags, caps.dwFlags, DIDC_FORCEFEEDBACK, "FORCEFEEDBACK");
    appendFlag(flags, caps.dwFlags, DIDC_FFATTACK, "FFATTACK");
    appendFlag(flags, caps.dwFlags, DIDC_FFFADE, "FFFADE");
    appendFlag(flags, caps.dwFlags, DIDC_SATURATION, "SATURATION");
    appendFlag(flags, caps.dwFlags, DIDC_POSNEGCOEFFICIENTS, "POSNEGCOEFFICIENTS");
    appendFlag(flags, caps.dwFlags, DIDC_POSNEGSATURATION, "POSNEGSATURATION");
    appendFlag(flags, caps.dwFlags, DIDC_DEADBAND, "DEADBAND");
    appendFlag(flags, caps.dwFlags, DIDC_STARTDELAY, "STARTDELAY");
    std::string out = format("flags=%s devType=", flags.c_str()) + describeDevType(caps.dwDevType) +
                      format(" axes=%lu buttons=%lu povs=%lu", caps.dwAxes, caps.dwButtons, caps.dwPOVs);
    // A DirectX 3 DIDEVCAPS ends after dwPOVs: the force feedback fields are not there to read.
    if (caps.dwSize >= sizeof(DIDEVCAPS)) {
        out += format(" ffSamplePeriod=%lu ffMinRes=%lu fw=%lu hw=%lu ffDriver=%lu", caps.dwFFSamplePeriod,
                      caps.dwFFMinTimeResolution, caps.dwFirmwareRevision, caps.dwHardwareRevision,
                      caps.dwFFDriverVersion);
    }
    return out;
}

std::string describeDataFormat(LPCDIDATAFORMAT format_) {
    if (format_ == nullptr) {
        return "null";
    }
    const char* guess = "custom";
    if (format_->dwDataSize == sizeof(DIJOYSTATE2)) {
        guess = "looks like c_dfDIJoystick2";
    } else if (format_->dwDataSize == sizeof(DIJOYSTATE)) {
        guess = "looks like c_dfDIJoystick";
    } else if (format_->dwDataSize == 256) {
        guess = "looks like c_dfDIKeyboard";
    } else if (format_->dwDataSize == sizeof(DIMOUSESTATE) || format_->dwDataSize == sizeof(DIMOUSESTATE2)) {
        guess = "looks like c_dfDIMouse";
    }
    return format("dataSize=%lu objects=%lu flags=%s (%s)", format_->dwDataSize, format_->dwNumObjs,
                  (format_->dwFlags & DIDF_RELAXIS) != 0 ? "RELAXIS" : "ABSAXIS", guess);
}

std::string describeDeviceState(const DWORD size, const void* const data) {
    if (data == nullptr || (size != sizeof(DIJOYSTATE) && size != sizeof(DIJOYSTATE2))) {
        return format("%lu bytes", size);
    }
    const auto* state = static_cast<const DIJOYSTATE*>(data);
    DWORD buttons = 0;
    for (int i = 0; i < 32; ++i) {
        if ((state->rgbButtons[i] & 0x80) != 0) {
            buttons |= 1UL << i;
        }
    }
    return format("x=%ld y=%ld z=%ld rx=%ld ry=%ld rz=%ld s0=%ld s1=%ld pov0=%lu buttons=%08lx", state->lX,
                  state->lY, state->lZ, state->lRx, state->lRy, state->lRz, state->rglSlider[0],
                  state->rglSlider[1], state->rgdwPOV[0], buttons);
}

std::string describeObjectType(const DWORD type) {
    std::string out;
    appendFlag(out, type, DIDFT_RELAXIS, "RELAXIS");
    appendFlag(out, type, DIDFT_ABSAXIS, "ABSAXIS");
    appendFlag(out, type, DIDFT_PSHBUTTON, "PSHBUTTON");
    appendFlag(out, type, DIDFT_TGLBUTTON, "TGLBUTTON");
    appendFlag(out, type, DIDFT_POV, "POV");
    appendFlag(out, type, DIDFT_COLLECTION, "COLLECTION");
    appendFlag(out, type, DIDFT_NODATA, "NODATA");
    appendFlag(out, type, DIDFT_FFACTUATOR, "FFACTUATOR");
    appendFlag(out, type, DIDFT_FFEFFECTTRIGGER, "FFEFFECTTRIGGER");
    appendFlag(out, type, DIDFT_OUTPUT, "OUTPUT");
    return format("0x%08lx %s #%lu", type, out.c_str(), static_cast<unsigned long>(DIDFT_GETINSTANCE(type)));
}

std::string describeEffectFlags(const DWORD flags) {
    std::string out;
    appendFlag(out, flags, DIEP_DURATION, "DURATION");
    appendFlag(out, flags, DIEP_SAMPLEPERIOD, "SAMPLEPERIOD");
    appendFlag(out, flags, DIEP_GAIN, "GAIN");
    appendFlag(out, flags, DIEP_TRIGGERBUTTON, "TRIGGERBUTTON");
    appendFlag(out, flags, DIEP_TRIGGERREPEATINTERVAL, "TRIGGERREPEATINTERVAL");
    appendFlag(out, flags, DIEP_AXES, "AXES");
    appendFlag(out, flags, DIEP_DIRECTION, "DIRECTION");
    appendFlag(out, flags, DIEP_ENVELOPE, "ENVELOPE");
    appendFlag(out, flags, DIEP_TYPESPECIFICPARAMS, "TYPESPECIFICPARAMS");
    appendFlag(out, flags, DIEP_STARTDELAY, "STARTDELAY");
    appendFlag(out, flags, DIEP_START, "START");
    appendFlag(out, flags, DIEP_NORESTART, "NORESTART");
    appendFlag(out, flags, DIEP_NODOWNLOAD, "NODOWNLOAD");
    return out.empty() ? "none" : out;
}

std::string describeEffect(const GUID& effectType, LPCDIEFFECT effect, const DWORD flags) {
    if (effect == nullptr) {
        return "effect=null";
    }
    // Also called on parameters a call rejected: nothing past dwSize is trusted unless the size is a real one.
    if (effect->dwSize != sizeof(DIEFFECT) && effect->dwSize != sizeof(DIEFFECT_DX5)) {
        return format("invalid parameters (dwSize=%lu)", effect->dwSize);
    }
    std::string out = "[" + describeEffectFlags(flags) + "]";
    if ((flags & DIEP_DURATION) != 0) {
        out += effect->dwDuration == INFINITE ? " duration=INFINITE" : format(" duration=%luus", effect->dwDuration);
    }
    if ((flags & DIEP_GAIN) != 0) {
        out += format(" gain=%lu", effect->dwGain);
    }
    if ((flags & DIEP_SAMPLEPERIOD) != 0) {
        out += format(" samplePeriod=%lu", effect->dwSamplePeriod);
    }
    if ((flags & DIEP_TRIGGERBUTTON) != 0) {
        out += effect->dwTriggerButton == DIEB_NOTRIGGER ? " trigger=none" : format(" trigger=0x%lx", effect->dwTriggerButton);
    }
    if ((flags & (DIEP_AXES | DIEP_DIRECTION)) != 0) {
        out += format(" coords=%s%s axes=%lu [",
                      (effect->dwFlags & DIEFF_POLAR) != 0 ? "POLAR" : (effect->dwFlags & DIEFF_SPHERICAL) != 0 ? "SPHERICAL" : "CARTESIAN",
                      (effect->dwFlags & DIEFF_OBJECTOFFSETS) != 0 ? "/OFFSETS" : "/IDS", effect->cAxes);
        // Each array is only read when its flag says the game filled it in (an unused pointer may be garbage).
        const DWORD* const axes = (flags & DIEP_AXES) != 0 ? effect->rgdwAxes : nullptr;
        const LONG* const directions = (flags & DIEP_DIRECTION) != 0 ? effect->rglDirection : nullptr;
        for (DWORD i = 0; i < effect->cAxes && i < kMaxAxes; ++i) {
            const DWORD axis = axes != nullptr ? axes[i] : 0;
            const LONG direction = directions != nullptr ? directions[i] : 0;
            out += format("%s0x%lx dir=%ld", i == 0 ? "" : ", ", axis, direction);
        }
        out += "]";
    }
    if ((flags & DIEP_ENVELOPE) != 0 && effect->lpEnvelope != nullptr) {
        const DIENVELOPE& envelope = *effect->lpEnvelope;
        out += format(" envelope{attack=%lu/%luus fade=%lu/%luus}", envelope.dwAttackLevel, envelope.dwAttackTime,
                      envelope.dwFadeLevel, envelope.dwFadeTime);
    }
    if ((flags & DIEP_STARTDELAY) != 0 && effect->dwSize >= sizeof(DIEFFECT)) {
        out += format(" startDelay=%luus", effect->dwStartDelay);
    }
    if ((flags & DIEP_TYPESPECIFICPARAMS) != 0) {
        out += describeTypeSpecific(effectType, *effect);
    }
    return out;
}

}  // namespace wheelbri::shim
