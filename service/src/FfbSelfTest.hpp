#pragma once

#include "wheelbri/Devices.hpp"
#include "wheelbri/Ffb.hpp"

#include <functional>

namespace wheelbri::service {

// Plays a short, low-strength sequence (spring, left push, right push, damper) through the
// safety limiter so the force feedback path can be checked on the real wheel without a game. `cancelled` (Ctrl+C):
// every force stops at once and the test ends, returning false.
[[nodiscard]] bool runFfbSelfTest(IFfbSink& sink, const FfbSafetyConfig& safety,
                                  const std::function<bool()>& cancelled = {});

}  // namespace wheelbri::service
