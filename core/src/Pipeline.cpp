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

bool Pipeline::tick(std::optional<float> clutchOverride, const std::uint64_t nowNs) {
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
    state_ = mapper_.map(inputs_);
    if (clutchOverride) {
        state_.clutch = *clutchOverride;
    }
    packet_.report = profile_.encode(state_);
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
                     packet_.report.steering, packet_.report.throttle, packet_.report.brake, packet_.report.clutch,
                     publishFailures_);
    }
    return true;
}

}  // namespace wheelbri
