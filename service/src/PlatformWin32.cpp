// Platform.hpp on Windows.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Platform.hpp"


#include <winsock2.h>   // before windows.h; AF_INET for the UDP table
#include <windows.h>
#include <cfgmgr32.h>
#include <commdlg.h>    // GetOpenFileNameW: pickFile
#include <iphlpapi.h>
#include <mmsystem.h>
#include <shlobj.h>     // SHGetKnownFolderPath: the Documents folder
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace wheelbri::service::platform {

namespace {

// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+): a sleep that ends within a fraction of a
// millisecond instead of on the next 1 ms (or 15.6 ms) tick.
constexpr DWORD kHighResolutionTimer = 0x00000002;

std::uint64_t frequency() {
    static const std::uint64_t value = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<std::uint64_t>(f.QuadPart);
    }();
    return value;
}

// One timer per thread (a waitable timer must not be shared by threads waiting at the same time).
HANDLE threadTimer() {
    struct Timer {
        HANDLE handle = nullptr;
        bool tried = false;
        ~Timer() {
            if (handle != nullptr) CloseHandle(handle);
        }
    };
    thread_local Timer timer;
    if (!timer.tried) {
        timer.tried = true;
        timer.handle = CreateWaitableTimerExW(nullptr, nullptr, kHighResolutionTimer, TIMER_ALL_ACCESS);
    }
    return timer.handle;
}

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

// One argument for CreateProcess's command line, quoted the way the C runtime splits it again.
void appendArgument(std::wstring& line, const std::wstring& arg) {
    if (!line.empty()) line += L' ';
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
        line += arg;
        return;
    }
    line += L'"';
    std::size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            line.append(backslashes * 2 + 1, L'\\');
            line += L'"';
            backslashes = 0;
        } else {
            line.append(backslashes, L'\\');
            line += c;
            backslashes = 0;
        }
    }
    line.append(backslashes * 2, L'\\');
    line += L'"';
}

std::string lastError(const char* what) {
    return std::string(what) + " failed (error " + std::to_string(GetLastError()) + ")";
}

}  // namespace

void init() {
    timeBeginPeriod(1);   // 1 ms scheduler ticks, for Sleep and everything else in the process
    (void)frequency();
}

std::uint64_t nowNs() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    const auto ticks = static_cast<std::uint64_t>(counter.QuadPart);
    const std::uint64_t f = frequency();
    return ticks / f * kNsPerSecond + ticks % f * kNsPerSecond / f;
}

void sleepNs(const std::uint64_t ns) {
    if (ns == 0) {
        SwitchToThread();
        return;
    }
    if (const HANDLE timer = threadTimer()) {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(ns / 100 > 0 ? ns / 100 : 1);   // relative, 100 ns units
        if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0) != FALSE) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    Sleep(static_cast<DWORD>((ns + kNsPerMs - 1) / kNsPerMs));
}

// --- WaitSet: WaitForMultipleObjects on the devices' events, the sockets' events and a high-resolution timer ---

struct WaitSet::Impl {
    std::vector<HANDLE> handles;        // what is waited for; the timer is added at the end of each wait
    std::vector<bool> ownedSocketEvent; // the handle is a socket's event (WSACreateEvent): reset after each wait
    std::vector<SOCKET> sockets;        // the sockets those belong to (INVALID_SOCKET for device events)
    HANDLE timer = nullptr;
    std::string failure;

    ~Impl() {
        for (std::size_t i = 0; i < handles.size(); ++i) {
            if (!ownedSocketEvent[i]) continue;
            WSAEventSelect(sockets[i], nullptr, 0);   // the socket stays non-blocking, as it was
            WSACloseEvent(handles[i]);
        }
        if (timer != nullptr) CloseHandle(timer);
    }
};

WaitSet::WaitSet() : impl_(std::make_unique<Impl>()) {
    impl_->timer = CreateWaitableTimerExW(nullptr, nullptr, kHighResolutionTimer, TIMER_ALL_ACCESS);
}

