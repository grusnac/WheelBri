#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The little the service needs from the operating system besides the wheel and the window: a monotonic
// clock, precise sleeps for the 1 kHz loop, a higher thread priority, and starting a compressor for old log
// files. PlatformWin32.cpp on Windows, PlatformPosix.cpp elsewhere (Linux, macOS).
namespace wheelbri::service::platform {

inline constexpr std::uint64_t kNsPerMs = 1'000'000ULL;
inline constexpr std::uint64_t kNsPerSecond = 1'000'000'000ULL;

// Call once at start-up, before the loop (Windows: 1 ms timer resolution).
void init();

// Monotonic nanoseconds since an arbitrary start.
[[nodiscard]] std::uint64_t nowNs();
[[nodiscard]] inline std::uint64_t nowMs() { return nowNs() / kNsPerMs; }

// Sleeps about `ns` (may wake a little late).
void sleepNs(std::uint64_t ns);
inline void sleepMs(const std::uint32_t ms) { sleepNs(ms * kNsPerMs); }

// Sleeps exactly `ns`: one real sleep until shortly before the end, then spinning for the rest. How much is spun is
// learnt from how late the calling thread's sleeps end (core `SpinMargin`: 50..300 us), so the thread is not kept
// busy through the whole wait after one slow wake-up.
void sleepPrecise(std::uint64_t ns);
// The spin margin sleepPrecise used last (any thread), for the loop's log.
[[nodiscard]] std::uint64_t preciseSleepMarginNs();

// Waits until one of several sources has something new, or a time passes: the service's events loop (`[emulator]
// loop_mode = events`), which sleeps instead of waking 1000 times a second. One per thread; not thread safe.
class WaitSet {
public:
    WaitSet();
    ~WaitSet();
    WaitSet(const WaitSet&) = delete;
    WaitSet& operator=(const WaitSet&) = delete;

    // A device's notification: an event object (Windows: the HANDLE DirectInput signals, auto-reset) or a file
    // descriptor that becomes readable (Linux: evdev). Its index, or -1 if it cannot be waited for.
    int addEvent(std::intptr_t source);
    // A UDP socket (SOCKET on Windows, a file descriptor elsewhere) that has a datagram to read. Its index, or -1.
    // Windows gives it an event (WSAEventSelect) for as long as this set lives.
    int addSocket(std::intptr_t socket);

    static constexpr int kTimedOut = -1;
    static constexpr int kFailed = -2;   // the system could not wait (failureText() says why); nothing was waited for

    // Until one of them has something (its index; the first if several, on Windows a socket with a datagram before
    // a device's event; a socket counts only while a datagram waits in it) or `deadlineNs` (nowNs()'s clock) is reached
    // (kTimedOut). A deadline already passed returns at once. Interrupted by a signal: kTimedOut early.
    int waitUntil(std::uint64_t deadlineNs);
    // How many sources were added.
    [[nodiscard]] int size() const;
    // Why the last waitUntil returned kFailed.
    [[nodiscard]] const std::string& failureText() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The calling thread above normal priority. False with `error` set if the system refused.
bool raiseThreadPriority(std::string& error);

// "Windows", "Linux", "macOS".
[[nodiscard]] const char* name();

// The running program's own file, empty if unknown.
[[nodiscard]] std::filesystem::path executablePath();
// Windows' System32 folder, from Windows itself (not from an environment variable a user could change); "/usr/bin"
// elsewhere.
[[nodiscard]] std::filesystem::path systemFolder();

// Windows: the process that owns IPv4 UDP port `port` (bound to 127.0.0.1 or any address): its id, 0 when no process
// owns it (any more), nullopt when the system could not say. Elsewhere always nullopt. For SenderGate.
[[nodiscard]] std::optional<std::uint32_t> udpPortOwner(std::uint16_t port);
// Windows: the full path of process `pid`'s program; empty when it cannot be read (and on other systems).
[[nodiscard]] std::filesystem::path processImage(std::uint32_t pid);
// Why the last processImage() on this thread could not open or read the process (also when the fallback by id then
// named it), e.g. "OpenProcess failed (error 5)"; empty when it could.
[[nodiscard]] std::string lastProcessImageError();

// Only one wheelbri-service session at a time on this computer: two would both read the wheel, both send state to
// the same port (the game would get both, interleaved) and both create the virtual devices. Held while it lives.
// Windows: a named mutex (released by Windows when the process dies). Elsewhere: an flock on a file in the
// runtime folder (released by the kernel when the process dies).
class SingleInstance {
public:
    // Null with `error` set ("another wheelbri-service is running (pid N)" or why the lock could not be made).
    [[nodiscard]] static std::unique_ptr<SingleInstance> acquire(const std::string& name, std::string& error);
    ~SingleInstance();

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

private:
    struct Impl;
    explicit SingleInstance(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Windows: closing the console window, Ctrl+Break, logging off or shutting down call `requestStop` and then wait
// (up to about 4.5 s, what Windows allows) while `busy` says the session is still undoing itself, so the real wheel
// is shown again and the virtual devices are removed. Ctrl+C only calls `requestStop`. Elsewhere: nothing (the
// service handles SIGINT/SIGTERM itself).
void installConsoleStopHandler(void (*requestStop)(), bool (*busy)());

// Whether this process runs as Administrator (Windows, elevated). Always true elsewhere.
[[nodiscard]] bool isElevated();

// A program run in the background with no console window and no input or output (stdin/out/err go to the
// null device). Arguments are UTF-8; argv[0] is the program's path. On Windows it runs in a job object: ending it
// (kill(), or this process ending) also ends everything it started (an installer's pnputil, for example).
class ChildProcess {
public:
    [[nodiscard]] static std::unique_ptr<ChildProcess> start(const std::vector<std::string>& argv, std::string& error);
    ~ChildProcess();   // kills it if it is still running

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // Its exit code once it has ended, nothing while it runs.
    [[nodiscard]] std::optional<int> poll();
    // Ends it now and waits for it.
    void kill();
    // Lets it run on by itself: neither this object nor this process ending stops it any more.
    void release();
    // Like release(), but poll() still reports its exit: for a child that must never be cut off halfway (an installer
    // changing a device's driver), even when this process ends first.
    void keepRunningAfterExit();

private:
    struct Impl;
    explicit ChildProcess(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace wheelbri::service::platform
