#include "ServiceLogging.hpp"

#include "Platform.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#endif

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#ifndef WHEELBRI_VERSION
#define WHEELBRI_VERSION "dev"
#endif

namespace wheelbri::service {

namespace {

std::string envOrEmpty(const char* name) {
    const char* const value = std::getenv(name);
    return value != nullptr ? value : "";
}

}  // namespace

std::string defaultLogFile() {
    std::filesystem::path directory;
#if defined(_WIN32)
    directory = std::filesystem::path(envOrEmpty("LOCALAPPDATA")) / "wheelbri" / "logs";
#elif defined(__APPLE__)
    directory = std::filesystem::path(envOrEmpty("HOME")) / "Library" / "Logs" / "wheelbri";
#else
    const std::string state = envOrEmpty("XDG_STATE_HOME");
    directory = state.empty() ? std::filesystem::path(envOrEmpty("HOME")) / ".local" / "state" / "wheelbri"
                              : std::filesystem::path(state) / "wheelbri";
#endif
    return (directory / "wheelbri-service.log").string();
}

namespace {

// %LOCALAPPDATA%\wheelbri, ~/.config/wheelbri (XDG) or ~/Library/Application Support/wheelbri.
std::filesystem::path userSettingsFolder() {
#if defined(_WIN32)
    return std::filesystem::path(envOrEmpty("LOCALAPPDATA")) / "wheelbri";
#elif defined(__APPLE__)
    return std::filesystem::path(envOrEmpty("HOME")) / "Library" / "Application Support" / "wheelbri";
#else
    const std::string config = envOrEmpty("XDG_CONFIG_HOME");
    return config.empty() ? std::filesystem::path(envOrEmpty("HOME")) / ".config" / "wheelbri"
                          : std::filesystem::path(config) / "wheelbri";
#endif
}

}  // namespace

std::filesystem::path deviceLabelsFolder() { return userSettingsFolder() / "devices"; }

std::filesystem::path userConfigFolder() { return userSettingsFolder() / "configs"; }

std::filesystem::path rigsFolder() { return userSettingsFolder() / "rigs"; }

std::filesystem::path appStateFile() { return userSettingsFolder() / "app.ini"; }

std::filesystem::path shimGamesFile() { return userSettingsFolder() / "shim-games.txt"; }

namespace {

constexpr const char* kOldName = "wheelemu";   // the project's name before the rename
constexpr const char* kName = "wheelbri";

// What moveFoldersFromOldName did, logged once logging runs.
std::vector<std::string>& migrationNotes() {
    static std::vector<std::string> notes;
    return notes;
}

// `folder` with its "wheelbri" component named the old way (the folder the old version used).
std::filesystem::path oldNameOf(const std::filesystem::path& folder) {
    std::filesystem::path result;
    for (const std::filesystem::path& part : folder) {
        result /= part == kName ? std::filesystem::path(kOldName) : part;
    }
    return result;
}

// Moves everything in `from` into `to` (entries already there stay, and the old one with them), then removes `from`
// if it ended up empty.
void moveInto(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    if (!std::filesystem::is_directory(from, ec)) return;
    if (!std::filesystem::exists(to, ec)) {
        std::filesystem::create_directories(to.parent_path(), ec);
        std::filesystem::rename(from, to, ec);
        if (!ec) {
            migrationNotes().push_back("moved " + from.string() + " to " + to.string() + " (the project's old name)");
            return;
        }
        std::filesystem::create_directories(to, ec);
    }
    int moved = 0;
    int left = 0;
    for (const auto& entry : std::filesystem::directory_iterator(from, ec)) {
        const std::filesystem::path target = to / entry.path().filename();
        std::error_code entryError;
        if (std::filesystem::exists(target, entryError)) {
            if (entry.is_directory(entryError) && std::filesystem::is_directory(target, entryError)) {
                moveInto(entry.path(), target);   // e.g. logs\ next to a new logs\: their files merge
            } else {
                ++left;
            }
            continue;
        }
        std::filesystem::rename(entry.path(), target, entryError);
        if (entryError) {
            ++left;
        } else {
            ++moved;
        }
    }
    std::filesystem::remove(from, ec);   // only when empty
    migrationNotes().push_back("moved " + std::to_string(moved) + " entries from " + from.string() + " to " +
                               to.string() + (left > 0 ? "; " + std::to_string(left) + " stay there (already in the "
                                                                                      "new folder, or in use)"
                                                       : ""));
}

// app.ini names the picked config by its full path, which may be in the old folder.
void rewriteOldPaths(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return;
    std::stringstream buffer;
    buffer << in.rdbuf();
    in.close();
    std::string text = buffer.str();
    bool changed = false;
    for (const char separator : {'\\', '/'}) {
        const std::string from = std::string(1, separator) + kOldName + separator;
        const std::string to = std::string(1, separator) + kName + separator;
        for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
            text.replace(at, from.size(), to);
            changed = true;
        }
    }
    if (changed) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
        migrationNotes().push_back("app.ini: paths to the old folder now point to the new one");
    }
}

}  // namespace

