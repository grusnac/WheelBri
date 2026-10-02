#include "wheelemu/DeviceMatch.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace wheelemu {

namespace {

std::string toLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

}  // namespace

bool deviceMatches(const std::string& name, const std::uint16_t vendorId, const std::uint16_t productId,
                   const std::string& needle) {
    const std::string lowered = toLower(needle);
    char ids[10];
    std::snprintf(ids, sizeof(ids), "%04x:%04x", vendorId, productId);
    return toLower(name).find(lowered) != std::string::npos || std::string(ids).find(lowered) != std::string::npos;
}

}  // namespace wheelemu
