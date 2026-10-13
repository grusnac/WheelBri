#pragma once

#include "wheelbri/GameProfiles.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The app's Game profiles tab (service MappingEditView, its Games page): the built-in game profiles (GameProfiles.hpp),
// each in three sections (mapping, force feedback, game config) with the user's own values for them. The portable
// parts are here so they are unit-tested.
namespace wheelbri {

// Which game the Game profiles tab shows when a session reports that a profiled game started. The session counts every
// time it starts following a profiled game (UiFrame::gameStarts, GameWatch); the app calls onFrame with each frame's
// count and the game followed now. It answers once per start: the tab switches to that game when the game starts, and
// the user is free to go to another tab while it runs.
class GameTabFollower {
public:
    // The profile id to show now: a start not seen yet with a game followed; nullopt otherwise (no new start, or the
    // game is gone again before the window saw it).
    [[nodiscard]] std::optional<std::string> onFrame(std::uint64_t starts, std::string_view id);
    // A new session: its count starts again from 0.
    void reset() { seen_ = 0; }

private:
    std::uint64_t seen_ = 0;
};

}  // namespace wheelbri
