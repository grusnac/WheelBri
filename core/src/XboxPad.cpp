#include "wheelemu/XboxPad.hpp"

#include <array>
#include <utility>

namespace wheelemu {

namespace {

constexpr std::uint32_t kMagic = 0x50584557;   // "WEXP"
constexpr std::uint16_t kVersion = 1;

void put16(std::array<std::uint8_t, kXusbPacketSize>& b, std::size_t at, std::uint16_t v) {
    b[at] = static_cast<std::uint8_t>(v & 0xFFU);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8U);
}

std::uint16_t get16(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8U));
}

// G920 button bit -> XInput button (A, B, X, Y, paddles R/L, Menu, View, RSB, LSB, Xbox).
constexpr std::array<std::pair<int, std::uint16_t>, 8> kButtonMap = {{
    {0, XusbPad::kA}, {1, XusbPad::kB}, {2, XusbPad::kX}, {3, XusbPad::kY},
    {6, XusbPad::kStart}, {7, XusbPad::kBack}, {8, XusbPad::kRightThumb}, {9, XusbPad::kLeftThumb},
}};

// Hat 0..7 (0 = up, clockwise) -> D-pad bits; -1 = centred.
std::uint16_t dpad(const int hat) {
    switch (hat) {
        case 0: return XusbPad::kDpadUp;
        case 1: return XusbPad::kDpadUp | XusbPad::kDpadRight;
        case 2: return XusbPad::kDpadRight;
        case 3: return XusbPad::kDpadDown | XusbPad::kDpadRight;
        case 4: return XusbPad::kDpadDown;
        case 5: return XusbPad::kDpadDown | XusbPad::kDpadLeft;
        case 6: return XusbPad::kDpadLeft;
        case 7: return XusbPad::kDpadUp | XusbPad::kDpadLeft;
        default: return 0;
    }
}

}  // namespace

std::array<std::uint8_t, kXusbPacketSize> encodeXusbPacket(const XusbPad& pad) {
    std::array<std::uint8_t, kXusbPacketSize> b{};
    put16(b, 0, static_cast<std::uint16_t>(kMagic & 0xFFFFU));
    put16(b, 2, static_cast<std::uint16_t>(kMagic >> 16U));
    put16(b, 4, kVersion);
    put16(b, 6, pad.buttons);
    b[8] = pad.leftTrigger;
    b[9] = pad.rightTrigger;
    put16(b, 10, static_cast<std::uint16_t>(pad.thumbLX));
    put16(b, 12, static_cast<std::uint16_t>(pad.thumbLY));
    put16(b, 14, static_cast<std::uint16_t>(pad.thumbRX));
    put16(b, 16, static_cast<std::uint16_t>(pad.thumbRY));
    return b;
}

std::optional<XusbPad> decodeXusbPacket(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kXusbPacketSize) return std::nullopt;
    const std::uint32_t magic = get16(bytes, 0) | (static_cast<std::uint32_t>(get16(bytes, 2)) << 16U);
    if (magic != kMagic || get16(bytes, 4) != kVersion) return std::nullopt;
    XusbPad pad;
    pad.buttons = get16(bytes, 6);
    pad.leftTrigger = bytes[8];
    pad.rightTrigger = bytes[9];
    pad.thumbLX = static_cast<std::int16_t>(get16(bytes, 10));
    pad.thumbLY = static_cast<std::int16_t>(get16(bytes, 12));
    pad.thumbRX = static_cast<std::int16_t>(get16(bytes, 14));
    pad.thumbRY = static_cast<std::int16_t>(get16(bytes, 16));
    return pad;
}

std::optional<XboxPadMode> parseXboxPadMode(const std::string& text) {
    if (text == "off") return XboxPadMode::Off;
    if (text == "idle") return XboxPadMode::Idle;
    if (text == "buttons") return XboxPadMode::Buttons;
    return std::nullopt;
}

XboxMirror::Result XboxMirror::update(const TargetReport& wheel, const std::int64_t nowMs, const float gameForce,
                                      const bool gameUsesForce) {
    Result result;
    result.g920 = wheel;
    if (mode_ != XboxPadMode::Buttons) {
        return result;
    }
    if (!haveForceTime_) {
        haveForceTime_ = true;
        lastForceMs_ = nowMs;
    }
    if (65535 - wheel.throttle > kPedalPressed || 65535 - wheel.brake > kPedalPressed) {
        lastPedalMs_ = nowMs;
    }
    const bool driving = nowMs - lastPedalMs_ < kPedalQuietMs;
    if (gameForce > kForceZero) {
        lastForceMs_ = nowMs;
    }
    const bool inMenu = !driving && gameUsesForce && nowMs - lastForceMs_ >= kForceQuietMs;
    if (inMenu != inMenu_) {
        inMenu_ = inMenu;
        heldSteering_ = wheel.steering;
        result.menuChanged = inMenu;
    }

    std::uint16_t buttons = 0;
    for (const auto& [bit, button] : kButtonMap) {
        if ((wheel.buttons & (1U << bit)) != 0) buttons |= button;
    }
    const std::uint16_t hat = dpad(wheel.hat);

    // A button held when driving stops is not a press: it must be let go first.
    if (driving) {
        blocked_ = buttons;
        hatBlocked_ = hat != 0;
    } else {
        blocked_ &= buttons;
        hatBlocked_ = hatBlocked_ && hat != 0;
    }
    const std::uint16_t padButtons = driving ? 0 : static_cast<std::uint16_t>(buttons & ~blocked_);
    const std::uint16_t padHat = driving || hatBlocked_ ? 0 : hat;

    if (padHat != 0) result.g920.hat = -1;                    // the D-pad goes to the pad instead
    // Menu and View too: NFS reads G920 Menu/View as keyboard-type input and switches its prompts back.
    // A press that went to the pad stays off the G920 until let go, so holding it into driving is no pause.
    if ((padButtons & XusbPad::kStart) != 0) g920Hidden_ |= 1U << 6;
    if ((padButtons & XusbPad::kBack) != 0) g920Hidden_ |= 1U << 7;
    g920Hidden_ &= wheel.buttons;
    result.g920.buttons &= ~g920Hidden_;
    if (inMenu_) result.g920.steering = heldSteering_;        // turning the wheel in a menu reaches nothing

    result.pad.buttons = static_cast<std::uint16_t>(padButtons | padHat);
    const std::uint16_t newlyPressed = static_cast<std::uint16_t>(result.pad.buttons & ~lastButtons_);
    if (newlyPressed != 0) {
        ++presses_;
        if (first_) {
            first_ = false;
            result.firstPress = true;
        }
    }
    lastButtons_ = result.pad.buttons;
    return result;
}

}  // namespace wheelemu
