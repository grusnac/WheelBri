#include "wheelbri/WaitFor.hpp"

#include <algorithm>

namespace wheelbri {

WaitOutcome waitFor(const WaitSettings& settings, const std::function<bool()>& ready,
                    const std::function<bool()>& cancelled, const std::function<std::uint64_t(std::uint64_t)>& sleep,
                    std::uint64_t* const waitedMs) {
    const std::uint64_t every = std::max<std::uint64_t>(settings.everyMs, 1);
    std::uint64_t waited = 0;
    WaitOutcome outcome = WaitOutcome::TimedOut;
    while (true) {
        if (ready()) {
            outcome = WaitOutcome::Ready;
            break;
        }
        if (cancelled()) {
            outcome = WaitOutcome::Cancelled;
            break;
        }
        if (waited >= settings.timeoutMs) break;
        const std::uint64_t step = std::min(every, settings.timeoutMs - waited);
        // Never more than asked (a sleep that overran ends the wait sooner, not later), never nothing (no spin on a
        // sleep that always ends at once).
        waited += std::clamp<std::uint64_t>(sleep(step), 1, step);
    }
    if (waitedMs != nullptr) *waitedMs = waited;
    return outcome;
}

}  // namespace wheelbri
