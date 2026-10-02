#pragma once

#include "wheelemu/DeviceProfile.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace wheelemu {

// The virtual Xbox 360 controller next to the virtual G920 (driver/xusb, `wheelemu-service --virtual-g920`), so
// games that pick their on-screen button prompts by controller (NFS 2015) show Xbox buttons. Its sticks and
// triggers never move: it can never steer or drive. The rules are the ones tools/vbridge/XboxPad.cs settled in NFS:
//
//   buttons: the wheel's A, B, X, Y, Menu (Start), View (Back) and D-pad press the pad's INSTEAD of the G920's while
//            no pedal is pressed and for 2 s after; while driving they are the G920's. One press never reaches both
//            (the game acted twice, and NFS reads G920 input as keyboard-type input and puts the keyboard prompts
//            back). Paddles, stick clicks (the pad's right stick click is NFS's photo) and the Xbox button are never
//            passed on. A button held when driving stops must be let go first.
//            In a menu the G920's steering is held too (turning the wheel put the keyboard prompts back). "In a
//            menu": no pedal for 2 s and either a pad button pressed since the last pedal (NFS's open-world menus
//            keep the driving spring on) or the game playing all its effects at zero for 1.5 s (its other menus; a
//            game that never used force feedback never gets this). The steering comes back with the next pedal.
//   idle:    the pad is there, never pressed (games that only look at which controllers exist).

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

// The rules above, applied to every wheel state the service publishes.
class XboxMirror {
public:
    explicit XboxMirror(XboxPadMode mode) : mode_(mode) {}

    struct Result {
        TargetReport g920;                  // what the virtual G920 reports (D-pad and steering possibly held back)
        XusbPad pad;                        // what the Xbox controller reports
        std::optional<bool> menuChanged;    // set when "in a menu" turned on (true) or off (false): for the log
        bool firstPress = false;            // the first pad press passed on: for the log
    };

    // `gameForce`: the strongest force the game plays now (0..1); `gameUsesForce`: it has used force feedback.
    [[nodiscard]] Result update(const TargetReport& wheel, std::int64_t nowMs, float gameForce, bool gameUsesForce);

    [[nodiscard]] std::uint64_t presses() const { return presses_; }
    [[nodiscard]] bool inMenu() const { return inMenu_; }

    static constexpr std::int64_t kPedalQuietMs = 2000;
    static constexpr std::int64_t kForceQuietMs = 1500;
    static constexpr float kForceZero = 0.01F;
    static constexpr int kPedalPressed = 65535 / 10;   // 10 % of travel (G920 pedals: 65535 = released)

private:
    XboxPadMode mode_;
    std::int64_t lastPedalMs_ = INT64_MIN / 2;
    std::int64_t lastForceMs_ = 0;
    bool haveForceTime_ = false;
    bool inMenu_ = false;
    bool menuByPad_ = false;   // a pad button was pressed since the last pedal
    int heldSteering_ = 32768;
    std::uint16_t blocked_ = 0;
    bool hatBlocked_ = false;
    std::uint32_t g920Hidden_ = 0;   // G920 Menu/View bits that went to the pad, kept off the G920 until released
    std::uint16_t lastButtons_ = 0;
    std::uint64_t presses_ = 0;
    bool first_ = true;
};

}  // namespace wheelemu
