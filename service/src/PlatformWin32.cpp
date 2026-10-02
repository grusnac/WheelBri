// Platform.hpp on Windows.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Platform.hpp"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <thread>

namespace wheelemu::service::platform {

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

void sleepPrecise(const std::uint64_t ns) {
    std::uint64_t current = nowNs();
    const std::uint64_t target = current + ns;
    constexpr std::uint64_t kShortSleep = kNsPerMs;
    std::uint64_t longestSleep = kShortSleep;
    while (current + longestSleep < target) {
        sleepNs(kShortSleep);
        const std::uint64_t now = nowNs();
        longestSleep = std::max(longestSleep, now - current);
        current = now;
    }
    if (current < target && target - current > longestSleep) {
        sleepNs(target - current - longestSleep);
        current = nowNs();
    }
    while (current < target) {
        YieldProcessor();
        current = nowNs();
    }
}

bool raiseThreadPriority(std::string& error) {
    if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != FALSE) {
        return true;
    }
    error = lastError("SetThreadPriority");
    return false;
}

const char* name() { return "Windows"; }

struct ChildProcess::Impl {
    PROCESS_INFORMATION info{};
    bool running = true;
    int exitCode = -1;

    ~Impl() {
        if (info.hProcess != nullptr) CloseHandle(info.hProcess);
        if (info.hThread != nullptr) CloseHandle(info.hThread);
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
    const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | (listOk ? EXTENDED_STARTUPINFO_PRESENT : 0);
    const BOOL created = CreateProcessW(program.c_str(), line.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr,
                                        &startup.StartupInfo, &impl->info);
    const DWORD createError = GetLastError();
    if (listOk) DeleteProcThreadAttributeList(attributes);
    CloseHandle(nul);
    if (created == FALSE) {
        error = "CreateProcess failed (error " + std::to_string(createError) + ")";
        return nullptr;
    }
    return std::unique_ptr<ChildProcess>(new ChildProcess(std::move(impl)));
}

ChildProcess::ChildProcess(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ChildProcess::~ChildProcess() {
    if (impl_->running) {
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

void ChildProcess::kill() {
    if (!impl_->running) {
        return;
    }
    TerminateProcess(impl_->info.hProcess, 1);
    WaitForSingleObject(impl_->info.hProcess, 5000);
    (void)poll();
    impl_->running = false;
}

}  // namespace wheelemu::service::platform
