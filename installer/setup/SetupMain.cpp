// SPDX-License-Identifier: GPL-3.0-or-later
// wheelbri-setup.exe: WheelBri's installer. A wizard (Welcome, Install folder, Options, Ready, Installing, Finish) in
// front of SetupSteps.cpp, which does the work on a thread of its own and reports each step to the checklist and
// the log. Plain Win32, no third-party code, built against msvcrt.dll (not the Universal C Runtime): it starts on a
// bare Windows 7 SP1, 8 or 8.1, and installs the Universal C Runtime the app needs there first (SetupSteps.cpp).
//
// Runs from the unpacked package (its files in their folders), or from Setup.exe's temporary folder, where IExpress
// put the package's files flat next to package-files.txt (scripts/make/package.ps1). Asks for Administrator itself.
//
// wheelbri-setup.exe --uninstall, or this exe under a name starting with "Uninstall" (the install folder's
// Uninstaller.exe): the uninstaller (What to keep, Removing, Finish). Installed in the install folder, which the
// uninstall deletes: it first runs again from a copy in a new folder under %SystemRoot%\Temp that only Administrators
// can change.
//
// --quiet: no window, no questions, the defaults or the options given (SetupLogic.hpp parseOptions); exit code 0 done,
// 1 failed, and everything in the log. --user-data <folder>: whose %LOCALAPPDATA% (the copy that asked for
// Administrator passes the signed-in user's on).

#ifndef UNICODE
#define UNICODE
#endif
#include "SetupLogic.hpp"
#include "SetupSteps.hpp"
#include "SetupSystemWin32.hpp"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace {

// The app's colours (service/src/UiStyle.hpp).
constexpr COLORREF kBackground = RGB(22, 24, 28);
constexpr COLORREF kRail = RGB(29, 32, 38);
constexpr COLORREF kRule = RGB(44, 48, 56);
constexpr COLORREF kField = RGB(34, 37, 43);
constexpr COLORREF kText = RGB(212, 214, 218);
constexpr COLORREF kDim = RGB(168, 173, 182);
constexpr COLORREF kFaint = RGB(138, 143, 152);
constexpr COLORREF kHeading = RGB(108, 182, 255);
constexpr COLORREF kHeadingTint = RGB(29, 48, 71);
constexpr COLORREF kActive = RGB(61, 187, 92);
constexpr COLORREF kCaution = RGB(224, 160, 32);
constexpr COLORREF kCautionBack = RGB(42, 36, 22);
constexpr COLORREF kWarning = RGB(232, 86, 76);
constexpr COLORREF kWarningBack = RGB(46, 27, 28);
constexpr COLORREF kDark = RGB(16, 18, 20);
constexpr COLORREF kButton = RGB(42, 46, 54);
constexpr COLORREF kFrame = RGB(90, 96, 106);
constexpr COLORREF kLog = RGB(16, 18, 21);

enum Page { kWelcome, kFolder, kOptions, kReady, kInstalling, kFinish, kChoose, kPageCount };

enum Id : int {
    kBack = 100, kNext, kCancel, kFolderEdit, kBrowse, kDefaultFolder, kStartMenu, kStartMenuName, kDesktop,
    kFirefox, kLaunch, kChecklist, kLogEdit, kCopyLog, kOpenLogs, kRestartNow, kRestartLater, kKeepSettings,
    kKeepLogs, kKeepShims, kKeepCertificate, kOpenData,
};

constexpr UINT kEventMessage = WM_APP + 1;   // lParam: Event* (from the steps' thread)
constexpr UINT kExitMessage = WM_APP + 2;    // wParam: exit code

namespace setup = wheelbri::setup;

enum class State { Wait, Run, Done, Warn, Fail, Skip };
struct Row {
    const wchar_t* text;
    int step;             // the numbered step it belongs to (SetupSteps.hpp)
    const char* item;     // its item key; nullptr: the row is the step itself (running from its step on)
    State state = State::Wait;
    std::wstring note{};
};
using ShimGame = setup::ShimGame;

// One thing the steps reported (setup::Reporter), handed to the window's thread.
struct Event {
    enum Kind { Step, Item, Note, Warn, Log, Fail, Restart } kind = Log;
    int step = 0;
    std::string key;
    setup::ItemState state = setup::ItemState::Running;
    std::wstring text;
};

struct App {
    HINSTANCE instance{};
    HWND window{};
    double scale = 1.0;
    HFONT font{}, bold{}, big{}, small{};
    HBRUSH backBrush{}, fieldBrush{}, logBrush{}, railBrush{};
    Page page = kWelcome;
    setup::Options options;      // the command line's; the wizard's choices go in at Install / Uninstall
    bool uninstall = false;      // --uninstall
    std::wstring userData;       // the signed-in user's %LOCALAPPDATA% (--user-data), not the Administrator's
    std::wstring tempCopy;       // the folder of the copy this uninstaller runs from (deleted at the next restart)
    std::vector<ShimGame> shimGames;
    bool restartNeeded = false;

    setup::Package package;
    std::wstring version = L"?";
    DWORD build = 0;
    bool withDrivers = false;
    std::wstring earlierVersion, earlierFolder;
    bool firefox = false;
    std::wstring logFolder;

    std::vector<HWND> pageControls[kPageCount];
    HWND back{}, next{}, cancel{}, folderEdit{}, startMenu{}, startMenuName{}, desktop{}, firefoxBox{}, launch{};
    HWND checklist{}, logEdit{}, progress{}, restartNow{}, restartLater{};
    HWND keepSettings{}, keepLogs{}, keepShims{}, keepCertificate{}, openData{};

    std::vector<Row> rows;
    bool running = false, finished = false;
    int exitCode = -1;
    std::vector<std::wstring> warnings;
    std::wstring failure;
    std::wstring logText;
};
App g;

int px(const double v) { return static_cast<int>(v * g.scale + 0.5); }

std::wstring windowText(HWND h) {
    std::wstring s(static_cast<size_t>(GetWindowTextLengthW(h)) + 1, L'\0');
    GetWindowTextW(h, s.data(), static_cast<int>(s.size()));
    s.resize(wcslen(s.c_str()));
    return s;
}

std::wstring environment(const wchar_t* name) {
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD n = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    return n > 0 && n < std::size(buffer) ? std::wstring(buffer) : std::wstring();
}

std::wstring registryText(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buffer[1024]{};
    DWORD size = sizeof(buffer);
    // A 64-bit program reads the 64-bit view (RRF_SUBKEY_WOW6464KEY is Windows 10 only).
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) {
        return {};
    }
    return buffer;
}

std::wstring exeFolder() {
    wchar_t path[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    std::wstring s(path);
    return s.substr(0, s.find_last_of(L'\\'));
}

std::wstring exePath() {
    wchar_t path[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return path;
}

std::wstring system32(const wchar_t* file) {
    wchar_t dir[MAX_PATH]{};
    GetSystemDirectoryW(dir, MAX_PATH);
    return std::wstring(dir) + L"\\" + file;
}

// Runs a command line, hidden, and waits; its exit code (or -1).
int runHidden(std::wstring commandLine) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

bool isElevated() {
    HANDLE token{};
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
                    GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    if (token) CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

DWORD windowsBuild() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (auto fn = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))) {
        fn(&info);
    }
    return info.dwBuildNumber;
}

// The package next to this exe: the unpacked zip, or Setup.exe's flat files with package-files.txt.
bool findPackage(std::wstring& problem) {
    setup::SetupSystemWin32 system;
    g.package = setup::readPackage(system, exeFolder(), exePath());
    if (!g.package.find(L"wheelbri.exe")) {
        problem = L"The WheelBri package is not next to wheelbri-setup.exe: unpack the whole package again.";
        return false;
    }
    g.version = g.package.version;
    return true;
}

