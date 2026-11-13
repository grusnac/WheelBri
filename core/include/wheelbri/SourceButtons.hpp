// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace wheelbri {

// The buttons a source device (the wheel, an accessory) has pressed: bit n = source button n, 0..127. DirectInput
// reads up to 128 (DIJOYSTATE2); a Moza base reports its shifter's gears on 112..119.
//
// Two words, not std::bitset<128>: constexpr in C++20, no exception path (bitset::test throws), the words for the
// telemetry's hex and a fast walk over what is pressed (next), and it builds everywhere WheelState.hpp goes (the
// driver too). No shifts on purpose: code still treating buttons as one integer does not compile.
struct SourceButtons {
    static constexpr int kCount = 128;
    static constexpr int kWordBits = 64;

    std::array<std::uint64_t, 2> words{};   // [0] = buttons 0..63, [1] = 64..127

    // Bits 0..63 (and 64..127): for tests and for readers that fill a word at once.
    [[nodiscard]] static constexpr SourceButtons fromBits(const std::uint64_t low, const std::uint64_t high = 0) {
        return SourceButtons{{low, high}};
    }

    [[nodiscard]] constexpr bool test(const int n) const {
        return n >= 0 && n < kCount &&
               ((words[static_cast<std::size_t>(n / kWordBits)] >> (n % kWordBits)) & 1U) != 0;
    }
    constexpr void set(const int n, const bool down = true) {
        if (n < 0 || n >= kCount) return;
        const std::uint64_t bit = std::uint64_t{1} << (n % kWordBits);
        std::uint64_t& word = words[static_cast<std::size_t>(n / kWordBits)];
        word = down ? (word | bit) : (word & ~bit);
    }
    constexpr void reset(const int n) { set(n, false); }
    constexpr void clear() { words = {}; }

    [[nodiscard]] constexpr bool any() const { return (words[0] | words[1]) != 0; }
    [[nodiscard]] constexpr bool none() const { return !any(); }
    [[nodiscard]] constexpr int count() const { return std::popcount(words[0]) + std::popcount(words[1]); }

    // The lowest pressed button at or after `from`, or -1: for (int b = s.next(0); b >= 0; b = s.next(b + 1)).
    [[nodiscard]] constexpr int next(const int from) const {
        for (int n = from < 0 ? 0 : from; n < kCount;) {
            const auto w = static_cast<std::size_t>(n / kWordBits);
            const std::uint64_t rest = words[w] >> (n % kWordBits);
            if (rest != 0) return n + std::countr_zero(rest);
            n = (static_cast<int>(w) + 1) * kWordBits;
        }
        return -1;
    }

    friend constexpr SourceButtons operator&(const SourceButtons& a, const SourceButtons& b) {
        return fromBits(a.words[0] & b.words[0], a.words[1] & b.words[1]);
    }
    friend constexpr SourceButtons operator|(const SourceButtons& a, const SourceButtons& b) {
        return fromBits(a.words[0] | b.words[0], a.words[1] | b.words[1]);
    }
    friend constexpr SourceButtons operator^(const SourceButtons& a, const SourceButtons& b) {
        return fromBits(a.words[0] ^ b.words[0], a.words[1] ^ b.words[1]);
    }
    friend constexpr SourceButtons operator~(const SourceButtons& a) { return fromBits(~a.words[0], ~a.words[1]); }
    friend constexpr bool operator==(const SourceButtons&, const SourceButtons&) = default;
};

}  // namespace wheelbri
