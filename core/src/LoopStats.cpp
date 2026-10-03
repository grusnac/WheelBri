#include "wheelbri/LoopStats.hpp"

#include <algorithm>

namespace wheelbri {

bool LoopStats::record(const std::uint64_t periodNs, const std::uint64_t workNs) {
    ++ticks_;
    totalPeriodNs_ += periodNs;
    totalWorkNs_ += workNs;
    maxPeriodNs_ = std::max(maxPeriodNs_, periodNs);
    maxWorkNs_ = std::max(maxWorkNs_, workNs);
    const bool overrun = periodNs > 2 * targetPeriodNs_;
    if (overrun) {
        ++overruns_;
    }
    return overrun;
}

LoopStats::Snapshot LoopStats::snapshot() const {
    Snapshot snapshot;
    snapshot.ticks = ticks_;
    snapshot.overruns = overruns_;
    if (ticks_ > 0) {
        snapshot.averagePeriodUs = static_cast<double>(totalPeriodNs_) / static_cast<double>(ticks_) / 1000.0;
        snapshot.averageWorkUs = static_cast<double>(totalWorkNs_) / static_cast<double>(ticks_) / 1000.0;
    }
    snapshot.maxPeriodUs = static_cast<double>(maxPeriodNs_) / 1000.0;
    snapshot.maxWorkUs = static_cast<double>(maxWorkNs_) / 1000.0;
    return snapshot;
}

void LoopStats::reset() {
    ticks_ = 0;
    overruns_ = 0;
    totalPeriodNs_ = 0;
    maxPeriodNs_ = 0;
    totalWorkNs_ = 0;
    maxWorkNs_ = 0;
}

}  // namespace wheelbri
