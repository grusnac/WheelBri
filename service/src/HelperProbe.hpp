#pragma once

#include "wheelbri/HelperStatus.hpp"

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
class HelperProbe {
public:
    using Check = std::function<helper::HelperState()>;

    static constexpr std::uint64_t kMinGapMs = 5 * 60'000;

    // The process's own, with the system's check, started (and checking) at its first use.
    static HelperProbe& shared();
    // Hello through the helper's pipe (HelperClient); NotWindows elsewhere.
    static Check systemCheck();

    // Checks at once, then on request().
    HelperProbe(Check check, std::uint64_t minGapMs);
    // Waits for a check that is running (at most its 5 s).
    ~HelperProbe();

    HelperProbe(const HelperProbe&) = delete;
    HelperProbe& operator=(const HelperProbe&) = delete;

    // Checks again soon, unless a check is waiting or running, or the last began less than `minGapMs` ago.
    void request();

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
    bool running_ = false;
    bool quit_ = false;
    std::uint64_t startedMs_ = 0;   // when the last check began (steady clock)
    helper::HelperState latest_;
    std::uint64_t checks_ = 0;
    std::thread thread_;   // last: started once everything above is ready
};

}  // namespace wheelbri::service
