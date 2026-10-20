#include "wheelbri/AsyncFfbSink.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <utility>

namespace wheelbri {

namespace {

std::uint32_t microseconds(const std::chrono::steady_clock::duration d) {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(d).count();
    if (us <= 0) return 0;
    return static_cast<std::uint32_t>(std::min<long long>(us, std::numeric_limits<std::uint32_t>::max()));
}

// Lower goes first.
int rankOf(const bool isStop, const FfbEffectType type) {
    if (isStop) return 0;
    if (type == FfbEffectType::Constant) return 1;
    if (isConditionEffect(type)) return 3;
    return 2;   // periodic
}

}  // namespace

AsyncFfbSink::AsyncFfbSink(std::unique_ptr<IFfbSink> inner, WorkerStart onWorkerStart)
    : inner_(std::move(inner)), onWorkerStart_(std::move(onWorkerStart)) {
    worker_ = std::thread([this] { run(); });
}

AsyncFfbSink::~AsyncFfbSink() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    wake_.notify_one();
    worker_.join();
    // Whatever was still waiting is dropped: the forces stop, here on the owner's thread, before the sink goes.
    if (inner_ != nullptr) {
        inner_->stopAll();
    }
}

void AsyncFfbSink::queue(const std::size_t slot, const Command command, const FfbEffect& effect) {
    Pending& pending = pending_[slot];
    const Clock::time_point since = pending.command == Command::None ? Clock::now() : pending.since;
    pending = Pending{command, effect, since};
}

bool AsyncFfbSink::play(const std::size_t slot, const FfbEffect& effect) {
    if (slot >= kWheelSlots) {
        return false;
    }
    bool accepted = true;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (refused_[slot]) {
            // The wheel refused the last play: the controller gives the effect up, and the slot is stopped so that
            // nothing of it is left playing.
            refused_[slot] = false;
            queue(slot, Command::Stop, {});
            accepted = false;
        } else {
            queue(slot, Command::Play, effect);
        }
    }
    wake_.notify_one();
    return accepted;
}

void AsyncFfbSink::stop(const std::size_t slot) {
    if (slot >= kWheelSlots) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        queue(slot, Command::Stop, {});
        refused_[slot] = false;
    }
    wake_.notify_one();
}

void AsyncFfbSink::stopAll() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        pending_.fill(Pending{});
        refused_.fill(false);
        if (!stopAllPending_) stopAllSince_ = Clock::now();
        stopAllPending_ = true;
    }
    wake_.notify_one();
}

void AsyncFfbSink::flush() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return !busy_ && !hasWork(); });
}

std::size_t AsyncFfbSink::drainApplied(const std::span<Applied> out) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t n = std::min(out.size(), appliedCount_);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = applied_[(appliedFirst_ + i) % kAppliedCapacity];
    }
    appliedFirst_ = (appliedFirst_ + n) % kAppliedCapacity;
    appliedCount_ -= n;
    return n;
}

void AsyncFfbSink::record(const Applied& applied) {
    if (appliedCount_ == kAppliedCapacity) {
        appliedFirst_ = (appliedFirst_ + 1) % kAppliedCapacity;   // the oldest goes
        --appliedCount_;
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    applied_[(appliedFirst_ + appliedCount_) % kAppliedCapacity] = applied;
    ++appliedCount_;
}

bool AsyncFfbSink::hasWork() const {
    if (stopAllPending_) return true;
    return std::any_of(pending_.begin(), pending_.end(),
                       [](const Pending& pending) { return pending.command != Command::None; });
}

std::size_t AsyncFfbSink::pickNext(const Clock::time_point now) const {
    std::size_t best = kWheelSlots;
    int bestRank = std::numeric_limits<int>::max();
    for (std::size_t slot = 0; slot < kWheelSlots; ++slot) {
        const Pending& pending = pending_[slot];
        if (pending.command == Command::None) continue;
        // Waited long enough: first, whatever it is (no effect starves behind a ramping constant force).
        const int rank = now - pending.since >= kMaxWait ? -1 : rankOf(pending.command == Command::Stop,
                                                                         pending.effect.type);
        if (rank < bestRank || (rank == bestRank && pending.since < pending_[best].since)) {
            best = slot;
            bestRank = rank;
        }
    }
    return best;
}

void AsyncFfbSink::run() {
    if (onWorkerStart_) {
        onWorkerStart_();
    }
    const auto updateLongest = [this](const std::uint32_t us) {
        if (us > longestCallUs_.load(std::memory_order_relaxed)) {
            longestCallUs_.store(us, std::memory_order_relaxed);
        }
    };

    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        if (!quit_ && !hasWork()) {
            busy_ = false;
            idle_.notify_all();
            wake_.wait(lock, [this] { return quit_ || hasWork(); });
        }
        if (quit_) {
            break;
        }
        // One call at a time, chosen under the lock and made without it: a play() from the loop meanwhile only
        // replaces what is waiting for that slot. The choice is made again after every call.
        busy_ = true;
        if (stopAllPending_) {
            stopAllPending_ = false;
            const Clock::time_point since = stopAllSince_;
            lock.unlock();
            const Clock::time_point start = Clock::now();
            inner_->stopAll();
            const Clock::time_point end = Clock::now();
            lock.lock();
            updateLongest(microseconds(end - start));
            record(Applied{Applied::Kind::StopAll, 0, FfbEffectType::Constant, true, microseconds(start - since),
                           microseconds(end - start)});
            continue;
        }
        const std::size_t slot = pickNext(Clock::now());
        if (slot == kWheelSlots) {
            continue;
        }
        const Pending pending = std::exchange(pending_[slot], Pending{});
        lock.unlock();
        const Clock::time_point start = Clock::now();
        bool ok = true;
        if (pending.command == Command::Play) {
            ok = inner_->play(slot, pending.effect);
        } else {
            inner_->stop(slot);
        }
        const Clock::time_point end = Clock::now();
        lock.lock();
        updateLongest(microseconds(end - start));
        record(Applied{pending.command == Command::Play ? Applied::Kind::Play : Applied::Kind::Stop,
                       static_cast<std::uint8_t>(slot), pending.effect.type, ok, microseconds(start - pending.since),
                       microseconds(end - start)});
        if (!ok) {
            failures_.fetch_add(1, std::memory_order_relaxed);
            // Said by the next play() on that slot, unless a stop or stopAll came meanwhile (they settle it). A
            // newer play waiting for the slot is dropped with it: the controller gives the effect up on that
            // answer and must not find it playing on the wheel.
            if (pending_[slot].command != Command::Stop && !stopAllPending_) {
                refused_[slot] = true;
                pending_[slot] = Pending{};
            }
        }
    }
    busy_ = false;
    idle_.notify_all();
}

