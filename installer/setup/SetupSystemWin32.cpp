// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef UNICODE
#define UNICODE
#endif
#include "SetupSystemWin32.hpp"

#include "SetupLogic.hpp"

#include <windows.h>
#include <sddl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <memory>

namespace wheelbri::setup {
namespace {

constexpr const wchar_t* kHelperService = L"WheelBriHelper";
constexpr std::uint64_t kMaxReadBytes = 256ull * 1024 * 1024;

DWORD attributes(const std::wstring& path) { return GetFileAttributesW(path.c_str()); }

HKEY rootOf(const Hive hive) {
    switch (hive) {
    case Hive::Machine: return HKEY_LOCAL_MACHINE;
    case Hive::Users: return HKEY_USERS;
    case Hive::CurrentUser: return HKEY_CURRENT_USER;
    }
    return HKEY_LOCAL_MACHINE;
}

struct Key {
    HKEY h{};
    ~Key() { if (h) RegCloseKey(h); }
};

bool openKey(const Hive hive, const std::wstring& key, const REGSAM access, Key& out) {
    return RegOpenKeyExW(rootOf(hive), key.c_str(), 0, access | KEY_WOW64_64KEY, &out.h) == ERROR_SUCCESS;
}

bool createKey(const Hive hive, const std::wstring& key, Key& out) {
    return RegCreateKeyExW(rootOf(hive), key.c_str(), 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &out.h,
                           nullptr) == ERROR_SUCCESS;
}

struct Service {
    SC_HANDLE h{};
    ~Service() { if (h) CloseServiceHandle(h); }
};

bool waitForState(SC_HANDLE service, const DWORD state, const DWORD milliseconds) {
    const ULONGLONG deadline = GetTickCount64() + milliseconds;
    SERVICE_STATUS status{};
    while (QueryServiceStatus(service, &status)) {
        if (status.dwCurrentState == state) return true;
        if (GetTickCount64() >= deadline) return false;
        Sleep(250);
    }
    return false;
}

// The processes named `exe` (with .exe) in `session` (any when -1), not this one.
std::vector<DWORD> processesNamed(const std::vector<std::wstring>& names, const DWORD session = static_cast<DWORD>(-1)) {
    std::vector<DWORD> ids;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return ids;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == GetCurrentProcessId()) continue;
        const bool named = std::any_of(names.begin(), names.end(),
                                       [&](const std::wstring& n) { return sameName(entry.szExeFile, n + L".exe"); });
        DWORD processSession = 0;
        if (named && (session == static_cast<DWORD>(-1) ||
                      (ProcessIdToSessionId(entry.th32ProcessID, &processSession) && processSession == session))) {
            ids.push_back(entry.th32ProcessID);
        }
    }
    CloseHandle(snapshot);
    return ids;
}

BOOL CALLBACK closeWindowOf(HWND window, const LPARAM processId) {
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner == static_cast<DWORD>(processId) && IsWindowVisible(window) && !GetWindow(window, GW_OWNER)) {
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

std::wstring sidText(PSID sid) {
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(sid, &text)) return {};
    std::wstring out(text);
    LocalFree(text);
    return out;
}

std::wstring tokenUserSid(HANDLE token) {
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0) return {};
    std::vector<unsigned char> buffer(size);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) return {};
    return sidText(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid);
}

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    return withoutTrailingSlash(out);
}

}  // namespace

