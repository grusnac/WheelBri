// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/GameTab.hpp"

namespace wheelbri {

std::optional<std::string> GameTabFollower::onFrame(const std::uint64_t starts, const std::string_view id) {
    if (starts == seen_) return std::nullopt;
    seen_ = starts;
    if (id.empty()) return std::nullopt;
    return std::string(id);
}

}  // namespace wheelbri