WaitSet::~WaitSet() = default;

int WaitSet::addEvent(const std::intptr_t source) {
    const auto handle = reinterpret_cast<HANDLE>(source);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE || impl_->handles.size() + 2 > MAXIMUM_WAIT_OBJECTS) {
        return -1;
    }
    impl_->handles.push_back(handle);
    impl_->ownedSocketEvent.push_back(false);
    impl_->sockets.push_back(INVALID_SOCKET);
    return static_cast<int>(impl_->handles.size()) - 1;
}

int WaitSet::addSocket(const std::intptr_t socket) {
    const auto s = static_cast<SOCKET>(socket);
    if (s == INVALID_SOCKET || impl_->handles.size() + 2 > MAXIMUM_WAIT_OBJECTS) return -1;
    const WSAEVENT event = WSACreateEvent();
    if (event == WSA_INVALID_EVENT) return -1;
    if (WSAEventSelect(s, event, FD_READ) != 0) {
        WSACloseEvent(event);
        return -1;
    }
    impl_->handles.push_back(event);
    impl_->ownedSocketEvent.push_back(true);
    impl_->sockets.push_back(s);
    return static_cast<int>(impl_->handles.size()) - 1;
}

namespace {

// A datagram waits on the socket, an empty one too. Not FIONREAD: it gives the size of the first datagram, so an
// empty one (any local program can send it) would read as "nothing" and hide the datagrams queued behind it until
// the timer. A one-byte peek leaves the datagram queued: it succeeds, or says WSAEMSGSIZE for a longer one.
bool hasDatagram(const SOCKET s) {
    char byte = 0;
    if (recv(s, &byte, 1, MSG_PEEK) != SOCKET_ERROR) return true;
    return WSAGetLastError() == WSAEMSGSIZE;
}

// A wait that keeps waking for nothing (an event set again and again with an empty queue) gives up after this many
// rounds and lets the loop run a pass: never a spin inside one wait.
constexpr int kMaxStaleWakes = 64;

}  // namespace

int WaitSet::waitUntil(const std::uint64_t deadlineNs) {
    std::vector<HANDLE>& handles = impl_->handles;
    // A socket's event says only that FD_READ was posted at some point: Winsock posts it again on each recv that
    // leaves data behind, so after a drain it is often still set, and resetting it for one socket must not hide
    // another socket's unread datagram. So the queue decides: reset each socket's event first, then look at its queue
    // (a datagram arriving after the reset sets the event again, one before it is seen in the queue), and a wake by a
    // socket's event whose queue is empty is stale: wait again for what is left of the time.
    for (int round = 0; round < kMaxStaleWakes; ++round) {
        for (std::size_t i = 0; i < handles.size(); ++i) {
            if (!impl_->ownedSocketEvent[i]) continue;
            WSAResetEvent(handles[i]);
            if (hasDatagram(impl_->sockets[i])) return static_cast<int>(i);
        }

        const std::uint64_t now = nowNs();
        const std::uint64_t remainingNs = deadlineNs > now ? deadlineNs - now : 0;
        DWORD timeoutMs = 0;
        bool withTimer = false;
        if (remainingNs > 0) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(remainingNs / 100 > 0 ? remainingNs / 100 : 1);   // relative, 100 ns
            withTimer =
                impl_->timer != nullptr && SetWaitableTimerEx(impl_->timer, &due, 0, nullptr, nullptr, nullptr, 0);
            // Without a high-resolution timer (Windows before 10 1803): whole milliseconds, rounded up.
            timeoutMs = withTimer ? INFINITE : static_cast<DWORD>((remainingNs + kNsPerMs - 1) / kNsPerMs);
        }
        if (withTimer) handles.push_back(impl_->timer);
        const DWORD result =
            WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, timeoutMs);
        if (withTimer) handles.pop_back();

        if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + handles.size()) {
            const auto index = static_cast<std::size_t>(result - WAIT_OBJECT_0);
            if (!impl_->ownedSocketEvent[index]) return static_cast<int>(index);   // a device's (auto-reset) event
            if (remainingNs == 0) {
                // Nothing left to wait for: only the queues count (checked again, the datagram may just have come).
                for (std::size_t i = 0; i < handles.size(); ++i) {
                    if (impl_->ownedSocketEvent[i] && hasDatagram(impl_->sockets[i])) return static_cast<int>(i);
                }
                return kTimedOut;
            }
            continue;   // the top of the loop resets the events and looks at the queues
        }
        if (result == WAIT_FAILED) {
            impl_->failure = lastError("WaitForMultipleObjects");
            return kFailed;
        }
        return kTimedOut;   // WAIT_TIMEOUT, the timer (index handles.size()), or WAIT_ABANDONED (never for these)
    }
    return kTimedOut;
}

