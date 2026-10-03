#pragma once

#include "wheelbri/DeviceProfile.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace wheelbri {

// The virtual Xbox 360 controller next to the virtual G920 (driver/xusb, `wheelbri-service --virtual-g920`), so
// games that pick their on-screen button prompts by controller (NFS 2015) show Xbox icons. EXPERIMENTAL: not shown to
// work yet. It never presses a button or a trigger: everything the wheel does goes to the G920.
//
//   buttons ("icons"): every input from the wheel (a button or D-pad press, a pedal pressed) reaches the G920 as it
//            is, and the pad then gets a "nudge": its right stick moved by kNudge for kNudgeMs. NFS reads G920 input
//            as keyboard-type input and shows keyboard icons; the pad's change right after should make the pad the
//            last controller used, so the icons go back to Xbox. A nudge of 2500 (inside XInput's 7849 dead zone,
//            on the left stick, also after steering) did nothing in NFS (2026-10-03): it is now outside the dead
//            zone, on the right stick (camera / car view: at most a brief look) and not after steering, so the
//            camera does not twitch while driving and the car is never steered by the pad. Until
//            2026-10-03 the wheel's buttons were moved to the pad instead (in menus), and the pad's own functions
//            took over (the Moza's R as the pad's right stick click took photos in NFS), pedal taps were swallowed
//            and the steering was held: all gone.
//   idle:    the pad is there, never touched (games that only look at which controllers exist).

// XINPUT_GAMEPAD: wButtons bits as XInput defines them, triggers 0..255, sticks -32768..32767.
struct XusbPad {
    static constexpr std::uint16_t kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008,
                                   kStart = 0x0010, kBack = 0x0020, kLeftThumb = 0x0040, kRightThumb = 0x0080,
                                   kLeftShoulder = 0x0100, kRightShoulder = 0x0200, kGuide = 0x0400, kA = 0x1000,
                                   kB = 0x2000, kX = 0x4000, kY = 0x8000;
    std::uint16_t buttons = 0;
    std::uint8_t leftTrigger = 0;
    std::uint8_t rightTrigger = 0;
    std::int16_t thumbLX = 0, thumbLY = 0, thumbRX = 0, thumbRY = 0;

    friend bool operator==(const XusbPad&, const XusbPad&) = default;
};

// Service -> driver/xusb over UDP 127.0.0.1 (kXusbPort): 20 bytes, little-endian, magic "WEXP".
//   0 u32 magic  4 u16 version  6 u16 buttons  8 u8 LT  9 u8 RT  10 i16 LX  12 i16 LY  14 i16 RX  16 i16 RY
//  18 u16 reserved
inline constexpr std::uint16_t kXusbPort = 41332;
inline constexpr std::size_t kXusbPacketSize = 20;
[[nodiscard]] std::array<std::uint8_t, kXusbPacketSize> encodeXusbPacket(const XusbPad& pad);
[[nodiscard]] std::optional<XusbPad> decodeXusbPacket(std::span<const std::uint8_t> bytes);

enum class XboxPadMode { Off, Idle, Buttons };
[[nodiscard]] std::optional<XboxPadMode> parseXboxPadMode(const std::string& text);

// The rules above, applied to every wheel state the service publishes. Allocation-free (the 1 kHz loop).
class XboxMirror {
public:
    explicit XboxMirror(XboxPadMode mode) : mode_(mode) {}

    // What made a nudge (for the log).
    enum class Cause { None, Button, Dpad, Pedal };

    struct Result {
        TargetReport g920;                  // what the virtual G920 reports: always the wheel, untouched
        XusbPad pad;                        // what the Xbox controller reports: at most a right stick nudge
        Cause nudged = Cause::None;         // a new nudge started now, and why
    };

    [[nodiscard]] Result update(const TargetReport& wheel, std::int64_t nowMs);

    [[nodiscard]] std::uint64_t nudges() const { return nudges_; }

    static constexpr std::int16_t kNudge = 10000;       // of 32767: ~30 %, past XInput's 7849 dead zone
    static constexpr std::int64_t kNudgeMs = 60;        // how long the stick stays nudged
    static constexpr std::int64_t kNudgeGapMs = 100;    // at least this between nudges
    static constexpr int kPedalPressed = 65535 / 10;    // 10 % of travel (G920 pedals: 65535 = released)

private:
    XboxPadMode mode_;
    bool started_ = false;
    std::uint32_t lastButtons_ = 0;
    std::int8_t lastHat_ = -1;
    std::array<bool, 3> pedalDown_{};
    std::int64_t nudgedAtMs_ = INT64_MIN / 2;
    bool pending_ = false;              // an input came within kNudgeGapMs of the last nudge: nudge when allowed
    Cause pendingCause_ = Cause::None;
    std::int16_t sign_ = 1;             // alternates, so every nudge is a change even if the last one is still on
    std::uint64_t nudges_ = 0;
};

}  // namespace wheelbri
