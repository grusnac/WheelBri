#pragma once

#include "DiFormat.hpp"

namespace wheelemu::shim {

// Entry points of the system's real dinput8.dll, loaded lazily (never from DllMain).
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

// Returns nullptr (and logs why) if the system DLL cannot be loaded.
[[nodiscard]] const RealDInput* realDInput();

}  // namespace wheelemu::shim
