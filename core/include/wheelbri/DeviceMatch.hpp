#pragma once

#include <cstdint>
#include <string>

namespace wheelbri {

// [emulator] source_device: true when `needle` (case-insensitive) is part of the device's name or of its
// "vvvv:pppp" USB id, so part of its name, its vendor id ("346e") or "346e:0005" all find the same wheel.
[[nodiscard]] bool deviceMatches(const std::string& name, std::uint16_t vendorId, std::uint16_t productId,
                                 const std::string& needle);

}  // namespace wheelbri
