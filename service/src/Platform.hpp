#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The little the service needs from the operating system besides the wheel and the window: a monotonic
// clock, precise sleeps for the 1 kHz loop, a higher thread priority, and starting a compressor for old log
// files. PlatformWin32.cpp on Windows, PlatformPosix.cpp elsewhere (Linux, macOS).
namespace wheelemu::service::platform {

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

// Sleeps exactly `ns`: real sleeps of 1 ms while there is time, then a short sleep, then spinning for the
// last part (what SDL_DelayPrecise does).
void sleepPrecise(std::uint64_t ns);

// The calling thread above normal priority. False with `error` set if the system refused.
bool raiseThreadPriority(std::string& error);

// "Windows", "Linux", "macOS".
[[nodiscard]] const char* name();

// A program run in the background with no console window and no input or output (stdin/out/err go to the
// null device). Arguments are UTF-8; argv[0] is the program's path.
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

private:
    struct Impl;
    explicit ChildProcess(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace wheelemu::service::platform
