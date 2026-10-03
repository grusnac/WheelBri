#pragma once

#include <cstdint>

namespace wheelbri {

// Timing statistics for the fixed-rate loop, reported periodically so stutter can be traced back
// to the emulator (long work) or the OS (late wake-ups).
class LoopStats {
public:
    struct Snapshot {
        std::uint64_t ticks = 0;
        std::uint64_t overruns = 0;          // periods longer than twice the target
        double averagePeriodUs = 0.0;
        double maxPeriodUs = 0.0;
        double averageWorkUs = 0.0;
        double maxWorkUs = 0.0;
    };

    explicit LoopStats(std::uint64_t targetPeriodNs) : targetPeriodNs_(targetPeriodNs) {}

    // Returns true when this period counts as an overrun.
    bool record(std::uint64_t periodNs, std::uint64_t workNs);

    [[nodiscard]] Snapshot snapshot() const;
    void reset();

    [[nodiscard]] std::uint64_t targetPeriodNs() const { return targetPeriodNs_; }

private:
    std::uint64_t targetPeriodNs_;
    std::uint64_t ticks_ = 0;
    std::uint64_t overruns_ = 0;
    std::uint64_t totalPeriodNs_ = 0;
    std::uint64_t maxPeriodNs_ = 0;
    std::uint64_t totalWorkNs_ = 0;
    std::uint64_t maxWorkNs_ = 0;
};

}  // namespace wheelbri