void detect() {
    g.build = windowsBuild();
    g.withDrivers = setup::withDrivers(g.build);
    const std::wstring key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\wheelbri";
    g.earlierFolder = registryText(HKEY_LOCAL_MACHINE, key.c_str(), L"InstallLocation");
    g.earlierVersion = registryText(HKEY_LOCAL_MACHINE, key.c_str(), L"DisplayVersion");
    const wchar_t* appPaths = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\firefox.exe";
    HKEY k{};
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        if (RegOpenKeyExW(root, appPaths, 0, KEY_READ, &k) == ERROR_SUCCESS) {
            g.firefox = true;
            RegCloseKey(k);
        }
    }
    if (g.userData.empty()) g.userData = environment(L"LOCALAPPDATA");
    setup::SetupSystemWin32 system;
    bool linked = false;
    g.logFolder = setup::logFolderFor(system, g.userData, linked);
}

std::wstring defaultFolder() {
    if (!g.earlierFolder.empty()) return g.earlierFolder;
    std::wstring programs = environment(L"ProgramW6432");
    if (programs.empty()) programs = environment(L"ProgramFiles");
    return programs + L"\\WheelBri";
}

std::wstring windowsName() { return setup::windowsName(g.build); }

// --- controls -------------------------------------------------------------------------------------------------

