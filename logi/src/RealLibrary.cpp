#include "RealLibrary.hpp"

#include "LogFolder.hpp"
#include "LogiLogging.hpp"

#include "wheelbri/FileText.hpp"
#include "wheelbri/IniDocument.hpp"
#include "wheelbri/LogiSdk.hpp"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace wheelbri::logi {

namespace {

const logging::Logger& log() {
    static const logging::Logger instance = logger("logi");
    return instance;
}

// x64 only (Exports.cpp): G HUB ships a _x86 twin and the old software an SDKs\\32 one, for 32-bit games.
constexpr const wchar_t* kGhubName = L"logi_steering_wheel_x64.dll";
constexpr const wchar_t* kLgsFolder = L"\\Logitech\\Gaming Software\\SDKs\\";

std::wstring environment(const wchar_t* name) {
    wchar_t value[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(name, value, MAX_PATH);
    return length > 0 && length < MAX_PATH ? std::wstring(value, length) : std::wstring();
}

// The 64-bit Program Files folder, also from a 32-bit process (G HUB installs both DLLs there).
std::wstring programFiles() {
    std::wstring folder = environment(L"ProgramW6432");
    if (folder.empty()) folder = environment(L"ProgramFiles");
    return folder;
}

std::wstring lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](const wchar_t c) { return std::towlower(c); });
    return text;
}

std::wstring fullPath(const std::wstring& path) {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetFullPathNameW(path.c_str(), MAX_PATH, buffer, nullptr);
    return length > 0 && length < MAX_PATH ? lower(std::wstring(buffer, length)) : lower(path);
}

// [logi] real_library in wheelbri-logi.ini next to this DLL, or empty.
std::wstring configuredLibrary() {
    const std::filesystem::path file = std::filesystem::path(gamelog::dllDirectory()) / L"wheelbri-logi.ini";
    const std::optional<std::string> text = readFileText(file);
    if (!text) return {};
    const IniDocument::ParseResult ini = IniDocument::parse(*text);
    for (const std::string& error : ini.errors) {
        WE_LOG_WARN(log(), "wheelbri-logi.ini: {}", error);
    }
    const std::optional<std::string> value = ini.document.get("logi", "real_library");
    if (!value || value->empty()) return {};
    WE_LOG_INFO(log(), "wheelbri-logi.ini names the real library: {}", *value);
    return std::filesystem::path(std::u8string(value->begin(), value->end())).wstring();
}

// G HUB's newest wheel_sdk version folder holding the DLL, or empty.
std::wstring ghubLibrary() {
    const std::wstring root = programFiles() + L"\\Logi\\wheel_sdk\\";
    std::vector<std::string> versions;
    WIN32_FIND_DATAW found{};
    const HANDLE search = FindFirstFileW((root + L"*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return {};
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            const std::wstring name(found.cFileName);
            if (std::all_of(name.begin(), name.end(), [](const wchar_t c) { return c < 0x80; })) {
                versions.emplace_back(name.begin(), name.end());
            }
        }
    } while (FindNextFileW(search, &found) != FALSE);
    FindClose(search);
    const std::string newest = newestVersionFolder(versions);
    if (newest.empty()) return {};
    return root + std::wstring(newest.begin(), newest.end()) + L"\\" + kGhubName;
}

struct Real {
    HMODULE module = nullptr;
    std::vector<void*> functions;
};

Real load() {
    Real real;
    real.functions.assign(functions().size(), nullptr);
    const std::wstring self = fullPath(gamelog::modulePath(gamelog::thisModule()));

    std::vector<std::wstring> candidates;
    if (std::wstring configured = configuredLibrary(); !configured.empty()) candidates.push_back(configured);
    if (std::wstring ghub = ghubLibrary(); !ghub.empty()) candidates.push_back(ghub);
    candidates.push_back(programFiles() + kLgsFolder + L"LogitechSteeringWheel.dll");

    for (const std::wstring& candidate : candidates) {
        const std::string name = gamelog::utf8(candidate);
        if (fullPath(candidate) == self) {
            WE_LOG_WARN(log(), "skipping {}: that is this DLL", name);
            continue;
        }
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) {
            WE_LOG_INFO(log(), "no real library at {}", name);
            continue;
        }
        // Its own folder first for its dependencies (G HUB's needs the Visual C++ runtime, found in System32).
        real.module = LoadLibraryExW(candidate.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (real.module == nullptr) {
            WE_LOG_ERROR(log(), "loading the real library {} failed (error {})", name, GetLastError());
            continue;
        }
        WE_LOG_INFO(log(), "real library: {} (every call goes on to it)", name);
        std::size_t missing = 0;
        for (std::size_t i = 0; i < functions().size(); ++i) {
            const std::string symbol(functions()[i].name);
            real.functions[i] = reinterpret_cast<void*>(GetProcAddress(real.module, symbol.c_str()));
            if (real.functions[i] == nullptr) {
                ++missing;
                WE_LOG_WARN(log(), "the real library has no {}: the game gets 0 (false) from it", symbol);
            }
        }
        WE_LOG_INFO(log(), "{} of {} functions found in the real library", functions().size() - missing,
                    functions().size());
        return real;
    }
    WE_LOG_ERROR(log(), "no real Logitech wheel library found: every call answers 0 and the game sees no wheel "
                        "(install G HUB, or name one in wheelbri-logi.ini next to this DLL)");
    return real;
}

}  // namespace

void* realFunction(const std::size_t index) {
    static std::once_flag once;
    static Real real;
    std::call_once(once, [] { real = load(); });
    return index < real.functions.size() ? real.functions[index] : nullptr;
}

}  // namespace wheelbri::logi