// --- FfbDelayStats ---

FfbDelayStats::Group FfbDelayStats::groupOf(const AsyncFfbSink::Applied& applied) {
    if (applied.kind != AsyncFfbSink::Applied::Kind::Play) return Group::Stop;
    if (applied.type == FfbEffectType::Constant) return Group::Constant;
    if (isConditionEffect(applied.type)) return Group::Condition;
    return Group::Periodic;
}

const char* FfbDelayStats::groupName(const Group group) {
    switch (group) {
        case Group::Constant: return "constant";
        case Group::Periodic: return "periodic";
        case Group::Condition: return "spring etc.";
        case Group::Stop: return "stops";
    }
    return "?";
}

void FfbDelayStats::record(const AsyncFfbSink::Applied& applied) {
    Totals& t = groups_[static_cast<std::size_t>(groupOf(applied))];
    ++t.count;
    t.waitSumUs += applied.waitUs;
    t.waitMaxUs = std::max<std::uint64_t>(t.waitMaxUs, applied.waitUs);
    t.callSumUs += applied.callUs;
    t.callMaxUs = std::max<std::uint64_t>(t.callMaxUs, applied.callUs);
    const std::uint64_t totalUs = static_cast<std::uint64_t>(applied.waitUs) + applied.callUs;
    std::size_t bucket = 0;
    while (bucket < kBucketMs.size() && totalUs > static_cast<std::uint64_t>(kBucketMs[bucket]) * 1000) ++bucket;
    ++t.buckets[bucket];
}

bool FfbDelayStats::any() const {
    return std::any_of(groups_.begin(), groups_.end(), [](const Totals& t) { return t.count > 0; });
}

double FfbDelayStats::shareWithin(const Group group, const int ms) const {
    const Totals& t = groups_[static_cast<std::size_t>(group)];
    if (t.count == 0) return 0.0;
    std::uint64_t within = 0;
    for (std::size_t i = 0; i < kBucketMs.size() && kBucketMs[i] <= ms; ++i) within += t.buckets[i];
    return static_cast<double>(within) / static_cast<double>(t.count);
}

std::string FfbDelayStats::line(const Group group) const {
    const Totals& t = groups_[static_cast<std::size_t>(group)];
    if (t.count == 0) return {};
    const double n = static_cast<double>(t.count);
    char text[200];
    std::snprintf(text, sizeof(text),
                  "%-11s %6llu calls, wait avg %.2f / max %.1f ms, wheel avg %.1f / max %.1f ms, total <= 5 ms %.0f%%, "
                  "<= 10 ms %.0f%%",
                  groupName(group), static_cast<unsigned long long>(t.count), static_cast<double>(t.waitSumUs) / n / 1000.0,
                  static_cast<double>(t.waitMaxUs) / 1000.0, static_cast<double>(t.callSumUs) / n / 1000.0,
                  static_cast<double>(t.callMaxUs) / 1000.0, shareWithin(group, 5) * 100.0,
                  shareWithin(group, 10) * 100.0);
    return text;
}

}  // namespace wheelbri