void moveFoldersFromOldName() {
    const std::filesystem::path settings = userSettingsFolder();
    const std::filesystem::path logs = std::filesystem::path(defaultLogFile()).parent_path();
    for (const std::filesystem::path& folder : {settings, logs}) {
        const std::filesystem::path old = oldNameOf(folder);
        if (old != folder) moveInto(old, folder);
    }
    rewriteOldPaths(appStateFile());
}

#ifdef _WIN32
namespace {

// LocalService (the drivers' host) may create, write and delete files in `folder` and everything under it.
DWORD grantLocalService(const std::filesystem::path& folder) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    DWORD status = GetNamedSecurityInfoW(folder.wstring().c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                         nullptr, &dacl, nullptr, &descriptor);
    if (status != ERROR_SUCCESS) return status;
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD sidSize = sizeof(sid);
    PACL updated = nullptr;
    if (CreateWellKnownSid(WinLocalServiceSid, nullptr, sid, &sidSize)) {
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE;
        access.grfAccessMode = GRANT_ACCESS;
        access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
        status = SetEntriesInAclW(1, &access, dacl, &updated);
        if (status == ERROR_SUCCESS) {
            std::wstring path = folder.wstring();
            status = SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                           updated, nullptr);
        }
    } else {
        status = GetLastError();
    }
    if (updated != nullptr) LocalFree(updated);
    LocalFree(descriptor);
    return status;
}

}  // namespace

void shareLogFolder(const std::filesystem::path& folder) {
    const logging::Logger logger = logging::get("service");
    if (!platform::isElevated() || folder.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::filesystem::path absolute = std::filesystem::absolute(folder, ec);
    if (const DWORD status = grantLocalService(absolute); status != ERROR_SUCCESS) {
        WE_LOG_WARN(logger, "logs: cannot let the drivers write in {} (error {}): they keep writing to "
                            "C:\\ProgramData\\wheelbri\\logs",
                    absolute.string(), status);
        return;
    }
    HKEY key = nullptr;
    LSTATUS written = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\wheelbri", 0, nullptr, REG_OPTION_NON_VOLATILE,
                                      KEY_SET_VALUE, nullptr, &key, nullptr);
    if (written == ERROR_SUCCESS) {
        const std::wstring value = absolute.wstring();
        written = RegSetValueExW(key, L"LogFolder", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                 static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
    if (written != ERROR_SUCCESS) {
        WE_LOG_WARN(logger, "logs: cannot write HKLM\\SOFTWARE\\wheelbri LogFolder (error {}): the drivers keep "
                            "writing to C:\\ProgramData\\wheelbri\\logs",
                    written);
        return;
    }
    // What the drivers and installers wrote before: into the one folder.
    const std::filesystem::path programData = std::filesystem::path(envOrEmpty("ProgramData")) / "wheelbri";
    const std::filesystem::path oldLogs = programData / "logs";
    int moved = 0;
    if (!envOrEmpty("ProgramData").empty() && std::filesystem::is_directory(oldLogs, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(oldLogs, ec)) {
            std::error_code entryError;
            const std::filesystem::path target = absolute / entry.path().filename();
            if (!entry.is_regular_file(entryError)) continue;
            if (std::filesystem::exists(target, entryError)) {
                // Same day's file on both sides: append the old part to the new one.
                std::ifstream in(entry.path(), std::ios::binary);
                std::ofstream out(target, std::ios::binary | std::ios::app);
                if (!in || !out) continue;
                out << in.rdbuf();
                in.close();
                out.close();
                std::filesystem::remove(entry.path(), entryError);
            } else {
                std::filesystem::rename(entry.path(), target, entryError);
                if (entryError) {   // another volume
                    std::filesystem::copy_file(entry.path(), target, entryError);
                    if (!entryError) std::filesystem::remove(entry.path(), entryError);
                }
            }
            if (!entryError) ++moved;
        }
        std::filesystem::remove(oldLogs, ec);       // only when empty
        std::filesystem::remove(programData, ec);   // only when empty
    }
    WE_LOG_INFO(logger, "logs: every log goes to {} (the drivers too){}", absolute.string(),
                moved > 0 ? "; moved " + std::to_string(moved) + " files there from " + oldLogs.string() : "");
}
#else
void shareLogFolder(const std::filesystem::path&) {}
#endif

void logStartupBanner(const std::string& configPath) {
    const logging::Logger logger = logging::get("service");
    WE_LOG_INFO(logger, "wheelbri-service {} starting on {}", WHEELBRI_VERSION, platform::name());
    WE_LOG_INFO(logger, "config: {}", configPath.empty() ? std::string("(built-in defaults)") : configPath);
    for (const std::string& note : migrationNotes()) {
        WE_LOG_INFO(logger, "rename: {}", note);
    }
    migrationNotes().clear();
    const std::string envLevels = envOrEmpty("SPDLOG_LEVEL");
    if (!envLevels.empty()) {
        WE_LOG_INFO(logger, "log levels overridden by SPDLOG_LEVEL='{}'", envLevels);
    }
}

}  // namespace wheelbri::service
