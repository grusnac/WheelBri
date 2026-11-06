// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/GameDetector.hpp"
#include "wheelbri/GameProfiles.hpp"

#include <cstdint>
#include <optional>

namespace wheelbri {

// A game that does not work the way the session runs ([game] ways in its profile, e.g. virtual only with the shim):
// told once each time it starts, as a Windows notification and a popup in the app, and in red next to Start while it
// runs. The session (GameWatch) feeds it every GameDetector poll; the app counts what it has shown (WayNoticeFollower).
// The game's profile need not be for the session's target: the way is the game's, whatever wheel it is shown.
class WayNotices {
public:
    // `shim`: the session runs the in-game shim; false: the virtual device (Windows' driver or Linux's uhid).
    explicit WayNotices(bool shim) : shim_(shim) {}

    // After a poll: a game newly followed (Started, Switched) is checked; when the followed game is gone the advice
    // goes. A restart of the same game (Restarted) is not a new start.
    void update(const GameDetector::Update& update, const GameDetector::State& state);

    // While the game it is about is followed: what to tell the user.
    [[nodiscard]] const std::optional<WayAdvice>& current() const { return current_; }
    // Notices given this session: one more for each start of a game that does not work this way.
    [[nodiscard]] std::uint64_t count() const { return count_; }

private:
    bool shim_;
    std::optional<WayAdvice> current_;
    std::uint64_t count_ = 0;
};

// The app's side: which notices it has shown. Each frame gives the session's count; true once for every new notice.
class WayNoticeFollower {
public:
    [[nodiscard]] bool onFrame(std::uint64_t count);
    // A new session: its count starts again from 0.
    void reset() { seen_ = 0; }

private:
    std::uint64_t seen_ = 0;
};

}  // namespace wheelbri
