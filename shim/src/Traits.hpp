#pragma once

#include "DiFormat.hpp"

namespace wheelbri::shim {

// DirectInput exists in ANSI (A) and wide (W) flavours that differ only in string and struct types.
// The proxies are written once as templates over these traits.

struct AnsiTraits {
    using DirectInput = IDirectInput8A;
    using Device = IDirectInputDevice8A;
    using DeviceInstance = DIDEVICEINSTANCEA;
    using DeviceObjectInstance = DIDEVICEOBJECTINSTANCEA;
    using DeviceObjectInstanceDx3 = DIDEVICEOBJECTINSTANCE_DX3A;   // the shorter struct older games still pass
    using EffectInfo = DIEFFECTINFOA;
    using EnumDevicesCallback = LPDIENUMDEVICESCALLBACKA;
    using EnumObjectsCallback = LPDIENUMDEVICEOBJECTSCALLBACKA;
    using EnumEffectsCallback = LPDIENUMEFFECTSCALLBACKA;
    using EnumBySemanticsCallback = LPDIENUMDEVICESBYSEMANTICSCBA;
    using ActionFormat = LPDIACTIONFORMATA;
    using ConfigureParams = LPDICONFIGUREDEVICESPARAMSA;
    using ImageInfoHeader = LPDIDEVICEIMAGEINFOHEADERA;
    using String = LPCSTR;

    static constexpr const char* kSuffix = "A";
    static const IID& directInputIid() { return IID_IDirectInput8A; }
    static const IID& deviceIid() { return IID_IDirectInputDevice8A; }
};

struct WideTraits {
    using DirectInput = IDirectInput8W;
    using Device = IDirectInputDevice8W;
    using DeviceInstance = DIDEVICEINSTANCEW;
    using DeviceObjectInstance = DIDEVICEOBJECTINSTANCEW;
    using DeviceObjectInstanceDx3 = DIDEVICEOBJECTINSTANCE_DX3W;
    using EffectInfo = DIEFFECTINFOW;
    using EnumDevicesCallback = LPDIENUMDEVICESCALLBACKW;
    using EnumObjectsCallback = LPDIENUMDEVICEOBJECTSCALLBACKW;
    using EnumEffectsCallback = LPDIENUMEFFECTSCALLBACKW;
    using EnumBySemanticsCallback = LPDIENUMDEVICESBYSEMANTICSCBW;
    using ActionFormat = LPDIACTIONFORMATW;
    using ConfigureParams = LPDICONFIGUREDEVICESPARAMSW;
    using ImageInfoHeader = LPDIDEVICEIMAGEINFOHEADERW;
    using String = LPCWSTR;

    static constexpr const char* kSuffix = "W";
    static const IID& directInputIid() { return IID_IDirectInput8W; }
    static const IID& deviceIid() { return IID_IDirectInputDevice8W; }
};

}  // namespace wheelbri::shim