int WaitSet::size() const { return static_cast<int>(impl_->handles.size()); }

const std::string& WaitSet::failureText() const { return impl_->failure; }

void nameThisThread(const char* const name) {
    using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static const auto setDescription = [] {
        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        return kernel != nullptr
                   ? reinterpret_cast<SetDescription>(reinterpret_cast<void*>(GetProcAddress(kernel, "SetThreadDescription")))
                   : nullptr;
    }();
    if (setDescription == nullptr || name == nullptr) return;
    wchar_t wide[64] = {};
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 63) == 0) return;
    setDescription(GetCurrentThread(), wide);
}

bool raiseThreadPriority(std::string& error) {
    if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != FALSE) {
        return true;
    }
    error = lastError("SetThreadPriority");
    return false;
}

const char* name() { return "Windows"; }

std::uint32_t windowsBuild() {
    // RtlGetVersion from ntdll: the real version (GetVersionEx says Windows 8 to a program without a manifest).
    using GetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    static const std::uint32_t build = [] {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto get = ntdll == nullptr ? nullptr
                                          : reinterpret_cast<GetVersion>(
                                                reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
        OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = sizeof(info);
        return get != nullptr && get(&info) == 0 ? static_cast<std::uint32_t>(info.dwBuildNumber) : 0U;
    }();
    return build;
}

std::filesystem::path systemFolder() {
    wchar_t folder[MAX_PATH] = {};
    const UINT size = GetSystemDirectoryW(folder, MAX_PATH);
    if (size == 0 || size >= MAX_PATH) return std::filesystem::path(L"C:\\Windows\\System32");
    return std::filesystem::path(std::wstring(folder, size));
}

std::optional<std::uint32_t> udpPortOwner(const std::uint16_t port) {
    // The table can grow between the size query and the read: a few tries.
    std::vector<std::uint8_t> buffer;
    for (int attempt = 0; attempt < 4; ++attempt) {
        DWORD size = static_cast<DWORD>(buffer.size());
        const DWORD status = GetExtendedUdpTable(buffer.empty() ? nullptr : buffer.data(), &size, FALSE, AF_INET,
                                                 UDP_TABLE_OWNER_PID, 0);
        if (status == ERROR_INSUFFICIENT_BUFFER) {
            buffer.resize(static_cast<std::size_t>(size) + 16 * sizeof(MIB_UDPROW_OWNER_PID));
            continue;
        }
        if (status != NO_ERROR || buffer.empty()) return std::nullopt;
        const auto* table = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(buffer.data());
        const std::uint16_t wanted = static_cast<std::uint16_t>((port >> 8) | (port << 8));   // network byte order
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const MIB_UDPROW_OWNER_PID& row = table->table[i];
            if (static_cast<std::uint16_t>(row.dwLocalPort & 0xFFFFU) == wanted) return row.dwOwningPid;
        }
        return 0U;
    }
    return std::nullopt;
}

std::optional<bool> udpPortIsOurUser(std::uint16_t /*port*/) { return std::nullopt; }

