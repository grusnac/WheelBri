#pragma once

#include "wheelemu/Devices.hpp"
#include "wheelemu/Ffb.hpp"

namespace wheelemu::service {

// Plays a short, low-strength sequence (spring, left push, right push, damper) through the
// safety limiter so the force feedback path can be checked on the real wheel without a game.
[[nodiscard]] bool runFfbSelfTest(IFfbSink& sink, const FfbSafetyConfig& safety);

}  // namespace wheelemu::service
