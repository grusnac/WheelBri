#include "wheelbri/Pipeline.hpp"

#include "wheelbri/Logging.hpp"

namespace wheelbri {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("core.pipeline");
    return logger;
}

}  // namespace

Pipeline::Pipeline(IInputSource& source, const Mapper& mapper, const IDeviceProfile& profile,
                   IStatePublisher& publisher)
    : source_(source), mapper_(mapper), profile_(profile), publisher_(publisher) {
    WE_LOG_INFO(log(), "pipeline: '{}' -> '{}' ({:04x}:{:04x})", source_.name(), profile_.identity().productName,
                profile_.identity().vendorId, profile_.identity().productId);
}

bool Pipeline::tick(const std::uint64_t nowNs) {
    const RawInputs previous = inputs_;   // a copy on the stack, no allocation
    inputChanged_ = false;                // until every device has been read
    if (!source_.poll(inputs_.devices[0])) {
        WE_LOG_WARN(log(), "pipeline: input source '{}' stopped delivering after {} ticks", source_.name(),
                    packet_.sequence);
        lostDevice_ = 0;
        return false;
    }
    for (std::size_t i = 0; i < accessories_.size(); ++i) {
        if (accessories_[i] != nullptr && !accessories_[i]->poll(inputs_.devices[i + 1])) {
            WE_LOG_WARN(log(), "pipeline: accessory '{}' stopped delivering after {} ticks", accessories_[i]->name(),
                        packet_.sequence);
            lostDevice_ = static_cast<int>(i + 1);
            return false;
        }
    }
    inputChanged_ = !(inputs_ == previous);
    const Mapper* const game = gameMapper();
    state_ = (game != nullptr ? *game : mapper_).map(inputs_);
    report_ = profile_.encode(state_);
    packet_.report = applyPedalRoute(report_, pedalRoute());
    if (nativeButtons()) packet_.report.buttons = profile_.nativeButtons(packet_.report.buttons);
    if (!gate_ || gate_->shouldSend(packet_.report, nowNs)) {
        ++packet_.sequence;
        if (!publisher_.publish(packet_)) {
            ++publishFailures_;
        }
    }

    static logging::CallSampler sampler(5, 5000);
    if (const std::uint64_t call = sampler.next(); call != 0) {
        WE_LOG_TRACE(log(),
                     "tick #{}: raw[{:+.3f} {:+.3f} {:+.3f} {:+.3f}] btn={:016x} hat={} -> steer={:+.4f} thr={:.3f} "
                     "brk={:.3f} clt={:.3f} btn={:08x} -> target steer={} thr={} brk={} clt={} (publish failures {})",
                     call, lastRaw().axes[0], lastRaw().axes[1], lastRaw().axes[2], lastRaw().axes[3], lastRaw().buttons,
                     lastRaw().hat,
                     state_.steering, state_.throttle, state_.brake, state_.clutch, state_.buttons,
                     report_.steering, report_.throttle, report_.brake, report_.clutch,
                     publishFailures_);
    }
    return true;
}

}  // namespace wheelbri
