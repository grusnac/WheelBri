#pragma once

#include "wheelbri/FfbPacket.hpp"
#include "wheelbri/LogLevel.hpp"
#include "wheelbri/FfbShape.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace wheelbri {

// HID PID force feedback, device side: what a virtual wheel with a PID descriptor (HidG920.hpp) receives
// from Windows' pid.dll when a game plays DirectInput effects, turned into the service's FfbPackets. The
// C++ counterpart of the old bridge's PidForwarder.cs (checked against the same reports), plus the effect
// block allocation the device itself must answer (HIDMaestro's driver did that for the bridge):
//
//   SetFeature 0x11 Create New Effect -> the next free effect block index (EBI)
//   GetFeature 0x12 Block Load        -> [0x12, EBI, status 1 ok / 2 full / 3 error, pool free LE16]
//   GetFeature 0x13 PID Pool          -> [0x13, pool size LE16, max simultaneous, 0x01 device-managed]
//   GetFeature 0x14 PID State         -> [0x14, EBI, actuators enabled | actuator power]
//
// Every other PID report is an output report (report ID removed): Set Effect 0x11, Set Envelope 0x12,
// Set Condition 0x13, Set Periodic 0x14, Set Constant 0x15, Set Ramp 0x16, Effect Operation 0x1A,
// Block Free 0x1B, Device Control 0x1C, Device Gain 0x1D.
//
// pid.dll sends the type-specific reports with EBI 0 just before the Set Effect naming the real block;
// they are kept pending for the next Set Effect of the same kind, which names the block they are for. Only when
// no Set Effect claims them within kPendingMs (a game changing a running effect without one) do they go to a block
// of that kind. Never at once: Windows sends reports twice, a new effect's parameters before its first Set
// Effect, and Effect Operations in between, and every early guess put a spring's values on the damper or back (30
// times a second in NFS; seen in the driver's trace of 2026-10-05).
// A game that only ever sends parameters (NFS Unbound, the trace of 2026-10-11: every 16 ms a constant force, two
// conditions and a periodic, all EBI 0, no Set Effect) is served too:
//   - kPendingMs counts from the first value that waits: a newer one of the same kind replaces it but does not
//     restart the wait, so a stream every 16 ms still lands every ~20 ms (it used to wait for a pause, and the
//     force came in bursts or not at all);
//   - a run is the reports of one kind that come one after the other (no other report between, at most kRunGapMs
//     apart). Two or more in a run (Unbound's damper and spring) go to the started blocks of that kind, each to the
//     block whose values it is nearest (the game's values change a little from one update to the next, while the
//     order it sends them in can change: Unbound swapped damper and spring in the middle of a race); a block gets at
//     most one, ties go by block number, and what is left over is dropped. A run of one goes to the only started
//     block of its kind, else to the last one set;
//   - an EBI-0 report with exactly the bytes of the one before it (within kCopyMs) is Windows' copy: ignored, so it
//     cannot shift a run (two different effects with the very same values lose the second one's update). Set Envelope (attack / fade) goes with the next
// constant, ramp or periodic effect. Envelopes and ramps change the force while an effect plays: tick() works the
// level out every time (FfbShape.hpp) and sends it.
//
// What the game does with its effects goes to the log, one line each (2026-10-08: NFS loaded its effects for a race
// and never started them until an alt-tab, and the log could not tell whether it had asked): a new effect block (or
// its refusal), an effect's type and timing when its Set Effect first comes or changes, every start, stop and free,
// every device command (enable / disable actuators, stop all, reset, pause, continue) and device gain change. At most
// kMaxEventLines in kEventWindowMs, then counted, so a game that restarts an effect very often cannot flood the log.
//
// Thread-safe: a driver calls it from I/O callbacks and a timer.
class HidPid {
public:
    static constexpr std::size_t kMaxSlots = 8;          // the service's slots (IFfbSink::kMaxSlots)
    static constexpr std::uint16_t kPoolSize = 0xFFFF;

    using Sender = std::function<void(const FfbPacket&)>;
    // One line and its level (LogLevel.hpp): the driver writes it when its level allows (the app's Level button).
    // Effect events are debug, the opt-in trace (enableTrace) trace, what goes wrong warn.
    using Logger = std::function<void(LogLevel level, const std::string& text)>;

    HidPid(Sender send, Logger log);

    // Output report `reportId` with its data (report ID removed).
    void output(std::uint8_t reportId, std::span<const std::uint8_t> data, std::int64_t nowMs);

    // Feature report written by the host (report ID removed). Create New Effect allocates a block.
    void setFeature(std::uint8_t reportId, std::span<const std::uint8_t> data, std::int64_t nowMs);

