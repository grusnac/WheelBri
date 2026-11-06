// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/HelperStatus.hpp"
#include "wheelbri/LogLevel.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace wheelbri::service {

// The app saying Hello to the WheelBri Helper, on a thread of its own (a Hello may wait up to 5 s; the window and
// the service loop never do): once when the app starts, so the helper writes the lines it kept since boot into this
// user's log folder (HelperServer.hpp), and again when the Logs tab asks, at most once every `minGapMs`. The latest
// outcome is the Logs tab's Helper row (describeHelper).
//
// Each Hello carries the app's log level (AppLogLevel), which the helper takes for its own log. A new level (the Logs
// tab's Level button) is said at once, whatever the gap; while a session runs it is also asked of the helper for the
// drivers (SetLogLevel: the registry value the hide filter reads), privileged work the app does only after Start
// (otherwise the next Start does it).
class HelperProbe {
public:
    struct Ask {
        LogLevel level = kDefaultAppLogLevel;   // said with the Hello
        bool driversToo = false;                // and SetLogLevel after it (a new level)
    };
    using Check = std::function<helper::HelperState(const Ask& ask)>;

    static constexpr std::uint64_t kMinGapMs = 5 * 60'000;

    // The process's own, with the system's check, started (and checking) at its first use.
    static HelperProbe& shared();
    // Ends the process's own probe's thread (waits for a check that runs: at most its 5 s), before the program ends
    // and its statics (the log among them) go: a check still running then would use them. Called last thing by main
    // and by the tests; latest() still answers afterwards, nothing checks any more. Nothing when never started.
    static void stopShared();
    // Hello through the helper's pipe (HelperClient); NotWindows elsewhere.
    static Check systemCheck();

    // Checks at once, then on request(). `level`: the log level its Hellos say until levelChanged.
    HelperProbe(Check check, std::uint64_t minGapMs, LogLevel level = kDefaultAppLogLevel);
    // Waits for a check that is running (at most its 5 s).
    ~HelperProbe();

    HelperProbe(const HelperProbe&) = delete;
    HelperProbe& operator=(const HelperProbe&) = delete;

    // Checks again soon, unless a check is waiting or running, or the last began less than `minGapMs` ago.
    void request();

    // The app's log level changed: said soon, whatever the gap (after a check that runs now); with SetLogLevel too
    // when `driversToo`.
    void levelChanged(LogLevel level, bool driversToo);

    // The latest outcome (NotChecked before the first one ends).
    [[nodiscard]] helper::HelperState latest() const;
    // How many checks have ended.
    [[nodiscard]] std::uint64_t checks() const;

private:
    void run();

    Check check_;
    std::uint64_t minGapMs_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool pending_ = true;    // the first check, at once
    LogLevel level_;
    bool driversToo_ = false;   // the next check also asks SetLogLevel
    bool running_ = false;
    bool quit_ = false;
    std::uint64_t startedMs_ = 0;   // when the last check began (steady clock)
    helper::HelperState latest_;
    std::uint64_t checks_ = 0;
    std::thread thread_;   // last: started once everything above is ready
};

}  // namespace wheelbri::service