namespace {

// Why the last processImage() on this thread came back empty (for the log).
thread_local std::string tImageError;

// \Device\HarddiskVolume3\Windows\... as C:\Windows\...: the drive whose device the path starts with; empty if none.
std::filesystem::path dosPathFromNtPath(const std::wstring& ntPath) {
    wchar_t drive[3] = L"A:";
    std::wstring target(MAX_PATH, L'\0');
    for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
        drive[0] = letter;
        const DWORD length = QueryDosDeviceW(drive, target.data(), static_cast<DWORD>(target.size()));
        if (length == 0) continue;
        const std::wstring device(target.c_str());   // the first of the list QueryDosDevice returns
        if (!device.empty() && ntPath.size() > device.size() && ntPath.compare(0, device.size(), device) == 0 &&
            ntPath[device.size()] == L'\\') {
            return std::filesystem::path(std::wstring(drive) + ntPath.substr(device.size()));
        }
    }
    return {};
}

// The program of a process this one cannot open or read. Seen on 2026-10-05 after a fresh install and restart: the
// program of WUDFHost.exe (the virtual G920's driver host, LocalService) could not be read, so the service refused
// all of the game's force feedback. NtQuerySystemInformation(SystemProcessIdInformation) names any process without
// opening it (Task Manager does the same for protected ones).
std::filesystem::path processImageWithoutOpening(const std::uint32_t pid) {
    using Query = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
    static const Query query = [] {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll == nullptr ? nullptr
                                : reinterpret_cast<Query>(
                                      reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQuerySystemInformation")));
    }();
    if (query == nullptr) {
        tImageError += "; NtQuerySystemInformation not found";
        return {};
    }
    struct CountedString {
        USHORT length;
        USHORT maximumLength;
        PWSTR buffer;
    };
    struct ProcessIdInformation {
        HANDLE processId;
        CountedString imageName;
    };
    constexpr ULONG kSystemProcessIdInformation = 88;
    std::wstring name(32767, L'\0');
    ProcessIdInformation info{reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(pid)),
                              {0, static_cast<USHORT>(name.size() * sizeof(wchar_t)), name.data()}};
    if (const LONG status = query(kSystemProcessIdInformation, &info, sizeof(info), nullptr); status < 0) {
        char text[64];
        std::snprintf(text, sizeof(text), "; by id: status 0x%08lX", static_cast<unsigned long>(status));
        tImageError += text;
        return {};
    }
    std::filesystem::path path =
        dosPathFromNtPath(std::wstring(info.imageName.buffer, info.imageName.length / sizeof(wchar_t)));
    if (path.empty()) tImageError += "; by id: no drive letter for its path";
    return path;
}

}  // namespace

std::optional<std::vector<RunningProgram>> runningPrograms() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return std::nullopt;
    std::vector<RunningProgram> programs;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    BOOL more = Process32FirstW(snapshot, &entry);
    if (more == FALSE) {   // even the system's own processes are missing: the list says nothing
        CloseHandle(snapshot);
        return std::nullopt;
    }
    for (; more != FALSE; more = Process32NextW(snapshot, &entry)) {
        entry.szExeFile[MAX_PATH - 1] = L'\0';   // never read past the array, whatever the system left in it
        const int length = WideCharToMultiByte(CP_UTF8, 0, entry.szExeFile, -1, nullptr, 0, nullptr, nullptr);
        if (length <= 1) continue;
        std::string name(static_cast<std::size_t>(length - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, entry.szExeFile, -1, name.data(), length, nullptr, nullptr);
        programs.push_back(RunningProgram{static_cast<std::uint32_t>(entry.th32ProcessID), std::move(name)});
    }
    CloseHandle(snapshot);
    return programs;
}