    // Feature report read by the host: fills `out` (report ID at [0]); the size written, 0 = not supported.
    [[nodiscard]] std::size_t getFeature(std::uint8_t reportId, std::span<std::uint8_t> out) const;

    // Call every ~10 ms: effects that ran out, keep-alives (once a game used force feedback), refresh.
    // A game that crashed sends nothing more, and the driver cannot tell (it never sees the game's handle close).
    // So when no report came for kGameQuietMs while a pushing force (constant, ramp, periodic) plays, keep-alives
    // and refreshes stop: the service's watchdog then stops every force. The game's next report brings them back.
    // Springs and dampers alone keep going: they only resist, and games set them once and leave them.
    // A game that streams its force feedback (a report at least every kStreamGapMs for kStreamMinMs, as NFS Unbound
    // does every 16 ms) and then sends nothing for kStreamQuietMs while any pushing force plays (above
    // kStreamPushingAbove) has closed, crashed or paused: keep-alives stop at once (2026-10-12: NFS Unbound closed
    // without stopping its effects, and a held force of under 1% took the R3 to full lock in 1.5 s and kept it there).
    // Springs and dampers alone keep going here too: a game at rest may stop sending while it holds only those.
    void tick(std::int64_t nowMs);
    static constexpr std::int64_t kGameQuietMs = 10'000;
    static constexpr std::int64_t kStreamGapMs = 250;
    static constexpr std::int64_t kStreamMinMs = 2'000;
    static constexpr std::int64_t kStreamQuietMs = 500;
    static constexpr float kStreamPushingAbove = 0.001F;   // any force at all
    static constexpr float kQuietPushingAbove = 0.02F;     // the 10 s rule: a force that is felt
    // How long parameters without a block wait for the Set Effect that names it (see above).
    static constexpr std::int64_t kPendingMs = 20;
    static constexpr std::int64_t kRunGapMs = 8;
    static constexpr std::int64_t kCopyMs = 8;
    // Diagnostics, off unless asked for: from the next report, `lines` lines go to the log, each report the host
    // sends (raw bytes) and each slot change sent to the service, so the real order can be read from the driver's
    // log. The driver turns it on from HKLM\SOFTWARE\wheelbri PidTraceLines (a DWORD; 1000 is a good number).
    void enableTrace(int lines);
    static constexpr int kMaxEventLines = 100;
    static constexpr std::int64_t kEventWindowMs = 10'000;
    static constexpr int kMaxTraceLines = 100'000;
    [[nodiscard]] bool gameQuiet() const;

    // Everything stops (device going away).
    void stopAll(std::int64_t nowMs);

    [[nodiscard]] std::uint64_t reportsHandled() const;
    [[nodiscard]] std::uint64_t packetsSent() const;
    [[nodiscard]] std::size_t activeSlots() const;

    static constexpr std::uint8_t kSetEffect = 0x11, kSetEnvelope = 0x12, kSetCondition = 0x13, kSetPeriodic = 0x14,
                                  kSetConstant = 0x15, kSetRamp = 0x16, kEffectOperation = 0x1A, kBlockFree = 0x1B,
                                  kDeviceControl = 0x1C, kDeviceGain = 0x1D;
    static constexpr std::uint8_t kCreateNewEffect = 0x11, kBlockLoad = 0x12, kPool = 0x13, kState = 0x14;

private:
    struct Block {
        int slot = -1;
        int pidType = 0;                     // 1..12 in the descriptor's order, 0 = not set yet
        std::optional<std::int64_t> durationMs;   // none = infinite
        std::int64_t startDelayMs = 0;
        float gain = 1.0F;
        float along = 1.0F;                  // share of the force along the wheel axis (-1..1)
        float constantMagnitude = 0.0F;
        float rampStart = 0.0F, rampEnd = 0.0F;
        std::optional<FfbEnvelope> envelope;
        float center = 0.0F, deadband = 0.0F, posCoef = 0.0F, negCoef = 0.0F, posSat = 1.0F, negSat = 1.0F;
        float magnitude = 0.0F, offset = 0.0F, periodMs = 0.0F, phaseDeg = 0.0F;
        bool started = false;
        std::int64_t startedAtMs = 0;
        std::optional<int> loops;            // none = infinite
    };
    enum class ParamKind { Constant, Ramp, Periodic, Condition, Envelope };
    using BlockAction = std::function<void(Block&)>;
    // A parameter report's numbers, to tell which block it continues (see above).
    using Values = std::array<float, 6>;
    struct Pending {
        BlockAction action;
        Values values{};
        std::int64_t sinceMs = 0;   // the first value that waited here (a newer one keeps it)
    };
    using PendingKey = std::pair<ParamKind, int>;   // the kind, the position in its run
    struct Sent {
        bool playing = false;
        FfbEffect effect;
    };

