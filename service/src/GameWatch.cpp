// SPDX-License-Identifier: GPL-3.0-or-later
#include "GameWatch.hpp"

#include "Platform.hpp"
#include "ServiceLogging.hpp"

#include "wheelbri/AxisTuning.hpp"
#include "wheelbri/FileText.hpp"
#include "wheelbri/GameSettings.hpp"
#include "wheelbri/Logging.hpp"
#include "wheelbri/Nfs2015Settings.hpp"

#include <algorithm>
#include <cmath>
#include <system_error>
#include <utility>

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.game");
    return logger;
}

// A settings file larger than this is not NFS 2015's (19 KB): not read.
constexpr std::uintmax_t kMaxSettingsBytes = 1024 * 1024;

std::string lower(const std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string upper(const std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

}  // namespace

namespace {

std::vector<GameProfile> allGames(std::vector<GameProfile> userGames) {
    std::vector<GameProfile> games(gameProfiles().begin(), gameProfiles().end());
    for (GameProfile& game : userGames) {
        // A game the user added that has a built-in profile too is that profile (Add game refuses it; a hand edit).
        // Built-in ones only: user games may share a program's name (speed.exe) when their paths differ.
        bool known = false;
        for (const GameProfile& builtIn : gameProfiles()) {
            known = known || builtIn.program == game.program || builtIn.id == game.id;
        }
        if (known) {
            WE_LOG_WARN(log(), "game: {} ({}) from games.ini has a built-in profile: the built-in one is used",
                        game.label, game.program);
        } else {
            games.push_back(std::move(game));
        }
    }
    return games;
}

}  // namespace

std::vector<GameProfile> loadUserGames() {
    const std::filesystem::path file = userGamesFile();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) return {};
    const std::optional<std::string> text = readFileText(file);
    if (!text) {
        WE_LOG_WARN(log(), "game: cannot read {}", pathToUtf8(file));
        return {};
    }
    UserGamesResult result = parseUserGames(*text);
    for (const std::string& problem : result.problems) WE_LOG_WARN(log(), "game: {}: {}", pathToUtf8(file), problem);
    return std::move(result.games);
}

GameWatch::GameWatch(GameSessionConfig session, const float mapping, GameRotations overrides,
                     std::vector<GameProfile> userGames, System system)
    : system_(std::move(system)), session_(std::move(session)), games_(allGames(std::move(userGames))),
      detector_(session_.target, 2, games_), target_(lower(session_.target)), routePedals_(session_.routePedals),
      mapping_(mapping), overrides_(std::move(overrides)), wayNotices_(!session_.routePedals) {
    for (const std::string& problem : gameProfileProblems()) {
        WE_LOG_ERROR(log(), "game profile {}: not used", problem);
    }
}

std::string GameWatch::describeFollowed() const {
    const GameDetector::State& state = detector_.state();
    if (state.game == nullptr) return {};
    // Where it runs from (Windows: the full path, so a stray program with the same name shows; Linux: as listed).
    std::string where =
        pathToUtf8(system_.processImage ? system_.processImage(state.pid) : platform::processImage(state.pid));
    if (where.empty()) where = state.program;
    std::string text = state.game->label + " (pid " + std::to_string(state.pid) + ", " + where + ")";
    if (!state.profileApplies) {
        return text + ": its game profile " + state.game->id + " is for the " + state.game->target +
               " and the target is " + target_ + ": not used";
    }
    const std::string overlay = describeOverlay(*state.game);
    if (overlay.empty()) return text + ": game profile " + state.game->id + " (no pedal changes)";
    text += ": game profile " + state.game->id + " laid over the defaults (" + overlay + ")";
    return text + (routePedals_ ? ": the virtual wheel's pedals are routed to match"
                                : ": the shim in the game applies it, the service leaves the pedals alone");
}

std::optional<float> GameWatch::currentRotation() const {
    const GameDetector::State& state = detector_.state();
    if (state.game == nullptr) return std::nullopt;
    // The user's own rotation for the game on every target; its profile's only when the profile fits the target.
    std::optional<float> rotation = rotationWhileRunning(*state.game, overrides_, state.profileApplies);
    // The mapping has it already: nothing to say.
    if (rotation && std::fabs(*rotation - mapping_) < 0.5F) return std::nullopt;
    return rotation;
}

