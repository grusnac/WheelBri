// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>

namespace wheelbri {

// Waiting a little for something that is about to be there, such as a wheel that Windows is still restarting when
// Start is pressed (after a reinstall, or while a driver or filter is put on it), instead of failing at once.
enum class WaitOutcome {
    Ready,       // `ready` said yes (at once, or within the time)
    TimedOut,    // still not there after `timeoutMs`
    Cancelled,   // `cancelled` said yes first (Stop pressed)
};

struct WaitSettings {
    std::uint64_t timeoutMs = 15'000;
    std::uint64_t everyMs = 250;       // how often `ready` and `cancelled` are asked
};

// Asks `ready` (first at once, so nothing waits when it is already there), then every `everyMs` until it says yes,
// `cancelled` says yes or `timeoutMs` have passed. `sleep(ms)` waits at most `ms` and returns how long it really did:
// it may end early (a device coming, so `ready` is asked at once), and only the time really waited counts towards
// `timeoutMs` (tests pass one that only counts). `waitedMs`, when given, gets the time waited.
[[nodiscard]] WaitOutcome waitFor(const WaitSettings& settings, const std::function<bool()>& ready,
                                  const std::function<bool()>& cancelled,
                                  const std::function<std::uint64_t(std::uint64_t)>& sleep,
                                  std::uint64_t* waitedMs = nullptr);

}  // namespace wheelbri
