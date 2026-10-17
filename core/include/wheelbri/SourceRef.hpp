#pragma once

#include "wheelbri/MapperConfig.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace wheelbri {

// A control on a device: `device` empty = the wheel. `index` is an axis or a button, kUnmapped for none.
struct SourceRef {
    DeviceId device;
    int index = kUnmapped;

    [[nodiscard]] bool mapped() const { return index >= 0; }
    friend bool operator==(const SourceRef&, const SourceRef&) = default;
};

// "2", "-1" or "044f:b696/2" (hex ids, 1 to 4 digits each); nullopt if it is none of those (MappingEdit.cpp).
[[nodiscard]] std::optional<SourceRef> parseSource(std::string_view text);
// The other way: "-1" for nothing, "2" on the wheel, "044f:b696/2" on an accessory.
[[nodiscard]] std::string formatSource(const SourceRef& source);

}  // namespace wheelbri
