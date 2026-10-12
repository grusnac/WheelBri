#include "GameWatch.hpp"

#include "Platform.hpp"

#include "wheelbri/AxisTuning.hpp"
#include "wheelbri/FileText.hpp"
#include "wheelbri/GameSettings.hpp"
#include "wheelbri/Logging.hpp"
#include "wheelbri/Nfs2015Settings.hpp"

#include <system_error>
#include <utility>

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.game");
    return logger;
}

// A settings file larger than this is not a game's (NFS 2015's is 19 KB, NFS Unbound's 128 KB): not read.
constexpr std::uintmax_t kMaxSettingsBytes = 1024 * 1024;

std::string lower(const std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

}  // namespace

GameWatch::GameWatch(const std::string_view targetProfile, const bool routePedals, const SteeringRotation mapping,
                     GameRotations overrides, GameSettingOverrides settings, GameFfbGains gains)
    : detector_(targetProfile), target_(lower(targetProfile)), routePedals_(routePedals), mapping_(mapping),
      overrides_(std::move(overrides)), settingOverrides_(std::move(settings)), ffbGains_(std::move(gains)) {
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

std::optional<SteeringRotation> GameWatch::currentRotation() const {
    const GameDetector::State& state = detector_.state();
    if (state.game == nullptr || !state.profileApplies) return std::nullopt;
    std::optional<SteeringRotation> rotation = rotationWhileRunning(*state.game, overrides_);
    if (rotation && *rotation == mapping_) return std::nullopt;   // the mapping has it already: nothing changes
    return rotation;
}

void GameWatch::sayRotation() {
    const std::optional<SteeringRotation> next = currentRotation();
    if (next == applied_) return;
    const GameDetector::State& state = detector_.state();
    if (next) {
        const bool own = overrides_.count(state.game->id) != 0;
        WE_LOG_WARN(log(), "game: {} steers with {} while it runs ({}), not the mapping's {}: set your wheel's own "
                           "software (Pit House for Moza) to {} degrees too, or the steering will not match",
                    state.game->label, describeRotation(*next),
                    own ? std::string("your mapping's [game_rotation]") : "its game profile: " + state.game->rotationNote,
                    describeRotation(mapping_), tuning::formatNumber(next->wheelDeg));
    } else if (applied_) {
        WE_LOG_INFO(log(), "game: the mapping's steering rotation again ({})", describeRotation(mapping_));
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

bool GameWatch::running(const GameProfile& game) const {
    const GameDetector::State& state = detector_.state();
    if (state.game == &game) return true;
    for (const RunningGame& other : state.others) {
        if (other.game == &game) return true;
    }
    return false;
}

void GameWatch::queueGameSettings(const GameDetector::Update& update) {
    if (!settingsQueued_) {
        settingsQueued_ = true;
        for (const GameProfile& game : gameProfiles()) {
            if (!game.settings.empty() && profileFitsTarget(game, target_)) settingsPending_.insert(game.id);
        }
    }
    for (const GameDetector::Event event : update.events) {
        const bool ended = event == GameDetector::Event::Ended || event == GameDetector::Event::Switched;
        if (ended && update.previous != nullptr && !update.previous->settings.empty() &&
            profileFitsTarget(*update.previous, target_)) {
            settingsPending_.insert(update.previous->id);   // it wrote its own values back as it closed
        }
    }
}

void GameWatch::writePendingGameSettings() {
    if (settingsPending_.empty() || !detector_.state().listing) return;   // unknown what runs: wait
    for (auto it = settingsPending_.begin(); it != settingsPending_.end();) {
        const GameProfile* game = nullptr;
        for (const GameProfile& candidate : gameProfiles()) {
            if (candidate.id == *it) game = &candidate;
        }
        if (game == nullptr) {
            it = settingsPending_.erase(it);
            continue;
        }
        if (running(*game)) {
            if (settingsWaitSaid_.insert(game->id).second) {
                WE_LOG_INFO(log(), "game settings: {} runs: its own settings are written after it closes (it reads "
                                   "them when it starts)", game->label);
            }
            ++it;
            continue;
        }
        settingsWaitSaid_.erase(game->id);
        writeGameSettings(*game);
        it = settingsPending_.erase(it);
    }
}

void GameWatch::writeGameSettings(const GameProfile& game) {
    const std::filesystem::path documents = platform::documentsFolder();
    if (documents.empty()) {
        if (!noDocumentsSaid_) {
            noDocumentsSaid_ = true;
            WE_LOG_INFO(log(), "game settings: {}'s own settings are only set on Windows for now (its settings file is "
                               "in the game's Proton folder here): not set",
                        game.label);
        }
        return;
    }
    const std::filesystem::path file = documents / pathFromUtf8(game.settingsFile);
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) {
        WE_LOG_INFO(log(), "game settings: {}: no settings file yet ({}): start the game once, WheelBri sets them "
                           "after it closes",
                    game.label, pathToUtf8(file));
        return;
    }
    const std::uintmax_t size = std::filesystem::file_size(file, error);
    const std::optional<std::string> bytes =
        !error && size <= kMaxSettingsBytes ? readFileText(file) : std::optional<std::string>();
    if (!bytes) {
        WE_LOG_WARN(log(), "game settings: {}: cannot read {}: not set", game.label, pathToUtf8(file));
        return;
    }
    // The steering rotation the game is played with: its own (GameRotation.hpp), else the mapping's.
    const std::optional<SteeringRotation> own = rotationWhileRunning(game, overrides_);
    const float steeringDeg = own ? own->gameDeg : mapping_.gameDeg;
    const std::vector<GameSettingText> values = gameSettingTexts(game, settingOverrides_, steeringDeg);
    const GameSettingsEdit edit = editFrostbiteSettings(*bytes, values);
    if (!edit.problems.empty()) {
        std::string problems;
        for (const std::string& problem : edit.problems) problems += (problems.empty() ? "" : "; ") + problem;
        WE_LOG_WARN(log(), "game settings: {}: nothing written to {}: {}", game.label, pathToUtf8(file), problems);
        return;
    }
    if (edit.changes.empty()) {
        WE_LOG_INFO(log(), "game settings: {}: its own settings are as set ({})", game.label, pathToUtf8(file));
        return;
    }
    // The user's file as it was, once: put it back to undo everything WheelBri set.
    std::filesystem::path backup = file;
    backup += ".before-wheelbri";
    if (!std::filesystem::exists(backup, error)) {
        std::filesystem::copy_file(file, backup, std::filesystem::copy_options::skip_existing, error);
        if (error) {
            WE_LOG_WARN(log(), "game settings: {}: cannot keep a copy of {} ({}): nothing written", game.label,
                        pathToUtf8(file), error.message());
            return;
        }
    }
    if (!writeFileReplacing(file, edit.bytes)) {
        WE_LOG_WARN(log(), "game settings: {}: cannot write {}: not set", game.label, pathToUtf8(file));
        return;
    }
    for (const GameSettingsEdit::Change& change : edit.changes) {
        WE_LOG_INFO(log(), "game settings: {}: {} {} -> {} ({}; the original is kept as {})", game.label, change.name,
                    change.before, change.after, game.settingsNote, pathToUtf8(backup));
    }
}

std::string GameWatch::rotationStatus() const {
    return applied_ ? describeRotation(*applied_) : std::string();
}

void GameWatch::logEvents(const GameDetector::Update& update) {
    const GameDetector::State& state = detector_.state();
    for (const GameDetector::Event event : update.events) {
        switch (event) {
            case GameDetector::Event::Started:
                WE_LOG_INFO(log(), "game: {} started: {}", state.game->label, describeFollowed());
                break;
            case GameDetector::Event::Ended:
                WE_LOG_INFO(log(), "game: {} ended: the defaults again", update.previous->label);
                break;
            case GameDetector::Event::Switched:
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
    queueGameSettings(update);
    writePendingGameSettings();
    return Decision{route, applied_ ? applied_->ratio() : 0.0F, ffbGain_};
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
