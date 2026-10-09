#include "GameWatch.hpp"

#include "Platform.hpp"

#include "wheelbri/FileText.hpp"
#include "wheelbri/Logging.hpp"
#include "wheelbri/Nfs2015Settings.hpp"

#include <system_error>

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

}  // namespace

GameWatch::GameWatch(const std::string_view targetProfile, const bool routePedals)
    : detector_(targetProfile), target_(lower(targetProfile)), routePedals_(routePedals) {
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
    text += ": game profile " + state.game->id + " laid over the defaults (" + describeOverlay(*state.game) + ")";
    return text + (routePedals_ ? ": the virtual G920's pedals are routed to match"
                                : ": the shim in the game applies it, the service leaves the pedals alone");
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

PedalRoute GameWatch::check() {
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
    return route;
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
