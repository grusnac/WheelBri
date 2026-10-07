// Platform.hpp on Windows.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Platform.hpp"

#include "wheelbri/Pacing.hpp"

#include <winsock2.h>   // before windows.h; AF_INET for the UDP table
#include <windows.h>
#include <iphlpapi.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
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

namespace {
std::atomic<std::uint64_t> gLastSpinMarginNs{0};   // what sleepPrecise used last, for the log
}  // namespace

void sleepPrecise(const std::uint64_t ns) {
    thread_local wheelbri::SpinMargin margin;
    const std::uint64_t start = nowNs();
    const std::uint64_t target = start + ns;
    if (ns > margin.ns()) {
        const std::uint64_t wakeAt = target - margin.ns();
        sleepNs(wakeAt - start);
        const std::uint64_t woke = nowNs();
        margin.observe(woke > wakeAt ? woke - wakeAt : 0);
        gLastSpinMarginNs.store(margin.ns(), std::memory_order_relaxed);
    }
    while (nowNs() < target) {
        YieldProcessor();
    }
}

std::uint64_t preciseSleepMarginNs() {
    return gLastSpinMarginNs.load(std::memory_order_relaxed);
}

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

std::unique_ptr<SingleInstance> SingleInstance::acquire(const std::string& name, std::string& error) {
    // Global\: one per computer, across user sessions (the app runs elevated, a console may not).
    const std::wstring wide = L"Global\\" + widen(name);
    HANDLE mutex = CreateMutexW(nullptr, FALSE, wide.c_str());
    if (mutex == nullptr) {
        // An elevated copy's lock cannot be opened by a non-elevated one: that copy is running.
        error = GetLastError() == ERROR_ACCESS_DENIED
                    ? "another wheelbri-service is already running on this computer (as Administrator): stop it first"
                    : lastError("creating the single-instance lock");
        return nullptr;
    }
    const DWORD waited = WaitForSingleObject(mutex, 0);
    if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) {   // abandoned: the last owner crashed, fine
        CloseHandle(mutex);
        error = "another wheelbri-service is already running on this computer (the app, or a console): stop it first";
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

}  // namespace wheelbri::service::platform