void GameWatch::sayRotation() {
    const std::optional<float> next = currentRotation();
    if (next == applied_) return;
    const GameDetector::State& state = detector_.state();
    if (next) {
        const bool own = overrides_.count(state.game->id) != 0;
        WE_LOG_WARN(log(), "game: {} is played with a steering rotation of {} ({}), not the mapping's {}: set your "
                           "wheel's own software (Pit House for Moza) to {} too while you play it",
                    state.game->label, describeRotation(*next),
                    own ? std::string("your mapping's [game_rotation]")
                        : "its game profile: " + state.game->rotationNote,
                    describeRotation(mapping_), describeRotation(*next));
    } else if (applied_) {
        WE_LOG_INFO(log(), "game: the mapping's steering rotation again ({}): set your wheel's own software back to it",
                    describeRotation(mapping_));
    }
    applied_ = next;
}

void GameWatch::sayTuning() {
    const GameDetector::State& state = detector_.state();
    const GameDecision decision = decideGame(state.game, state.profileApplies, session_);
    const std::string tuned = decision.tuning != nullptr ? state.game->id : std::string();
    if (tuned == tunedGame_) return;
    if (!tuned.empty()) {
        WE_LOG_INFO(log(), "game: {} plays with your own values for it (the Games tab, [game_tuning])",
                    state.game->label);
    } else if (!tunedGame_.empty()) {
        WE_LOG_INFO(log(), "game: the mapping's own values again");
    }
    tunedGame_ = tuned;
}

GameWatch::Live GameWatch::live() const {
    Live live;
    live.starts = starts_;
    const GameDetector::State& state = detector_.state();
    // Any game WheelBri knows (a built-in profile, also one for another target, or one the user added): the Game
    // profiles tab shows it running and its values can be changed while it runs.
    if (state.game == nullptr) return live;
    live.id = state.game->id;
    live.profileApplies = state.profileApplies;
    live.pid = state.pid;
    live.sinceMs = followedSinceMs_;
    return live;
}

std::string GameWatch::rotationStatus() const {
    return applied_ ? describeRotation(*applied_) : std::string();
}

void GameWatch::logEvents(const GameDetector::Update& update) {
    const GameDetector::State& state = detector_.state();
    for (const GameDetector::Event event : update.events) {
        switch (event) {
            case GameDetector::Event::Started:
                followedSinceMs_ = platform::nowMs();
                ++starts_;
                WE_LOG_INFO(log(), "game: {} started: {}", state.game->label, describeFollowed());
                break;
            case GameDetector::Event::Ended:
                WE_LOG_INFO(log(), "game: {} ended: the defaults again", update.previous->label);
                break;
            case GameDetector::Event::Switched:
                followedSinceMs_ = platform::nowMs();
                ++starts_;
                WE_LOG_INFO(log(), "game: {} ended, {} runs: {}", update.previous->label, state.game->label,
                            describeFollowed());
                break;
            case GameDetector::Event::Restarted:
                WE_LOG_INFO(log(), "game: {} runs again as pid {}: its profile kept", state.game->label, state.pid);
                break;
            case GameDetector::Event::Others:
                if (state.others.empty()) {
                    WE_LOG_INFO(log(), "game: no other game with a profile runs now");
                } else {
                    std::string others;
                    for (const RunningGame& other : state.others) {
                        others += (others.empty() ? "" : ", ") + other.game->label + " (pid " +
                                  std::to_string(other.pid) + ")";
                    }
                    WE_LOG_WARN(log(), "game: also running: {}; the pedals follow {} (the one found first): close the "
                                       "game you are not playing",
                                others, state.game != nullptr ? state.game->label : std::string("none"));
                }
                break;
            case GameDetector::Event::ListFailed:
                // Only worth saying while a profile is in use: without one nothing would change anyway.
                if (state.game != nullptr) {
                    WE_LOG_WARN(log(), "game: cannot list the running programs: {}'s profile stays",
                                state.game->label);
                }
                break;
            case GameDetector::Event::ListBack:
                if (state.game != nullptr) WE_LOG_INFO(log(), "game: the running programs can be listed again");
                break;
        }
    }
}

void GameWatch::setValues(GameTunings tunings, GameRotations overrides) {
    session_.tunings = std::move(tunings);
    overrides_ = std::move(overrides);
}

void GameWatch::setMapping(const float degrees) {
    if (std::fabs(degrees - mapping_) < 0.5F) return;
    WE_LOG_INFO(log(), "steering rotation: the mapping's is {} now (was {}): set your wheel's own software (Pit House "
                       "for Moza) to it",
                describeRotation(degrees), describeRotation(mapping_));
    mapping_ = degrees;
    sayRotation();   // a game's own rotation that now equals the mapping's is nothing to say any more, and the reverse
}