HWND add(const Page page, const wchar_t* cls, const wchar_t* text, const DWORD style, const int id, const int x,
         const int y, const int w, const int h, const DWORD exStyle = 0) {
    HWND h1 = CreateWindowExW(exStyle, cls, text, WS_CHILD | style, px(x), px(y), px(w), px(h), g.window,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g.instance, nullptr);
    SendMessageW(h1, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    if (page != kPageCount) g.pageControls[page].push_back(h1);
    return h1;
}

void initRows() {
    if (g.uninstall) {
        g.rows = {
            {L"Close running WheelBri", 1, nullptr},
            {L"Remove WheelBri Helper", 1, "helper"},
            {L"Take the shim out of games", 2, nullptr},
            {L"Remove hiding filter driver", 3, "hide-driver"},
            {L"Remove virtual wheel driver", 3, "wheel-driver"},
            {L"Remove the drivers' certificate", 3, "certificate"},
            {L"Shortcuts, Settings > Apps, registry", 4, nullptr},
            {L"Rig, mappings and settings", 5, nullptr},
            {L"Logs", 6, nullptr},
            {L"Remove program files", 7, nullptr},
        };
        return;
    }
    g.rows = {
        {L"Check this PC", 1, nullptr},
        {L"Close running WheelBri", 2, nullptr},
        {L"Copy program files", 3, nullptr},
        {L"Install virtual wheel driver", 4, "wheel-driver"},
        {L"Install hiding filter driver", 4, "hide-driver"},
        {L"Start WheelBri Helper", 4, "helper"},
        {L"Shortcuts and Start menu", 5, nullptr},
        {L"Add to Settings > Apps", 6, nullptr},
        {L"Check the installation", 7, nullptr},
    };
    if (!g.withDrivers) {
        for (Row& r : g.rows) {
            if (r.item) r.state = State::Skip, r.note = L"not on " + windowsName();
        }
    }
}

void createControls() {
    constexpr int kLeft = 210;   // the page's left edge (the rail is 0..178)
    constexpr int kWidth = 520;
    const DWORD visible = 0;

    // Install folder
    g.folderEdit = add(kFolder, L"EDIT", defaultFolder().c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP | visible,
                       kFolderEdit, kLeft, 132, kWidth - 110, 26);
    add(kFolder, L"BUTTON", L"Browse...", BS_OWNERDRAW | WS_TABSTOP, kBrowse, kLeft + kWidth - 100, 132, 100, 26);
    add(kFolder, L"BUTTON", L"Back to default", BS_OWNERDRAW | WS_TABSTOP, kDefaultFolder, kLeft, 330, 150, 26);

    // Options
    // The Start menu folder is always made (SetupSteps.cpp): only its name is a choice.
    g.startMenu = add(kOptions, L"STATIC", L"Start menu folder:", SS_LEFT, kStartMenu, kLeft, 99, 190, 22);
    g.startMenuName = add(kOptions, L"EDIT", L"WheelBri", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, kStartMenuName,
                          kLeft + 200, 94, 200, 26);
    g.desktop = add(kOptions, L"BUTTON", L"Desktop shortcut", BS_AUTOCHECKBOX | WS_TABSTOP, kDesktop, kLeft, 126, 400,
                    22);
    g.firefoxBox = add(kOptions, L"BUTTON", L"Turn off game controllers in Firefox", BS_AUTOCHECKBOX | WS_TABSTOP,
                       kFirefox, kLeft, 318, 420, 22);
    g.launch = add(kOptions, L"BUTTON", L"Open WheelBri when setup finishes", BS_AUTOCHECKBOX | WS_TABSTOP, kLaunch,
                   kLeft, 400, 420, 22);
    SendMessageW(g.desktop, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(g.launch, BM_SETCHECK, BST_CHECKED, 0);

    // Installing
    g.progress = add(kInstalling, PROGRESS_CLASSW, L"", 0, 0, kLeft, 62, kWidth, 10);
    SendMessageW(g.progress, PBM_SETRANGE32, 0, 1000);
    SendMessageW(g.progress, PBM_SETBARCOLOR, 0, kActive);
    SendMessageW(g.progress, PBM_SETBKCOLOR, 0, kButton);
    g.checklist = add(kInstalling, L"LISTBOX", L"", LBS_OWNERDRAWFIXED | LBS_NOSEL | WS_VSCROLL, kChecklist, kLeft, 82,
                      kWidth, 9 * 21 + 6);
    add(kInstalling, L"BUTTON", L"Copy log", BS_OWNERDRAW, kCopyLog, kLeft + kWidth - 250, 294, 110, 22);
    add(kInstalling, L"BUTTON", L"Open log folder", BS_OWNERDRAW, kOpenLogs, kLeft + kWidth - 130, 294, 130, 22);
    g.logEdit = add(kInstalling, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, kLogEdit,
                    kLeft, 322, kWidth, 150);
    SendMessageW(g.logEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g.small), TRUE);
    SendMessageW(g.logEdit, EM_SETLIMITTEXT, 0, 0);
    for (size_t i = 0; i < g.rows.size(); ++i) SendMessageW(g.checklist, LB_ADDSTRING, 0, static_cast<LPARAM>(i));

    // Finish
    g.restartNow = add(kFinish, L"BUTTON", L"Restart Windows now (save your work first)",
                       BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, kRestartNow, kLeft + 14, 300, 480, 22);
    g.restartLater = add(kFinish, L"BUTTON", L"I'll restart later", BS_AUTORADIOBUTTON, kRestartLater, kLeft + 14, 328,
                         480, 22);
    SendMessageW(g.restartNow, BM_SETCHECK, BST_CHECKED, 0);

    // Uninstall: what to keep
    g.keepSettings = add(kChoose, L"BUTTON", L"Rig, mappings and settings", BS_AUTOCHECKBOX | WS_TABSTOP, kKeepSettings,
                         kLeft, 112, 330, 22);
    g.keepLogs = add(kChoose, L"BUTTON", L"Logs", BS_AUTOCHECKBOX | WS_TABSTOP, kKeepLogs, kLeft, 164, 330, 22);
    // One rule for every box: ticked stays, unticked goes.
    g.keepShims = add(kChoose, L"BUTTON", L"The shim in game folders", BS_AUTOCHECKBOX | WS_TABSTOP, kKeepShims, kLeft,
                      216, 330, 22);
    g.keepCertificate = add(kChoose, L"BUTTON", L"The drivers' certificate", BS_AUTOCHECKBOX | WS_TABSTOP,
                            kKeepCertificate, kLeft, 280, 330, 22);
    SendMessageW(g.keepSettings, BM_SETCHECK, BST_CHECKED, 0);
    g.openData = add(kFinish, L"BUTTON", L"Open folder", BS_OWNERDRAW, kOpenData, kLeft + kWidth - 120, 92, 120, 24);

    // The bottom bar
    g.back = add(kPageCount, L"BUTTON", L"< Back", BS_OWNERDRAW | WS_TABSTOP | WS_VISIBLE, kBack, 470, 524, 92, 30);
    g.next = add(kPageCount, L"BUTTON", L"Next >", BS_OWNERDRAW | WS_TABSTOP | WS_VISIBLE, kNext, 570, 524, 92, 30);
    g.cancel = add(kPageCount, L"BUTTON", L"Cancel", BS_OWNERDRAW | WS_TABSTOP | WS_VISIBLE, kCancel, 670, 524, 92, 30);
}

bool checked(HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }

void showPage(const Page page) {
    g.page = page;
    for (int p = 0; p < kPageCount; ++p) {
        for (HWND h : g.pageControls[p]) ShowWindow(h, p == page ? SW_SHOW : SW_HIDE);
    }
    if (page == kOptions) ShowWindow(g.firefoxBox, g.firefox ? SW_SHOW : SW_HIDE);
    if (page == kChoose) {
        ShowWindow(g.keepShims, g.shimGames.empty() ? SW_HIDE : SW_SHOW);
        ShowWindow(g.keepCertificate, g.withDrivers ? SW_SHOW : SW_HIDE);
    }
    if (page == kFinish) {
        ShowWindow(g.openData, g.uninstall && checked(g.keepSettings) ? SW_SHOW : SW_HIDE);
        const bool askRestart = g.exitCode == 0 && (g.restartNeeded || (!g.uninstall && g.withDrivers));
        ShowWindow(g.restartNow, askRestart ? SW_SHOW : SW_HIDE);
        ShowWindow(g.restartLater, askRestart ? SW_SHOW : SW_HIDE);
    }
    SetWindowTextW(g.next, page == kReady ? L"Install" : page == kChoose ? L"Uninstall" : page == kFinish ? L"Finish"
                                                                                                  : L"Next >");
    EnableWindow(g.back, page != kWelcome && page != kInstalling && page != kFinish && page != kChoose);
    EnableWindow(g.next, page != kInstalling || g.finished);
    EnableWindow(g.cancel, page != kInstalling && page != kFinish);
    ShowWindow(g.back, page == kFinish || g.uninstall ? SW_HIDE : SW_SHOW);
    ShowWindow(g.cancel, page == kFinish ? SW_HIDE : SW_SHOW);
    InvalidateRect(g.window, nullptr, TRUE);
}

// --- the install ------------------------------------------------------------------------------------------------

std::wstring quoted(const std::wstring& s) { return L"\"" + s + L"\""; }

// The folder must be a full path on a local drive; the install does the rest of the checks (empty or an install).
std::wstring folderProblem(const std::wstring& folder) {
    if (folder.size() < 3 || folder[1] != L':' || folder[2] != L'\\') return L"Choose a full folder path (C:\\...).";
    if (folder.find(L'"') != std::wstring::npos) return L"A folder name cannot hold a quote.";
    const std::wstring root = folder.substr(0, 3);
    const UINT type = GetDriveTypeW(root.c_str());
    if (type != DRIVE_FIXED) return L"Choose a folder on a local drive.";
    return {};
}

std::wstring menuNameProblem(const std::wstring& name) {
    if (name.empty()) return L"Give the Start menu folder a name.";
    if (name.find_first_of(L"/:*?\"<>|") != std::wstring::npos || name.find(L"..") != std::wstring::npos) {
        return L"A Start menu folder name cannot hold / : * ? \" < > | or ..";
    }
    return {};
}

// This run's log: "<stem>-<date>-<NN>.log" (the first free number), lines in WheelBri's log format
// (HelperLog::formatLine), UTF-8. No file when the folder cannot take one: the window still shows everything.
class SetupLog {
public:
    void open(const std::wstring& folder, const wchar_t* stem) {
        SYSTEMTIME t{};
        GetLocalTime(&t);
        wchar_t date[16];
        swprintf(date, 16, L"%04u-%02u-%02u", t.wYear, t.wMonth, t.wDay);
        CreateDirectoryW(setup::parentOf(folder).c_str(), nullptr);   // %LOCALAPPDATA%\wheelbri on a new PC
        CreateDirectoryW(folder.c_str(), nullptr);
        for (int n = 1; n < 100 && file_ == INVALID_HANDLE_VALUE; ++n) {
            wchar_t name[64];
            swprintf(name, 64, L"\\%ls-%ls-%02d.log", stem, date, n);
            const std::wstring candidate = folder + name;
            file_ = CreateFileW(candidate.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file_ != INVALID_HANDLE_VALUE) path_ = candidate;
            else if (GetLastError() != ERROR_FILE_EXISTS) break;
        }
    }
    void write(const bool warning, const std::wstring& text) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (file_ == INVALID_HANDLE_VALUE) return;
        SYSTEMTIME t{};
        GetLocalTime(&t);
        char stamp[64];
        snprintf(stamp, sizeof(stamp), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%s] [setup         ] [tid %-6lu] ",
                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, warning ? "warn " : "info ",
                 static_cast<unsigned long>(GetCurrentThreadId()));
        const std::string line = stamp + setup::toUtf8(text) + "\r\n";
        DWORD written = 0;
        WriteFile(file_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }
    [[nodiscard]] const std::wstring& path() const { return path_; }
    ~SetupLog() {
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::wstring path_;
    std::mutex mutex_;
};
SetupLog gLog;
std::atomic<bool> gRestart{false};   // the steps said Windows must restart (read by --quiet)

// The steps' reports: to the log file at once, and to the window (when there is one) as events.
class Reporting final : public setup::Reporter {
public:
    void step(const int number, const std::wstring& text) override { post({Event::Step, number, {}, {}, text}); }
    void item(const char* key, const setup::ItemState state, const std::wstring& note) override {
        gLog.write(state == setup::ItemState::Warn || state == setup::ItemState::Failed,
                   L"    " + setup::fromUtf8(key) + L": " + note);
        post({Event::Item, 0, key, state, note});
    }
    void note(const std::wstring& text) override { post({Event::Note, 0, {}, {}, text}); }
    void warn(const std::wstring& text) override {
        gLog.write(true, L"    ! " + text);
        post({Event::Warn, 0, {}, {}, text});
    }
    void log(const std::wstring& text) override {
        gLog.write(false, text);
        post({Event::Log, 0, {}, {}, text});
    }
    void fail(const std::wstring& text) override {
        gLog.write(true, L"failed: " + text);
        post({Event::Fail, 0, {}, {}, text});
    }
    void restartNeeded() override {
        gRestart = true;
        post({Event::Restart, 0, {}, {}, {}});
    }

private:
    static void post(Event e) {
        if (!g.window) return;
        auto* copy = new Event(std::move(e));
        if (!PostMessageW(g.window, kEventMessage, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
    }
};

int runSteps(const setup::Options& options) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    setup::SetupSystemWin32 system;
    Reporting reporting;
    const int code = options.uninstall ? setup::uninstall(system, options, reporting)
                                       : setup::install(system, g.package, options, reporting);
    CoUninitialize();
    return code;
}

DWORD WINAPI stepsThread(void* p) {
    auto* options = static_cast<setup::Options*>(p);
    const int code = runSteps(*options);
    delete options;
    PostMessageW(g.window, kExitMessage, static_cast<WPARAM>(code), 0);
    return 0;
}

// The log file for this run: with the user's other logs, except an uninstall that removes them (then %TEMP%).
void openLog(const setup::Options& options) {
    if (options.uninstall && options.removeLogs) {
        gLog.open(environment(L"TEMP"), L"wheelbri-uninstall");
    } else {
        setup::SetupSystemWin32 system;
        bool linked = false;
        gLog.open(setup::logFolderFor(system, options.userData, linked), L"wheelbri-install");
    }
}

void appendLog(const std::wstring& line) {
    g.logText += line + L"\r\n";
    const int length = GetWindowTextLengthW(g.logEdit);
    SendMessageW(g.logEdit, EM_SETSEL, length, length);
    SendMessageW(g.logEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>((line + L"\r\n").c_str()));
}

void updateProgress() {
    int done = 0;
    for (const Row& r : g.rows) done += r.state != State::Wait && r.state != State::Run;
    SendMessageW(g.progress, PBM_SETPOS, g.rows.empty() ? 0 : done * 1000 / static_cast<int>(g.rows.size()), 0);
    InvalidateRect(g.checklist, nullptr, TRUE);
    RECT top{px(210), px(36), px(760), px(60)};
    InvalidateRect(g.window, &top, TRUE);
}

// A full path without a trailing backslash ("C:\x\..\y\" -> "C:\y").
std::wstring fullPath(const std::wstring& path) {
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD n = GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(buffer)), buffer, nullptr);
    return setup::withoutTrailingSlash(n > 0 && n < std::size(buffer) ? std::wstring(buffer) : path);
}

