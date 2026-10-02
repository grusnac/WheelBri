#pragma once

#include <cstdint>
#include <string>

namespace wheelemu {

// [emulator] source_device: true when `needle` (case-insensitive) is part of the device's name or of its
// "vvvv:pppp" USB id, so "Moza", "346e" (Moza's vendor id) and "346e:0005" all find the R3.
[[nodiscard]] bool deviceMatches(const std::string& name, std::uint16_t vendorId, std::uint16_t productId,
                                 const std::string& needle);

}  // namespace wheelemu
