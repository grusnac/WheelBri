#pragma once

#include "wheelbri/FfbPacket.hpp"
#include "wheelbri/LogLevel.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>

namespace wheelbri {

// Logitech's classic force feedback commands, played: what Logitech's older-wheel force feedback driver (G HUB's
// jerry_forcefeedback, DirectInput's driver for a G25 or G27) writes to the virtual wheel's classic identity
// (VirtualWheel::classic), turned into the FfbPackets HID PID's effects become (HidPid.hpp), so the service plays them
// on the rig. Written from "Logitech Force Feedback Protocol V1.6" (Logitech, 2014; chapter 3, Classic Format), and
// checked against what the driver sent in ACC (2026-11-03).
//
// A command is one 7-byte output report: [0] F3 F2 F1 F0 (bits 7..4: the force slots it applies to) | CMD (bits 3..0),
// [1] force type or command parameter, [2..6] parameters. Wheel positions run 0 (left) .. 255 (right); a force level is
// one byte, 127/128 none, 0 the full force towards the 0 end (left, -1 here), 255 towards the 255 end (right, +1). (The
// document also says a level of 0 spins the wheel clockwise; played that way, ACC's steering force pushed a Moza R3
// away from the centre, 2026-11-03: the wheel left of centre with a force to the right in 2328 of 2465 samples.)
//   0x00 download (to play later)  0x01 download and play  0x02 play  0x03 stop  0x0c refresh (new parameters, playing)
//   0x04/0x05 default spring on/off, 0x0e set default spring: logged only (WheelBri's own centring spring is the
//   target's, DeviceIdentity::centringSpring)
//   0xf8 (F3..F0 all set, CMD 0x08) extended: wheel range and identity commands, logged only (the rotation is the
//   mapping's); 0x08 normal mode, 0x09 LEDs, 0x0a watchdog, 0x0b raw mode, 0x0d fixed loop, 0x0f dead band: logged
// Force types played (per slot; coefficients as DirectInput's, -1..1, + = pushes back towards the centre):
//   0x00 constant (bytes 2..5: F0..F3's levels)    0x08 variable (F0's level byte 2, F2's byte 3; steps not played)
//   0x01 spring, 0x0b high-resolution spring       (D1, D2 dead band limits, K1 left / K2 right, S1/S2 inverted, CLIP)
//   0x02 damper, 0x0c high-resolution damper       (K1 left, S1, K2 right, S2)
//   0x03, 0x0d auto-centring spring                (K1, K2, CLIP; centred, dead band 1 count each side)
//   0x0e friction                                  (K1, K2 0..255, CLIP, S1/S2)
// K of the 3-bit types: 1/4, 1/2, 3/4, 1, 3/2, 2, 3, 4 times the offset (a G25's and G27's table), as a quarter of
// that; of the high-resolution ones 0..15, linear. Periodic types (0x04..0x07, 0x0a) and ramps (0x09) are logged, not
// played yet: no game seen uses them.
// Keep-alives and quiet rules as HidPpFfb's. Thread-safe: reports and ticks come on different threads.
class ClassicFfb {
public:
    using Sender = std::function<void(const FfbPacket&)>;
    using Logger = std::function<void(LogLevel, const std::string&)>;
    static constexpr std::size_t kSlots = 4;            // F0..F3: the service's slots 0..3
    static constexpr std::size_t kReportSize = 7;
    static constexpr std::int64_t kKeepAliveMs = 100;
    static constexpr std::int64_t kRefreshMs = 250;
    static constexpr int kShownCommands = 30;           // decoded at info, the rest at debug

    ClassicFfb(Sender send, Logger log);

    // One output report (kReportSize bytes; shorter ones are padded with zeros, longer ones cut).
    void command(std::span<const std::uint8_t> report, std::int64_t nowMs);
    // Every ~10 ms: refreshes, keep-alives, quiet rules.
    void tick(std::int64_t nowMs);
    // Everything stops (device going away).
    void stopAll(std::int64_t nowMs);
    [[nodiscard]] std::uint64_t packetsSent() const;

    // The effect a download's bytes describe for force slot `slot` (0..3); nullopt for a type not played.
    [[nodiscard]] static std::optional<FfbEffect> decode(const std::array<std::uint8_t, kReportSize>& report,
                                                         std::size_t slot);
    // A force level byte as a signed level: 0 -> -1 (left), 127.5 -> 0, 255 -> +1 (right).
    [[nodiscard]] static float level(std::uint8_t byte);

private:
    struct Slot {
        std::optional<FfbEffect> effect;   // loaded
        bool playing = false;
    };
    struct Sent {
        bool playing = false;
        FfbEffect effect;
    };

    void request(std::int64_t nowMs);
    void note(const std::string& what);
    void sendChanges(std::int64_t nowMs, bool refresh);
    void send(FfbPacket::Kind kind, std::uint8_t slot, bool playing, const FfbEffect& effect, std::int64_t nowMs);
    [[nodiscard]] bool pushing(float above) const;

    Sender send_;
    Logger log_;
    mutable std::mutex mutex_;
    std::array<Slot, kSlots> slots_{};
    std::array<std::optional<Sent>, kSlots> sent_{};
    std::set<std::string> noted_;   // things logged once (commands and types not played)
    std::uint32_t sequence_ = 0;
    std::uint64_t packetsSent_ = 0;
    std::uint64_t requests_ = 0;
    int shown_ = 0;
    bool effectsSeen_ = false;
    bool quiet_ = false;
    std::int64_t lastRequestMs_ = 0;
    std::int64_t streamStartMs_ = 0;
    std::int64_t lastSendMs_ = 0;
    std::int64_t lastRefreshMs_ = 0;
};

}  // namespace wheelbri
