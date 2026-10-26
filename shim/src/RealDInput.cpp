#include "RealDInput.hpp"

#include "ShimConfig.hpp"
#include "ShimLogging.hpp"

#include "wheelbri/ShimGames.hpp"

#include <mutex>
#include <string>

namespace wheelbri::shim {

namespace {

template <typename Fn>
Fn resolve(HMODULE module, const char* name, const std::wstring& path, const logging::Logger& log) {
    const FARPROC proc = GetProcAddress(module, name);
    if (proc == nullptr) {
        WE_LOG_WARN(log, "'{}' does not export {} (error {})", toUtf8(path.c_str()), name, GetLastError());
    }
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(proc));
}

HMODULE selfModule() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&selfModule), &self);
    return self;
}

// Loads the dinput8.dll at `path` into `real`. False (and logged why) when it cannot be used.
bool load(const std::wstring& path, RealDInput& real, const logging::Logger& log) {
    const std::string name = toUtf8(path.c_str());
    const HMODULE module = LoadLibraryW(path.c_str());
    if (module == nullptr) {
        WE_LOG_CRITICAL(log, "LoadLibrary('{}') failed (error {})", name, GetLastError());
        return false;
    }
    // Under Wine a DLL override can hand back this very file: calling "the real one" would then call itself
    // until the stack overflows.
    if (module == selfModule()) {
        WE_LOG_CRITICAL(log, "'{}' is this proxy itself (a DLL override?): no real DirectInput to forward to", name);
        return false;
    }
    // A copy of WheelBri's shim under another name would read the same wheelbri-shim.ini and chain to itself.
    if (GetProcAddress(module, kShimMarkExport) != nullptr) {
        WE_LOG_CRITICAL(log, "'{}' is a WheelBri shim, not DirectInput: not forwarded to", name);
        return false;
    }
    real.module = module;
    real.directInput8Create = resolve<RealDInput::DirectInput8CreateFn>(module, "DirectInput8Create", path, log);
    real.dllCanUnloadNow = resolve<RealDInput::DllCanUnloadNowFn>(module, "DllCanUnloadNow", path, log);
    real.dllGetClassObject = resolve<RealDInput::DllGetClassObjectFn>(module, "DllGetClassObject", path, log);
    real.dllRegisterServer = resolve<RealDInput::DllRegisterServerFn>(module, "DllRegisterServer", path, log);
    real.dllUnregisterServer = resolve<RealDInput::DllRegisterServerFn>(module, "DllUnregisterServer", path, log);
    real.getdfDIJoystick = resolve<RealDInput::GetdfDIJoystickFn>(module, "GetdfDIJoystick", path, log);
    if (real.directInput8Create == nullptr) return false;
    WE_LOG_INFO(log, "loaded real dinput8.dll from '{}'", name);
    return true;
}

}  // namespace

const RealDInput* systemDInput() {
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
        loaded = load(std::wstring(systemDir, length) + L"\\dinput8.dll", real, log);
        if (!loaded) WE_LOG_CRITICAL(log, "the game will get no DirectInput");
    });
    return loaded ? &real : nullptr;
}

const RealDInput* realDInput() {
    static RealDInput chained;
    static const RealDInput* real = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        const std::string& chain = shimConfig().chain;   // validChainName: a bare ASCII file name
        if (!chain.empty()) {
            const logging::Logger log = logger("shim.loader");
            const std::wstring path = dllDirectory() + std::wstring(chain.begin(), chain.end());
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                WE_LOG_ERROR(log, "chain = {}: no such file next to the shim; DirectInput goes to Windows' own", chain);
            } else if (load(path, chained, log)) {
                WE_LOG_INFO(log, "chained: DirectInput goes to '{}' (another mod's dinput8.dll), which hands it on",
                            chain);
                real = &chained;
                return;
            } else {
                WE_LOG_ERROR(log, "chain = {} cannot be used; DirectInput goes to Windows' own", chain);
            }
        }
        real = systemDInput();
    });
    return real;
}

}  // namespace wheelbri::shim
