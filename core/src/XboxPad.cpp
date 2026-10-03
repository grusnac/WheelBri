#include "wheelbri/XboxPad.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <utility>

namespace wheelbri {

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

XboxMirror::Result XboxMirror::update(const TargetReport& wheel, const std::int64_t nowMs) {
    Result result;
    result.g920 = wheel;   // the G920 always gets everything
    if (mode_ != XboxPadMode::Buttons) {
        return result;
    }
    const auto pressedBy = [](const std::int32_t pedal) { return 65535 - std::clamp<std::int32_t>(pedal, 0, 65535); };
    const std::array<std::int32_t, 3> pedals = {wheel.throttle, wheel.brake, wheel.clutch};
    if (!started_) {   // what is held at the start is not an input
        started_ = true;
        lastButtons_ = wheel.buttons;
        lastHat_ = wheel.hat;
        for (std::size_t i = 0; i < pedals.size(); ++i) pedalDown_[i] = pressedBy(pedals[i]) > kPedalPressed;
        return result;
    }

    // An input from the wheel: a button or the D-pad pressed, a pedal pressed (not steering: see the header).
    Cause cause = Cause::None;
    if ((wheel.buttons & ~lastButtons_) != 0) cause = Cause::Button;
    lastButtons_ = wheel.buttons;
    if (cause == Cause::None && wheel.hat != lastHat_ && wheel.hat >= 0) cause = Cause::Dpad;
    lastHat_ = wheel.hat;
    for (std::size_t i = 0; i < pedals.size(); ++i) {
        const bool down = pressedBy(pedals[i]) > kPedalPressed;
        if (down && !pedalDown_[i] && cause == Cause::None) cause = Cause::Pedal;
        pedalDown_[i] = down;
    }

    if (cause != Cause::None && !pending_) {
        pending_ = true;
        pendingCause_ = cause;
    }
    if (pending_ && nowMs - nudgedAtMs_ >= kNudgeGapMs) {
        pending_ = false;
        nudgedAtMs_ = nowMs;
        sign_ = static_cast<std::int16_t>(-sign_);
        ++nudges_;
        result.nudged = pendingCause_;
    }
    if (nowMs - nudgedAtMs_ < kNudgeMs) {
        result.pad.thumbRY = static_cast<std::int16_t>(sign_ * kNudge);   // the right stick: the camera, briefly
    }
    return result;
}

}  // namespace wheelbri
