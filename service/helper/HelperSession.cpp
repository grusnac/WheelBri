// SPDX-License-Identifier: GPL-3.0-or-later
#include "HelperSession.hpp"

#include "HideFilter.hpp"
#include "Privileged.hpp"
#include "VirtualWheelDevice.hpp"

#include "wheelbri/FileText.hpp"
#include "wheelbri/Logging.hpp"

#include <windows.h>
#include <aclapi.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

namespace wheelbri::helper {

namespace {

logging::Logger& log() {
    static logging::Logger logger = logging::get("helper");
    return logger;
}

std::string utf8(const std::wstring& wide) {
    return pathToUtf8(std::filesystem::path(wide));
}

// `path` without a "\\?\" prefix (and "\\?\UNC\" as "\\"), for comparing what Windows opened with what was asked.
std::wstring plainPath(std::wstring path) {
    if (path.rfind(LR"(\\?\UNC\)", 0) == 0) return L"\\" + path.substr(7);
    if (path.rfind(LR"(\\?\)", 0) == 0) return path.substr(4);
    return path;
}

bool sameIgnoringCase(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

// LocalService (the drivers' host) may create, write and delete files in the folder `directory` (a handle opened
// with READ_CONTROL | WRITE_DAC) and in what is created there from now on.
DWORD grantLocalService(const HANDLE directory) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    DWORD status = GetSecurityInfo(directory, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                                   nullptr, &descriptor);
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
            status = SetSecurityInfo(directory, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, updated,
                                     nullptr);
        }
    } else {
        status = GetLastError();
    }
    if (updated != nullptr) LocalFree(updated);
    LocalFree(descriptor);
    return status;
}

// A new folder `directory` (opened with READ_CONTROL | WRITE_DAC | WRITE_OWNER) that only SYSTEM and Administrators
// may change or read: owned by Administrators, its permissions its own (none inherited).
DWORD lockDown(const HANDLE directory) {
    BYTE system[SECURITY_MAX_SID_SIZE];
    BYTE admins[SECURITY_MAX_SID_SIZE];
    DWORD systemSize = sizeof(system);
    DWORD adminsSize = sizeof(admins);
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &systemSize) ||
        !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins, &adminsSize)) {
        return GetLastError();
    }
    std::array<EXPLICIT_ACCESSW, 2> access{};
    const std::array<BYTE*, 2> who = {system, admins};
    for (std::size_t i = 0; i < access.size(); ++i) {
        access[i].grfAccessPermissions = GENERIC_ALL;
        access[i].grfAccessMode = SET_ACCESS;
        access[i].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        access[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access[i].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        access[i].Trustee.ptstrName = reinterpret_cast<LPWSTR>(who[i]);
    }
    PACL dacl = nullptr;
    DWORD status = SetEntriesInAclW(static_cast<ULONG>(access.size()), access.data(), nullptr, &dacl);
    if (status != ERROR_SUCCESS) return status;
    status = SetSecurityInfo(directory, SE_FILE_OBJECT,
                             OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                                 PROTECTED_DACL_SECURITY_INFORMATION,
                             admins, nullptr, dacl, nullptr);
    LocalFree(dacl);
    return status;
}

