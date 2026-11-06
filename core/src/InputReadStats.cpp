// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/InputReadStats.hpp"

#include <algorithm>
#include <cstdio>

namespace wheelbri {

void InputReadStats::addPackets(const std::size_t packets, const bool capped) {
    pendingPackets_ += packets;
    pendingCapped_ = pendingCapped_ || capped;
}

void InputReadStats::recordRead(const std::uint64_t startNs, const std::uint64_t endNs) {
    if (!started_) {
        started_ = true;
        windowStartNs_ = startNs;
    }
    ++reads_;
    if (lastReadNs_ != 0 && startNs > lastReadNs_) {
        const std::uint64_t gap = startNs - lastReadNs_;
        gapSumNs_ += gap;
        ++gaps_;
        gapMaxNs_ = std::max(gapMaxNs_, gap);
    }
    lastReadNs_ = startNs;
    packets_ += pendingPackets_;
    if (pendingPackets_ == 0) ++readsWithoutNewState_;
    if (pendingCapped_) ++readsBehind_;
    pendingPackets_ = 0;
    pendingCapped_ = false;
    const std::uint64_t shim = endNs > startNs ? endNs - startNs : 0;
    shimSumNs_ += shim;
    shimMaxNs_ = std::max(shimMaxNs_, shim);
}

bool InputReadStats::due(const std::uint64_t nowNs) const {
    // A time before the window (a read timed on another thread before logReads() restarted it) is not a minute later.
    return started_ && reads_ > 0 && nowNs >= windowStartNs_ && nowNs - windowStartNs_ >= kWindowNs;
}

std::string InputReadStats::summary(const std::uint64_t nowNs) const {
    if (reads_ == 0) return "input: no reads";
    const double seconds = started_ && nowNs > windowStartNs_ ? static_cast<double>(nowNs - windowStartNs_) / 1e9 : 0.0;
    const double n = static_cast<double>(reads_);
    const double gapAvgMs = gaps_ > 0 ? static_cast<double>(gapSumNs_) / static_cast<double>(gaps_) / 1e6 : 0.0;
    char text[400];
    std::snprintf(text, sizeof(text),
                  "input: %llu reads in %.1f s (every %.1f ms on average, longest gap %.1f ms), %.1f packets per read, "
                  "%llu reads without new state, %llu reads behind (older state handed over: packets still waiting), "
                  "shim time per read avg %.0f us / max %.0f us",
                  static_cast<unsigned long long>(reads_), seconds, gapAvgMs, static_cast<double>(gapMaxNs_) / 1e6,
                  static_cast<double>(packets_) / n, static_cast<unsigned long long>(readsWithoutNewState_),
                  static_cast<unsigned long long>(readsBehind_), static_cast<double>(shimSumNs_) / n / 1e3,
                  static_cast<double>(shimMaxNs_) / 1e3);
    return text;
}

void InputReadStats::restart(const std::uint64_t nowNs) {
    const std::uint64_t lastRead = lastReadNs_;
    *this = InputReadStats{};
    started_ = true;
    windowStartNs_ = nowNs;
    lastReadNs_ = lastRead;
}

}  // namespace wheelbri
