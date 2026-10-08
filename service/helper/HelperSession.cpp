#include "HelperSession.hpp"

#include "HideFilter.hpp"
#include "Privileged.hpp"
#include "VirtualG920.hpp"

#include "wheelbri/FileText.hpp"
#include "wheelbri/Logging.hpp"

#include <windows.h>
#include <aclapi.h>
#include <shlobj.h>

#include <cwctype>
#include <filesystem>

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

}  // namespace

HelperSession::HelperSession(void* const process, void* const token, const int scriptLevels, Cancelled cancelled)
    : process_(process), token_(token), scriptLevels_(scriptLevels), cancelled_(std::move(cancelled)) {
    pid_ = GetProcessId(static_cast<HANDLE>(process_));
}

HelperSession::~HelperSession() {
    if (g920_) {
        WE_LOG_INFO(log(), "helper: pid {} is gone: removing its virtual G920", pid_);
        g920_.reset();
    }
    showRigIfOurs();
    if (token_ != nullptr) CloseHandle(static_cast<HANDLE>(token_));
    CloseHandle(static_cast<HANDLE>(process_));
}

Reply HelperSession::handle(const Request& request) {
    switch (request.type) {
        case RequestType::CreateVirtualG920: return createVirtualG920(request.seq);
        case RequestType::RemoveVirtualG920:
            if (g920_) {
                g920_.reset();
                WE_LOG_INFO(log(), "helper: virtual G920 removed for pid {}", pid_);
            }
            return Reply{request.seq, Status::Ok, {}};
        case RequestType::HideRig: return hideRig(request.seq);
        case RequestType::ShowRig: return showRig(request.seq);
        case RequestType::PutHideFilter: return putHideFilter(request);
        case RequestType::InstallG920Driver: return installG920Driver(request.seq);
        case RequestType::ShareLogFolder: return shareLogFolder(request.seq);
        case RequestType::SetLogLevel: return setLogLevel(request);
        case RequestType::Hello: break;
    }
    return Reply{request.seq, Status::Unsupported, "not a request for a session"};
}

Reply HelperSession::createVirtualG920(const std::uint32_t seq) {
    if (g920_) return Reply{seq, Status::Ok, g920_->instanceId()};
    std::string error;
    g920_ = service::VirtualG920::create(error, cancelled_, /*userHive=*/false);
    if (!g920_) return Reply{seq, Status::Failed, error};
    WE_LOG_INFO(log(), "helper: virtual G920 {} created for pid {}", g920_->instanceId(), pid_);
    return Reply{seq, Status::Ok, g920_->instanceId()};
}

Reply HelperSession::hideRig(const std::uint32_t seq) {
    std::string message;
    if (!service::privileged::allowOnly(process_, message)) return Reply{seq, Status::Failed, message};
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
    if (service::privileged::allowedPid() != pid_) {
        hid_ = false;
        return;
    }
    if (service::privileged::writeAllowedPid(0, 0)) {
        hid_ = false;
        WE_LOG_INFO(log(), "helper: the rig is visible to games again (pid {})", pid_);
    }
}

Reply HelperSession::putHideFilter(const Request& request) {
    for (const std::string& id : request.devices) {
        if (const std::string problem = service::HideFilter::interfaceProblem(id); !problem.empty()) {
            WE_LOG_WARN(log(), "helper: refused PutHideFilter from pid {}: {}", pid_, problem);
            return Reply{request.seq, Status::Refused, problem};
        }
    }
    std::string message;
    // The script skips the interfaces that have the filter already; then all of them are waited for.
    switch (service::privileged::putHideFilter(request.devices, request.devices, scriptLevels_, scriptLogFolder(),
                                               cancelled_, message)) {
        case service::privileged::HideSetup::Running:
        case service::privileged::HideSetup::NothingToDo: return Reply{request.seq, Status::Ok, {}};
        case service::privileged::HideSetup::Waiting: return Reply{request.seq, Status::Pending, message};
        case service::privileged::HideSetup::Failed: break;
    }
    return Reply{request.seq, Status::Failed, message};
}

Reply HelperSession::installG920Driver(const std::uint32_t seq) {
    std::string message;
    return service::privileged::installG920Driver(scriptLevels_, scriptLogFolder(), cancelled_, message)
               ? Reply{seq, Status::Ok, {}}
               : Reply{seq, Status::Failed, message};
}

// The drivers (LocalService) may write into the caller's own %LOCALAPPDATA%\wheelbri\logs, and HKLM\SOFTWARE\wheelbri
// LogFolder names it. The folder is made and opened as the caller (so Windows checks the caller's own rights, and a
// link leads only where the caller could change permissions anyway), must be a real folder reached without a link
// (what Windows opened is compared with what was asked), and its permission is changed through that handle.
Reply HelperSession::setLogLevel(const Request& request) {
    const std::optional<LogLevel> level = requestLogLevel(request);   // requestProblem checked it
    if (!level) return Reply{request.seq, Status::Refused, "SetLogLevel takes one level: trace, debug, info or warn"};
    std::string message;
    if (!service::privileged::writeLogLevel(*level, message)) return Reply{request.seq, Status::Failed, message};
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

std::filesystem::path HelperSession::scriptLogFolder() const {
    if (!logFolder_.empty()) return logFolder_;
    wchar_t programData[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
    const std::filesystem::path base =
        length > 0 && length < MAX_PATH ? std::filesystem::path(programData) : std::filesystem::path(L"C:\\ProgramData");
    return base / "wheelbri" / "logs";
}

}  // namespace wheelbri::helper
