// SPDX-License-Identifier: GPL-3.0-or-later
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

#include "wheelbri/FileText.hpp"
#include "wheelbri/Retention.hpp"

#include <cstdio>
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

// An environment variable as a path. On Windows read as UTF-16: the narrow getenv gives the ANSI code page, which
// std::filesystem would take for UTF-8 (a user named José: an exception before anything is logged).
std::filesystem::path envPath(const char* name) {
#if defined(_WIN32)
    const std::wstring wide(name, name + std::char_traits<char>::length(name));
    const wchar_t* const value = _wgetenv(wide.c_str());
    return value != nullptr ? std::filesystem::path(std::wstring(value)) : std::filesystem::path();
#else
    return std::filesystem::path(envOrEmpty(name));
#endif
}

std::filesystem::path logsFolder() {
#if defined(_WIN32)
    return envPath("LOCALAPPDATA") / "wheelbri" / "logs";
#elif defined(__APPLE__)
    return envPath("HOME") / "Library" / "Logs" / "wheelbri";
#else
    const std::filesystem::path state = envPath("XDG_STATE_HOME");
    return state.empty() ? envPath("HOME") / ".local" / "state" / "wheelbri" : state / "wheelbri";
#endif
}

#if defined(_WIN32)
bool isAscii(const std::string& text) {
    for (const char c : text) {
        if (static_cast<unsigned char>(c) >= 0x80) return false;
    }
    return true;
}

// spdlog opens log files by narrow name, which Windows reads in the ANSI code page while std::filesystem reads it as
// UTF-8: only ASCII means the same to both. A folder that is not ASCII gets its 8.3 short name (ASCII) when the
// volume has them; otherwise it stays as it is (the log may then not open, as before).
std::filesystem::path asciiFolder(const std::filesystem::path& folder) {
    if (isAscii(pathToUtf8(folder))) return folder;
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::wstring wide = folder.wstring();
    const DWORD size = GetShortPathNameW(wide.c_str(), nullptr, 0);
    if (size == 0) return folder;
    std::wstring shortName(size, L'\0');
    const DWORD written = GetShortPathNameW(wide.c_str(), shortName.data(), size);
    if (written == 0 || written >= size) return folder;
    shortName.resize(written);
    const std::filesystem::path result(shortName);
    return isAscii(pathToUtf8(result)) ? result : folder;
}
#endif

}  // namespace

std::string defaultLogFile() {
#if defined(_WIN32)
    return pathToUtf8(asciiFolder(logsFolder()) / "wheelbri-service.log");
#else
    return pathToUtf8(logsFolder() / "wheelbri-service.log");
#endif
}

std::filesystem::path userGamesFile() { return appStateFile().parent_path() / "games.ini"; }

std::filesystem::path userSettingsFolder() {
#if defined(_WIN32)
    return envPath("LOCALAPPDATA") / "wheelbri";
#elif defined(__APPLE__)
    return envPath("HOME") / "Library" / "Application Support" / "wheelbri";
#else
    const std::filesystem::path config = envPath("XDG_CONFIG_HOME");
    return config.empty() ? envPath("HOME") / ".config" / "wheelbri" : config / "wheelbri";
#endif
}

