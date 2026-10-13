#include "GameWatch.hpp"

#include "Platform.hpp"

#include "wheelbri/AxisTuning.hpp"
#include "wheelbri/FileText.hpp"
#include "wheelbri/GameSettings.hpp"
#include "wheelbri/Logging.hpp"
#include "wheelbri/Nfs2015Settings.hpp"

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

GameWatch::GameWatch(const std::string_view targetProfile, const bool routePedals, const float mapping,
                     GameRotations overrides, GameFfbGains gains)
    : detector_(targetProfile), target_(lower(targetProfile)), routePedals_(routePedals), mapping_(mapping),
      overrides_(std::move(overrides)), ffbGains_(std::move(gains)), wayNotices_(!routePedals) {
    for (const std::string& problem : gameProfileProblems()) {
        WE_LOG_ERROR(log(), "game profile {}: not used", problem);
    }
}

std::string GameWatch::describeFollowed() const {
    const GameDetector::State& state = detector_.state();
    if (state.game == nullptr) return {};
    // Where it runs from (Windows: the full path, so a stray program with the same name shows; Linux: as listed).
    std::string where = pathToUtf8(platform::processImage(state.pid));
    if (where.empty()) where = state.program;
    std::string text = state.game->label + " (pid " + std::to_string(state.pid) + ", " + where + ")";
    if (!state.profileApplies) {
        return text + ": its game profile " + state.game->id + " is for the " + state.game->target +
               " and the target is " + target_ + ": not used";
    }
    const std::string overlay = describeOverlay(*state.game);
    if (overlay.empty()) return text + ": game profile " + state.game->id + " (no pedal changes)";
    text += ": game profile " + state.game->id + " laid over the defaults (" + overlay + ")";
    return text + (routePedals_ ? ": the virtual G920's pedals are routed to match"
                                : ": the shim in the game applies it, the service leaves the pedals alone");
}

std::optional<float> GameWatch::currentRotation() const {
    const GameDetector::State& state = detector_.state();
    if (state.game == nullptr || !state.profileApplies) return std::nullopt;
    std::optional<float> rotation = rotationWhileRunning(*state.game, overrides_);
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

void GameWatch::sayFfbGain() {
    const GameDetector::State& state = detector_.state();
    const float next =
        state.game != nullptr && state.profileApplies ? gameFfbGain(*state.game, ffbGains_) : 1.0F;
    if (next == ffbGain_) return;
    if (next != 1.0F) {
        const bool own = ffbGains_.count(state.game->id) != 0;
        WE_LOG_INFO(log(), "game: {}'s force feedback {} times stronger while it runs ({}); the cap and the slew limit "
                           "still apply, its springs and dampers stay as they are",
                    state.game->label, tuning::formatNumber(next),
                    own ? std::string("your mapping's [game_ffb_gain]") : state.game->ffbNote);
    } else {
        WE_LOG_INFO(log(), "game: the games' force feedback as they ask again");
    }
    ffbGain_ = next;
}

GameWatch::Live GameWatch::live() const {
    Live live;
    live.starts = starts_;
    live.ffbGain = ffbGain_;
    const GameDetector::State& state = detector_.state();
    if (state.game == nullptr || !state.profileApplies) return live;
    live.id = state.game->id;
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
                if (state.profileApplies) ++starts_;
                WE_LOG_INFO(log(), "game: {} started: {}", state.game->label, describeFollowed());
                break;
            case GameDetector::Event::Ended:
                WE_LOG_INFO(log(), "game: {} ended: the defaults again", update.previous->label);
                break;
            case GameDetector::Event::Switched:
                followedSinceMs_ = platform::nowMs();
                if (state.profileApplies) ++starts_;
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

GameWatch::Decision GameWatch::check() {
    const GameDetector::Update update = detector_.poll(platform::runningPrograms());
    logEvents(update);
    const std::uint64_t notices = wayNotices_.count();
    wayNotices_.update(update, detector_.state());
    if (wayNotices_.count() != notices && wayNotices_.current()) {
        WE_LOG_WARN(log(), "game: {}", wayNotices_.current()->text);
    }
    wayWarning_ = wayNotices_.current() ? upper(wayNotices_.current()->title) : std::string();
    const GameDetector::State& state = detector_.state();
    const bool active = state.game != nullptr && state.profileApplies;

    PedalRoute route;   // straight through
    if (active && routePedals_) {
        const JoystickLayout device = layoutFor(target_, 0);
        if (const std::optional<PedalRoute> routed = routePedals(device, overlay(device, *state.game))) {
            route = *routed;
            unroutable_ = nullptr;
        } else if (unroutable_ != state.game) {
            unroutable_ = state.game;
            WE_LOG_ERROR(log(), "game profile {} ({}) cannot be applied to the virtual {}: its pedal axes are not the "
                                "device's (y, rz, slider); pedals left as they are",
                         state.game->id, describeOverlay(*state.game), target_);
        }
    }

    if (active && state.game->id == "nfs2015") {
        checkNfs2015Bindings();
    } else {
        warning_.clear();
        settingsWritten_.reset();
    }
    sayRotation();
    sayFfbGain();
    return Decision{route, ffbGain_};
}

void GameWatch::checkNfs2015Bindings() {
    const std::filesystem::path documents = platform::documentsFolder();
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