std::optional<std::filesystem::path> pickFile(const std::string& title, const std::filesystem::path& folder,
                                              const std::string& fileName) {
    std::wstring file(32768, L'\0');   // long paths
    const std::wstring name = widen(fileName);
    std::copy_n(name.begin(), std::min(name.size(), file.size() - 1), file.begin());
    const std::wstring wideTitle = widen(title);
    const std::wstring start = folder.wstring();
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetActiveWindow();   // the app's window, called from its thread
    dialog.lpstrFilter = L"All files\0*\0";
    dialog.lpstrFile = file.data();
    dialog.nMaxFile = static_cast<DWORD>(file.size());
    dialog.lpstrInitialDir = start.empty() ? nullptr : start.c_str();
    dialog.lpstrTitle = wideTitle.c_str();
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT |
                   OFN_NODEREFERENCELINKS | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (GetOpenFileNameW(&dialog) == FALSE) return std::nullopt;   // cancelled (or failed: CommDlgExtendedError)
    return std::filesystem::path(file.c_str());
}

bool canPickFile() { return true; }

std::filesystem::path documentsFolder() {
    PWSTR folder = nullptr;
    std::filesystem::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &folder)) && folder != nullptr) {
        path = folder;
    }
    CoTaskMemFree(folder);
    return path;
}

std::filesystem::path processImage(const std::uint32_t pid) {
    tImageError.clear();
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        tImageError = lastError("OpenProcess");
        return processImageWithoutOpening(pid);
    }
    // MAX_PATH-sized first (almost every program), the longest path Windows allows only if that is too short.
    std::wstring path(MAX_PATH, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    BOOL ok = QueryFullProcessImageNameW(process, 0, path.data(), &size);
    if (ok == FALSE && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        path.assign(32768, L'\0');
        size = static_cast<DWORD>(path.size());
        ok = QueryFullProcessImageNameW(process, 0, path.data(), &size);
    }
    if (ok == FALSE) tImageError = lastError("QueryFullProcessImageName");
    CloseHandle(process);
    if (ok == FALSE) return processImageWithoutOpening(pid);
    path.resize(size);
    return std::filesystem::path(path);
}

std::string lastProcessImageError() { return tImageError; }

