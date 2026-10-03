#include "RealDInput.hpp"

#include "ShimLogging.hpp"

#include <mutex>
#include <string>

namespace wheelbri::shim {

namespace {

template <typename Fn>
Fn resolve(HMODULE module, const char* name, const logging::Logger& log) {
    const FARPROC proc = GetProcAddress(module, name);
    if (proc == nullptr) {
        WE_LOG_WARN(log, "real dinput8.dll does not export {} (error {})", name, GetLastError());
    }
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(proc));
}

}  // namespace

const RealDInput* realDInput() {
    static RealDInput real;
    static bool loaded = false;
    static std::once_flag once;
    std::call_once(once, [] {
        const logging::Logger log = logger("shim.loader");
        wchar_t systemDir[MAX_PATH] = {};
        const UINT length = GetSystemDirectoryW(systemDir, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) {
            WE_LOG_CRITICAL(log, "GetSystemDirectoryW failed (error {}); cannot find the real dinput8.dll",
                            GetLastError());
            return;
        }
        const std::wstring path = std::wstring(systemDir, length) + L"\\dinput8.dll";
        real.module = LoadLibraryW(path.c_str());
        if (real.module == nullptr) {
            WE_LOG_CRITICAL(log, "LoadLibrary('{}') failed (error {}); the game will get no DirectInput",
                            toUtf8(path.c_str()), GetLastError());
            return;
        }
        // Under Wine a DLL override can hand back this very file: calling "the real one" would then call itself
        // until the stack overflows.
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&realDInput), &self);
        if (real.module == self) {
            WE_LOG_CRITICAL(log, "'{}' is this proxy itself (a DLL override?): no real DirectInput to forward to",
                            toUtf8(path.c_str()));
            real.module = nullptr;
            return;
        }
        real.directInput8Create = resolve<RealDInput::DirectInput8CreateFn>(real.module, "DirectInput8Create", log);
        real.dllCanUnloadNow = resolve<RealDInput::DllCanUnloadNowFn>(real.module, "DllCanUnloadNow", log);
        real.dllGetClassObject = resolve<RealDInput::DllGetClassObjectFn>(real.module, "DllGetClassObject", log);
        real.dllRegisterServer = resolve<RealDInput::DllRegisterServerFn>(real.module, "DllRegisterServer", log);
        real.dllUnregisterServer = resolve<RealDInput::DllRegisterServerFn>(real.module, "DllUnregisterServer", log);
        real.getdfDIJoystick = resolve<RealDInput::GetdfDIJoystickFn>(real.module, "GetdfDIJoystick", log);
        loaded = real.directInput8Create != nullptr;
        WE_LOG_INFO(log, "loaded real dinput8.dll from '{}'", toUtf8(path.c_str()));
    });
    return loaded ? &real : nullptr;
}

}  // namespace wheelbri::shim