    static std::optional<ParamKind> kindOf(int pidType);
    static std::optional<FfbEffectType> typeFor(int pidType);

    Block* withBlock(std::uint8_t ebi);
    void withParams(std::uint8_t ebi, ParamKind kind, const BlockAction& action, const Values& values);
    // The same numbers as a block holds them now.
    [[nodiscard]] static Values valuesOf(ParamKind kind, const Block& b);
    // Parameters no Set Effect claimed: to a block of their kind (see above; `all`, or those waiting kPendingMs).
    void flushPending(std::int64_t nowMs, bool all);
    // Unclaimed parameters of one kind (in run order) to their blocks (see above).
    void applyUnclaimed(ParamKind kind, const std::vector<Pending>& waiting);
    // The first (lowest position) waiting parameter of `kind`, applied to `b` and dropped.
    void claimPending(ParamKind kind, Block& b);
    void onSetEffect(std::span<const std::uint8_t> d, std::int64_t nowMs);
    void onSetCondition(std::span<const std::uint8_t> d);
    void onSetPeriodic(std::span<const std::uint8_t> d);
    void onEffectOperation(std::span<const std::uint8_t> d, std::int64_t nowMs);
    void onBlockFree(std::uint8_t ebi, std::int64_t nowMs);
    void onDeviceControl(std::uint8_t command, std::int64_t nowMs);
    void resetAll(std::int64_t nowMs);
    [[nodiscard]] bool playing(const Block& b, std::int64_t nowMs) const;
    // The effect at `nowMs`: its envelope and a ramp's level worked out for that moment.
    [[nodiscard]] FfbEffect toEffect(const Block& b, std::int64_t nowMs) const;
    void sendChanges(std::int64_t nowMs, bool force);
    void send(FfbPacket::Kind kind, std::uint8_t slot, bool playing, const FfbEffect& effect, std::int64_t nowMs);

    Sender send_;
    Logger log_;
    mutable std::mutex mutex_;
    std::map<std::uint8_t, Block> blocks_;
    std::array<std::optional<std::uint8_t>, kMaxSlots> slotOwner_{};
    std::array<std::optional<Sent>, kMaxSlots> sent_{};
    std::map<PendingKey, Pending> pending_;
    // The run of EBI-0 parameters being received (see above).
    std::optional<ParamKind> runKind_;
    int runPosition_ = 0;
    std::int64_t runLastMs_ = 0;
    bool runBroken_ = true;                   // another report came since the last EBI-0 parameter
    std::uint8_t lastOutId_ = 0;              // the report before (for Windows' copies)
    std::vector<std::uint8_t> lastOutBytes_;
    std::int64_t lastOutMs_ = INT64_MIN / 2;
    std::map<ParamKind, std::uint8_t> lastOfKind_;
    std::set<std::string> seen_;
    bool loggedEbiZero_ = false;
    int traceLeft_ = 0;        // off
    int traceLines_ = 0;
    bool traceQuiet_ = false;                // a refresh is being sent (not traced)
    std::int64_t traceStartMs_ = 0;
    void trace(const std::string& text, std::int64_t nowMs);
    // One line of what the game does (see above), within the limit.
    void event(const std::string& text, std::int64_t nowMs, LogLevel level = LogLevel::Debug);
    [[nodiscard]] std::string describe(std::uint8_t ebi, const Block& b) const;
    std::int64_t eventWindowMs_ = INT64_MIN / 2;
    int eventsInWindow_ = 0;
    int eventsSkipped_ = 0;

    // Effect block allocation (device-managed pool).
    std::uint32_t allocated_ = 0;            // bit n = EBI n+1
    std::uint8_t lastAllocated_ = 0;
    std::uint8_t lastLoadStatus_ = 3;

    std::uint32_t sequence_ = 0;
    std::int64_t lastSendMs_ = INT64_MIN / 2;
    std::int64_t lastRefreshMs_ = INT64_MIN / 2;
    float deviceGain_ = 1.0F;
    bool actuatorsOn_ = true;
    bool paused_ = false;
    std::uint64_t reportsHandled_ = 0;
    std::uint64_t packetsSent_ = 0;
    std::int64_t lastReportMs_ = 0;
    std::int64_t streamStartMs_ = 0;         // the first report of the current stream (gaps under kStreamGapMs)
    bool quiet_ = false;                     // kGameQuietMs without a report while a pushing force plays
    void noteReport(std::int64_t nowMs);
};

}  // namespace wheelbri
