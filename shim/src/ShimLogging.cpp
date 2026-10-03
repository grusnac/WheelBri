#include "ShimLogging.hpp"

#include "Guard.hpp"

#include "DiFormat.hpp"

#include <windows.h>

#include <mutex>
#include <string>

namespace wheelbri::shim {

namespace {

std::once_flag gInitOnce;

std::wstring modulePath(HMODULE module) {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    return std::wstring(path, length);
}

HMODULE thisModule() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&thisModule), &self);
    return self;
}

}  // namespace

std::wstring dllDirectory() {
    const std::wstring path = modulePath(thisModule());
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
}

namespace {

// Next to the service's logs and telemetry (%LOCALAPPDATA%\wheelbri\logs), so every file is in one folder
// that the service's window shows and its [retention] settings clean up. One file per day:
// wheelbri-shim-YYYY-MM-DD.log. Falls back to the DLL's folder if LOCALAPPDATA is not set.
std::string logFilePath() {
    wchar_t localAppData[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    const std::wstring folder = length > 0 && length < MAX_PATH ? std::wstring(localAppData) + L"\\wheelbri\\logs\\"
                                                                : dllDirectory();
    return toUtf8((folder + L"wheelbri-shim.log").c_str());
}

}  // namespace

void ensureLoggingInitialised() {
    std::call_once(gInitOnce, [] {
        logging::LogConfig config;
        config.level = "debug";
        config.console = false;
        config.flushLevel = "trace";
        config.periodicFlush = false;
        logging::init(config, logFilePath());

        const logging::Logger log = logging::get("shim");
        WE_LOG_INFO(log, "wheelbri dinput8 proxy loaded into '{}' (pid {})", toUtf8(modulePath(nullptr).c_str()),
                    GetCurrentProcessId());
        WE_LOG_INFO(log, "proxy dll: '{}'", toUtf8(modulePath(thisModule()).c_str()));
#if defined(_WIN64)
        WE_LOG_INFO(log, "architecture: x64");
#else
        WE_LOG_INFO(log, "architecture: x86");
#endif
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr) {
            using WineVersionFn = const char*(__cdecl*)();
            const auto wineVersion = reinterpret_cast<WineVersionFn>(
                reinterpret_cast<void*>(GetProcAddress(ntdll, "wine_get_version")));
            WE_LOG_INFO(log, "running under Wine/Proton {}", wineVersion());
        }
    });
}

void onDllDetach(const bool processTerminating) {
    if (processTerminating) {
        return;
    }
    const logging::Logger log = logging::get("shim");
    WE_LOG_INFO(log, "proxy unloaded by FreeLibrary");
    logging::shutdown();
}

logging::Logger logger(const char* component) {
    ensureLoggingInitialised();
    return logging::get(component);
}

// Guard.hpp: an exception stopped at the boundary to the game.
void noteEscapedException(const char* where, const char* what) noexcept {
    try {
        static const logging::Logger instance = logger("shim");
        static logging::RateLimiter limiter(std::chrono::seconds(5));
        WE_LOG_ERROR_LIMITED(instance, limiter, "{}: stopped on an error, the game gets an error code: {}", where,
                             what != nullptr ? what : "?");
    } catch (...) {
        // Logging itself failed (out of memory?): nothing more can be done safely.
    }
}

}  // namespace wheelbri::shim