bool SetupSystemWin32::isFile(const std::wstring& path) {
    const DWORD a = attributes(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool SetupSystemWin32::isFolder(const std::wstring& path) {
    const DWORD a = attributes(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool SetupSystemWin32::isLink(const std::wstring& path) {
    const DWORD a = attributes(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

std::vector<DirEntry> SetupSystemWin32::list(const std::wstring& folder) {
    std::vector<DirEntry> entries;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(joinPath(folder, L"*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return entries;
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") continue;
        entries.push_back({name, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                           (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0});
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return entries;
}

bool SetupSystemWin32::makeFolders(const std::wstring& path) {
    if (isFolder(path)) return true;
    const std::wstring parent = parentOf(path);
    if (!parent.empty() && parent.size() > 2 && !makeFolders(parent)) return false;
    return CreateDirectoryW(path.c_str(), nullptr) || isFolder(path);
}

bool SetupSystemWin32::copyFile(const std::wstring& from, const std::wstring& to) {
    const DWORD a = attributes(to);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW(to.c_str(), a & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
    }
    return CopyFileW(from.c_str(), to.c_str(), FALSE) != 0;
}

bool SetupSystemWin32::moveFile(const std::wstring& from, const std::wstring& to) {
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH) != 0;
}

bool SetupSystemWin32::removeFile(const std::wstring& path) {
    const DWORD a = attributes(path);
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path.c_str(), a & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
    return DeleteFileW(path.c_str()) != 0;
}

bool SetupSystemWin32::removeFolder(const std::wstring& path) { return RemoveDirectoryW(path.c_str()) != 0; }

std::optional<std::string> SetupSystemWin32::readFile(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER size{};
    std::optional<std::string> out;
    if (GetFileSizeEx(file, &size) && static_cast<std::uint64_t>(size.QuadPart) <= kMaxReadBytes) {
        std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
        DWORD done = 0;
        size_t at = 0;
        while (at < bytes.size() &&
               ReadFile(file, bytes.data() + at, static_cast<DWORD>(std::min<size_t>(bytes.size() - at, 1u << 20)), &done,
                        nullptr) &&
               done > 0) {
            at += done;
        }
        if (at == bytes.size()) out = std::move(bytes);
    }
    CloseHandle(file);
    return out;
}

// Through a temporary file and a rename: never a half-written file under the real name.
bool SetupSystemWin32::writeFile(const std::wstring& path, const std::string& bytes) {
    const std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = bytes.empty() ||
                    (WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size());
    const bool flushed = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    if (!ok || !flushed || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

std::uint64_t SetupSystemWin32::fileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

std::optional<std::wstring> SetupSystemWin32::regText(const Hive hive, const std::wstring& key, const std::wstring& value) {
    Key k;
    if (!openKey(hive, key, KEY_QUERY_VALUE, k)) return std::nullopt;
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(k.h, value.c_str(), nullptr, &type, nullptr, &size) != ERROR_SUCCESS) return std::nullopt;
    if (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ) return std::nullopt;
    std::wstring data(size / sizeof(wchar_t) + 2, L'\0');
    DWORD bytes = static_cast<DWORD>(data.size() * sizeof(wchar_t));
    if (RegQueryValueExW(k.h, value.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(data.data()), &bytes) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    data.resize(std::min<size_t>(data.size(), bytes / sizeof(wchar_t)));
    if (type != REG_MULTI_SZ) return std::wstring(data.c_str());
    // The strings, each ended by a null, the list by another: joined by \n.
    std::wstring joined;
    size_t start = 0;
    while (start < data.size() && data[start] != L'\0') {
        const size_t end = data.find(L'\0', start);
        joined += (joined.empty() ? L"" : L"\n") + data.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return joined;
}

bool SetupSystemWin32::regKeyExists(const Hive hive, const std::wstring& key) {
    Key k;
    return openKey(hive, key, KEY_QUERY_VALUE, k);
}

bool SetupSystemWin32::regValueExists(const Hive hive, const std::wstring& key, const std::wstring& value) {
    Key k;
    return openKey(hive, key, KEY_QUERY_VALUE, k) &&
           RegQueryValueExW(k.h, value.c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

std::vector<std::wstring> SetupSystemWin32::regValueNames(const Hive hive, const std::wstring& key) {
    std::vector<std::wstring> names;
    Key k;
    if (!openKey(hive, key, KEY_QUERY_VALUE, k)) return names;
    for (DWORD i = 0;; ++i) {
        wchar_t name[16384];
        DWORD length = static_cast<DWORD>(std::size(name));
        if (RegEnumValueW(k.h, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        names.emplace_back(name, length);
    }
    return names;
}

std::vector<std::wstring> SetupSystemWin32::regSubKeys(const Hive hive, const std::wstring& key) {
    std::vector<std::wstring> names;
    Key k;
    if (!openKey(hive, key, KEY_ENUMERATE_SUB_KEYS, k)) return names;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD length = static_cast<DWORD>(std::size(name));
        if (RegEnumKeyExW(k.h, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        names.emplace_back(name, length);
    }
    return names;
}

bool SetupSystemWin32::regSetText(const Hive hive, const std::wstring& key, const std::wstring& value,
                                  const std::wstring& text) {
    Key k;
    return createKey(hive, key, k) &&
           RegSetValueExW(k.h, value.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(text.c_str()),
                          static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool SetupSystemWin32::regSetMultiText(const Hive hive, const std::wstring& key, const std::wstring& value,
                                       const std::wstring& text) {
    std::wstring data = text;
    std::replace(data.begin(), data.end(), L'\n', L'\0');
    data.push_back(L'\0');
    data.push_back(L'\0');
    Key k;
    return createKey(hive, key, k) &&
           RegSetValueExW(k.h, value.c_str(), 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(data.data()),
                          static_cast<DWORD>(data.size() * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool SetupSystemWin32::regSetDword(const Hive hive, const std::wstring& key, const std::wstring& value, const std::uint32_t data) {
    Key k;
    const DWORD d = data;
    return createKey(hive, key, k) &&
           RegSetValueExW(k.h, value.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&d), sizeof(d)) == ERROR_SUCCESS;
}

void SetupSystemWin32::regDeleteValue(const Hive hive, const std::wstring& key, const std::wstring& value) {
    Key k;
    if (openKey(hive, key, KEY_SET_VALUE, k)) RegDeleteValueW(k.h, value.c_str());
}

void SetupSystemWin32::regDeleteTree(const Hive hive, const std::wstring& key) {
    const std::wstring parent = parentOf(key);
    const std::wstring leaf = nameOf(key);
    if (leaf.empty()) return;
    Key k;
    const REGSAM access = DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE;
    if (parent.empty()) {
        RegDeleteTreeW(rootOf(hive), leaf.c_str());   // HKEY_USERS\<sid>: never asked for; HKLM\<x> has no 32-bit view
        return;
    }
    if (openKey(hive, parent, access, k)) RegDeleteTreeW(k.h, leaf.c_str());
}

bool SetupSystemWin32::shortcut(const std::wstring& path, const std::wstring& target, const std::wstring& arguments,
                                const std::wstring& workFolder, const std::wstring& description, const std::wstring& icon) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
    link->SetPath(target.c_str());
    link->SetArguments(arguments.c_str());
    link->SetWorkingDirectory(workFolder.c_str());
    link->SetDescription(description.c_str());
    link->SetIconLocation(icon.c_str(), 0);
    IPersistFile* file = nullptr;
    bool ok = SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)));
    if (ok) {
        ok = SUCCEEDED(file->Save(path.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

int SetupSystemWin32::run(const std::wstring& commandLine, const std::wstring& workFolder,
                          const std::function<void(const std::wstring&)>& line) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd{}, writeEnd{};
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return -1;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    PROCESS_INFORMATION pi{};
    std::wstring command = commandLine;
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        workFolder.empty() ? nullptr : workFolder.c_str(), &si, &pi);
    CloseHandle(writeEnd);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started) {
        CloseHandle(readEnd);
        return -1;
    }
    CloseHandle(pi.hThread);
    // Console programs (PowerShell, pnputil, wusa) write in the OEM code page.
    auto send = [&](const std::string& bytes) {
        std::string text = bytes;
        if (!text.empty() && text.back() == '\r') text.pop_back();
        const int size = MultiByteToWideChar(CP_OEMCP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring wide(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(CP_OEMCP, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
        line(wide);
    };
    std::string pending;
    char buffer[4096];
    DWORD n = 0;
    while (ReadFile(readEnd, buffer, sizeof(buffer), &n, nullptr) && n > 0) {
        pending.append(buffer, n);
        size_t end;
        while ((end = pending.find('\n')) != std::string::npos) {
            send(pending.substr(0, end));
            pending.erase(0, end + 1);
        }
    }
    if (!pending.empty()) send(pending);
    CloseHandle(readEnd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

int SetupSystemWin32::closePrograms(const std::vector<std::wstring>& names, const int seconds,
                                    std::vector<std::wstring>& ended) {
    struct Running {
        DWORD id;
        HANDLE handle;
        std::wstring name;
    };
    std::vector<Running> running;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
            if (entry.th32ProcessID == GetCurrentProcessId()) continue;
            for (const std::wstring& n : names) {
                if (!sameName(entry.szExeFile, n + L".exe")) continue;
                HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
                if (h) running.push_back({entry.th32ProcessID, h, n});
            }
        }
        CloseHandle(snapshot);
    }
    // The window first, so the app stops its session cleanly (the real wheel shown again).
    for (const Running& p : running) EnumWindows(closeWindowOf, static_cast<LPARAM>(p.id));
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    for (const Running& p : running) {
        const ULONGLONG now = GetTickCount64();
        const DWORD left = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
        if (WaitForSingleObject(p.handle, left) != WAIT_OBJECT_0) {
            TerminateProcess(p.handle, 1);
            WaitForSingleObject(p.handle, 5000);
            ended.push_back(p.name);
        }
        CloseHandle(p.handle);
    }
    return static_cast<int>(running.size());
}

bool SetupSystemWin32::helperExists() {
    Service manager, service;
    manager.h = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager.h) return false;
    service.h = OpenServiceW(manager.h, kHelperService, SERVICE_QUERY_STATUS);
    return service.h != nullptr;
}

void SetupSystemWin32::stopHelper() {
    Service manager, service;
    manager.h = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager.h) return;
    service.h = OpenServiceW(manager.h, kHelperService, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!service.h) return;
    SERVICE_STATUS status{};
    if (QueryServiceStatus(service.h, &status) && status.dwCurrentState != SERVICE_STOPPED) {
        ControlService(service.h, SERVICE_CONTROL_STOP, &status);
        waitForState(service.h, SERVICE_STOPPED, 20000);
    }
    // Its process may take a moment longer to go; the program file stays locked until it has.
    const ULONGLONG deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline && !processesNamed({L"wheelbri-helper"}, 0).empty()) Sleep(250);
}

bool SetupSystemWin32::deleteHelper() {
    Service manager, service;
    manager.h = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager.h) return false;
    service.h = OpenServiceW(manager.h, kHelperService, DELETE);
    if (!service.h) return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST;
    return DeleteService(service.h) || GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE;
}

std::wstring SetupSystemWin32::installHelper(const std::wstring& exe) {
    Service manager, service;
    manager.h = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!manager.h) return L"cannot open the service manager (error " + std::to_wstring(GetLastError()) + L")";
    const std::wstring binary = L"\"" + exe + L"\"";   // quoted: the path has spaces (Program Files)
    const DWORD access = SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_QUERY_STATUS;
    service.h = OpenServiceW(manager.h, kHelperService, access);
    if (service.h) {
        if (!ChangeServiceConfigW(service.h, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                  binary.c_str(), nullptr, nullptr, nullptr, L"LocalSystem", nullptr, L"WheelBri Helper")) {
            return L"cannot update the service (error " + std::to_wstring(GetLastError()) + L")";
        }
    } else {
        service.h = CreateServiceW(manager.h, kHelperService, L"WheelBri Helper", access, SERVICE_WIN32_OWN_PROCESS,
                                   SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, binary.c_str(), nullptr, nullptr, nullptr,
                                   nullptr, nullptr);
        if (!service.h) return L"cannot register the service (error " + std::to_wstring(GetLastError()) + L")";
    }
    std::wstring text = L"Lets WheelBri create its virtual wheel and hide your rig from games without running as Administrator.";
    SERVICE_DESCRIPTIONW description{text.data()};
    ChangeServiceConfig2W(service.h, SERVICE_CONFIG_DESCRIPTION, &description);
    // Restarted 5 s after a crash (twice), then after a minute; the count resets after a day.
    SC_ACTION actions[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 60000}};
    SERVICE_FAILURE_ACTIONSW failure{};
    failure.dwResetPeriod = 86400;
    failure.cActions = 3;
    failure.lpsaActions = actions;
    ChangeServiceConfig2W(service.h, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);
    // Also when it stops with an error of its own, not only a crash.
    SERVICE_FAILURE_ACTIONS_FLAG flag{TRUE};
    ChangeServiceConfig2W(service.h, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag);
    if (!StartServiceW(service.h, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        return L"the service did not start (error " + std::to_wstring(GetLastError()) + L")";
    }
    if (!waitForState(service.h, SERVICE_RUNNING, 15000)) return L"the service did not start within 15 s";
    return {};
}

void SetupSystemWin32::startHelper() {
    Service manager, service;
    manager.h = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager.h) return;
    service.h = OpenServiceW(manager.h, kHelperService, SERVICE_START);
    if (service.h) StartServiceW(service.h, 0, nullptr);
}

bool SetupSystemWin32::helperRunning() {
    Service manager, service;
    manager.h = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager.h) return false;
    service.h = OpenServiceW(manager.h, kHelperService, SERVICE_QUERY_STATUS);
    SERVICE_STATUS status{};
    return service.h && QueryServiceStatus(service.h, &status) && status.dwCurrentState == SERVICE_RUNNING;
}

std::uint32_t SetupSystemWin32::windowsBuild() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (auto fn = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))) {
        fn(&info);
    }
    return info.dwBuildNumber;
}

// This program is 64-bit: it only runs on 64-bit Windows.
bool SetupSystemWin32::is64BitWindows() { return sizeof(void*) == 8; }

std::wstring SetupSystemWin32::folder(const Folder which) {
    wchar_t buffer[MAX_PATH]{};
    switch (which) {
    case Folder::SystemRoot: GetWindowsDirectoryW(buffer, MAX_PATH); return withoutTrailingSlash(buffer);
    case Folder::System32: GetSystemDirectoryW(buffer, MAX_PATH); return withoutTrailingSlash(buffer);
    case Folder::ProgramFiles: return knownFolder(FOLDERID_ProgramFiles);
    case Folder::ProgramFilesX86: return knownFolder(FOLDERID_ProgramFilesX86);
    case Folder::ProgramData: return knownFolder(FOLDERID_ProgramData);
    case Folder::CommonPrograms: return knownFolder(FOLDERID_CommonPrograms);
    case Folder::CommonDesktop: return knownFolder(FOLDERID_PublicDesktop);
    case Folder::Temp: GetTempPathW(MAX_PATH, buffer); return withoutTrailingSlash(buffer);
    }
    return {};
}

std::wstring SetupSystemWin32::desktopUserSid() {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    for (const DWORD id : processesNamed({L"explorer"}, session)) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
        if (!process) continue;
        HANDLE token{};
        std::wstring sid;
        if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
            sid = tokenUserSid(token);
            CloseHandle(token);
        }
        CloseHandle(process);
        if (!sid.empty()) return sid;
    }
    HANDLE token{};
    std::wstring sid;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        sid = tokenUserSid(token);
        CloseHandle(token);
    }
    return sid;
}

LocalTime SetupSystemWin32::now() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    return {t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond};
}

}  // namespace wheelbri::setup
