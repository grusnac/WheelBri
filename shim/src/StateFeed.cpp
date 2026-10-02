#include "StateFeed.hpp"

#include "ShimConfig.hpp"
#include "ShimLogging.hpp"

namespace wheelemu::shim {

namespace {

const logging::Logger& log() {
    static const logging::Logger instance = logger("shim.feed");
    return instance;
}

}  // namespace

StateFeed& StateFeed::instance() {
    static StateFeed feed;
    return feed;
}

TargetReport StateFeed::current(const IDeviceProfile& profile) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    const ShimConfig& config = shimConfig();

    if (!opened_) {
        opened_ = true;
        firstRequest_ = now;
        receiver_ = UdpLoopbackReceiver::create(config.statePort);
        if (receiver_ == nullptr) {
            WE_LOG_ERROR(log(), "cannot listen on 127.0.0.1:{}; the emulated wheel will stay centred",
                         config.statePort);
        }
    }

    if (receiver_ != nullptr) {
        if (auto packet = receiver_->pollLatest()) {
            latest_ = *packet;
            lastArrival_ = now;
            if (stale_) {
                stale_ = false;
                WE_LOG_INFO(log(), "receiving wheel state from wheelemu-service (packet #{})", packet->sequence);
            }
        }
    }

    const auto staleAfter = std::chrono::milliseconds(config.staleAfterMs);
    if (latest_ && now - lastArrival_ <= staleAfter) {
        return latest_->report;
    }

    if (!stale_) {
        stale_ = true;
        WE_LOG_WARN(log(), "no wheel state from wheelemu-service for {} ms: reporting a centred wheel and released "
                           "pedals until it comes back",
                    config.staleAfterMs);
    } else if (!latest_ && !warnedNeverReceived_ && now - firstRequest_ > std::chrono::seconds(2)) {
        warnedNeverReceived_ = true;
        WE_LOG_WARN(log(), "nothing received on port {} yet: is wheelemu-service running with the same state_port?",
                    config.statePort);
    }
    return profile.encode(WheelState{});
}

}  // namespace wheelemu::shim
