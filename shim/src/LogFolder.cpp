// SPDX-License-Identifier: GPL-3.0-or-later
#include "LogFolder.hpp"

#include "wheelbri/ConfigLibrary.hpp"

#include <filesystem>
#include <optional>

namespace wheelbri::gamelog {

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

std::wstring dllDirectory() {
    const std::wstring path = modulePath(thisModule());
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
}

std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                         nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
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
// else in std::filesystem. Only ASCII reads the same in both, so the folder's 8.3 short name is used
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
    return utf8(shortName) + "\\";   // ASCII: the same bytes in every encoding
}

std::wstring environment(const wchar_t* name) {
    wchar_t value[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(name, value, MAX_PATH);
    return length > 0 && length < MAX_PATH ? std::wstring(value, length) : std::wstring();
}

// Under Wine (Proton) the game's %LOCALAPPDATA% is inside its own prefix, where neither the app nor the user looks: the
// Linux user's WheelBri folder instead, where the service on Linux keeps it ($`variable`/wheelbri, else
// ~/`fallback`/wheelbri), as the Windows path Wine gives it (Z:\home\...). Wine hands the Unix HOME over as WINEHOME.
// Empty when not under Wine or Wine cannot name the folder.
std::wstring wineUserFolder(const wchar_t* const variable, const wchar_t* const fallback) {
    using DosNameFn = WCHAR*(WINAPI*)(const char*);   // Wine's kernel32: a Unix path to a Windows one
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const auto dosName = kernel32 == nullptr ? nullptr
                                             : reinterpret_cast<DosNameFn>(reinterpret_cast<void*>(
                                                   GetProcAddress(kernel32, "wine_get_dos_file_name")));
    if (dosName == nullptr) return {};
    std::wstring folder = environment(variable);
    if (folder.empty() || folder.front() != L'/') {
        std::wstring home = environment(L"WINEHOME");
        if (home.empty()) home = environment(L"HOME");
        if (home.empty() || home.front() != L'/') return {};
        folder = home + L"/" + fallback;
    }
    folder += L"/wheelbri";
    const std::string unixPath = utf8(folder);   // Wine reads Unix names as UTF-8
    WCHAR* const dos = dosName(unixPath.c_str());
    if (dos == nullptr) return {};
    std::wstring result(dos);
    HeapFree(GetProcessHeap(), 0, dos);
    return result;
}

}  // namespace

LogLevel appLevelAtStart() {
    std::wstring folder = wineUserFolder(L"XDG_CONFIG_HOME", L".config");
    if (folder.empty()) {
        const std::wstring localAppData = environment(L"LOCALAPPDATA");
        if (localAppData.empty()) return kDefaultAppLogLevel;
        folder = localAppData + L"\\wheelbri";
    }
    const std::filesystem::path file = std::filesystem::path(folder) / L"app.ini";
    return appLogLevel(readAppSetting(file, "log_level"));
}

std::string logFilePath(const std::wstring& fileName) {
    std::wstring folder = wineUserFolder(L"XDG_STATE_HOME", L".local/state");
    if (!folder.empty()) {
        folder += L"\\";
    } else {
        const std::wstring localAppData = environment(L"LOCALAPPDATA");
        folder = !localAppData.empty() ? localAppData + L"\\wheelbri\\logs\\" : dllDirectory();
    }
    if (!isAscii(folder)) {
        if (const std::optional<std::string> narrow = asciiFolder(folder)) {
            return *narrow + utf8(fileName);
        }
    }
    return utf8(folder + fileName);
}

bool underWine(std::string& version) {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) return false;
    using WineVersionFn = const char*(__cdecl*)();
    const auto wineVersion =
        reinterpret_cast<WineVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "wine_get_version")));
    if (wineVersion == nullptr) return false;
    version = wineVersion();
    return true;
}

}  // namespace wheelbri::gamelog
