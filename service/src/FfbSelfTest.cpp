#include "FfbSelfTest.hpp"

#include "wheelemu/Logging.hpp"

#include "Platform.hpp"

#include <algorithm>
#include <cstdio>

namespace wheelemu::service {

namespace {

constexpr float kTestStrength = 0.2F;       // never more than 20% of the wheel's torque
constexpr std::uint64_t kStepNs = 2 * platform::kNsPerSecond;
constexpr std::uint64_t kTickNs = platform::kNsPerMs;

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.ffbtest");
    return logger;
}

// Re-sends the effect every millisecond so the slew limiter ramps it in smoothly.
bool hold(IFfbSink& sink, FfbSafetyLimiter& limiter, const FfbEffect& effect, const char* label) {
    std::printf("  %s\n", label);   // instructions for the person at the wheel
    std::fflush(stdout);
    WE_LOG_INFO(log(), "self-test step: {}", label);
    for (std::uint64_t elapsed = 0; elapsed < kStepNs; elapsed += kTickNs) {
        const FfbEffect limited = limiter.apply(effect, static_cast<float>(kTickNs) / static_cast<float>(platform::kNsPerSecond));
        if (!sink.play(0, limited)) {
            std::printf("  -> not supported by this device\n");
            WE_LOG_WARN(log(), "self-test step failed after {} ms: {}", elapsed / platform::kNsPerMs, label);
            return false;
        }
        platform::sleepPrecise(kTickNs);
    }
    sink.stop(0);
    return true;
}

}  // namespace

bool runFfbSelfTest(IFfbSink& sink, const FfbSafetyConfig& safety) {
    FfbSafetyConfig gentle = safety;
    gentle.maxMagnitude = std::min(safety.maxMagnitude, kTestStrength);
    FfbSafetyLimiter limiter(gentle);

    std::puts("force feedback self-test (hold the wheel lightly):");

    FfbEffect spring;
    spring.type = FfbEffectType::Spring;
    spring.condition.positiveCoefficient = 0.5F;
    spring.condition.negativeCoefficient = 0.5F;
    bool ok = hold(sink, limiter, spring, "spring: wheel should pull gently to the centre");

    FfbEffect push;
    push.type = FfbEffectType::Constant;
    push.constantLevel = -1.0F;
    limiter.reset();
    ok = hold(sink, limiter, push, "constant: wheel should push LEFT (if RIGHT, set [ffb] invert = true)") && ok;
    push.constantLevel = 1.0F;
    limiter.reset();
    ok = hold(sink, limiter, push, "constant: wheel should push RIGHT") && ok;

    FfbEffect damper;
    damper.type = FfbEffectType::Damper;
    damper.condition.positiveCoefficient = 0.8F;
    damper.condition.negativeCoefficient = 0.8F;
    ok = hold(sink, limiter, damper, "damper: turning the wheel should feel heavier") && ok;

    sink.stopAll();
    std::puts(ok ? "self-test finished" : "self-test finished with unsupported effects");
    if (ok) {
        WE_LOG_INFO(log(), "self-test passed (max strength {:.0f}%)", gentle.maxMagnitude * 100.0F);
    } else {
        WE_LOG_WARN(log(), "self-test finished with failures, see messages above");
    }
    return ok;
}

}  // namespace wheelemu::service
