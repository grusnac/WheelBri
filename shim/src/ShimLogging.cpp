#include "ShimLogging.hpp"

#include "Guard.hpp"

#include "DiFormat.hpp"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <string>

namespace wheelbri::shim {

namespace {

std::once_flag gInitOnce;
// Set once logging::init has run. onDllDetach checks it so an unload before the first export call never
// creates a logger inside DllMain.
std::atomic<bool> gInitialised{false};

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

bool isAscii(const std::wstring& text) {
    for (const wchar_t c : text) {
        if (c > 0x7F) {
            return false;
        }
    }
    return true;
}

// The folder (with a trailing separator) as a pure ASCII name, or nullopt.
//
// The log file's name goes through two readers that disagree on non-ASCII bytes: logging::init hands it to
// std::filesystem (libstdc++ on Windows reads narrow names as UTF-8) and spdlog, built with narrow file names,
// opens it with fopen (the ANSI code page). So a user folder with an accent (C:\Users\Jose with an acute e)
// gave no log: in UTF-8 fopen looks for a folder that does not exist, and an ANSI name fails or means something
// else in std::filesystem. Only ASCII reads the same in both, so the shim uses the folder's 8.3 short name
// (GetShortPathNameW, kernel32 only: no new DLL dependency, and the spdlog build is left alone). The short name
// exists only for an existing folder, so the folder is created here first. On a volume with 8.3 names turned
// off the long name comes back, which is not ASCII: the caller then falls back to UTF-8 as before (no log, but
// nothing worse; with the system code page set to UTF-8 that fallback even works).
std::optional<std::string> asciiFolder(std::wstring folder) {
    while (!folder.empty() && (folder.back() == L'\\' || folder.back() == L'/')) {
        folder.pop_back();
    }
    if (folder.size() < 3) {
        return std::nullopt;
    }
    // Create each missing level (only wheelbri\logs is ever new). A failure shows up as no short name below.
    for (std::size_t slash = folder.find_first_of(L"\\/", 3);; slash = folder.find_first_of(L"\\/", slash + 1)) {
        CreateDirectoryW(folder.substr(0, slash).c_str(), nullptr);
        if (slash == std::wstring::npos) {
            break;
        }
    }
    const DWORD needed = GetShortPathNameW(folder.c_str(), nullptr, 0);
    if (needed == 0) {
        return std::nullopt;
    }
    std::wstring shortName(needed, L'\0');
    const DWORD written = GetShortPathNameW(folder.c_str(), shortName.data(), needed);
    if (written == 0 || written >= needed) {
        return std::nullopt;
    }
    shortName.resize(written);
    if (!isAscii(shortName)) {
        return std::nullopt;
    }
    return toUtf8(shortName.c_str()) + "\\";   // ASCII: the same bytes in every encoding
}

// Next to the service's logs and telemetry (%LOCALAPPDATA%\wheelbri\logs), so every file is in one folder
// that the service's window shows and its [retention] settings clean up. One file per run of the game (and day):
// wheelbri-shim-YYYY-MM-DD-NN.log (LogFiles.hpp). Falls back to the DLL's folder if LOCALAPPDATA is not set.
std::string logFilePath() {
    wchar_t localAppData[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    const std::wstring folder = length > 0 && length < MAX_PATH ? std::wstring(localAppData) + L"\\wheelbri\\logs\\"
                                                                : dllDirectory();
    if (!isAscii(folder)) {
        if (const std::optional<std::string> narrow = asciiFolder(folder)) {
            return *narrow + "wheelbri-shim.log";
        }
    }
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
        gInitialised = true;

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

void onDllDetach(const bool processTerminating) noexcept {
    // Never logged: nothing to flush, and creating a logger here would take locks and open files in DllMain.
    if (processTerminating || !gInitialised.load()) {
        return;
    }
    try {   // nothing may throw out of DllMain
        const logging::Logger log = logging::get("shim");
        WE_LOG_INFO(log, "proxy unloaded by FreeLibrary");
        logging::shutdown();
    } catch (...) {
    }
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
