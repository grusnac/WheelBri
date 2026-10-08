#include "wheelbri/Pacing.hpp"

namespace wheelbri {

bool StateSendGate::shouldSend(const TargetReport& report, const std::uint64_t nowNs) {
    const std::uint64_t since = nowNs - lastSentNs_;
    bool send = false;
    if (!any_) {
        send = true;
    } else if (report != last_) {
        send = since >= minIntervalNs_;
        if (!send) ++heldBack_;
    } else {
        send = since >= keepAliveNs_;
        if (!send) ++unchanged_;
    }
    if (send) {
        any_ = true;
        last_ = report;
        lastSentNs_ = nowNs;
        ++sent_;
    }
    return send;
}

}  // namespace wheelbri