std::filesystem::path executablePath() {
    std::wstring path(MAX_PATH, L'\0');
    while (true) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return {};
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

struct SingleInstance::Impl {
    HANDLE mutex = nullptr;
    ~Impl() {
        if (mutex != nullptr) {
            ReleaseMutex(mutex);
            CloseHandle(mutex);
        }
    }
};

namespace {

// The other copies of WheelBri running now, for the message when one holds the lock: "wheelbri.exe, pid 23852,
// D:\projects\WheelBri\build\service\wheelbri.exe" (2026-11-02: a copy started from the build folder was still
// running, and "stop it first" did not say which). Empty when none is listed.
std::string otherCopies() {
    const auto programs = runningPrograms();
    if (!programs) return {};
    const auto self = static_cast<std::uint32_t>(GetCurrentProcessId());
    std::string text;
    for (const RunningProgram& program : *programs) {
        std::string lower = program.name;
        for (char& c : lower) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        if (program.pid == self || (lower != "wheelbri.exe" && lower != "wheelbri-service.exe")) continue;
        const std::u8string image = processImage(program.pid).u8string();   // u8: a path in any language
        text += (text.empty() ? "" : "; ") + program.name + ", pid " + std::to_string(program.pid) +
                (image.empty() ? std::string() : ", " + std::string(image.begin(), image.end()));
    }
    return text;
}

}  // namespace

std::unique_ptr<SingleInstance> SingleInstance::acquire(const std::string& name, std::string& error) {
    // Global\: one per computer, across user sessions (the app runs elevated, a console may not).
    const std::wstring wide = L"Global\\" + widen(name);
    HANDLE mutex = CreateMutexW(nullptr, FALSE, wide.c_str());
    if (mutex == nullptr) {
        // An elevated copy's lock cannot be opened by a non-elevated one: that copy is running.
        if (GetLastError() != ERROR_ACCESS_DENIED) {
            error = lastError("creating the single-instance lock");
            return nullptr;
        }
        const std::string copies = otherCopies();
        error = "another wheelbri-service is already running on this computer (as Administrator" +
                (copies.empty() ? std::string() : ": " + copies) + "): stop it first";
        return nullptr;
    }
    const DWORD waited = WaitForSingleObject(mutex, 0);
    if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) {   // abandoned: the last owner crashed, fine
        CloseHandle(mutex);
        const std::string copies = otherCopies();
        error = "another wheelbri-service is already running on this computer (" +
                (copies.empty() ? std::string("the app, or a console") : copies) + "): stop it first";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->mutex = mutex;
    return std::unique_ptr<SingleInstance>(new SingleInstance(std::move(impl)));
}

SingleInstance::SingleInstance(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SingleInstance::~SingleInstance() = default;

namespace {
void (*gRequestStop)() = nullptr;
bool (*gBusy)() = nullptr;

BOOL WINAPI onConsoleEvent(const DWORD event) {
    if (gRequestStop != nullptr) gRequestStop();
    if (event == CTRL_C_EVENT) return TRUE;   // the process goes on and stops by itself
    // The process ends when this returns: give the session time to undo itself first.
    for (int waited = 0; waited < 4500 && gBusy != nullptr && gBusy(); waited += 50) Sleep(50);
    return TRUE;
}
}  // namespace

void installConsoleStopHandler(void (*requestStop)(), bool (*busy)()) {
    gRequestStop = requestStop;
    gBusy = busy;
    SetConsoleCtrlHandler(onConsoleEvent, TRUE);
}

bool isElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE &&
                          elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

struct ChildProcess::Impl {
    PROCESS_INFORMATION info{};
    HANDLE job = nullptr;   // kill-on-close: the child and everything it started end with it
    bool running = true;
    bool detached = false;   // keepRunningAfterExit(): never killed by this object
    int exitCode = -1;

    ~Impl() {
        if (info.hProcess != nullptr) CloseHandle(info.hProcess);
        if (info.hThread != nullptr) CloseHandle(info.hThread);
        if (job != nullptr) CloseHandle(job);
    }
};

std::unique_ptr<ChildProcess> ChildProcess::start(const std::vector<std::string>& argv, std::string& error) {
    if (argv.empty()) {
        error = "no program";
        return nullptr;
    }
    std::wstring line;
    for (const std::string& arg : argv) {
        appendArgument(line, widen(arg));
    }
    const std::wstring program = widen(argv.front());

    // Input and output to NUL, and only that handle inherited (not the service's sockets or files).
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    const HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                   OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) {
        error = lastError("opening NUL");
        return nullptr;
    }
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<unsigned char> attributeBuffer(attributeSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
    HANDLE inherited[] = {nul};
    const bool listOk =
        InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize) != FALSE &&
        UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
                                  nullptr, nullptr) != FALSE;

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nul;
    startup.StartupInfo.hStdOutput = nul;
    startup.StartupInfo.hStdError = nul;
    startup.lpAttributeList = listOk ? attributes : nullptr;

    auto impl = std::make_unique<Impl>();
    impl->job = CreateJobObjectW(nullptr, nullptr);
    if (impl->job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(impl->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            CloseHandle(impl->job);
            impl->job = nullptr;
        }
    }
    // Suspended until it is in the job, so nothing it starts can escape the job.
    const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED |
                        (listOk ? EXTENDED_STARTUPINFO_PRESENT : 0);
    const BOOL created = CreateProcessW(program.c_str(), line.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr,
                                        &startup.StartupInfo, &impl->info);
    const DWORD createError = GetLastError();
    if (listOk) DeleteProcThreadAttributeList(attributes);
    CloseHandle(nul);
    if (created == FALSE) {
        error = "CreateProcess failed (error " + std::to_string(createError) + ")";
        return nullptr;
    }
    if (impl->job != nullptr && !AssignProcessToJobObject(impl->job, impl->info.hProcess)) {
        CloseHandle(impl->job);   // runs without a job: kill() then ends only the child itself
        impl->job = nullptr;
    }
    ResumeThread(impl->info.hThread);
    return std::unique_ptr<ChildProcess>(new ChildProcess(std::move(impl)));
}