// The Start menu folder as typed: no spaces or backslashes around it.
std::wstring menuName(std::wstring name) {
    while (!name.empty() && (name.back() == L' ' || name.back() == L'\\')) name.pop_back();
    while (!name.empty() && (name.front() == L' ' || name.front() == L'\\')) name.erase(0, 1);
    return name;
}

void startInstall() {
    initRows();
    for (size_t i = 0; i < g.rows.size(); ++i) SendMessageW(g.checklist, LB_SETITEMDATA, i, static_cast<LPARAM>(i));
    auto* options = new setup::Options(g.options);
    options->userData = g.userData;
    options->quiet = false;
    if (g.uninstall) {
        options->installDir = g.earlierFolder;
        options->keepConfig = checked(g.keepSettings);
        options->removeSettings = !checked(g.keepSettings);
        options->removeLogs = !checked(g.keepLogs);
        options->keepShims = g.shimGames.empty() || checked(g.keepShims);
        options->removeCertificate = g.withDrivers && !checked(g.keepCertificate);
    } else {
        options->installDir = fullPath(windowText(g.folderEdit));
        options->startMenuFolder = menuName(windowText(g.startMenuName));
        options->desktopShortcut = checked(g.desktop);
        options->stopBrowserGamepads = g.firefox && checked(g.firefoxBox);
    }
    openLog(*options);
    g.running = true;
    HANDLE thread = CreateThread(nullptr, 0, stepsThread, options, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        delete options;
        g.failure = L"Could not start the setup's work.";
        PostMessageW(g.window, kExitMessage, 1, 0);
    }
}

int rowOfStep(const int step) {
    for (size_t i = 0; i < g.rows.size(); ++i) if (g.rows[i].step == step && !g.rows[i].item) return static_cast<int>(i);
    return -1;
}

void finishRunning(const State to) {
    for (Row& r : g.rows) if (r.state == State::Run) r.state = to;
}

void onEvent(const Event& e) {
    switch (e.kind) {
    case Event::Log: appendLog(e.text); return;
    case Event::Step:
        finishRunning(State::Done);
        // The rows of the steps before that nothing named: not needed here.
        for (Row& r : g.rows) {
            if (r.step < e.step && r.state == State::Wait) r.state = r.item ? State::Skip : State::Done;
        }
        if (const int row = rowOfStep(e.step); row >= 0) g.rows[static_cast<size_t>(row)].state = State::Run;
        break;
    case Event::Item:
        for (Row& r : g.rows) {
            if (!r.item || e.key != r.item) continue;
            r.state = e.state == setup::ItemState::Running ? State::Run
                    : e.state == setup::ItemState::Done ? State::Done
                    : e.state == setup::ItemState::Warn ? State::Warn : State::Fail;
            if (r.state != State::Run) r.note = e.text;
        }
        break;
    case Event::Note:
        // On the step's own row (the running item rows of a step say their own).
        for (Row& r : g.rows) if (r.state == State::Run && !r.item) r.note = e.text;
        break;
    case Event::Restart: g.restartNeeded = true; break;
    case Event::Warn:
        g.warnings.push_back(e.text);
        appendLog(L"  ! " + e.text);
        break;
    case Event::Fail:
        g.failure = e.text;
        appendLog(L"  failed: " + e.text);
        finishRunning(State::Fail);
        break;
    }
    updateProgress();
}

void onExit(const int code) {
    g.running = false;
    g.finished = true;
    g.exitCode = code;
    if (code == 0) {
        finishRunning(State::Done);
        for (Row& r : g.rows) if (r.state == State::Wait) r.state = r.item ? State::Skip : State::Done;
        updateProgress();
        showPage(kFinish);
        return;
    }
    finishRunning(State::Fail);
    for (Row& r : g.rows) if (r.state == State::Wait) r.state = State::Skip, r.note = L"not run";
    if (g.failure.empty()) g.failure = L"Setup ended with exit code " + std::to_wstring(code) + L".";
    if (!gLog.path().empty()) appendLog(L"Log: " + gLog.path());
    updateProgress();
    SetWindowTextW(g.next, L"Close");
    EnableWindow(g.next, TRUE);
    InvalidateRect(g.window, nullptr, TRUE);
}

// Opens a program as the signed-in user, not as Administrator: through Explorer, which runs as that user.
void openAsUser(const std::wstring& target) {
    ShellExecuteW(nullptr, L"open", system32(L"..\\explorer.exe").c_str(), quoted(target).c_str(), nullptr,
                  SW_SHOWNORMAL);
}

void finish() {
    const bool askedRestart = g.exitCode == 0 && (g.restartNeeded || (!g.uninstall && g.withDrivers));
    if (askedRestart && checked(g.restartNow)) {
        runHidden(quoted(system32(L"shutdown.exe")) +
                  L" /r /t 2 /c \"WheelBri: restarting so its drivers are installed properly.\" /d p:4:2");
    } else if (!g.uninstall && g.exitCode == 0 && checked(g.launch)) {
        openAsUser(windowText(g.folderEdit) + L"\\wheelbri.exe");
    }
    DestroyWindow(g.window);
}

void browse() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Install WheelBri into");
    if (SUCCEEDED(dialog->Show(g.window))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                std::wstring chosen(path);
                // A chosen folder that is not empty gets a WheelBri folder inside it (the install refuses others).
                const std::wstring tail = chosen.substr(chosen.find_last_of(L'\\') + 1);
                if (_wcsicmp(tail.c_str(), L"WheelBri") != 0) chosen += (chosen.back() == L'\\' ? L"" : L"\\") + std::wstring(L"WheelBri");
                SetWindowTextW(g.folderEdit, chosen.c_str());
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    InvalidateRect(g.window, nullptr, TRUE);
}

void copyLog() {
    if (!OpenClipboard(g.window)) return;
    EmptyClipboard();
    const size_t bytes = (g.logText.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(memory), g.logText.c_str(), bytes);
        GlobalUnlock(memory);
        if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
    }
    CloseClipboard();
}

bool confirmCancel() {
    if (g.uninstall) return true;   // its one page asks nothing that a second question would protect
    return MessageBoxW(g.window, L"Leave WheelBri setup? Nothing has been changed yet.", L"WheelBri Setup",
                       MB_YESNO | MB_ICONQUESTION) == IDYES;
}

void onNext() {
    switch (g.page) {
    case kWelcome: showPage(kFolder); break;
    case kFolder: {
        const std::wstring problem = folderProblem(windowText(g.folderEdit));
        if (!problem.empty()) { MessageBoxW(g.window, problem.c_str(), L"WheelBri Setup", MB_ICONWARNING); return; }
        showPage(kOptions);
        break;
    }
    case kOptions: {
        {
            const std::wstring problem = menuNameProblem(windowText(g.startMenuName));
            if (!problem.empty()) { MessageBoxW(g.window, problem.c_str(), L"WheelBri Setup", MB_ICONWARNING); return; }
        }
        showPage(kReady);
        break;
    }
    case kReady: showPage(kInstalling); startInstall(); break;
    case kChoose: showPage(kInstalling); startInstall(); break;
    case kInstalling: if (g.finished) DestroyWindow(g.window); break;   // after a failure: Close
    case kFinish: finish(); break;
    default: break;
    }
}

// --- painting ---------------------------------------------------------------------------------------------------

