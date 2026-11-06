// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace wheelbri {

// How the game reads the wheel through the in-game shim, for the shim's log: how often it reads (its own rate, the
// largest part of the input delay we cannot change), whether each read got new state from the service (it sends every
// 1 ms, so a read normally finds several packets and hands over one at most ~1 ms old), whether a read fell behind
// (UdpLoopbackReceiver::kMaxPerPoll stopped it with newer packets still waiting: after the game stalled, it is given
// old state for a few reads) and how long the shim itself took in the game's thread. No OS calls: the shim passes
// monotonic times in ns.
class InputReadStats {
public:
    static constexpr std::uint64_t kWindowNs = 60'000'000'000ULL;   // a log line a minute

    // Packets the feed took in since the last read (every refresh, Poll() included), and whether one stopped at the cap.
    void addPackets(std::size_t packets, bool capped);
    // One read by the game (GetDeviceState), from its start to its end.
    void recordRead(std::uint64_t startNs, std::uint64_t endNs);

    [[nodiscard]] bool due(std::uint64_t nowNs) const;
    [[nodiscard]] std::uint64_t reads() const { return reads_; }
    // "input: 5400 reads in 60.0 s (every 11.1 ms on average, longest gap 48.2 ms), 11.0 packets per read, ..."
    [[nodiscard]] std::string summary(std::uint64_t nowNs) const;
    void restart(std::uint64_t nowNs);   // a new window (keeps nothing but the time of the last read)

private:
    std::uint64_t windowStartNs_ = 0;
    bool started_ = false;
    std::uint64_t lastReadNs_ = 0;   // 0: none yet
    std::uint64_t reads_ = 0;
    std::uint64_t gapSumNs_ = 0;
    std::uint64_t gaps_ = 0;
    std::uint64_t gapMaxNs_ = 0;
    std::uint64_t packets_ = 0;
    std::uint64_t pendingPackets_ = 0;   // since the last read
    bool pendingCapped_ = false;
    std::uint64_t readsWithoutNewState_ = 0;
    std::uint64_t readsBehind_ = 0;
    std::uint64_t shimSumNs_ = 0;
    std::uint64_t shimMaxNs_ = 0;
};

}  // namespace wheelbri
