#pragma once

#include "wheelbri/Devices.hpp"
#include "wheelbri/Ffb.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>

namespace wheelbri {

// Plays force feedback on a thread of its own, so the 1 kHz loop never waits for the wheel's driver. A DirectInput
// SetParameters, CreateEffect or Start takes 4 to 27 ms on a direct drive wheel; called from the loop, that made the
// ticks late (the wheel's input and the game's state waited) every time an effect started.
//
// play() and stop() store the newest command for the slot and return at once; the worker applies only the newest
// (the effect as it is now is all the wheel needs: an older update that never reached it changes nothing).
// stopAll() drops every command still waiting and stops everything, before anything sent after it.
// A play the wheel refused is reported by the next play() on that slot returning false, so FfbController stops
// asking for it as it did when the call was direct.
//
// Order, one call at a time: stops first (they take force away), then constant forces (road, kerbs, crashes: what
// is felt at once), then periodic effects, then conditions (spring, damper, ...). A command that has waited
// kMaxWait goes first whatever it is, so nothing waits for ever behind a ramping constant force.
//
// The inner sink is only ever called by one thread at a time: the worker while it runs, the owner's thread in the
// destructor (stopAll, then the inner sink is destroyed where it was created).
class AsyncFfbSink final : public IFfbSink {
public:
    // Called first on the worker thread (the service raises its priority there).
    using WorkerStart = std::function<void()>;

    explicit AsyncFfbSink(std::unique_ptr<IFfbSink> inner, WorkerStart onWorkerStart = {});
    ~AsyncFfbSink() override;   // waits for the call in progress, stops every force, then destroys the inner sink

    AsyncFfbSink(const AsyncFfbSink&) = delete;
    AsyncFfbSink& operator=(const AsyncFfbSink&) = delete;

    bool play(std::size_t slot, const FfbEffect& effect) override;
    void stop(std::size_t slot) override;
    void stopAll() override;

    // Waits until every command given so far has reached the inner sink (tests; the loop never calls it).
    void flush();

    static constexpr std::chrono::milliseconds kMaxWait{20};

    // One call made to the inner sink, for measuring the delay to the wheel (drainApplied).
    struct Applied {
        enum class Kind : std::uint8_t { Play, Stop, StopAll };
        Kind kind = Kind::Play;
        std::uint8_t slot = 0;
        FfbEffectType type = FfbEffectType::Constant;   // of a play
        bool ok = true;                                 // a play the wheel accepted (always true for stops)
        std::uint32_t waitUs = 0;   // from the command first waiting for its slot until the call started
        std::uint32_t callUs = 0;   // the call itself: the wheel's driver
    };
    static constexpr std::size_t kAppliedCapacity = 64;
    // Moves the calls made since the last drain into `out`, oldest first, and returns how many. More than
    // kAppliedCapacity between two drains: the oldest are dropped (droppedApplied). No allocation: for the loop.
    std::size_t drainApplied(std::span<Applied> out);
    [[nodiscard]] std::uint64_t droppedApplied() const { return dropped_.load(std::memory_order_relaxed); }

    // Plays the inner sink refused, since the start.
    [[nodiscard]] std::uint64_t failures() const { return failures_.load(std::memory_order_relaxed); }
    // The longest single call to the inner sink, in microseconds, since the start.
    [[nodiscard]] std::uint64_t longestCallUs() const { return longestCallUs_.load(std::memory_order_relaxed); }

private:
    using Clock = std::chrono::steady_clock;
    enum class Command : std::uint8_t { None, Play, Stop };
    struct Pending {
        Command command = Command::None;
        FfbEffect effect;
        Clock::time_point since;   // when the slot started waiting (kept when a newer command replaces this one)
    };

    void run();
    [[nodiscard]] bool hasWork() const;   // under mutex_
    [[nodiscard]] std::size_t pickNext(Clock::time_point now) const;   // under mutex_; kMaxSlots: none
    void record(const Applied& applied);   // under mutex_
    void queue(std::size_t slot, Command command, const FfbEffect& effect);   // under mutex_

    std::unique_ptr<IFfbSink> inner_;
    WorkerStart onWorkerStart_;
    std::mutex mutex_;
    std::condition_variable wake_;   // a command or quit
    std::condition_variable idle_;   // the worker has nothing left to do
    std::array<Pending, kMaxSlots> pending_{};
    std::array<bool, kMaxSlots> refused_{};   // the last play on that slot failed, not reported yet
    bool stopAllPending_ = false;
    Clock::time_point stopAllSince_;
    bool busy_ = false;
    bool quit_ = false;
    std::array<Applied, kAppliedCapacity> applied_{};   // ring
    std::size_t appliedFirst_ = 0;
    std::size_t appliedCount_ = 0;
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> failures_{0};
    std::atomic<std::uint64_t> longestCallUs_{0};
    std::thread worker_;   // last: started once everything above exists
};

// The delay from FfbController handing a force over to the wheel having it, per kind of effect (AsyncFfbSink's
// Applied records), for the log. The loop records; nothing allocates until a line is asked for.
class FfbDelayStats {
public:
    enum class Group : std::uint8_t { Constant, Periodic, Condition, Stop };
    static constexpr std::size_t kGroups = 4;

    void record(const AsyncFfbSink::Applied& applied);
    [[nodiscard]] bool any() const;
    [[nodiscard]] std::uint64_t count(Group group) const { return groups_[static_cast<std::size_t>(group)].count; }
    // Share of that group's calls whose wait + call was at most `ms` (0..1; 0 with no calls).
    [[nodiscard]] double shareWithin(Group group, int ms) const;
    // "constant    812 calls, wait avg 0.04 / max 2.1 ms, wheel avg 4.1 / max 15.0 ms, total <= 5 ms 93%, <= 10 ms 99%"
    // (empty for a group without calls).
    [[nodiscard]] std::string line(Group group) const;
    static const char* groupName(Group group);
    static Group groupOf(const AsyncFfbSink::Applied& applied);

    // Total (wait + call) buckets, upper bounds in ms; the last one is everything above.
    static constexpr std::array<int, 6> kBucketMs = {1, 2, 5, 10, 20, 50};

private:
    struct Totals {
        std::uint64_t count = 0;
        std::uint64_t waitSumUs = 0;
        std::uint64_t waitMaxUs = 0;
        std::uint64_t callSumUs = 0;
        std::uint64_t callMaxUs = 0;
        std::array<std::uint64_t, kBucketMs.size() + 1> buckets{};
    };
    std::array<Totals, kGroups> groups_{};
};

}  // namespace wheelbri
