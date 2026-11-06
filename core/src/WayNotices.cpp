// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/WayNotices.hpp"

namespace wheelbri {

void WayNotices::update(const GameDetector::Update& update, const GameDetector::State& state) {
    for (const GameDetector::Event event : update.events) {
        if (event == GameDetector::Event::Started || event == GameDetector::Event::Switched) {
            current_ = wayAdvice(*state.game, shim_);
            if (current_) ++count_;
        }
    }
    if (state.game == nullptr) current_.reset();   // ended (also when its end and a list failure meet)
}

bool WayNoticeFollower::onFrame(const std::uint64_t count) {
    if (count <= seen_) {
        seen_ = count;   // a new session's count from 0 (reset missed): nothing to show
        return false;
    }
    seen_ = count;
    return true;
}

}  // namespace wheelbri
