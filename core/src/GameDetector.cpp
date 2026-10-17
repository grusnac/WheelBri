#include "wheelbri/GameDetector.hpp"

#include <algorithm>

namespace wheelbri {

namespace {

std::string lower(const std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool sameGames(const std::vector<RunningGame>& a, const std::vector<RunningGame>& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](const RunningGame& x, const RunningGame& y) { return x.game == y.game; });
}

}  // namespace

GameDetector::GameDetector(const std::string_view target, const int pollsToEnd,
                           const std::span<const GameProfile> profiles)
    : target_(lower(target)), pollsToEnd_(std::max(1, pollsToEnd)), profiles_(profiles) {}

void GameDetector::follow(const RunningGame* const running) {
    misses_ = 0;
    if (running == nullptr) {
        state_.game = nullptr;
        state_.pid = 0;
        state_.program.clear();
    } else {
        state_.game = running->game;
        state_.pid = running->pid;
        state_.program = running->program;
    }
    state_.profileApplies = state_.game != nullptr && profileFitsTarget(*state_.game, target_);
}

GameDetector::Update GameDetector::poll(const std::optional<std::vector<RunningProgram>>& programs) {
    Update update;
    update.previous = state_.game;
    if (!programs) {
        if (state_.listing) {
            state_.listing = false;
            update.events.push_back(Event::ListFailed);
        }
        return update;   // nothing else changes: a failed listing never moves the pedals
    }
    if (!state_.listing) {
        state_.listing = true;
        update.events.push_back(Event::ListBack);
    }

    const std::vector<RunningGame> running = findRunningGames(*programs, profiles_);
    const auto runningOf = [&](const GameProfile* const game) -> const RunningGame* {
        const auto it = std::find_if(running.begin(), running.end(),
                                     [&](const RunningGame& r) { return r.game == game; });
        return it == running.end() ? nullptr : &*it;
    };

    if (state_.game != nullptr) {
        if (const RunningGame* const same = runningOf(state_.game)) {
            misses_ = 0;
            if (same->pid != state_.pid) {
                state_.pid = same->pid;
                state_.program = same->program;
                update.events.push_back(Event::Restarted);
            }
        } else if (++misses_ >= pollsToEnd_) {
            const RunningGame* const next = running.empty() ? nullptr : &running.front();
            follow(next);
            update.events.push_back(next != nullptr ? Event::Switched : Event::Ended);
        }
    } else if (!running.empty()) {
        follow(&running.front());
        update.events.push_back(Event::Started);
    }

    std::vector<RunningGame> others;
    for (const RunningGame& r : running) {
        if (r.game != state_.game) others.push_back(r);
    }
    if (!sameGames(others, state_.others)) {
        update.events.push_back(Event::Others);
    }
    state_.others = std::move(others);
    return update;
}

}  // namespace wheelbri