ChildProcess::ChildProcess(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ChildProcess::~ChildProcess() {
    if (impl_->running && !impl_->detached) {
        kill();
    }
}

std::optional<int> ChildProcess::poll() {
    if (impl_->running) {
        if (WaitForSingleObject(impl_->info.hProcess, 0) != WAIT_OBJECT_0) {
            return std::nullopt;
        }
        DWORD code = 0;
        GetExitCodeProcess(impl_->info.hProcess, &code);
        impl_->exitCode = static_cast<int>(code);
        impl_->running = false;
    }
    return impl_->exitCode;
}

void ChildProcess::release() {
    if (impl_->job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};   // no kill-on-close any more
        SetInformationJobObject(impl_->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    impl_->running = false;
}

void ChildProcess::keepRunningAfterExit() {
    if (impl_->job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};   // no kill-on-close any more
        SetInformationJobObject(impl_->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    impl_->detached = true;
}

void ChildProcess::kill() {
    if (!impl_->running) {
        return;
    }
    if (impl_->job == nullptr || !TerminateJobObject(impl_->job, 1)) {
        TerminateProcess(impl_->info.hProcess, 1);
    }
    WaitForSingleObject(impl_->info.hProcess, 5000);
    (void)poll();
    impl_->running = false;
}

// --- FolderWatch: not used on Windows (DeviceChanges uses the system's device notices) ---

struct FolderWatch::Impl {};

std::unique_ptr<FolderWatch> FolderWatch::create(const std::filesystem::path&, std::function<void()>) {
    return nullptr;
}

FolderWatch::FolderWatch(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

FolderWatch::~FolderWatch() = default;

// --- DeviceChanges: Windows' notifications of HID device interfaces coming and going ---

namespace {

// GUID_DEVINTERFACE_HID (hidclass.h), spelled out so no GUID has to be instantiated here.
constexpr GUID kHidInterfaces = {0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};

// Called by Windows on a thread of its own: only counts (DeviceChanges), never lists or waits here.
DWORD CALLBACK onHidInterfaceChange(HCMNOTIFICATION, PVOID context, const CM_NOTIFY_ACTION action,
                                    PCM_NOTIFY_EVENT_DATA, DWORD) {
    if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL || action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) {
        (*static_cast<const std::function<void()>*>(context))();
    }
    return ERROR_SUCCESS;
}

}  // namespace

namespace detail {

bool watchDeviceChanges(std::function<void()> onChange) {
    // CM_Register_Notification (Windows 8 and later) is looked up: not every SDK's cfgmgr32.h declares it (MinGW's).
    using Register = CONFIGRET(WINAPI*)(PCM_NOTIFY_FILTER, PVOID, PCM_NOTIFY_CALLBACK, PHCMNOTIFICATION);
    const HMODULE library = LoadLibraryExW(L"cfgmgr32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (library == nullptr) return false;
    const auto registerNotification =
        reinterpret_cast<Register>(reinterpret_cast<void*>(GetProcAddress(library, "CM_Register_Notification")));
    if (registerNotification == nullptr) return false;
    CM_NOTIFY_FILTER filter{};
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    filter.u.DeviceInterface.ClassGuid = kHidInterfaces;
    // Kept, as the registration, for the life of the process (DeviceChanges is never destroyed).
    auto* const context = new std::function<void()>(std::move(onChange));
    HCMNOTIFICATION handle = nullptr;
    if (registerNotification(&filter, context, onHidInterfaceChange, &handle) != CR_SUCCESS) {
        delete context;
        return false;
    }
    return true;
}

}  // namespace detail

}  // namespace wheelbri::service::platform