// A new scratch folder for one script's log: %SystemRoot%\Temp\wheelbri-script-<random>, made by this call (it
// fails if anything of that name is there) and locked down. Empty when it cannot be had.
std::filesystem::path makeScriptLogStage() {
    wchar_t windows[MAX_PATH] = {};
    const UINT length = GetSystemWindowsDirectoryW(windows, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return {};
    wchar_t name[64];
    std::swprintf(name, 64, L"wheelbri-script-%08lx%04x%04x", guid.Data1, guid.Data2, guid.Data3);
    const std::wstring folder = std::wstring(windows) + L"\\Temp\\" + name;
    if (!CreateDirectoryW(folder.c_str(), nullptr)) {
        WE_LOG_WARN(log(), "helper: cannot make {} (error {}): the driver script keeps no log", utf8(folder),
                    GetLastError());
        return {};
    }
    const HANDLE directory = CreateFileW(folder.c_str(), READ_CONTROL | WRITE_DAC | WRITE_OWNER | FILE_READ_ATTRIBUTES,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    const DWORD status = directory == INVALID_HANDLE_VALUE ? GetLastError() : lockDown(directory);
    if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
    if (status != ERROR_SUCCESS) {
        WE_LOG_WARN(log(), "helper: cannot keep {} to SYSTEM (error {}): the driver script keeps no log", utf8(folder),
                    status);
        RemoveDirectoryW(folder.c_str());
        return {};
    }
    return std::filesystem::path(folder);
}

// Whether `path` (a folder) is a real folder reached without a link and owned by SYSTEM or Administrators, so no user
// can swap it for a link.
bool systemsFolder(const std::wstring& path) {
    const HANDLE directory = CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    bool ok = GetFileInformationByHandle(directory, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
              (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (ok && GetSecurityInfo(directory, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr,
                              &descriptor) == ERROR_SUCCESS) {
        ok = IsWellKnownSid(owner, WinLocalSystemSid) || IsWellKnownSid(owner, WinBuiltinAdministratorsSid);
        LocalFree(descriptor);
    } else {
        ok = false;
    }
    CloseHandle(directory);
    return ok;
}

// Appends `text` to `file` (opened or made, never through a link, and refused when it has more than one name: a hard
// link a user made to another file). As the caller when the thread impersonates it, else as SYSTEM.
bool appendLog(const std::wstring& file, const std::string& text) {
    const HANDLE handle = CreateFileW(file.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    bool ok = GetFileInformationByHandle(handle, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
              info.nNumberOfLinks == 1;
    if (ok) {
        DWORD done = 0;
        ok = WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &done, nullptr) && done == text.size();
    }
    CloseHandle(handle);
    return ok;
}

// The logs a script wrote into `stage`, appended to the same files in the usual place, then `stage` removed: the
// caller's own folder (`callersFolder`, written as the caller with `token`) once it shared one; else ProgramData's
// wheelbri\logs, written as SYSTEM only when it and wheelbri above it are SYSTEM's or Administrators' (as WheelBri's
// installer made them) and not links.
void deliverLogs(const std::filesystem::path& stage, const std::filesystem::path& callersFolder, void* const token) {
    if (stage.empty()) return;
    std::error_code ec;
    std::vector<std::pair<std::filesystem::path, std::string>> logs;
    for (const auto& entry : std::filesystem::directory_iterator(stage, ec)) {
        std::error_code entryError;
        if (!entry.is_regular_file(entryError)) continue;
        if (const std::optional<std::string> text = readFileText(entry.path())) {
            logs.emplace_back(entry.path().filename(), *text);
        }
    }
    std::filesystem::remove_all(stage, ec);
    if (logs.empty()) return;
    const bool asCaller = !callersFolder.empty() && token != nullptr;
    std::filesystem::path folder = callersFolder;
    if (!asCaller) {
        wchar_t programData[MAX_PATH] = {};
        const DWORD length = GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
        const std::filesystem::path base = length > 0 && length < MAX_PATH ? std::filesystem::path(programData)
                                                                           : std::filesystem::path(L"C:\\ProgramData");
        folder = base / "wheelbri" / "logs";
        CreateDirectoryW((base / "wheelbri").c_str(), nullptr);
        CreateDirectoryW(folder.c_str(), nullptr);
        if (!systemsFolder((base / "wheelbri").wstring()) || !systemsFolder(folder.wstring())) {
            WE_LOG_WARN(log(), "helper: {} is not a folder only Administrators control: the driver script's log is "
                               "not written there",
                        pathToUtf8(folder));
            return;
        }
    }
    if (asCaller && !SetThreadToken(nullptr, static_cast<HANDLE>(token))) {
        WE_LOG_WARN(log(), "helper: cannot act as the caller to write the script's log (error {})", GetLastError());
        return;
    }
    for (const auto& [name, text] : logs) {
        if (!appendLog((folder / name).wstring(), text)) {
            WE_LOG_WARN(log(), "helper: cannot write {} into {} (error {})", pathToUtf8(name), pathToUtf8(folder),
                        GetLastError());
        }
    }
    if (asCaller) RevertToSelf();
}

}  // namespace

HelperSession::Steps HelperSession::systemSteps() {
    namespace privileged = service::privileged;
    Steps steps;
    steps.allowOnly = privileged::allowOnly;
    steps.allowedPid = privileged::allowedPid;
    steps.writeAllowedPid = privileged::writeAllowedPid;
    steps.interfaceProblem = service::HideFilter::interfaceProblem;
    steps.putHideFilter = privileged::putHideFilter;
    steps.takeHideFilterOff = privileged::takeHideFilterOff;
    steps.installWheelDriver = privileged::installWheelDriver;
    steps.writeLogLevel = privileged::writeLogLevel;
    steps.scriptLogStage = makeScriptLogStage;
    steps.deliverScriptLogs = deliverLogs;
    return steps;
}

HelperSession::HelperSession(void* const process, void* const token, const int scriptLevels, Cancelled cancelled,
                             Steps steps)
    : process_(process),
      token_(token),
      scriptLevels_(scriptLevels),
      cancelled_(std::move(cancelled)),
      steps_(std::move(steps)) {
    pid_ = GetProcessId(static_cast<HANDLE>(process_));
}

HelperSession::~HelperSession() {
    if (device_) {
        WE_LOG_INFO(log(), "helper: pid {} is gone: removing its virtual wheel", pid_);
        device_.reset();
    }
    showRigIfOurs();
    if (!filterOn_.empty()) {
        WE_LOG_INFO(log(), "helper: pid {} is gone: taking the hide filter off the rig", pid_);
        std::string message;
        (void)steps_.takeHideFilterOff(filterOn_, {}, message);
    }
    if (token_ != nullptr) CloseHandle(static_cast<HANDLE>(token_));
    CloseHandle(static_cast<HANDLE>(process_));
}

Reply HelperSession::handle(const Request& request) {
    switch (request.type) {
        case RequestType::CreateVirtualWheel: return createVirtualWheel(request);
        case RequestType::RemoveVirtualWheel:
            if (device_) {
                device_.reset();
                WE_LOG_INFO(log(), "helper: virtual wheel removed for pid {}", pid_);
            }
            return Reply{request.seq, Status::Ok, {}};
        case RequestType::HideRig: return hideRig(request.seq);
        case RequestType::ShowRig: return showRig(request.seq);
        case RequestType::PutHideFilter: return putHideFilter(request);
        case RequestType::TakeHideFilterOff: return takeHideFilterOff(request);
        case RequestType::InstallWheelDriver: return installWheelDriver(request.seq);
        case RequestType::ShareLogFolder: return shareLogFolder(request.seq);
        case RequestType::SetLogLevel: return setLogLevel(request);
        case RequestType::Hello: break;
    }
    return Reply{request.seq, Status::Unsupported, "not a request for a session"};
}

Reply HelperSession::createVirtualWheel(const Request& request) {
    const std::uint32_t seq = request.seq;
    const std::optional<std::string> name = requestVirtualWheel(request);   // checked by requestProblem
    const VirtualWheel* wheel = name ? virtualWheelForRequestName(*name) : nullptr;
    if (wheel == nullptr) {
        return Reply{seq, Status::Refused, "CreateVirtualWheel takes one wheel: g25, g27, g920 or g923"};
    }
    // Another wheel than this connection's: the one there goes first (one virtual wheel per session).
    if (device_ && deviceWheel_ != wheel) {
        device_.reset();
        WE_LOG_INFO(log(), "helper: virtual {} removed for pid {} (it asks for the {} now)",
                    deviceWheel_->instancePrefix, pid_, wheel->instancePrefix);
    }
    if (device_) return Reply{seq, Status::Ok, device_->instanceId()};
    std::string error;
    device_ = service::VirtualWheelDevice::create(*wheel, error, cancelled_, /*userHive=*/false,
                                                  requestTrueForceProbe(request));
    if (!device_) return Reply{seq, Status::Failed, error};
    deviceWheel_ = wheel;
    WE_LOG_INFO(log(), "helper: virtual {} {} created for pid {}", wheel->instancePrefix, device_->instanceId(), pid_);
    return Reply{seq, Status::Ok, device_->instanceId()};
}

Reply HelperSession::hideRig(const std::uint32_t seq) {
    std::string message;
    if (!steps_.allowOnly(process_, message)) return Reply{seq, Status::Failed, message};
    hid_ = true;
    WE_LOG_INFO(log(), "helper: the rig is hidden from games (only pid {} may open its devices)", pid_);
    return Reply{seq, Status::Ok, {}};
}

Reply HelperSession::showRig(const std::uint32_t seq) {
    if (!hid_) return Reply{seq, Status::Ok, {}};
    showRigIfOurs();
    return hid_ ? Reply{seq, Status::Failed, "cannot write HKLM\\SOFTWARE\\wheelbri\\hide"} : Reply{seq, Status::Ok, {}};
}

// Shows the rig again when this session hid it and nobody hid it since (another app session's setting is left alone).
void HelperSession::showRigIfOurs() {
    if (!hid_) return;
    if (steps_.allowedPid() != pid_) {
        hid_ = false;
        return;
    }
    if (steps_.writeAllowedPid(0, 0)) {
        hid_ = false;
        WE_LOG_INFO(log(), "helper: the rig is visible to games again (pid {})", pid_);
    }
}

Reply HelperSession::putHideFilter(const Request& request) {
    for (const std::string& id : request.devices) {
        if (const std::string problem = steps_.interfaceProblem(id); !problem.empty()) {
            WE_LOG_WARN(log(), "helper: refused PutHideFilter from pid {}: {}", pid_, problem);
            return Reply{request.seq, Status::Refused, problem};
        }
    }
    std::string message;
    // The script skips the interfaces that have the filter already; then all of them are waited for.
    const std::filesystem::path stage = steps_.scriptLogStage();
    const service::privileged::HideSetup result =
        steps_.putHideFilter(request.devices, request.devices, scriptLevels_, stage, cancelled_, message);
    steps_.deliverScriptLogs(stage, logFolder_, token_);
    if (result == service::privileged::HideSetup::Running || result == service::privileged::HideSetup::Waiting) {
        for (const std::string& id : request.devices) {
            if (std::find(filterOn_.begin(), filterOn_.end(), id) == filterOn_.end()) filterOn_.push_back(id);
        }
    }
    switch (result) {
        case service::privileged::HideSetup::Running:
        case service::privileged::HideSetup::NothingToDo: return Reply{request.seq, Status::Ok, {}};
        case service::privileged::HideSetup::Waiting: return Reply{request.seq, Status::Pending, message};
        case service::privileged::HideSetup::Failed: break;
    }
    return Reply{request.seq, Status::Failed, message};
}

// Only interfaces with wheelbri-hide's own driver are changed (privileged::takeHideFilterOff); unplugged ones are
// left as they are.
Reply HelperSession::takeHideFilterOff(const Request& request) {
    std::string message;
    const service::privileged::HideSetup result =
        steps_.takeHideFilterOff(request.devices, cancelled_, message);
    if (result != service::privileged::HideSetup::Failed) {
        std::erase_if(filterOn_, [&request](const std::string& id) {
            return std::find(request.devices.begin(), request.devices.end(), id) != request.devices.end();
        });
        WE_LOG_INFO(log(), "helper: hide filter taken off for pid {}", pid_);
    }
    switch (result) {
        case service::privileged::HideSetup::Running:
        case service::privileged::HideSetup::NothingToDo: return Reply{request.seq, Status::Ok, {}};
        case service::privileged::HideSetup::Waiting: return Reply{request.seq, Status::Pending, message};
        case service::privileged::HideSetup::Failed: break;
    }
    return Reply{request.seq, Status::Failed, message};
}

Reply HelperSession::installWheelDriver(const std::uint32_t seq) {
    std::string message;
    const std::filesystem::path stage = steps_.scriptLogStage();
    const bool installed = steps_.installWheelDriver(scriptLevels_, stage, cancelled_, message);
    steps_.deliverScriptLogs(stage, logFolder_, token_);
    return installed ? Reply{seq, Status::Ok, {}} : Reply{seq, Status::Failed, message};
}

// The drivers (LocalService) may write into the caller's own %LOCALAPPDATA%\wheelbri\logs, and HKLM\SOFTWARE\wheelbri
// LogFolder names it. The folder is made and opened as the caller (so Windows checks the caller's own rights, and a
// link leads only where the caller could change permissions anyway), must be a real folder reached without a link
// (what Windows opened is compared with what was asked), and its permission is changed through that handle.
Reply HelperSession::setLogLevel(const Request& request) {
    const std::optional<LogLevel> level = requestLogLevel(request);   // requestProblem checked it
    if (!level) return Reply{request.seq, Status::Refused, "SetLogLevel takes one level: trace, debug, info or warn"};
    std::string message;
    if (!steps_.writeLogLevel(*level, message)) return Reply{request.seq, Status::Failed, message};
    return Reply{request.seq, Status::Ok, {}};
}

Reply HelperSession::shareLogFolder(const std::uint32_t seq) {
    if (token_ == nullptr) return Reply{seq, Status::Refused, "the caller did not let the helper act as it"};
    const HANDLE token = static_cast<HANDLE>(token_);
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, token, &localAppData))) {
        CoTaskMemFree(localAppData);
        return Reply{seq, Status::Failed, "the caller's local application data folder is unknown"};
    }
    const std::wstring base = std::wstring(localAppData) + L"\\wheelbri";
    CoTaskMemFree(localAppData);
    const std::wstring folder = base + L"\\logs";

    HANDLE directory = INVALID_HANDLE_VALUE;
    if (!SetThreadToken(nullptr, token)) {
        return Reply{seq, Status::Failed, "cannot act as the caller (error " + std::to_string(GetLastError()) + ")"};
    }
    CreateDirectoryW(base.c_str(), nullptr);
    CreateDirectoryW(folder.c_str(), nullptr);
    directory = CreateFileW(folder.c_str(), READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    const DWORD openError = GetLastError();
    RevertToSelf();
    if (directory == INVALID_HANDLE_VALUE) {
        return Reply{seq, Status::Failed, "cannot open " + utf8(folder) + " as the caller (error " +
                                              std::to_string(openError) + ")"};
    }
    std::string problem;
    BY_HANDLE_FILE_INFORMATION info{};
    std::wstring opened(32768, L'\0');
    if (!GetFileInformationByHandle(directory, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        problem = utf8(folder) + " is not a plain folder";
    } else {
        const DWORD length = GetFinalPathNameByHandleW(directory, opened.data(), static_cast<DWORD>(opened.size()),
                                                       FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        opened.resize(length < opened.size() ? length : 0);
        opened = plainPath(opened);
        if (opened.empty() || !sameIgnoringCase(opened, folder)) {
            problem = utf8(folder) + " is reached through a link (" + utf8(opened) + ")";
        }
    }
    if (problem.empty()) {
        if (const DWORD status = grantLocalService(directory); status != ERROR_SUCCESS) {
            problem = "cannot let the drivers write in " + utf8(folder) + " (error " + std::to_string(status) + ")";
        }
    }
    CloseHandle(directory);
    if (!problem.empty()) {
        WE_LOG_WARN(log(), "helper: log folder for pid {}: {}; the drivers keep writing to "
                           "C:\\ProgramData\\wheelbri\\logs",
                    pid_, problem);
        return Reply{seq, Status::Refused, problem};
    }
    HKEY key = nullptr;
    LSTATUS written = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\wheelbri", 0, nullptr, REG_OPTION_NON_VOLATILE,
                                      KEY_SET_VALUE, nullptr, &key, nullptr);
    if (written == ERROR_SUCCESS) {
        written = RegSetValueExW(key, L"LogFolder", 0, REG_SZ, reinterpret_cast<const BYTE*>(opened.c_str()),
                                 static_cast<DWORD>((opened.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
    if (written != ERROR_SUCCESS) {
        return Reply{seq, Status::Failed, "cannot write HKLM\\SOFTWARE\\wheelbri LogFolder (error " +
                                              std::to_string(written) + ")"};
    }
    WE_LOG_INFO(log(), "helper: the drivers write their logs into {} (pid {})", utf8(opened), pid_);
    logFolder_ = std::filesystem::path(opened);
    return Reply{seq, Status::Ok, utf8(opened)};
}


}  // namespace wheelbri::helper
