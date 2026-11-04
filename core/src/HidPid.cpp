#include "wheelbri/HidPid.hpp"

#include "wheelbri/ByteOrder.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>

namespace wheelbri {

namespace {

std::int16_t i16(std::span<const std::uint8_t> d, std::size_t at) { return bytes::le16s(d, at); }

std::uint16_t u16(std::span<const std::uint8_t> d, std::size_t at) { return bytes::le16(d, at); }

std::uint32_t u32(std::span<const std::uint8_t> d, std::size_t at) { return bytes::le32(d, at); }

std::string hex(std::span<const std::uint8_t> d) {
    std::string text;
    char byte[4];
    for (const std::uint8_t b : d) {
        std::snprintf(byte, sizeof(byte), "%02X", b);
        text += byte;
    }
    return text;
}

// PID effect types in the descriptor's order (1-based).
const char* pidTypeName(const int pidType) {
    static constexpr const char* kNames[] = {"no type yet", "constant", "ramp",   "square",  "sine",
                                             "triangle",    "saw up",   "saw down", "spring", "damper",
                                             "inertia",     "friction", "custom"};
    return pidType >= 0 && pidType <= 12 ? kNames[pidType] : "unknown type";
}

}  // namespace

HidPid::HidPid(Sender send, Logger log) : sender_(std::move(send)), logger_(std::move(log)) {}

// Hands on what was queued once the caller's lock on mutex_ is released (declared before that lock).
class HidPid::FlushOnExit {
public:
    explicit FlushOnExit(const HidPid& pid) : pid_(pid) {}
    ~FlushOnExit() { pid_.flush(); }
    FlushOnExit(const FlushOnExit&) = delete;
    FlushOnExit& operator=(const FlushOnExit&) = delete;

private:
    const HidPid& pid_;
};

void HidPid::queueLog(const LogLevel level, std::string text) const { logbox_.emplace_back(level, std::move(text)); }

void HidPid::flush() const {
    const std::lock_guard order(flushMutex_);
    std::vector<FfbPacket> packets;
    std::vector<std::pair<LogLevel, std::string>> lines;
    {
        const std::lock_guard lock(mutex_);
        packets.swap(outbox_);
        lines.swap(logbox_);
    }
    for (const auto& [level, text] : lines) logger_(level, text);
    for (const FfbPacket& packet : packets) sender_(packet);
}

// PID effect type (1-based, descriptor order: constant, ramp, square, sine, triangle, saw up, saw down, spring,
// damper, inertia, friction, custom) -> the service's type. A ramp is a constant force going from its start level to
// its end over the duration (its mean level when the duration is infinite).
std::optional<FfbEffectType> HidPid::typeFor(int pidType) {
    switch (pidType) {
        case 1:
        case 2: return FfbEffectType::Constant;
        case 3: return FfbEffectType::Square;
        case 4: return FfbEffectType::Sine;
        case 5: return FfbEffectType::Triangle;
        case 6: return FfbEffectType::SawtoothUp;
        case 7: return FfbEffectType::SawtoothDown;
        case 8: return FfbEffectType::Spring;
        case 9: return FfbEffectType::Damper;
        case 10: return FfbEffectType::Inertia;
        case 11: return FfbEffectType::Friction;
        default: return std::nullopt;
    }
}

std::optional<HidPid::ParamKind> HidPid::kindOf(int pidType) {
    if (pidType == 1) return ParamKind::Constant;
    if (pidType == 2) return ParamKind::Ramp;
    if (pidType >= 3 && pidType <= 7) return ParamKind::Periodic;
    if (pidType >= 8 && pidType <= 11) return ParamKind::Condition;
    return std::nullopt;
}

void HidPid::noteReport(const std::int64_t nowMs) {
    if (reportsHandled_ == 0 || nowMs - lastReportMs_ > kStreamGapMs) streamStartMs_ = nowMs;
    ++reportsHandled_;
    lastReportMs_ = nowMs;
    if (quiet_) {
        quiet_ = false;
        queueLog(LogLevel::Info, "pid: the game sends force feedback again: forces back on");
    }
}

void HidPid::enableTrace(const int lines) {
    const FlushOnExit flushOnExit(*this);
    std::lock_guard lock(mutex_);
    traceLines_ = std::clamp(lines, 0, kMaxTraceLines);
    traceLeft_ = traceLines_;
}

void HidPid::trace(const std::string& text, const std::int64_t nowMs) {
    if (traceLeft_ <= 0) return;
    if (traceLeft_ == traceLines_) {
        traceStartMs_ = nowMs;
        queueLog(LogLevel::Trace, "pid trace: the next " + std::to_string(traceLines_) +
                                  " reports and slot changes (+ms from the first)");
    }
    char at[32];
    std::snprintf(at, sizeof(at), "pid trace +%lld ", static_cast<long long>(nowMs - traceStartMs_));
    queueLog(LogLevel::Trace, at + text);
    if (--traceLeft_ == 0) queueLog(LogLevel::Trace, "pid trace: end");
}

void HidPid::event(const std::string& text, const std::int64_t nowMs, const LogLevel level) {
    if (nowMs - eventWindowMs_ >= kEventWindowMs) {
        if (eventsSkipped_ > 0) {
            queueLog(LogLevel::Debug, "pid: " + std::to_string(eventsSkipped_) + " more effect events in " +
                 std::to_string(kEventWindowMs / 1000) + " s were not logged");
        }
        eventWindowMs_ = nowMs;
        eventsInWindow_ = 0;
        eventsSkipped_ = 0;
    }
    if (eventsInWindow_ >= kMaxEventLines) {
        ++eventsSkipped_;
        return;
    }
    ++eventsInWindow_;
    queueLog(level, "pid: " + text);
}

std::string HidPid::describe(const std::uint8_t ebi, const Block& b) const {
    std::string text = "block " + std::to_string(ebi) + " (" + pidTypeName(b.pidType);
    text += b.slot >= 0 ? ", slot " + std::to_string(b.slot) + ")" : ", no slot)";
    return text;
}

void HidPid::output(std::uint8_t reportId, std::span<const std::uint8_t> d, std::int64_t nowMs) {
    const FlushOnExit flushOnExit(*this);
    std::lock_guard lock(mutex_);
    noteReport(nowMs);
    if (traceLeft_ > 0) {
        char id[16];
        std::snprintf(id, sizeof(id), "out %02X ", reportId);
        trace(id + hex(d), nowMs);
    }
    // A parameter report with EBI 0 (the type-specific ones: envelope, condition, periodic, constant, ramp) that
    // repeats the one before it byte for byte is Windows' copy: nothing to do, and it must not count in a run.
    const bool parameters = reportId >= kSetEnvelope && reportId <= kSetRamp && !d.empty() && d[0] == 0;
    const bool copy = parameters && reportId == lastOutId_ && nowMs - lastOutMs_ <= kCopyMs &&
                      std::equal(d.begin(), d.end(), lastOutBytes_.begin(), lastOutBytes_.end());
    lastOutId_ = reportId;
    lastOutBytes_.assign(d.begin(), d.end());
    lastOutMs_ = nowMs;
    if (copy) return;
    if (!parameters) runBroken_ = true;   // a run is parameters one after the other
    char kind[24];
    std::snprintf(kind, sizeof(kind), "output 0x%02X", reportId);
    if (seen_.insert(kind).second) {
        queueLog(LogLevel::Debug, std::string("pid: first ") + kind + ": " + hex(d));
    }
    switch (reportId) {
        case kSetEffect:
            if (d.size() >= 13) onSetEffect(d, nowMs);
            break;
        case kSetEnvelope:
            // 0 EBI, 1 attack level, 3 fade level (0..10000), 5 attack time ms, 9 fade time ms (32 bits each).
            if (d.size() >= 13) {
                FfbEnvelope envelope;
                envelope.attackLevel = std::clamp(static_cast<float>(u16(d, 1)) / 10000.0F, 0.0F, 1.0F);
                envelope.fadeLevel = std::clamp(static_cast<float>(u16(d, 3)) / 10000.0F, 0.0F, 1.0F);
                envelope.attackMs = static_cast<float>(u32(d, 5));
                envelope.fadeMs = static_cast<float>(u32(d, 9));
                withParams(d[0], ParamKind::Envelope, [envelope](Block& b) { b.envelope = envelope; },
                           {envelope.attackLevel, envelope.fadeLevel, envelope.attackMs / 1000.0F,
                            envelope.fadeMs / 1000.0F, 0.0F, 0.0F});
            }
            break;
        case kSetCondition:
            if (d.size() >= 14) onSetCondition(d);
            break;
        case kSetPeriodic:
            if (d.size() >= 11) onSetPeriodic(d);
            break;
        case kSetConstant:
            if (d.size() >= 3) {
                const float magnitude = static_cast<float>(i16(d, 1)) / 10000.0F;
                withParams(d[0], ParamKind::Constant, [magnitude](Block& b) { b.constantMagnitude = magnitude; },
                           {magnitude, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F});
            }
            break;
        case kSetRamp:
            if (d.size() >= 5) {
                const float start = static_cast<float>(i16(d, 1)) / 10000.0F;
                const float end = static_cast<float>(i16(d, 3)) / 10000.0F;
                withParams(
                    d[0], ParamKind::Ramp,
                    [start, end](Block& b) {
                        b.rampStart = start;
                        b.rampEnd = end;
                    },
                    {start, end, 0.0F, 0.0F, 0.0F, 0.0F});
            }
            break;
        case kEffectOperation:
            if (d.size() >= 3) onEffectOperation(d, nowMs);
            break;
        case kBlockFree:
            if (!d.empty()) onBlockFree(d[0], nowMs);
            break;
        case kDeviceControl:
            if (!d.empty()) onDeviceControl(d[0], nowMs);
            break;
        case kDeviceGain:
            if (!d.empty()) {
                const float gain = static_cast<float>(d[0]) / 255.0F;
                if (gain != deviceGain_) {
                    event("device gain " + std::to_string(static_cast<int>(std::lround(gain * 100.0F))) + "%", nowMs);
                }
                deviceGain_ = gain;
            }
            break;
        default: break;
    }
    sendChanges(nowMs, false);
}

void HidPid::setFeature(std::uint8_t reportId, std::span<const std::uint8_t> d, std::int64_t nowMs) {
    const FlushOnExit flushOnExit(*this);
    std::lock_guard lock(mutex_);
    noteReport(nowMs);
    if (traceLeft_ > 0) {
        char id[16];
        std::snprintf(id, sizeof(id), "feat %02X ", reportId);
        trace(id + hex(d), nowMs);
    }
    char kind[24];
    std::snprintf(kind, sizeof(kind), "feature 0x%02X", reportId);
    if (seen_.insert(kind).second) {
        queueLog(LogLevel::Debug, std::string("pid: first ") + kind + ": " + hex(d));
    }
    // pid.dll may send Block Free and Device Control through the feature path too.
    if (reportId == kBlockFree && !d.empty()) {
        onBlockFree(d[0], nowMs);
        sendChanges(nowMs, false);
        return;
    }
    if (reportId == kDeviceControl && !d.empty()) {
        onDeviceControl(d[0], nowMs);
        sendChanges(nowMs, false);
        return;
    }
    if (reportId != kCreateNewEffect) {
        return;
    }
    lastAllocated_ = 0;
    lastLoadStatus_ = 2;   // full
    for (std::uint8_t ebi = 1; ebi <= kMaxSlots; ++ebi) {
        const std::uint32_t bit = 1U << (ebi - 1U);
        if ((allocated_ & bit) == 0) {
            allocated_ |= bit;
            lastAllocated_ = ebi;
            lastLoadStatus_ = 1;
            break;
        }
    }
    if (lastLoadStatus_ == 1) {
        event("new effect: block " + std::to_string(lastAllocated_), nowMs);
    } else {
        event("new effect refused: all " + std::to_string(kMaxSlots) + " effect blocks are in use", nowMs,
              LogLevel::Warn);
    }
}

std::size_t HidPid::getFeature(std::uint8_t reportId, std::span<std::uint8_t> out) const {
    const FlushOnExit flushOnExit(*this);
    std::lock_guard lock(mutex_);
    switch (reportId) {
        case kBlockLoad: {
            if (out.size() < 5) return 0;
            const auto used = static_cast<std::uint32_t>(std::popcount(allocated_));
            const std::uint32_t free = (kPoolSize / kMaxSlots) * (kMaxSlots - std::min<std::uint32_t>(used, kMaxSlots));
            out[0] = reportId;
            out[1] = lastAllocated_;
            out[2] = lastLoadStatus_;
            out[3] = static_cast<std::uint8_t>(free & 0xFFU);
            out[4] = static_cast<std::uint8_t>((free >> 8U) & 0xFFU);
            return 5;
        }
        case kPool:
            if (out.size() < 5) return 0;
            out[0] = reportId;
            out[1] = static_cast<std::uint8_t>(kPoolSize & 0xFFU);
            out[2] = static_cast<std::uint8_t>(kPoolSize >> 8U);
            out[3] = static_cast<std::uint8_t>(kMaxSlots);
            out[4] = 0x01;   // device-managed pool, no shared parameter blocks
            return 5;
        case kState:
            if (out.size() < 3) return 0;
            out[0] = reportId;
            out[1] = 0;
            out[2] = static_cast<std::uint8_t>((actuatorsOn_ ? 0x02U : 0U) | 0x10U | (paused_ ? 0x01U : 0U));
            return 3;
        case kCreateNewEffect:
            if (out.empty()) return 0;
            out[0] = reportId;   // bidirectional in the descriptor; nothing to report
            return 1;
        default: return 0;
    }
}

HidPid::Block* HidPid::withBlock(std::uint8_t ebi) {
    if (ebi == 0) return nullptr;
    // Only block numbers this device hands out (1..kMaxSlots) get a slot: a stray number above them would otherwise
    // hold one of the 8 slots until a Device Reset. (Hosts may skip Create New Effect, so 1..8 stay accepted.)
    if (ebi > kMaxSlots && !blocks_.contains(ebi)) {
        if (seen_.insert("out of range " + std::to_string(ebi)).second) {   // once per block number
            queueLog(LogLevel::Warn, "pid: a report for effect block " + std::to_string(ebi) +
                                     ", a number this device never hands out: ignored");
        }
        return nullptr;
    }
    Block& block = blocks_[ebi];
    if (block.slot < 0) {
        const auto free = std::find(slotOwner_.begin(), slotOwner_.end(), std::nullopt);
        if (free == slotOwner_.end()) {
            if (seen_.insert("slots full " + std::to_string(ebi)).second) {   // once per block, not per report
                queueLog(LogLevel::Warn, "pid: all " + std::to_string(kMaxSlots) + " slots in use, effect block " +
                                         std::to_string(ebi) + " is not played");
            }
        } else {
            block.slot = static_cast<int>(free - slotOwner_.begin());
            *free = ebi;
        }
    }
    return &block;
}

void HidPid::withParams(std::uint8_t ebi, ParamKind kind, const BlockAction& action, const Values& values) {
    if (ebi != 0) {
        if (Block* b = withBlock(ebi)) action(*b);
        return;
    }
    if (!loggedEbiZero_) {
        loggedEbiZero_ = true;
        queueLog(LogLevel::Debug, "pid: parameters come with effect block 0: matched to the Set Effect that follows");
    }
    // Its place in the run of this kind (see HidPid.hpp).
    if (!runBroken_ && runKind_ == kind && lastReportMs_ - runLastMs_ <= kRunGapMs) {
        ++runPosition_;
    } else {
        runKind_ = kind;
        runPosition_ = 0;
    }
    runBroken_ = false;
    runLastMs_ = lastReportMs_;
    // Kept for the Set Effect that names the block, never applied here: Windows sends a new effect's parameters
    // before its first Set Effect and may put an Effect Operation in between; each of those put the values on
    // another effect of the same kind (the trace of 2026-10-05). A later one at the same place replaces this one
    // (the newer values) but keeps its wait, so a stream of them still lands (the trace of 2026-10-11).
    const PendingKey key{kind, runPosition_};
    if (const auto it = pending_.find(key); it != pending_.end()) {
        it->second.action = action;
        it->second.values = values;
    } else {
        pending_.emplace(key, Pending{action, values, lastReportMs_});
    }
}

void HidPid::claimPending(const ParamKind kind, Block& b) {
    const auto it = pending_.lower_bound(PendingKey{kind, 0});
    if (it == pending_.end() || it->first.first != kind) return;
    it->second.action(b);
    pending_.erase(it);
}

HidPid::Values HidPid::valuesOf(const ParamKind kind, const Block& b) {
    switch (kind) {
        case ParamKind::Constant: return {b.constantMagnitude, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
        case ParamKind::Ramp: return {b.rampStart, b.rampEnd, 0.0F, 0.0F, 0.0F, 0.0F};
        case ParamKind::Periodic: return {b.magnitude, b.offset, b.phaseDeg / 360.0F, b.periodMs / 1000.0F, 0.0F, 0.0F};
        case ParamKind::Condition: return {b.center, b.posCoef, b.negCoef, b.posSat, b.negSat, b.deadband};
        case ParamKind::Envelope:
            if (!b.envelope) return {};
            return {b.envelope->attackLevel, b.envelope->fadeLevel, b.envelope->attackMs / 1000.0F,
                    b.envelope->fadeMs / 1000.0F, 0.0F, 0.0F};
    }
    return {};
}

void HidPid::applyUnclaimed(const ParamKind kind, const std::vector<Pending>& waiting) {
    // The blocks of this kind, by block number (an envelope: those that can have one). Not only the started ones:
    // a game sends a new effect's values just before it starts it (Unbound's damper, 17 ms before).
    std::vector<std::uint8_t> blocks;
    for (const auto& [ebi, block] : blocks_) {
        const auto blockKind = kindOf(block.pidType);
        if (!blockKind) continue;
        if (kind == ParamKind::Envelope ? *blockKind == ParamKind::Condition : *blockKind != kind) continue;
        blocks.push_back(ebi);
    }
    const auto apply = [this](const std::uint8_t ebi, const Pending& pending) {
        if (Block* b = withBlock(ebi)) pending.action(*b);
    };
    if (blocks.empty()) return;
    // Each to the block nearest its values, one per block (ties by block number), the cheapest way over all of
    // them: tried exhaustively (a few blocks and updates: small), but never more than kMaxSearchSteps steps.
    const std::size_t n = std::min(waiting.size(), blocks.size());
    std::vector<std::vector<double>> cost(waiting.size(), std::vector<double>(blocks.size()));
    for (std::size_t i = 0; i < waiting.size(); ++i) {
        for (std::size_t j = 0; j < blocks.size(); ++j) {
            const Values now = valuesOf(kind, blocks_.at(blocks[j]));
            double d = 0.0;
            for (std::size_t k = 0; k < now.size(); ++k) d += std::fabs(static_cast<double>(now[k] - waiting[i].values[k]));
            // A hair for the order: equally near, the i-th goes to the i-th block.
            cost[i][j] = d + 1e-9 * static_cast<double>(i > j ? i - j : j - i);
        }
    }
    std::vector<int> best(waiting.size(), -1);
    std::vector<int> pick(waiting.size(), -1);
    std::vector<bool> used(blocks.size(), false);
    double bestCost = std::numeric_limits<double>::max();
    // First each in turn to the nearest block still free: the answer when the search below runs out of steps (a
    // burst of updates from a game must not hold mutex_, and the I/O callbacks waiting on it, for long).
    {
        double sum = 0.0;
        std::size_t given = 0;
        for (std::size_t i = 0; i < waiting.size() && given < n; ++i) {
            int nearest = -1;
            for (std::size_t j = 0; j < blocks.size(); ++j) {
                if (!used[j] && (nearest < 0 || cost[i][j] < cost[i][static_cast<std::size_t>(nearest)])) {
                    nearest = static_cast<int>(j);
                }
            }
            used[static_cast<std::size_t>(nearest)] = true;
            best[i] = nearest;
            sum += cost[i][static_cast<std::size_t>(nearest)];
            ++given;
        }
        bestCost = sum;
        std::fill(used.begin(), used.end(), false);
    }
    std::size_t steps = 0;
    // Every update gets a block while blocks last; the ones past `n` in the run are the left-overs.
    const std::function<void(std::size_t, std::size_t, double)> search = [&](const std::size_t i, const std::size_t given,
                                                                          const double sum) {
        if (sum >= bestCost || ++steps > kMaxSearchSteps) return;
        if (given == n || i == waiting.size()) {
            if (given == n) {
                bestCost = sum;
                best = pick;
            }
            return;
        }
        for (std::size_t j = 0; j < blocks.size(); ++j) {
            if (used[j]) continue;
            used[j] = true;
            pick[i] = static_cast<int>(j);
            search(i + 1, given + 1, sum + cost[i][j]);
            used[j] = false;
            pick[i] = -1;
        }
        if (waiting.size() - i > n - given) search(i + 1, given, sum);   // this one left over
    };
    search(0, 0, 0.0);
    for (std::size_t i = 0; i < waiting.size(); ++i) {
        if (best[i] >= 0) apply(blocks[static_cast<std::size_t>(best[i])], waiting[i]);
    }
}

void HidPid::flushPending(const std::int64_t nowMs, const bool all) {
    // A kind is due once its oldest waiting value waited kPendingMs; then its whole run goes at once (they belong
    // together: which block each goes to depends on the others).
    for (const ParamKind kind :
         {ParamKind::Constant, ParamKind::Ramp, ParamKind::Periodic, ParamKind::Condition, ParamKind::Envelope}) {
        auto first = pending_.lower_bound(PendingKey{kind, 0});
        bool due = false;
        for (auto it = first; it != pending_.end() && it->first.first == kind; ++it) {
            due = due || all || nowMs - it->second.sinceMs >= kPendingMs;
        }
        if (!due) continue;
        std::vector<Pending> waiting;
        while (first != pending_.end() && first->first.first == kind) {
            waiting.push_back(std::move(first->second));
            first = pending_.erase(first);
        }
        applyUnclaimed(kind, waiting);
    }
}

// Set Effect: 0 EBI, 1 type, 2 duration ms, 4 trigger repeat, 6 sample period, 8 start delay ms,
// 10 gain 0..255, 11 trigger button, 12 bit0 X enable / bit1 Y enable / bit2 direction enable,
// 13 direction 1 (0..32767 = 0..360 degrees), 15 direction 2, 17/19 type-specific block offsets.
void HidPid::onSetEffect(std::span<const std::uint8_t> d, const std::int64_t nowMs) {
    const std::uint8_t ebi = d[0];
    const int type = d[1];
    const std::uint16_t duration = u16(d, 2);
    const std::uint16_t startDelay = u16(d, 8);
    const float gain = static_cast<float>(d[10]) / 255.0F;
    const std::uint8_t axes = d[12];
    const std::uint16_t direction = d.size() >= 15 ? u16(d, 13) : 0;
    Block* b = withBlock(ebi);
    if (b == nullptr) return;
    const std::optional<std::int64_t> durationMs =
        (duration == 0xFFFF || duration == 0) ? std::nullopt : std::optional<std::int64_t>(duration);
    const std::int64_t delayMs = startDelay == 0xFFFF ? 0 : startDelay;
    // Logged when the block gets its type, or its type or timing changes (a game updating a running effect sends
    // the same Set Effect over and over: not logged again).
    if (b->pidType != type || b->durationMs != durationMs || b->startDelayMs != delayMs) {
        b->pidType = type;
        std::string text = describe(ebi, *b) + ": " +
                           (durationMs ? std::to_string(*durationMs) + " ms" : std::string("until stopped"));
        if (delayMs > 0) text += ", starts " + std::to_string(delayMs) + " ms after its start";
        text += ", gain " + std::to_string(static_cast<int>(std::lround(gain * 100.0F))) + "%";
        event(text, nowMs);
    }
    b->pidType = type;
    b->durationMs = durationMs;
    b->startDelayMs = delayMs;
    b->gain = gain;
    if (const auto kind = kindOf(type)) {
        lastOfKind_[*kind] = ebi;
        claimPending(*kind, *b);
        // An envelope sent just before belongs to this effect when it can have one (not a condition).
        if (*kind == ParamKind::Condition) {
            for (auto it = pending_.begin(); it != pending_.end();) {
                it = it->first.first == ParamKind::Envelope ? pending_.erase(it) : std::next(it);
            }
        } else {
            lastOfKind_[ParamKind::Envelope] = ebi;
            claimPending(ParamKind::Envelope, *b);
        }
    }
    // With a direction, the force points that way (polar, 90 degrees = +X, as the shim reads DirectInput
    // polar directions); without one it acts along the enabled axes: here only the wheel counts.
    if ((axes & 0x04U) != 0) {
        const double radians = static_cast<double>(direction) / 32767.0 * 2.0 * std::numbers::pi;
        b->along = static_cast<float>(std::sin(radians));
    } else {
        b->along = (axes & 0x01U) != 0 ? 1.0F : 0.0F;
    }
}

// Set Condition: 0 EBI, 1 low nibble = parameter block (0 = X, 1 = Y; pid.dll may write 0xF), 2 centre,
// 4 +coef, 6 -coef (all -10000..10000), 8 +sat, 10 -sat, 12 deadband (0..10000).
void HidPid::onSetCondition(std::span<const std::uint8_t> d) {
    if ((d[1] & 0x0FU) == 1) return;          // only the X (wheel) block matters
    const float center = static_cast<float>(i16(d, 2)) / 10000.0F;
    const float pos = static_cast<float>(i16(d, 4)) / 10000.0F;
    const float neg = static_cast<float>(i16(d, 6)) / 10000.0F;
    const float posSat = static_cast<float>(u16(d, 8)) / 10000.0F;
    const float negSat = static_cast<float>(u16(d, 10)) / 10000.0F;
    const float dead = static_cast<float>(u16(d, 12)) / 10000.0F;
    withParams(
        d[0], ParamKind::Condition,
        [=](Block& b) {
            b.center = center;
            b.posCoef = pos;
            b.negCoef = neg;
            b.posSat = posSat;
            b.negSat = negSat;
            b.deadband = dead;
        },
        {center, pos, neg, posSat, negSat, dead});
}

// Set Periodic: 0 EBI, 1 magnitude 0..10000, 3 offset -10000..10000, 5 phase (1/100 degree), 7 period ms.
void HidPid::onSetPeriodic(std::span<const std::uint8_t> d) {
    const float magnitude = static_cast<float>(u16(d, 1)) / 10000.0F;
    const float offset = static_cast<float>(i16(d, 3)) / 10000.0F;
    const float phase = static_cast<float>(u16(d, 5)) / 100.0F;
    const auto period = static_cast<float>(u32(d, 7));
    withParams(
        d[0], ParamKind::Periodic,
        [=](Block& b) {
            b.magnitude = magnitude;
            b.offset = offset;
            b.phaseDeg = phase;
            b.periodMs = period;
        },
        {magnitude, offset, phase / 360.0F, period / 1000.0F, 0.0F, 0.0F});
}

// Effect Operation: 0 EBI, 1 op (1 start, 2 start solo, 3 stop), 2 loop count (255 = infinite).
void HidPid::onEffectOperation(std::span<const std::uint8_t> d, std::int64_t nowMs) {
    const std::uint8_t ebi = d[0];
    const std::uint8_t op = d[1];
    const std::uint8_t loops = d[2];
    if (op == 2) {
        for (auto& [other, block] : blocks_) {
            if (other != ebi) block.started = false;
        }
    }
    Block* b = withBlock(ebi);
    if (b == nullptr) {
        event("effect operation " + std::to_string(op) + " for block " + std::to_string(ebi) +
                  ", a block this device does not have: ignored",
              nowMs, LogLevel::Warn);
        return;
    }
    if (op == 1 || op == 2) {
        b->started = true;
        b->startedAtMs = nowMs;
        b->loops = loops == 255 ? std::nullopt : std::optional<int>(std::max(1, static_cast<int>(loops)));
        std::string text = std::string(op == 2 ? "start solo " : "start ") + describe(ebi, *b) +
                           (b->loops ? ", " + std::to_string(*b->loops) + " time(s)" : std::string());
        if (b->pidType == 0) text += ": its Set Effect has not come yet, it plays once that does";
        if (!actuatorsOn_) text += ": the actuators are off (the game disabled them)";
        if (paused_) text += ": the device is paused (the game paused it)";
        event(text, nowMs);
    } else if (op == 3) {
        b->started = false;
        event("stop " + describe(ebi, *b), nowMs);
    } else {
        event("effect operation " + std::to_string(op) + " for " + describe(ebi, *b) + ": unknown, ignored", nowMs,
              LogLevel::Warn);
    }
}

void HidPid::onBlockFree(std::uint8_t ebi, std::int64_t nowMs) {
    if (ebi >= 1 && ebi <= 32) {
        allocated_ &= ~(1U << (ebi - 1U));
    }
    for (auto it = lastOfKind_.begin(); it != lastOfKind_.end();) {
        it = it->second == ebi ? lastOfKind_.erase(it) : std::next(it);
    }
    const auto found = blocks_.find(ebi);
    if (found == blocks_.end()) {
        event("free block " + std::to_string(ebi) + ", which was not in use", nowMs);
        return;
    }
    event("free " + describe(ebi, found->second), nowMs);
    const int slot = found->second.slot;
    blocks_.erase(found);
    if (slot < 0) return;
    const auto s = static_cast<std::size_t>(slot);
    slotOwner_[s].reset();
    if (sent_[s] && sent_[s]->playing) {
        send(FfbPacket::Kind::SlotState, static_cast<std::uint8_t>(slot), false, sent_[s]->effect, nowMs);
    }
    sent_[s].reset();
}

void HidPid::resetAll(std::int64_t nowMs) {
    blocks_.clear();
    slotOwner_.fill(std::nullopt);
    sent_.fill(std::nullopt);
    pending_.clear();
    runKind_.reset();
    runBroken_ = true;
    lastOfKind_.clear();
    allocated_ = 0;
    lastAllocated_ = 0;
    lastLoadStatus_ = 3;
    paused_ = false;
    actuatorsOn_ = true;
    send(FfbPacket::Kind::StopAll, 0, false, FfbEffect{}, nowMs);
}

// Device Control: 1 enable actuators, 2 disable, 3 stop all, 4 reset, 5 pause, 6 continue.
void HidPid::onDeviceControl(std::uint8_t command, std::int64_t nowMs) {
    static constexpr const char* kCommands[] = {
        "",
        "enable actuators",
        "disable actuators (no force until they are enabled again)",
        "stop all effects",
        "reset (every effect is gone)",
        "pause (no force until it continues)",
        "continue",
    };
    event(command >= 1 && command <= 6 ? std::string("device control: ") + kCommands[command]
                                       : "device control " + std::to_string(command) + ": unknown, ignored",
          nowMs, command >= 1 && command <= 6 ? LogLevel::Debug : LogLevel::Warn);
    switch (command) {
        case 1: actuatorsOn_ = true; break;
        case 2: actuatorsOn_ = false; break;
        case 3:
            for (auto& [ebi, block] : blocks_) block.started = false;
            break;
        case 4: resetAll(nowMs); break;
        case 5: paused_ = true; break;
        case 6: paused_ = false; break;
        default: break;
    }
}

bool HidPid::playing(const Block& b, std::int64_t nowMs) const {
    if (!b.started || !actuatorsOn_ || paused_ || !typeFor(b.pidType)) return false;
    const std::int64_t elapsed = nowMs - b.startedAtMs;
    if (elapsed < b.startDelayMs) return false;
    if (!b.durationMs || !b.loops) return true;
    return elapsed - b.startDelayMs < *b.durationMs * *b.loops;
}

FfbEffect HidPid::toEffect(const Block& b, const std::int64_t nowMs) const {
    FfbEffect e;
    e.type = typeFor(b.pidType).value_or(FfbEffectType::Constant);
    e.gain = std::clamp(b.gain * deviceGain_, 0.0F, 1.0F);
    if (b.pidType == 1) {
        e.constantLevel = std::clamp(b.constantMagnitude * b.along, -1.0F, 1.0F);
    } else if (b.pidType == 2) {
        e.constantLevel = std::clamp(b.rampStart * b.along, -1.0F, 1.0F);   // to rampEnd, below
    } else if (b.pidType >= 3 && b.pidType <= 7) {
        e.periodic.magnitude = b.magnitude * std::abs(b.along);
        e.periodic.offset = b.offset * b.along;
        e.periodic.periodMs = b.periodMs;
        e.periodic.phaseDeg = b.along < 0 ? std::fmod(b.phaseDeg + 180.0F, 360.0F) : b.phaseDeg;
    } else {
        e.condition.center = b.center;
        e.condition.deadband = b.deadband;
        e.condition.positiveCoefficient = b.posCoef;
        e.condition.negativeCoefficient = b.negCoef;
        e.condition.positiveSaturation = b.posSat;
        e.condition.negativeSaturation = b.negSat;
    }
    FfbShape shape;
    shape.envelope = b.envelope;
    if (b.pidType == 2) shape.rampEndLevel = std::clamp(b.rampEnd * b.along, -1.0F, 1.0F);
    if (!shape.envelope && !shape.rampEndLevel) return e;
    // Time into the current play (each loop plays the envelope and the ramp again).
    std::int64_t elapsed = b.started ? std::max<std::int64_t>(0, nowMs - b.startedAtMs - b.startDelayMs) : 0;
    if (b.durationMs && *b.durationMs > 0) elapsed %= *b.durationMs;
    const double duration = b.durationMs ? static_cast<double>(*b.durationMs) : kUntilStopped;
    return shapeAt(e, shape, static_cast<double>(elapsed), duration);
}

void HidPid::sendChanges(std::int64_t nowMs, bool force) {
    for (std::size_t slot = 0; slot < kMaxSlots; ++slot) {
        if (!slotOwner_[slot]) continue;
        const auto found = blocks_.find(*slotOwner_[slot]);
        if (found == blocks_.end()) continue;
        const bool isPlaying = playing(found->second, nowMs);
        const FfbEffect effect = toEffect(found->second, nowMs);
        const auto& last = sent_[slot];
        const bool changed = !last || last->playing != isPlaying || (isPlaying && !(last->effect == effect));
        if (changed || (force && last && last->playing)) {
            traceQuiet_ = !changed;   // a refresh repeats what was sent: not traced
            send(FfbPacket::Kind::SlotState, static_cast<std::uint8_t>(slot), isPlaying, effect, nowMs);
            traceQuiet_ = false;
            sent_[slot] = Sent{isPlaying, effect};
        }
    }
}

void HidPid::send(FfbPacket::Kind kind, std::uint8_t slot, bool isPlaying, const FfbEffect& effect, std::int64_t nowMs) {
    FfbPacket packet;
    packet.kind = kind;
    packet.slot = slot;
    packet.sequence = ++sequence_;
    packet.playing = isPlaying;
    packet.effect = effect;
    if (traceLeft_ > 0 && !traceQuiet_ && kind == FfbPacket::Kind::SlotState) {
        char text[160];
        std::snprintf(text, sizeof(text), "-> slot %u type %d %s level %.3f k %.3f/%.3f sat %.3f mag %.3f period %.0f",
                      static_cast<unsigned>(slot), static_cast<int>(effect.type), isPlaying ? "on" : "off",
                      effect.constantLevel, effect.condition.positiveCoefficient,
                      effect.condition.negativeCoefficient, effect.condition.positiveSaturation,
                      effect.periodic.magnitude, effect.periodic.periodMs);
        trace(text, nowMs);
    }
    outbox_.push_back(packet);
    lastSendMs_ = nowMs;
    ++packetsSent_;
}

void HidPid::tick(std::int64_t nowMs) {
    constexpr std::int64_t kKeepAliveMs = 100;
    constexpr std::int64_t kRefreshMs = 250;
    const FlushOnExit flushOnExit(*this);
    std::lock_guard lock(mutex_);
    // Whether a force that pushes the wheel one way (constant, ramp, periodic: not springs and dampers, which only
    // resist) plays at more than `above` of full scale. Not Ffb.cpp's helpers: the driver compiles this file without
    // the service's logging.
    const auto pushing = [this](const float above) {
        const auto pushes = [above](const FfbEffect& e) {
            switch (e.type) {
                case FfbEffectType::Spring:
                case FfbEffectType::Damper:
                case FfbEffectType::Inertia:
                case FfbEffectType::Friction: return false;
                case FfbEffectType::Constant: return std::fabs(e.constantLevel * e.gain) > above;
                default: return (std::fabs(e.periodic.magnitude) + std::fabs(e.periodic.offset)) * e.gain > above;
            }
        };
        return std::any_of(sent_.begin(), sent_.end(),
                           [&](const auto& s) { return s && s->playing && pushes(s->effect); });
    };
    // A stream that stops: only when something pushes (even a little: 0.7% took the R3 to full lock). A game at
    // rest that only holds its springs and dampers may stop sending while it waits; those stay.
    const bool streamed = lastReportMs_ - streamStartMs_ >= kStreamMinMs;
    if (!quiet_ && reportsHandled_ > 0 && streamed && nowMs - lastReportMs_ >= kStreamQuietMs &&
        pushing(kStreamPushingAbove)) {
        quiet_ = true;
        queueLog(LogLevel::Info, "pid: the game streamed force feedback for " +
                                 std::to_string((lastReportMs_ - streamStartMs_) / 1000) + " s and then nothing for " +
                                 std::to_string(kStreamQuietMs) +
                                 " ms (closed, crashed or paused?): keep-alives stopped, so the service stops the "
                                 "forces; they come back with the game's next report");
    }
    if (!quiet_ && reportsHandled_ > 0 && nowMs - lastReportMs_ >= kGameQuietMs) {
        if (pushing(kQuietPushingAbove)) {
            quiet_ = true;
            queueLog(LogLevel::Warn, "pid: no force feedback report from the game for " +
                                     std::to_string(kGameQuietMs / 1000) +
                                     " s while a pushing force plays (did the game crash?): keep-alives stopped, so "
                                     "the service stops the forces; they come back with the game's next report");
        }
    }
    flushPending(nowMs, false);
    const bool refresh = !quiet_ && nowMs - lastRefreshMs_ >= kRefreshMs;
    if (refresh) lastRefreshMs_ = nowMs;
    sendChanges(nowMs, refresh);
    // Keep-alives only once a game used force feedback: otherwise the service would report a game that is not there.
    if (!quiet_ && reportsHandled_ > 0 && nowMs - lastSendMs_ >= kKeepAliveMs) {
        send(FfbPacket::Kind::KeepAlive, 0, false, FfbEffect{}, nowMs);
    }
}

void HidPid::stopAll(std::int64_t nowMs) {
    const FlushOnExit flushOnExit(*this);
    std::lock_guard lock(mutex_);
    for (auto& [ebi, block] : blocks_) block.started = false;
    sent_.fill(std::nullopt);
    send(FfbPacket::Kind::StopAll, 0, false, FfbEffect{}, nowMs);
}

bool HidPid::gameQuiet() const {
    std::lock_guard lock(mutex_);
    return quiet_;
}

std::uint64_t HidPid::reportsHandled() const {
    std::lock_guard lock(mutex_);
    return reportsHandled_;
}

std::uint64_t HidPid::packetsSent() const {
    std::lock_guard lock(mutex_);
    return packetsSent_;
}

std::size_t HidPid::activeSlots() const {
    std::lock_guard lock(mutex_);
    return static_cast<std::size_t>(std::count_if(sent_.begin(), sent_.end(), [](const auto& s) { return s && s->playing; }));
}

}  // namespace wheelbri