void text(HDC dc, const std::wstring& s, const int x, const int y, const int w, const int h, const COLORREF colour,
          HFONT font, const UINT format = DT_LEFT | DT_TOP | DT_WORDBREAK) {
    RECT r{px(x), px(y), px(x + w), px(y + h)};
    SelectObject(dc, font);
    SetTextColor(dc, colour);
    DrawTextW(dc, s.c_str(), -1, &r, format | DT_NOPREFIX);
}

void fill(HDC dc, const int x, const int y, const int w, const int h, const COLORREF colour) {
    RECT r{px(x), px(y), px(x + w), px(y + h)};
    HBRUSH b = CreateSolidBrush(colour);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void notice(HDC dc, const std::wstring& s, const int x, const int y, const int w, const int h, const COLORREF colour,
            const COLORREF back) {
    fill(dc, x, y, w, h, back);
    text(dc, s, x + 10, y + 8, w - 20, h - 12, colour, g.font);
}

struct RailEntry { Page page; const wchar_t* name; };
std::vector<RailEntry> railEntries() {
    if (g.uninstall) return {{kChoose, L"What to keep"}, {kInstalling, L"Removing"}, {kFinish, L"Finish"}};
    return {{kWelcome, L"Welcome"}, {kFolder, L"Install folder"}, {kOptions, L"Options"}, {kReady, L"Ready"},
            {kInstalling, L"Installing"}, {kFinish, L"Finish"}};
}

// The size of a folder's files, all the way down (links are not followed), in bytes.
std::uint64_t folderSize(const std::wstring& folder, const std::vector<std::wstring>& skip = {}) {
    std::uint64_t total = 0;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((folder + L"\\*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") continue;
        if (std::any_of(skip.begin(), skip.end(), [&](const std::wstring& s) { return _wcsicmp(s.c_str(), name.c_str()) == 0; })) continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += folderSize(folder + L"\\" + name);
        else total += (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return total;
}

std::wstring sizeText(const std::uint64_t bytes) {
    wchar_t text[32];
    if (bytes < 1024 * 1024) swprintf(text, 32, L"%.0f KB", static_cast<double>(bytes) / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024) swprintf(text, 32, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else swprintf(text, 32, L"%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    return text;
}

// The games the app put the shim into: "game<TAB>name<TAB>folder" lines of <user data>\wheelbri\shim-games.txt (UTF-8),
// folders that are still there (the uninstall's step 2 reads it the same way).
void readShimGames() {
    setup::SetupSystemWin32 system;
    const auto text = system.readFile(g.userData + L"\\wheelbri\\shim-games.txt");
    if (!text) return;
    for (ShimGame& game : setup::readShimGames(*text)) {
        if (system.isFolder(game.folder) && !system.isLink(game.folder)) g.shimGames.push_back(std::move(game));
    }
}

void paint(HDC dc) {
    RECT client{};
    GetClientRect(g.window, &client);
    HBRUSH back = CreateSolidBrush(kBackground);
    FillRect(dc, &client, back);
    DeleteObject(back);
    SetBkMode(dc, TRANSPARENT);

    // The rail
    fill(dc, 0, 0, 178, 510, kRail);
    fill(dc, 178, 0, 1, 510, kRule);
    const bool failed = g.finished && g.exitCode != 0;
    const std::vector<RailEntry> rail = railEntries();
    int currentIndex = 0;
    for (size_t i = 0; i < rail.size(); ++i) if (rail[i].page == g.page) currentIndex = static_cast<int>(i);
    for (int i = 0; i < static_cast<int>(rail.size()); ++i) {
        const Page p = rail[i].page;
        const int y = 18 + i * 32;
        const bool current = i == currentIndex;
        const bool past = i < currentIndex;
        COLORREF colour = past ? kDim : kFaint;
        std::wstring mark = std::to_wstring(i + 1);
        if (past) mark = L"\u2713";
        if (current) {
            const COLORREF accent = (failed && p == kInstalling) ? kWarning : (p == kFinish ? kActive : kHeading);
            fill(dc, 0, y, 178, 30, failed && p == kInstalling ? kWarningBack : kHeadingTint);
            fill(dc, 0, y, 3, 30, accent);
            colour = accent;
            if (failed && p == kInstalling) mark = L"\u2717";
            if (p == kFinish) mark = L"\u2713";
        }
        text(dc, mark, 14, y + 6, 18, 20, past && !current ? kActive : colour, current ? g.bold : g.font, DT_CENTER);
        text(dc, rail[i].name, 40, y + 6, 136, 20, colour, current ? g.bold : g.font, DT_LEFT | DT_SINGLELINE);
    }

    // The bottom bar
    fill(dc, 0, 510, 780, 1, kRule);
    fill(dc, 0, 511, 780, 60, RGB(26, 29, 34));
    std::wstring footer = L"WheelBri " + g.version + L" \u00b7 64-bit";
    if (g.uninstall) footer = g.userData + L"\\wheelbri";
    if (g.page == kInstalling && g.running) footer = g.uninstall ? L"Can't cancel while it removes." : L"Can't cancel while it installs.";
    text(dc, footer, 16, 530, 440, 20, kFaint, g.small, DT_LEFT | DT_SINGLELINE);

    constexpr int x = 210, w = 540;
    switch (g.page) {
    case kWelcome: {
        text(dc, L"WELCOME TO WHEELBRI " + g.version, x, 26, w, 30, kHeading, g.big);
        text(dc, L"Your rig as a Logitech G25, G27, G920 or G923, so games accept it, with force feedback.", x, 60, w,
             40, kDim, g.font);
        fill(dc, x, 112, w, 150, kRail);
        text(dc, L"WHAT SETUP FOUND", x + 14, 122, w, 18, kDim, g.small);
        const std::wstring os = windowsName() + L", 64-bit: " +
                                (g.withDrivers ? L"full install (Mode Virtual and Mode Shim)"
                                               : L"Mode Shim only (no drivers, no restart)");
        text(dc, L"\u2713", x + 14, 146, 16, 20, kActive, g.font);
        text(dc, os, x + 36, 146, w - 50, 40, kText, g.font);
        if (!g.earlierVersion.empty()) {
            text(dc, L"\u2191", x + 14, 186, 16, 20, kHeading, g.font);
            text(dc, L"WheelBri " + g.earlierVersion + L" in " + g.earlierFolder +
                         L": this upgrades it. Your rig, mappings and settings stay.",
                 x + 36, 186, w - 50, 40, kText, g.font);
        } else {
            text(dc, L"\u2713", x + 14, 186, 16, 20, kActive, g.font);
            text(dc, L"A new install.", x + 36, 186, w - 50, 20, kText, g.font);
        }
        text(dc, L"\u2713", x + 14, 226, 16, 20, kActive, g.font);
        text(dc, L"Running as Administrator (needed for the drivers)", x + 36, 226, w - 50, 20, kText, g.font);
        text(dc, L"WheelBri is free software under the GNU GPL v3 or later (LICENSE.txt), with no warranty. "
                 L"It is not affiliated with or endorsed by Logitech; Logitech and its wheel names are its trademarks.",
             x, 290, w, 60, kDim, g.font);
        text(dc, L"Close WheelBri and any game before you go on. Setup closes a running WheelBri for you.", x, 440, w,
             40, kDim, g.font);
        break;
    }
    case kFolder: {
        text(dc, L"INSTALL FOLDER", x, 26, w, 24, kHeading, g.bold);
        text(dc, L"Where the program, its drivers and the shim files go. Only Administrators can change files there.",
             x, 56, w, 40, kDim, g.font);
        text(dc, L"FOLDER", x, 110, w, 18, kDim, g.small);
        const std::wstring folder = windowText(g.folderEdit);
        ULARGE_INTEGER free{};
        std::wstring space = L"Free on " + folder.substr(0, 2) + L": ";
        if (folder.size() >= 3 && GetDiskFreeSpaceExW(folder.substr(0, 3).c_str(), &free, nullptr, nullptr)) {
            wchar_t gb[32];
            swprintf(gb, 32, L"%.1f GB", static_cast<double>(free.QuadPart) / (1024.0 * 1024.0 * 1024.0));
            space += gb;
        } else {
            space += L"?";
        }
        text(dc, space, x, 170, w, 20, kDim, g.font);
        fill(dc, x, 200, w, 116, kRail);
        const bool upgrade = !g.earlierFolder.empty() && _wcsicmp(folder.c_str(), g.earlierFolder.c_str()) == 0;
        text(dc, upgrade ? L"\u2713  An earlier WheelBri is here: it is upgraded in place."
                         : L"i  A new or empty folder. A folder that holds other files is refused, so the "
                           L"uninstaller never deletes your files.",
             x + 14, 210, w - 28, 40, upgrade ? kActive : kHeading, g.font);
        text(dc, L"i  Outside Program Files, setup locks the folder down the same way (only Administrators may "
                 L"change it).",
             x + 14, 256, w - 28, 50, kDim, g.font);
        break;
    }
    case kOptions: {
        text(dc, L"OPTIONS", x, 26, w, 24, kHeading, g.bold);
        fill(dc, x, 64, w, 22, RGB(32, 36, 43));
        fill(dc, x, 64, 3, 22, kHeading);
        text(dc, L"SHORTCUTS", x + 10, 67, w, 18, kText, g.small);
        fill(dc, x, 164, w, 22, RGB(32, 36, 43));
        fill(dc, x, 164, 3, 22, kHeading);
        text(dc, L"COMPONENTS", x + 10, 167, w, 18, kText, g.small);
        if (g.withDrivers) {
            text(dc, L"\u25a0 App and Mode Shim files\n\u25a0 Virtual wheel and hiding filter drivers\n"
                     L"\u25a0 WheelBri Helper service and the drivers' certificate",
                 x + 4, 194, w, 66, kFaint, g.font);
            text(dc, L"required", x + w - 80, 194, 80, 20, kFaint, g.small, DT_RIGHT);
        } else {
            text(dc, L"\u25a0 App and Mode Shim files  (no drivers on " + windowsName() + L")", x + 4, 194, w, 22,
                 kFaint, g.font);
        }
        fill(dc, x, 286, w, 22, RGB(32, 36, 43));
        fill(dc, x, 286, 3, 22, kHeading);
        text(dc, L"OTHER", x + 10, 289, w, 18, kText, g.small);
        if (g.firefox) {
            text(dc, L"Firefox keeps the wheel open, so WheelBri cannot hide it. The uninstaller turns it back on.",
                 x + 22, 342, w - 22, 40, kFaint, g.small);
        }
        break;
    }
    case kReady: {
        text(dc, L"READY TO INSTALL", x, 26, w, 24, kHeading, g.bold);
        text(dc, L"Check your choices. Back changes them.", x, 56, w, 20, kDim, g.font);
        fill(dc, x, 88, w, 210, kRail);
        struct Kv { std::wstring k, v; };
        const std::wstring folder = windowText(g.folderEdit);
        const bool upgrade = !g.earlierFolder.empty() && _wcsicmp(folder.c_str(), g.earlierFolder.c_str()) == 0;
        const std::vector<Kv> kv = {
            {L"Program", folder + (upgrade ? L"  (upgrade from " + g.earlierVersion + L")" : L"")},
            {L"Settings, logs", g.userData + L"\\wheelbri"},
            {L"Start menu", windowText(g.startMenuName) + L" (WheelBri, uninstall, logs)"},
            {L"Desktop", checked(g.desktop) ? L"shortcut" : L"none"},
            {L"Drivers", g.withDrivers ? L"virtual wheel, hiding filter" : L"none (Mode Shim only)"},
            {L"Service", g.withDrivers ? L"WheelBri Helper (starts with Windows)" : L"none"},
            {L"Firefox", g.firefox && checked(g.firefoxBox) ? L"game controllers off" : L"left as it is"},
            {L"When done", checked(g.launch) ? L"open WheelBri" : L"nothing"},
        };
        for (size_t i = 0; i < kv.size(); ++i) {
            const int y = 98 + static_cast<int>(i) * 24;
            text(dc, kv[i].k, x + 14, y, 140, 20, kDim, g.font, DT_LEFT | DT_SINGLELINE);
            text(dc, kv[i].v, x + 160, y, w - 174, 20, kText, g.font, DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);
        }
        if (g.withDrivers) {
            notice(dc, L"The drivers are signed with WheelBri's own certificate, not Microsoft's. Setup adds it to "
                       L"the certificates this PC trusts; the uninstaller removes it.",
                   x, 316, w, 60, kCaution, kCautionBack);
        }
        break;
    }
    case kChoose: {
        auto band = [&](const wchar_t* title, const int y) {
            fill(dc, x, y, w, 22, RGB(32, 36, 43));
            fill(dc, x, y, 3, 22, kHeading);
            text(dc, title, x + 10, y + 3, w, 18, kText, g.small);
        };
        text(dc, L"REMOVE WHEELBRI " + g.earlierVersion, x, 22, w, 24, kHeading, g.bold);
        text(dc, L"From " + g.earlierFolder + L". The program, drivers, Helper, shortcuts and Settings > Apps entry "
                 L"always go.", x, 46, w, 36, kDim, g.small);
        band(L"TICK WHAT TO KEEP: EVERYTHING ELSE IS REMOVED", 84);
        const std::wstring data = g.userData + L"\\wheelbri";
        text(dc, sizeText(folderSize(data, {L"logs", L"dumps"})), x + w - 100, 114, 100, 20, kFaint, g.small, DT_RIGHT);
        text(dc, L"rigs, configs, per-game values, device names, app.ini", x + 22, 134, w - 22, 18, kFaint, g.small);
        text(dc, sizeText(folderSize(data + L"\\logs") + folderSize(data + L"\\dumps")), x + w - 100, 166, 100, 20,
             kFaint, g.small, DT_RIGHT);
        text(dc, L"app, service, drivers, telemetry, ffb, install, dumps", x + 22, 186, w - 22, 18, kFaint, g.small);
        int y = 216;
        if (!g.shimGames.empty()) {
            std::wstring names;
            for (const ShimGame& game : g.shimGames) names += (names.empty() ? L"" : L", ") + game.name;
            text(dc, std::to_wstring(g.shimGames.size()) + (g.shimGames.size() == 1 ? L" game" : L" games"),
                 x + w - 100, y + 2, 100, 20, kFaint, g.small, DT_RIGHT);
            text(dc, names + L". Removed: another mod's dll gets its name back.", x + 22, y + 22, w - 22, 32, kFaint,
                 g.small, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
        } else {
            text(dc, L"The shim in game folders: no game has it.", x + 22, y + 2, w - 22, 20, kFaint, g.font);
        }
        if (g.withDrivers) {
            text(dc, L"in this PC's trusted certificates (only WheelBri's drivers used it)", x + 22, 302, w - 22, 18,
                 kFaint, g.small);
        }
        text(dc, L"Firefox's game controllers are turned back on if setup turned them off. Other Windows accounts' "
                 L"data stays.", x, 450, w, 40, kFaint, g.small);
        break;
    }
    case kInstalling: {
        const bool bad = g.finished && g.exitCode != 0;
        text(dc, bad ? (g.uninstall ? L"UNINSTALL FAILED" : L"INSTALLATION FAILED")
                     : (g.uninstall ? L"REMOVING" : L"INSTALLING"),
             x, 26, 300, 24, bad ? kWarning : kHeading, g.bold);
        int done = 0;
        for (const Row& r : g.rows) done += r.state != State::Wait && r.state != State::Run;
        text(dc, std::to_wstring(done) + L" of " + std::to_wstring(g.rows.size()), x + w - 200, 30, 200 - 20, 20,
             kDim, g.small, DT_RIGHT | DT_SINGLELINE);
        text(dc, L"LOG", x, 298, 100, 18, kDim, g.small);
        if (bad) notice(dc, g.failure, x, 478, w, 30, kWarning, kWarningBack);
        break;
    }
    case kFinish: {
        if (g.uninstall) {
            text(dc, L"\u2713 WHEELBRI IS REMOVED", x, 26, w, 30, kActive, g.big);
            std::wstring kept, removed = L"program, drivers, Helper, shortcuts";
            if (checked(g.keepSettings)) kept = L"rig, mappings and settings"; else removed += L", settings";
            if (checked(g.keepLogs)) kept += (kept.empty() ? L"" : L", ") + std::wstring(L"logs"); else removed += L", logs";
            if (!g.shimGames.empty()) {
                if (checked(g.keepShims)) kept += (kept.empty() ? L"" : L", ") + std::wstring(L"the shim in games");
                else removed += L", the shim in games";
            }
            if (g.withDrivers) {
                if (checked(g.keepCertificate)) kept += (kept.empty() ? L"" : L", ") + std::wstring(L"certificate");
                else removed += L", certificate";
            }
            fill(dc, x, 70, w, 90, kRail);
            text(dc, L"Kept", x + 14, 96, 100, 20, kDim, g.font);
            text(dc, kept.empty() ? L"nothing" : kept, x + 120, 96, w - 260, 20, kText, g.font);
            text(dc, L"Removed", x + 14, 124, 100, 20, kDim, g.font);
            text(dc, removed, x + 120, 124, w - 134, 36, kText, g.font);
            int y = 176;
            if (!g.warnings.empty()) {
                std::wstring list = std::to_wstring(g.warnings.size()) +
                                    (g.warnings.size() == 1 ? L" thing to look at:" : L" things to look at:");
                for (const std::wstring& line : g.warnings) list += L"\n\u2022 " + line;
                const int h = 34 + 20 * static_cast<int>(std::min<size_t>(g.warnings.size(), 3));
                notice(dc, list, x, y, w, h, kCaution, kCautionBack);
            }
            if (g.restartNeeded) {
                text(dc, L"Windows finishes removing the drivers at the next restart.", x, 276, w, 20, kCaution, g.font);
            }
            if (checked(g.keepSettings)) {
                text(dc, L"Install again any time: your rig and mappings are picked up where they were kept.", x, 470,
                     w, 20, kFaint, g.small);
            }
            break;
        }
        text(dc, L"\u2713 WHEELBRI " + g.version + L" IS INSTALLED", x, 26, w, 30, kActive, g.big);
        int y = 70;
        if (!g.warnings.empty()) {
            std::wstring list = std::to_wstring(g.warnings.size()) +
                                (g.warnings.size() == 1 ? L" thing to look at:" : L" things to look at:");
            for (const std::wstring& line : g.warnings) list += L"\n\u2022 " + line;
            const int h = 34 + 20 * static_cast<int>(std::min<size_t>(g.warnings.size(), 5));
            notice(dc, list, x, y, w, h, kCaution, kCautionBack);
            y += h + 14;
        }
        text(dc, L"NEXT", x, y, w, 18, kDim, g.small);
        text(dc, g.withDrivers ? L"1. Restart Windows, plug in your wheel.\n2. Open WheelBri, set up your rig (Rig "
                                 L"tab).\n3. Press Start: games now see a Logitech wheel."
                               : L"1. Open WheelBri, set up your rig (Rig tab).\n2. Games tab: pick your game, choose "
                                 L"its folder, Use for Start.\n3. Press Start before the game.",
             x, y + 20, w, 70, kText, g.font);
        text(dc, L"Uninstall: Start menu > WheelBri > Uninstaller, or Settings > Apps.", x, 470, w, 20, kFaint,
             g.small);
        break;
    }
    default: break;
    }
}

void drawButton(const DRAWITEMSTRUCT& d) {
    const bool primary = d.CtlID == kNext;
    const bool danger = primary && g.page == kChoose;
    const bool disabled = d.itemState & ODS_DISABLED;
    const bool pressed = d.itemState & ODS_SELECTED;
    COLORREF backColour = danger ? kWarning : primary ? kActive : kButton;
    COLORREF edge = danger ? kWarning : primary ? kActive : kFrame;
    COLORREF fore = primary ? kDark : kText;
    if (d.CtlID == kCopyLog || d.CtlID == kOpenLogs || d.CtlID == kOpenData) {
        backColour = kHeadingTint, edge = kHeadingTint, fore = kHeading;
    }
    if (disabled) backColour = RGB(31, 34, 40), edge = kRule, fore = RGB(107, 112, 121);
    if (pressed && !disabled) backColour = primary ? RGB(48, 160, 76) : kRule;
    HBRUSH b = CreateSolidBrush(backColour);
    FillRect(d.hDC, &d.rcItem, b);
    DeleteObject(b);
    HBRUSH e = CreateSolidBrush(edge);
    FrameRect(d.hDC, &d.rcItem, e);
    DeleteObject(e);
    wchar_t label[64]{};
    GetWindowTextW(d.hwndItem, label, 64);
    SetBkMode(d.hDC, TRANSPARENT);
    SetTextColor(d.hDC, fore);
    SelectObject(d.hDC, primary ? g.bold : g.font);
    RECT r = d.rcItem;
    DrawTextW(d.hDC, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (d.itemState & ODS_FOCUS) {
        InflateRect(&r, -3, -3);
        DrawFocusRect(d.hDC, &r);
    }
}

void drawRow(const DRAWITEMSTRUCT& d) {
    if (d.itemID == static_cast<UINT>(-1) || d.itemID >= g.rows.size()) return;
    const Row& row = g.rows[d.itemID];
    COLORREF back = kRail, fore = kText, mark = kActive;
    const wchar_t* glyph = L"\u25cb";
    switch (row.state) {
    case State::Wait: fore = kFaint; mark = kFaint; glyph = L"\u25cb"; break;
    case State::Run: back = kHeadingTint; fore = kHeading; mark = kHeading; glyph = L"\u25d0"; break;
    case State::Done: glyph = L"\u2713"; break;
    case State::Warn: fore = kCaution; mark = kCaution; glyph = L"!"; break;
    case State::Fail: back = kWarningBack; fore = kWarning; mark = kWarning; glyph = L"\u2717"; break;
    case State::Skip: fore = RGB(107, 112, 121); mark = fore; glyph = L"\u2013"; break;
    }
    HBRUSH b = CreateSolidBrush(back);
    FillRect(d.hDC, &d.rcItem, b);
    DeleteObject(b);
    SetBkMode(d.hDC, TRANSPARENT);
    RECT r = d.rcItem;
    r.left += px(10);
    SelectObject(d.hDC, g.bold);
    SetTextColor(d.hDC, mark);
    RECT m = r;
    m.right = m.left + px(16);
    DrawTextW(d.hDC, glyph, -1, &m, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    r.left += px(26);
    SelectObject(d.hDC, row.state == State::Run || row.state == State::Fail ? g.bold : g.font);
    SetTextColor(d.hDC, fore);
    DrawTextW(d.hDC, row.text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    std::wstring note = row.note;
    if (row.state == State::Run) note = L"running...";
    if (!note.empty()) {
        r.right -= px(10);
        SelectObject(d.hDC, g.small);
        SetTextColor(d.hDC, row.state == State::Run ? kHeading : kFaint);
        DrawTextW(d.hDC, note.c_str(), -1, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        // Double-buffered: no flicker while the checklist updates.
        RECT client{};
        GetClientRect(hwnd, &client);
        HDC memory = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ old = SelectObject(memory, bitmap);
        paint(memory);
        BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old);
        DeleteObject(bitmap);
        DeleteDC(memory);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        const bool log = reinterpret_cast<HWND>(lParam) == g.logEdit;
        SetTextColor(dc, log ? kDim : kText);
        SetBkColor(dc, log ? kLog : kBackground);
        return reinterpret_cast<LRESULT>(log ? g.logBrush : g.backBrush);
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kText);
        SetBkColor(dc, message == WM_CTLCOLORLISTBOX ? kRail : kField);
        return reinterpret_cast<LRESULT>(message == WM_CTLCOLORLISTBOX ? g.railBrush : g.fieldBrush);
    }
    case WM_MEASUREITEM: {
        auto* m = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        m->itemHeight = static_cast<UINT>(px(21));
        return TRUE;
    }
    case WM_DRAWITEM: {
        const auto* d = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (d->CtlType == ODT_BUTTON) drawButton(*d);
        else if (d->CtlType == ODT_LISTBOX) drawRow(*d);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kNext: onNext(); break;
        case kBack: if (g.page > kWelcome) showPage(static_cast<Page>(g.page - 1)); break;
        case kCancel: if (confirmCancel()) DestroyWindow(hwnd); break;
        case kBrowse: browse(); break;
        case kDefaultFolder: SetWindowTextW(g.folderEdit, defaultFolder().c_str()); InvalidateRect(hwnd, nullptr, TRUE); break;
        case kFolderEdit: if (HIWORD(wParam) == EN_CHANGE) { RECT r{px(200), px(160), px(780), px(320)}; InvalidateRect(hwnd, &r, TRUE); } break;
        case kCopyLog: copyLog(); break;
        case kOpenLogs: openAsUser(g.uninstall && !checked(g.keepLogs) ? environment(L"TEMP") : g.logFolder); break;
        case kOpenData: openAsUser(g.userData + L"\\wheelbri"); break;
        default: break;
        }
        return 0;
    case kEventMessage: {
        auto* event = reinterpret_cast<Event*>(lParam);
        onEvent(*event);
        delete event;
        return 0;
    }
    case kExitMessage: onExit(static_cast<int>(wParam)); return 0;
    case WM_CLOSE:
        if (g.running) return 0;   // never in the middle of a driver install
        if (g.page < kInstalling && !confirmCancel()) return 0;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

HFONT makeFont(const int size, const int weight) {
    return CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
}

}  // namespace

// Makes a new folder under %SystemRoot%\Temp that only Administrators and SYSTEM can change, copies this exe into it
// and starts that copy with <arguments>; returns whether it started. The uninstaller runs from there, not from the
// install folder it deletes.
bool runFromTempCopy(const std::wstring& arguments) {
    wchar_t windows[MAX_PATH]{};
    GetWindowsDirectoryW(windows, MAX_PATH);
    GUID guid{};
    CoCreateGuid(&guid);
    wchar_t name[80]{};
    swprintf(name, 80, L"\\Temp\\wheelbri-uninstall-%08lx%04x%04x", static_cast<unsigned long>(guid.Data1), guid.Data2,
             guid.Data3);
    const std::wstring folder = std::wstring(windows) + name;
    if (!CreateDirectoryW(folder.c_str(), nullptr)) return false;   // new: fails if anything is there
    const int locked = runHidden(quoted(system32(L"icacls.exe")) + L" " + quoted(folder) +
                                 L" /inheritance:r /grant:r *S-1-5-32-544:(OI)(CI)F *S-1-5-18:(OI)(CI)F");
    wchar_t self[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    const std::wstring copy = folder + L"\\wheelbri-setup.exe";
    if (locked != 0 || !CopyFileW(self, copy.c_str(), TRUE)) {
        RemoveDirectoryW(folder.c_str());
        return false;
    }
    std::wstring command = quoted(copy) + L" " + arguments + L" --temp-copy";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> arguments;
    for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
    LocalFree(argv);
    std::wstring problem;
    g.options = setup::parseOptions(arguments, problem);
    // Named "Uninstall ...": the uninstaller, without --uninstall.
    if (_wcsnicmp(setup::nameOf(exePath()).c_str(), L"uninstall", 9) == 0) g.options.uninstall = true;
    g.uninstall = g.options.uninstall;
    const bool quiet = g.options.quiet;
    const wchar_t* title = g.uninstall ? L"Uninstall WheelBri" : L"WheelBri Setup";
    auto stop = [&](const std::wstring& text) {
        if (!quiet) MessageBoxW(nullptr, text.c_str(), title, MB_ICONERROR);
        return 1;
    };
    if (!problem.empty()) return stop(problem);
    // Whose data: the user who started it, before Administrator (who may be another account).
    if (g.options.userData.empty()) g.options.userData = environment(L"LOCALAPPDATA");
    g.userData = g.options.userData;
    // Administrator first (the drivers need it): ask once, then this copy ends.
    if (!isElevated()) {
        const std::wstring self = exePath();
        const std::wstring passedOn = setup::optionsCommandLine(g.options);
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS;
        info.lpVerb = L"runas";
        info.lpFile = self.c_str();
        info.lpParameters = passedOn.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info)) return stop(L"WheelBri's setup needs Administrator, and that was refused.");
        // Waits for it: inside Setup.exe, IExpress deletes this folder (and the package) once this copy ends.
        DWORD code = 0;
        if (info.hProcess) {
            WaitForSingleObject(info.hProcess, INFINITE);
            GetExitCodeProcess(info.hProcess, &code);
            CloseHandle(info.hProcess);
        }
        return static_cast<int>(code);
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    g.instance = instance;

    if (g.uninstall && !g.options.tempCopy) {
        // Out of the install folder first; this copy waits for that one (a quiet uninstall's caller wants its result).
        setup::Options copy = g.options;
        copy.tempCopy = false;
        // Its exit code is not this run's result: this copy must end at once, or its exe stays locked in the folder.
        return runFromTempCopy(setup::optionsCommandLine(copy)) ? 0
                                                                : stop(L"Could not prepare the uninstaller's copy under Windows\\Temp.");
    }
    if (g.options.tempCopy) {
        // This copy's own exe and folder: Windows deletes them at the next restart (a running exe cannot delete
        // itself). Asked first thing, so they go even when this copy fails; no helper process is left behind.
        g.tempCopy = exeFolder();
        MoveFileExW(exePath().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        MoveFileExW(g.tempCopy.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);   // after the file, so empty by then
    }
    if (!g.uninstall && !findPackage(problem)) return stop(problem);
    detect();
    if (g.build < 7601) return stop(L"WheelBri needs Windows 7 SP1 or newer.");
    if (g.uninstall) {
        g.version = g.earlierVersion.empty() ? L"?" : g.earlierVersion;
        if (g.earlierFolder.empty()) return stop(L"WheelBri is not installed here (Settings > Apps has no entry for it).");
        readShimGames();
    }

    if (quiet) {
        setup::Options options = g.options;
        if (g.uninstall) {
            options.installDir = setup::withoutTrailingSlash(g.earlierFolder);
        } else {
            options.installDir = fullPath(options.installDir.empty() ? defaultFolder() : options.installDir);
            options.startMenuFolder = menuName(options.startMenuFolder);
        }
        openLog(options);
        const int code = runSteps(options);
        if (code == 0 && !g.uninstall && options.restartNow && (g.withDrivers || gRestart)) {
            runHidden(quoted(system32(L"shutdown.exe")) +
                      L" /r /t 2 /c \"WheelBri: restarting so its drivers are installed properly.\" /d p:4:2");
        }
        CoUninitialize();
        return code;
    }

    HDC screen = GetDC(nullptr);
    g.scale = GetDeviceCaps(screen, LOGPIXELSY) / 96.0;
    ReleaseDC(nullptr, screen);
    g.font = makeFont(14, FW_NORMAL);
    g.bold = makeFont(14, FW_BOLD);
    g.big = makeFont(19, FW_BOLD);
    g.small = makeFont(12, FW_NORMAL);
    g.backBrush = CreateSolidBrush(kBackground);
    g.fieldBrush = CreateSolidBrush(kField);
    g.logBrush = CreateSolidBrush(kLog);
    g.railBrush = CreateSolidBrush(kRail);

    WNDCLASSW wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"WheelBriSetup";
    RegisterClassW(&wc);

    const std::wstring windowTitle = g.uninstall ? L"Uninstall WheelBri " + g.version : L"WheelBri " + g.version + L" Setup";
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT size{0, 0, px(780), px(570)};
    AdjustWindowRect(&size, style, FALSE);
    g.window = CreateWindowExW(0, wc.lpszClassName, windowTitle.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                               size.right - size.left, size.bottom - size.top, nullptr, nullptr, instance, nullptr);
    initRows();
    createControls();
    showPage(g.uninstall ? kChoose : kWelcome);
    ShowWindow(g.window, show);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g.window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CoUninitialize();
    return g.exitCode > 0 ? g.exitCode : 0;
}