bool isInsideUserFolders(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path target = std::filesystem::weakly_canonical(std::filesystem::absolute(path, ec), ec);
    if (ec || target.empty()) return false;
    for (const std::filesystem::path& root : {userSettingsFolder(), logsFolder()}) {
        if (root.empty()) continue;
        std::error_code rootEc;
        const std::filesystem::path base = std::filesystem::weakly_canonical(root, rootEc);
        if (rootEc) continue;
        // `target` is `base` or under it: every component of `base` matches (case-blind on Windows).
        auto t = target.begin();
        bool inside = true;
        for (auto b = base.begin(); b != base.end(); ++b, ++t) {
            if (t == target.end()) {
                inside = false;
                break;
            }
#if defined(_WIN32)
            if (_wcsicmp(t->c_str(), b->c_str()) != 0) {
#else
            if (*t != *b) {
#endif
                inside = false;
                break;
            }
        }
        if (inside) return true;
    }
    return false;
}

std::string userFoldersProblem() {
#if defined(_WIN32)
    // The app runs as Administrator but these folders belong to the user: a link (junction) put in place of one of them
    // would send the Administrator's writes (logs, settings) somewhere the user could not write themselves.
    const std::filesystem::path root = userSettingsFolder();
    for (const std::filesystem::path& folder :
         {root, root / "logs", root / "configs", root / "rigs", root / "devices"}) {
        const DWORD attributes = GetFileAttributesW(folder.wstring().c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return pathToUtf8(folder) + " is a link to another folder: WheelBri does not write through links. Delete "
                   "the link (not what it points to) and start WheelBri again.";
        }
    }
#endif
    return {};
}

std::vector<std::string> removeLinkedLogFiles(const std::filesystem::path& folder) {
    std::vector<std::string> notes;
#if defined(_WIN32)
    if (folder.empty()) return notes;
    forEachEntry(folder, [&](const std::filesystem::directory_entry& entry) {
        const std::filesystem::path& file = entry.path();
        const std::string name = pathToUtf8(file.filename());
        const std::string extension = pathToUtf8(file.extension());
        if (!isManagedFile(name) && extension != ".log" && extension != ".csv") return;
        std::error_code ec;
        bool linked = entry.is_symlink(ec);
        if (!linked) {
            const DWORD attributes = GetFileAttributesW(file.wstring().c_str());
            linked = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        }
        if (!linked) {
            const HANDLE handle = CreateFileW(file.wstring().c_str(), FILE_READ_ATTRIBUTES,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                              nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
                BY_HANDLE_FILE_INFORMATION info{};
                linked = GetFileInformationByHandle(handle, &info) != 0 && info.nNumberOfLinks > 1 &&
                         (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
                CloseHandle(handle);
            }
        }
        if (!linked) return;
        // remove() deletes a link itself, not its target (and only this name of a hard-linked file).
        std::filesystem::remove(file, ec);
        notes.push_back(ec ? "could not remove the link " + pathToUtf8(file) + ": " + ec.message()
                           : "removed " + pathToUtf8(file) + ": it was a link to another file, and WheelBri does not "
                                                             "write its logs through links");
    });
#else
    static_cast<void>(folder);
#endif
    return notes;
}

void reportStartupProblem(const std::string& text, const bool window) {
#if defined(_WIN32)
    if (window) {
        std::wstring wide(text.size() + 1, L'\0');
        const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                                               static_cast<int>(wide.size()));
        wide.resize(length > 0 ? static_cast<std::size_t>(length) : 0);
        MessageBoxW(nullptr, wide.empty() ? L"WheelBri cannot start." : wide.c_str(), L"WheelBri",
                    MB_OK | MB_ICONERROR);
        return;
    }
#else
    static_cast<void>(window);
#endif
    std::fprintf(stderr, "wheelbri: %s\n", text.c_str());
}

std::filesystem::path deviceLabelsFolder() { return userSettingsFolder() / "devices"; }

std::filesystem::path userConfigFolder() { return userSettingsFolder() / "configs"; }

std::filesystem::path rigsFolder() { return userSettingsFolder() / "rigs"; }

std::filesystem::path appStateFile() { return userSettingsFolder() / "app.ini"; }

std::filesystem::path shimGamesFile() { return userSettingsFolder() / "shim-games.txt"; }

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
    // Only WheelBri's own logs folder is ever opened to LocalService and handed to the drivers: a folder named in a
    // config (which any program of the user can write) must not get a Windows permission changed by Administrator.
    std::filesystem::create_directories(logsFolder(), ec);
    if (!std::filesystem::equivalent(folder, logsFolder(), ec)) {
        WE_LOG_WARN(logger, "logs: {} is not WheelBri's logs folder ({}): the drivers keep writing to {}",
                    pathToUtf8(folder), pathToUtf8(logsFolder()), pathToUtf8(logsFolder()));
    }
    if (const std::string problem = userFoldersProblem(); !problem.empty()) {
        WE_LOG_ERROR(logger, "logs: {}", problem);
        return;
    }
    const std::filesystem::path absolute = std::filesystem::absolute(logsFolder(), ec);
    if (const DWORD status = grantLocalService(absolute); status != ERROR_SUCCESS) {
        WE_LOG_WARN(logger, "logs: cannot let the drivers write in {} (error {}): they keep writing to "
                            "C:\\ProgramData\\wheelbri\\logs",
                    pathToUtf8(absolute), status);
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
    const std::filesystem::path programData = envPath("ProgramData") / "wheelbri";
    const std::filesystem::path oldLogs = programData / "logs";
    int moved = 0;
    if (!envPath("ProgramData").empty() && std::filesystem::is_directory(oldLogs, ec)) {
        forEachEntry(oldLogs, [&](const std::filesystem::directory_entry& entry) {
            std::error_code entryError;
            const std::filesystem::path target = absolute / entry.path().filename();
            if (!entry.is_regular_file(entryError) || entry.is_symlink(entryError)) return;
            if (std::filesystem::exists(target, entryError)) {
                // Same day's file on both sides: append the old part to the new one.
                std::ifstream in(entry.path(), std::ios::binary);
                std::ofstream out(target, std::ios::binary | std::ios::app);
                if (!in || !out) return;
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
        });
        std::filesystem::remove(oldLogs, ec);       // only when empty
        std::filesystem::remove(programData, ec);   // only when empty
    }
    WE_LOG_INFO(logger, "logs: every log goes to {} (the drivers too){}", pathToUtf8(absolute),
                moved > 0 ? "; moved " + std::to_string(moved) + " files there from " + pathToUtf8(oldLogs) : "");
}

#else
void shareLogFolder(const std::filesystem::path&) {}
#endif

void logStartupBanner(const std::string& configPath) {
    const logging::Logger logger = logging::get("service");
    WE_LOG_INFO(logger, "wheelbri-service {} starting on {}", WHEELBRI_VERSION, platform::name());
    WE_LOG_INFO(logger, "config: {}", configPath.empty() ? std::string("(built-in defaults)") : configPath);
    const std::string envLevels = envOrEmpty("SPDLOG_LEVEL");
    if (!envLevels.empty()) {
        WE_LOG_INFO(logger, "log levels overridden by SPDLOG_LEVEL='{}'", envLevels);
    }
}

}  // namespace wheelbri::service
