#pragma once

// Little-endian bytes, as every wire format here has them (StatePacket, FfbPacket, LogLevelPacket, the helper's pipe,
// HID reports): one bounds-checked reader and writer, and the plain functions for a field at an offset. A read past
// the end gives 0 and marks the reader failed; a write past the end is dropped and marks the writer failed. The
// decoders check the size first, so neither happens on their paths; the checks are what keeps a mistake there from
// reading or writing outside the buffer.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace wheelbri::bytes {

[[nodiscard]] constexpr std::uint16_t le16(const std::span<const std::uint8_t> d, const std::size_t at) {
    return at + 2 <= d.size() ? static_cast<std::uint16_t>(d[at] | (d[at + 1] << 8U)) : 0;
}

[[nodiscard]] constexpr std::int16_t le16s(const std::span<const std::uint8_t> d, const std::size_t at) {
    return static_cast<std::int16_t>(le16(d, at));
}

[[nodiscard]] constexpr std::uint32_t le32(const std::span<const std::uint8_t> d, const std::size_t at) {
    return at + 4 <= d.size()
               ? static_cast<std::uint32_t>(le16(d, at)) | (static_cast<std::uint32_t>(le16(d, at + 2)) << 16U)
               : 0;
}

class Reader {
public:
    explicit constexpr Reader(const std::span<const std::uint8_t> in) : in_(in) {}

    constexpr std::uint8_t u8() {
        if (offset_ + 1 > in_.size()) return fail();
        return in_[offset_++];
    }
    constexpr std::uint16_t u16() {
        if (offset_ + 2 > in_.size()) return fail();
        const std::uint16_t value = le16(in_, offset_);
        offset_ += 2;
        return value;
    }
    constexpr std::uint32_t u32() {
        if (offset_ + 4 > in_.size()) return fail();
        const std::uint32_t value = le32(in_, offset_);
        offset_ += 4;
        return value;
    }
    constexpr std::int8_t i8() { return static_cast<std::int8_t>(u8()); }
    constexpr std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    // As stored: the caller decides what a non-finite value becomes.
    constexpr float f32() { return std::bit_cast<float>(u32()); }

    [[nodiscard]] constexpr bool failed() const { return failed_; }
    [[nodiscard]] constexpr std::size_t offset() const { return offset_; }

private:
    constexpr std::uint8_t fail() {
        failed_ = true;
        offset_ = in_.size();
        return 0;
    }

    std::span<const std::uint8_t> in_;
    std::size_t offset_ = 0;
    bool failed_ = false;
};

class Writer {
public:
    explicit constexpr Writer(const std::span<std::uint8_t> out) : out_(out) {}

    constexpr void u8(const std::uint8_t value) {
        if (offset_ + 1 > out_.size()) {
            failed_ = true;
            return;
        }
        out_[offset_++] = value;
    }
    constexpr void u16(const std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value));
        u8(static_cast<std::uint8_t>(value >> 8U));
    }
    constexpr void u32(const std::uint32_t value) {
        u16(static_cast<std::uint16_t>(value));
        u16(static_cast<std::uint16_t>(value >> 16U));
    }
    constexpr void i8(const std::int8_t value) { u8(static_cast<std::uint8_t>(value)); }
    constexpr void i32(const std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }
    constexpr void f32(const float value) { u32(std::bit_cast<std::uint32_t>(value)); }

    [[nodiscard]] constexpr bool failed() const { return failed_; }
    [[nodiscard]] constexpr std::size_t offset() const { return offset_; }

private:
    std::span<std::uint8_t> out_;
    std::size_t offset_ = 0;
    bool failed_ = false;
};

}  // namespace wheelbri::bytes
