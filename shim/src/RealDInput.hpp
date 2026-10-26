#pragma once

#include "DiFormat.hpp"

namespace wheelbri::shim {

// Entry points of a real dinput8.dll, loaded lazily (never from DllMain): Windows' own, or another mod's that the app
// chained in front of it ([shim] chain, ShimConfig.hpp).
struct RealDInput {
    using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    using DllCanUnloadNowFn = HRESULT(WINAPI*)();
    using DllGetClassObjectFn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    using DllRegisterServerFn = HRESULT(WINAPI*)();
    using GetdfDIJoystickFn = LPCDIDATAFORMAT(WINAPI*)();

    HMODULE module = nullptr;
    DirectInput8CreateFn directInput8Create = nullptr;
    DllCanUnloadNowFn dllCanUnloadNow = nullptr;
    DllGetClassObjectFn dllGetClassObject = nullptr;
    DllRegisterServerFn dllRegisterServer = nullptr;
    DllRegisterServerFn dllUnregisterServer = nullptr;
    GetdfDIJoystickFn getdfDIJoystick = nullptr;
};

// Where DirectInput goes: the chained mod's DLL when wheelbri-shim.ini names one that loads, else Windows' own (a chain
// that cannot be used is logged and Windows' own used instead). nullptr (and logged why) when neither can be loaded.
[[nodiscard]] const RealDInput* realDInput();
// Windows' own dinput8.dll, never a chained one.
[[nodiscard]] const RealDInput* systemDInput();

// Marks a call from the shim into a real DLL, on this thread. A chained mod that loads "dinput8.dll" by name gets this
// shim back (Windows hands out the module already loaded under that name) and calls into it from inside our call:
// that call must go to Windows' own (systemDInput) and pass its objects through unwrapped, else it loops for ever or
// the wheel is wrapped twice.
class RealCall {
public:
    RealCall() { ++depth_; }
    ~RealCall() { --depth_; }
    RealCall(const RealCall&) = delete;
    RealCall& operator=(const RealCall&) = delete;
    [[nodiscard]] static bool inside() { return depth_ > 0; }

private:
    inline static thread_local int depth_ = 0;
};

}  // namespace wheelbri::shim
