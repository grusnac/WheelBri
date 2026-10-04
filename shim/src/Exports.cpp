// Exported entry points of the proxy dinput8.dll. Names are mapped in dinput8.def so they do not
// clash with the declarations in <dinput.h> and <combaseapi.h>.

#include "ProxyDirectInput8.hpp"
#include "Guard.hpp"
#include "RealDInput.hpp"
#include "ShimLogging.hpp"

namespace {

using namespace wheelbri::shim;

const wheelbri::logging::Logger& log() {
    static const wheelbri::logging::Logger instance = logger("shim");
    return instance;
}

// Every export can be the first call into the DLL; none may run without the real DLL.
const RealDInput* requireReal(const char* caller) {
    const RealDInput* const real = realDInput();
    if (real == nullptr) {
        WE_LOG_CRITICAL(log(), "{}: real dinput8.dll unavailable, returning DIERR_NOTINITIALIZED", caller);
    }
    return real;
}

}  // namespace

using wheelbri::shim::guarded;

extern "C" {

HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE instance, DWORD version, REFIID riid, LPVOID* out,
                                        LPUNKNOWN outer) {
    return guarded("Proxy_DirectInput8Create", [&]() -> HRESULT {
        WE_LOG_INFO(log(), "DirectInput8Create(version 0x{:x}, {}, outer={})", version, describeGuid(riid),
                    static_cast<void*>(outer));
        const RealDInput* const real = requireReal("DirectInput8Create");
        if (real == nullptr) {
            return DIERR_NOTINITIALIZED;
        }
        const HRESULT result = real->directInput8Create(instance, version, riid, out, outer);
        if (FAILED(result) || out == nullptr || *out == nullptr) {
            WE_LOG_ERROR(log(), "real DirectInput8Create -> {}", describeHresult(result));
            return result;
        }
        if (outer != nullptr) {
            // COM aggregation: the object belongs to the caller's outer object and must not be wrapped.
            WE_LOG_WARN(log(), "aggregated DirectInput object requested, passing it through unwrapped");
        } else if (riid == IID_IDirectInput8W || riid == IID_IDirectInput8A) {
            // The game gets the wrapper or nothing: if wrapping fails, the real object is released and the
            // exception goes on to guarded().
            auto* const realObject = static_cast<IUnknown*>(*out);
            *out = nullptr;
            try {
                if (riid == IID_IDirectInput8W) {
                    *out = static_cast<IDirectInput8W*>(
                        new ProxyDirectInput8<WideTraits>(static_cast<IDirectInput8W*>(realObject)));
                } else {
                    *out = static_cast<IDirectInput8A*>(
                        new ProxyDirectInput8<AnsiTraits>(static_cast<IDirectInput8A*>(realObject)));
                }
            } catch (...) {
                realObject->Release();
                throw;
            }
        } else {
            WE_LOG_WARN(log(), "unexpected interface {} requested, passing it through unwrapped", describeGuid(riid));
        }
        WE_LOG_DEBUG(log(), "DirectInput8Create -> {}", describeHresult(result));
        return result;
    });
}

HRESULT WINAPI Proxy_DllCanUnloadNow() {
    return guarded("Proxy_DllCanUnloadNow", [&]() -> HRESULT {
        const RealDInput* const real = requireReal("DllCanUnloadNow");
        return real != nullptr && real->dllCanUnloadNow != nullptr ? real->dllCanUnloadNow() : S_FALSE;
    });
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    return guarded("Proxy_DllGetClassObject", [&]() -> HRESULT {
        // COM activation (CoCreateInstance(CLSID_DirectInput8)) normally loads the registered system DLL,
        // not this one; if a game gets here, its objects are not proxied. Worth knowing in Phase 0.
        WE_LOG_WARN(log(), "DllGetClassObject({}, {}) - objects created this way are NOT proxied", describeGuid(clsid),
                    describeGuid(riid));
        const RealDInput* const real = requireReal("DllGetClassObject");
        if (real == nullptr || real->dllGetClassObject == nullptr) {
            return CLASS_E_CLASSNOTAVAILABLE;
        }
        return real->dllGetClassObject(clsid, riid, out);
    });
}

HRESULT WINAPI Proxy_DllRegisterServer() {
    return guarded("Proxy_DllRegisterServer", [&]() -> HRESULT {
        WE_LOG_WARN(log(), "DllRegisterServer called on the proxy");
        const RealDInput* const real = requireReal("DllRegisterServer");
        return real != nullptr && real->dllRegisterServer != nullptr ? real->dllRegisterServer() : E_FAIL;
    });
}

HRESULT WINAPI Proxy_DllUnregisterServer() {
    return guarded("Proxy_DllUnregisterServer", [&]() -> HRESULT {
        WE_LOG_WARN(log(), "DllUnregisterServer called on the proxy");
        const RealDInput* const real = requireReal("DllUnregisterServer");
        return real != nullptr && real->dllUnregisterServer != nullptr ? real->dllUnregisterServer() : E_FAIL;
    });
}

LPCDIDATAFORMAT WINAPI Proxy_GetdfDIJoystick() {
    return wheelbri::shim::guardedOr<LPCDIDATAFORMAT>(nullptr, "Proxy_GetdfDIJoystick", [&]() -> LPCDIDATAFORMAT {
        const RealDInput* const real = requireReal("GetdfDIJoystick");
        return real != nullptr && real->getdfDIJoystick != nullptr ? real->getdfDIJoystick() : nullptr;
    });
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    // Keep DllMain trivial: no logging, no LoadLibrary, no threads (loader lock). Work starts on the first export call.
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(instance);
            break;
        case DLL_PROCESS_DETACH:
            onDllDetach(reserved != nullptr);
            break;
        default:
            break;
    }
    return TRUE;
}

}  // extern "C"
