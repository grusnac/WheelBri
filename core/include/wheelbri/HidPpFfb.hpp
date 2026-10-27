#pragma once

#include "wheelbri/FfbPacket.hpp"
#include "wheelbri/FfbShape.hpp"
#include "wheelbri/LogLevel.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>

namespace wheelbri {

// HID++ 0x8123 (ForceFeedback) effects, played: what Logitech's force feedback driver (hidpp_forcefeedback_x64.dll,
// DirectInput's driver for a G923 Xbox once G HUB registers it, the route a TrueForce game's force takes) and G HUB
// download, turned into the FfbPackets HID PID's effects become (HidPid.hpp), so the service plays them on the rig.
// Written from the feature's public description; checked against Logitech's driver in the driver log.
//
// Parameters after the function byte, big-endian; levels and coefficients signed 16-bit (+-0x7FFF = +-1, + = right):
//   download  [0] slot (0: pick a free one) [1] type (0x80 = start at once) [2..3] duration ms (0: until stopped)
//             [4..5] start delay ms, then by type:
//     0x00 constant          [6..7] level, envelope from 8
//     0x01..0x05 periodic    (sine, square, triangle, saw up, saw down) [6..7] magnitude [8..9] offset
//                            [10..11] period ms [12..13] phase (0x10000 = 360 deg), envelope from 14
//     0x06..0x09 condition   (spring, damper, friction, inertia) [6..7] left saturation [8..9] left coefficient
//                            [10..11] deadband [12..13] centre [14..15] right coefficient [16..17] right saturation;
//                            saturation and deadband 0..0x7FFF = 0..1
//     0x0A ramp              [6..7] start [8..9] end, envelope from 10
//     envelope               [0] attack level (0..0xFF) [1..2] attack ms [3] fade level [4..5] fade ms
//   effect state  [0] slot [1] 1 stop, 2 play, 3 pause (as stop)
//   destroy       [0] slot
// A download to a slot in use updates it (how a game moves its constant force) and keeps it playing. At most
// kServiceSlots effects play at once (the service's slots); one more is not played, and the log says so.
// Keep-alives and quiet rules as HidPid's: one every kKeepAliveMs once anything was asked; none when no request came
// for HidPid::kGameQuietMs (or HidPid::kStreamQuietMs after a stream) while a pushing force plays, so the service's
// watchdog stops a game that crashed. Thread-safe: requests and ticks come on different threads.
class HidPpFfb {
public:
    using Sender = std::function<void(const FfbPacket&)>;
    using Logger = std::function<void(LogLevel, const std::string&)>;
    static constexpr std::uint8_t kSlots = 63;            // 1..63 (HidPp's info says 64: one is reserved)
    static constexpr std::size_t kServiceSlots = 8;       // IFfbSink::kMaxSlots
    static constexpr std::int64_t kKeepAliveMs = 100;
    static constexpr std::int64_t kRefreshMs = 250;
    static constexpr int kShownDownloads = 20;           // decoded at info, the rest at debug

    HidPpFfb(Sender send, Logger log);

    // The slot the effect went to (1..kSlots), or 0: no free slot, a slot out of range, or a type it does not know.
    std::uint8_t download(std::span<const std::uint8_t> params, std::int64_t nowMs);
    void setState(std::uint8_t slot, std::uint8_t state, std::int64_t nowMs);
    void destroy(std::uint8_t slot, std::int64_t nowMs);
    void resetAll(std::int64_t nowMs);
    // The global gain (0..1) for every effect.
    void setGain(float gain, std::int64_t nowMs);
    // Every ~10 ms: delays and durations, envelopes and ramps, refreshes, keep-alives.
    void tick(std::int64_t nowMs);
    // Everything stops (device going away).
    void stopAll(std::int64_t nowMs);
    [[nodiscard]] std::uint64_t packetsSent() const;

private:
    struct Effect {
        bool loaded = false;
        FfbEffect effect;
        FfbShape shape;
        std::int64_t durationMs = 0;   // 0: until stopped
        std::int64_t delayMs = 0;
        bool playing = false;
        std::int64_t startedAtMs = 0;
        int serviceSlot = -1;
    };
    struct Sent {
        bool playing = false;
        FfbEffect effect;
    };

    void request(std::int64_t nowMs);
    void play(std::uint8_t slot, std::int64_t nowMs);
    void stop(std::uint8_t slot, std::int64_t nowMs);
    // The effect as it plays now; nullopt while it waits for its delay or once its duration ran out.
    [[nodiscard]] std::optional<FfbEffect> now(const Effect& e, std::int64_t nowMs) const;
    void sendChanges(std::int64_t nowMs, bool refresh);
    void send(FfbPacket::Kind kind, std::uint8_t slot, bool playing, const FfbEffect& effect, std::int64_t nowMs);
    [[nodiscard]] bool pushing(float above) const;

    Sender send_;
    Logger log_;
    mutable std::mutex mutex_;
    std::array<Effect, kSlots + 1> effects_{};               // by HID++ slot; 0 unused
    std::array<std::uint8_t, kServiceSlots> owner_{};        // the HID++ slot each service slot plays (0: none)
    std::array<std::optional<Sent>, kServiceSlots> sent_{};
    float gain_ = 1.0F;
    std::uint32_t sequence_ = 0;
    std::uint64_t packetsSent_ = 0;
    std::uint64_t requests_ = 0;
    int shown_ = 0;
    bool effectsSeen_ = false;
    bool quiet_ = false;
    bool overflowNoted_ = false;
    std::int64_t lastRequestMs_ = 0;
    std::int64_t streamStartMs_ = 0;
    std::int64_t lastSendMs_ = 0;
    std::int64_t lastRefreshMs_ = 0;
};

}  // namespace wheelbri
