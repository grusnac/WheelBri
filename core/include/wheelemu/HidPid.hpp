#pragma once

#include "wheelemu/FfbPacket.hpp"

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

namespace wheelemu {

// HID PID force feedback, device side: what a virtual wheel with a PID descriptor (HidG920.hpp) receives
// from Windows' pid.dll when a game plays DirectInput effects, turned into the service's FfbPackets. The
// C++ counterpart of tools/vbridge/PidForwarder.cs (checked against the same reports), plus the effect
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
// they are kept pending for the next Set Effect of the same kind (and applied to the last block of that
// kind, for a game changing the magnitude of a running effect).
//
// Thread-safe: a driver calls it from I/O callbacks and a timer.
class HidPid {
public:
    static constexpr std::size_t kMaxSlots = 8;          // the service's slots (IFfbSink::kMaxSlots)
    static constexpr std::uint16_t kPoolSize = 0xFFFF;

    using Sender = std::function<void(const FfbPacket&)>;
    using Logger = std::function<void(const std::string&)>;

    HidPid(Sender send, Logger log);

    // Output report `reportId` with its data (report ID removed).
    void output(std::uint8_t reportId, std::span<const std::uint8_t> data, std::int64_t nowMs);

    // Feature report written by the host (report ID removed). Create New Effect allocates a block.
    void setFeature(std::uint8_t reportId, std::span<const std::uint8_t> data, std::int64_t nowMs);

    // Feature report read by the host: fills `out` (report ID at [0]); the size written, 0 = not supported.
    [[nodiscard]] std::size_t getFeature(std::uint8_t reportId, std::span<std::uint8_t> out) const;

    // Call every ~10 ms: effects that ran out, keep-alives (once a game used force feedback), refresh.
    void tick(std::int64_t nowMs);

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
        float center = 0.0F, deadband = 0.0F, posCoef = 0.0F, negCoef = 0.0F, posSat = 1.0F, negSat = 1.0F;
        float magnitude = 0.0F, offset = 0.0F, periodMs = 0.0F, phaseDeg = 0.0F;
        bool started = false;
        std::int64_t startedAtMs = 0;
        std::optional<int> loops;            // none = infinite
    };
    enum class ParamKind { Constant, Ramp, Periodic, Condition };
    using BlockAction = std::function<void(Block&)>;
    struct Sent {
        bool playing = false;
        FfbEffect effect;
    };

    static std::optional<ParamKind> kindOf(int pidType);
    static std::optional<FfbEffectType> typeFor(int pidType);

    Block* withBlock(std::uint8_t ebi);
    void withParams(std::uint8_t ebi, ParamKind kind, const BlockAction& action);
    void onSetEffect(std::span<const std::uint8_t> d);
    void onSetCondition(std::span<const std::uint8_t> d);
    void onSetPeriodic(std::span<const std::uint8_t> d);
    void onEffectOperation(std::span<const std::uint8_t> d, std::int64_t nowMs);
    void onBlockFree(std::uint8_t ebi, std::int64_t nowMs);
    void onDeviceControl(std::uint8_t command, std::int64_t nowMs);
    void resetAll(std::int64_t nowMs);
    [[nodiscard]] bool playing(const Block& b, std::int64_t nowMs) const;
    [[nodiscard]] FfbEffect toEffect(const Block& b) const;
    void sendChanges(std::int64_t nowMs, bool force);
    void send(FfbPacket::Kind kind, std::uint8_t slot, bool playing, const FfbEffect& effect, std::int64_t nowMs);

    Sender send_;
    Logger log_;
    mutable std::mutex mutex_;
    std::map<std::uint8_t, Block> blocks_;
    std::array<std::optional<std::uint8_t>, kMaxSlots> slotOwner_{};
    std::array<std::optional<Sent>, kMaxSlots> sent_{};
    std::map<ParamKind, BlockAction> pending_;
    std::map<ParamKind, std::uint8_t> lastOfKind_;
    std::set<std::string> seen_;
    bool loggedEbiZero_ = false;

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
};

}  // namespace wheelemu
