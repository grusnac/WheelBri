#pragma once

// The fuzz targets (FuzzTargets.cpp): one input, the first byte picking the target (kTargets of them).

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace wheelbri::fuzz {

inline constexpr std::uint8_t kTargets = 14;

// A promise broken by an input (a decoded packet that does not encode back, a force that is not a number).
struct Broken : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Runs one input. Throws Broken when a promise does not hold; anything worse is a crash.
void runOne(std::span<const std::uint8_t> input);

}  // namespace wheelbri::fuzz