GameWatch::Decision GameWatch::check() {
    std::optional<std::vector<RunningProgram>> programs =
        system_.runningPrograms ? system_.runningPrograms() : platform::runningPrograms();
    if (programs) {
        // Where it runs from, only for a game known by its path (two games may share a program's name); Windows only.
        for (RunningProgram& program : *programs) {
            const std::string name = programName(program.name);
            if (std::none_of(games_.begin(), games_.end(),
                             [&name](const GameProfile& g) { return !g.path.empty() && g.program == name; })) {
                continue;
            }
            program.path =
                pathToUtf8(system_.processImage ? system_.processImage(program.pid) : platform::processImage(program.pid));
        }
    }
    const GameDetector::Update update = detector_.poll(programs);
    logEvents(update);
    const std::uint64_t notices = wayNotices_.count();
    wayNotices_.update(update, detector_.state());
    if (wayNotices_.count() != notices && wayNotices_.current()) {
        WE_LOG_WARN(log(), "game: {}", wayNotices_.current()->text);
    }
    wayWarning_ = wayNotices_.current() ? upper(wayNotices_.current()->title) : std::string();
    const GameDetector::State& state = detector_.state();
    const bool active = state.game != nullptr && state.profileApplies;

    const GameDecision decision = decideGame(state.game, state.profileApplies, session_);
    if (!decision.unroutable) {
        unroutable_ = nullptr;
    } else if (unroutable_ != state.game && state.game != nullptr) {
        unroutable_ = state.game;
        WE_LOG_ERROR(log(), "game {} cannot have its pedal axes on the virtual {}: they must be the device's (y, rz, "
                            "slider), each once; pedals left as they are",
                     state.game->label, target_);
    }
    if (active && state.game->id == "nfs2015") {
        checkNfs2015Bindings();
    } else {
        warning_.clear();
        settingsWritten_.reset();
    }
    sayRotation();
    sayTuning();
    Decision out;
    out.route = decision.route;
    out.combinedPedals = decision.combinedPedals;
    if (decision.combinedPedals && state.game != nullptr) {
        out.combinedPedalsFrom = decision.combinedPedalsFromUser ? "your value for " + state.game->label
                                                                 : state.game->label + "'s profile";
    }
    if (decision.tuning != nullptr && changesMapping(*decision.tuning)) {
        out.mapperGame = state.game->id;
        out.tuning = decision.tuning;
    }
    return out;
}

void GameWatch::checkNfs2015Bindings() {
    const std::filesystem::path documents =
        system_.documentsFolder ? system_.documentsFolder() : platform::documentsFolder();
    if (documents.empty()) return;   // not on Windows: not checked
    const std::filesystem::path file = documents / "Need For Speed" / "settings" / "PROFILEOPTIONS_profile";
    std::error_code error;
    const auto written = std::filesystem::last_write_time(file, error);
    if (error) return;   // no settings yet: the game's defaults, right with the profile
    if (settingsWritten_ && *settingsWritten_ == written) return;   // unchanged since the last look
    settingsWritten_ = written;
    const std::uintmax_t size = std::filesystem::file_size(file, error);
    const std::optional<std::string> text =
        !error && size <= kMaxSettingsBytes ? readFileText(file) : std::optional<std::string>();
    if (!text) return;
    const Nfs2015WheelBindings bindings = wheelbri::checkNfs2015Settings(*text);
    switch (bindings.verdict) {
        case Nfs2015WheelBindings::Verdict::Unreadable:
        case Nfs2015WheelBindings::Verdict::Defaults:
        case Nfs2015WheelBindings::Verdict::AsExpected:
            warning_.clear();   // right with the profile (or nothing known): nothing to say
            break;
        case Nfs2015WheelBindings::Verdict::PedalsMoved:
            warning_ = "NFS 2015: RESET ITS WHEEL CONTROLS";
            WE_LOG_WARN(log(), "game: NFS 2015's own wheel controls bind accelerate to {} and brake to {} (custom "
                               "wheel controls, {}): WheelBri already gives NFS 2015 its pedals, so they end up "
                               "swapped again. In the game: Settings, Controls, wheel: reset to default",
                        nfs2015WheelInputName(bindings.accelerate.value_or(-1)),
                        nfs2015WheelInputName(bindings.brake.value_or(-1)), pathToUtf8(file));
            break;
    }
}

std::string GameWatch::status() const {
    // Only while a profile is in use: a game without one is never named.
    const GameDetector::State& state = detector_.state();
    if (state.game == nullptr || !state.profileApplies) return {};
    return state.game->label;   // the profile's own label, as it is
}

}  // namespace wheelbri::service
