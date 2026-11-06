// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/ShimWatch.hpp"

namespace wheelbri {

std::optional<std::string> ShimWatch::update(const std::uint64_t nowMs, const std::string& gameId,
                                             const std::uint64_t gameStarts, const bool heard) {
    if (heard) heard_ = true;
    if (gameId.empty()) {   // no game runs: nothing to wait for
        game_.clear();
        return std::nullopt;
    }
    if (gameId != game_ || gameStarts != starts_) {   // a game started (again): the wait starts now
        game_ = gameId;
        starts_ = gameStarts;
        since_ = nowMs;
        warned_ = false;
    }
    if (heard_ || warned_ || nowMs - since_ < kQuietMs) return std::nullopt;
    warned_ = true;
    return game_;
}

}  // namespace wheelbri
