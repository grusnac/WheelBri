#include "ShimGamesView.hpp"
#include "UiStyle.hpp"

#include "Platform.hpp"

#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/FileText.hpp"
#include "wheelbri/IniDocument.hpp"
#include "wheelbri/Logging.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.shim");
    return logger;
}

using Colour = ui::Colour;

// The same colours as the rest of the window (App.cpp, ConfigPicker.cpp).
constexpr Colour kStripe = style::kField;
constexpr Colour kText = style::kText;
constexpr Colour kDim = style::kDim;
constexpr Colour kHeading = style::kHeading;
constexpr Colour kCaution = style::kCaution;

constexpr float kRowHeight = 16;
constexpr float kFieldWidth = 230;
constexpr float kGamesWidth = 96;
constexpr std::uint64_t kStateEveryMs = 2000;

std::string fit(std::string text, const std::size_t chars) {
    if (text.size() > chars) text = text.substr(0, chars - 2) + "..";
    return text;
}

}  // namespace

ShimGamesView::ShimGamesView(ui::UiWindow& window, Settings settings)
    : window_(window), settings_(std::move(settings)), list_(readShimGames(settings_.listFile)) {
    if (!settings_.stateFile.empty()) {
        hotSwap_ = readAppSetting(settings_.stateFile, "shim_hot_swap") != "off";
        const std::string atStop = readAppSetting(settings_.stateFile, "shim_at_stop");
        atStop_ = atStop == "keep" ? AtStop::Keep : atStop == "delete" ? AtStop::Delete : AtStop::Ask;
    }
    const auto where = [](const std::filesystem::path& dll) {
        return dll.empty() ? std::string("not found") : pathToUtf8(dll);
    };
    WE_LOG_INFO(log(), "shim games: {} in {}; WheelBri's shim: 64-bit {}, 32-bit {}", list_.games.size(),
                pathToUtf8(settings_.listFile), where(settings_.sources.x64), where(settings_.sources.x86));
}

std::filesystem::path ShimGamesView::source(const std::filesystem::path& folder) const {
    return shimSourceFor(folder, settings_.sources);
}

void ShimGamesView::setAtStop(const AtStop atStop) {
    atStop_ = atStop;
    const char* const value = atStop == AtStop::Keep ? "keep" : atStop == AtStop::Delete ? "delete" : "ask";
    if (!settings_.stateFile.empty()) writeAppSetting(settings_.stateFile, "shim_at_stop", value);
    WE_LOG_INFO(log(), "shim: at Stop, {}", atStop == AtStop::Keep     ? "the shims stay (not asked)"
                                         : atStop == AtStop::Delete ? "the shims are deleted (not asked)"
                                                                    : "the app asks what to do with the shims");
}

std::string ShimGamesView::labelOf(const std::filesystem::path& folder) const {
    if (!settings_.gameLabel) return {};
    const std::uint64_t now = platform::nowMs();
    if (now - labelsReadMs_ >= kStateEveryMs) {   // the folder is looked into, not every frame
        labels_.clear();
        labelsReadMs_ = now;
    }
    auto it = labels_.find(folder);
    if (it == labels_.end()) it = labels_.emplace(folder, settings_.gameLabel(folder)).first;
    return it->second;
}

std::string ShimGamesView::displayName(const ShimGame& game) const {
    const std::string label = labelOf(game.folder);
    if (label.empty() || label == game.name) return game.name;
    return label + " (" + game.name + ")";
}

std::vector<std::string> ShimGamesView::swapShims(const std::string& target, const AxesFor& axesFor) {
    std::vector<std::string> problems;
    std::size_t swapped = 0;
    for (const ShimGame& game : list_.games) {
        const ShimState state = shimState(game.folder, source(game.folder));
        if (state != ShimState::Installed && state != ShimState::Outdated) continue;   // no shim of ours there
        const ShimResult result = setShimTarget(game.folder, target);
        if (!result.ok) {
            WE_LOG_WARN(log(), "shim hot swap: {} ({}) not set to the {}: {}", game.name, pathToUtf8(game.folder),
                        target, result.error);
            problems.push_back(game.name + ": " + result.error);
            continue;
        }
        for (const std::string& file : result.changed) {
            WE_LOG_INFO(log(), "shim hot swap: {} now presents the {} ({})", game.name, target, file);
            ++swapped;
        }
        const std::optional<JoystickLayout> axes = axesFor ? axesFor(game.folder) : std::nullopt;
        const ShimResult written = setShimGameAxes(game.folder, axes);
        if (!written.ok) {
            WE_LOG_WARN(log(), "shim hot swap: {}'s pedal axes not set: {}", game.name, written.error);
            problems.push_back(game.name + ": its pedal axes: " + written.error);
            continue;
        }
        for (const std::string& file : written.changed) {
            if (axes) {
                WE_LOG_INFO(log(), "shim hot swap: {}'s pedal axes for the {}: throttle {} brake {} clutch {} ({})",
                            game.name, target, axisName(axes->throttle), axisName(axes->brake),
                            axisName(axes->clutch), file);
            } else {
                WE_LOG_INFO(log(), "shim hot swap: WheelBri's pedal axes taken out of {} (none set for the game)",
                            file);
            }
        }
    }
    statesReadMs_ = 0;   // read again
    if (swapped > 0) {
        std::string upper = target;
        for (char& ch : upper) ch = ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 'a' + 'A') : ch;
        toast_ = "Hot swap: " + std::to_string(swapped) + (swapped == 1 ? " game presents" : " games present") +
                 " the " + upper;
    }
    return problems;
}

void ShimGamesView::save() {
    ++changes_;
    if (!writeShimGames(settings_.listFile, list_)) {
        WE_LOG_WARN(log(), "cannot save the shim games in {}", pathToUtf8(settings_.listFile));
    }
}

void ShimGamesView::refreshStates() {
    states_.clear();
    targets_.clear();
    asiLoaders_.clear();
    chained_.clear();
    chainProblems_.clear();
    statesReadMs_ = platform::nowMs();
    for (const ShimGame& game : list_.games) {
        states_.push_back(shimState(game.folder, source(game.folder)));
        asiLoaders_.push_back(shimDllIn(game.folder) != game.folder / kShimDll);
        chained_.push_back(shimChained(game.folder));
        chainProblems_.push_back(states_.back() == ShimState::Foreign ? chainProblem(game.folder) : std::string());
        const auto text = readFileText(game.folder / kShimSettings);
        targets_.push_back(text ? IniDocument::parse(*text).document.get("shim", "target_profile").value_or("g920")
                                : std::string());
    }
}

void ShimGamesView::put(const std::size_t index) {
    if (index >= list_.games.size()) return;
    const ShimGame& game = list_.games[index];
    const ShimResult result = installShim(game.folder, source(game.folder), settings_.sourceSettings);
    if (!result.ok) {
        problem_ = "The shim could not be put into " + game.name + ": " + result.error + ".\n" + pathToUtf8(game.folder);
        WE_LOG_WARN(log(), "shim not put into {}: {}", pathToUtf8(game.folder), result.error);
    }
    for (const std::string& file : result.changed) WE_LOG_INFO(log(), "shim: wrote {}", file);
    if (result.ok) toast_ = "Shim put into " + displayName(game);
    ++changes_;   // a new shim in a folder presents the default target: hot swap sets it
    refreshStates();
}

void ShimGamesView::remove(const std::size_t index) {
    if (index >= list_.games.size()) return;
    const ShimGame game = list_.games[index];
    std::string error;
    if (!removeFrom(game.folder, error)) {
        problem_ = "The shim could not be deleted from " + game.name + ": " + error + ".\n" + pathToUtf8(game.folder);
    } else {
        toast_ = "Shim deleted from " + displayName(game);
    }
    refreshStates();
}

void ShimGamesView::forget(const std::size_t index) {
    if (index >= list_.games.size()) return;
    const ShimGame game = list_.games[index];
    const ShimState state = shimState(game.folder, source(game.folder));
    if (!removeShimGame(list_, index)) return;
    save();
    refreshStates();
    WE_LOG_INFO(log(), "shim game forgotten: {} ({})", game.name, pathToUtf8(game.folder));
    if (state == ShimState::Installed || state == ShimState::Outdated) {
        problem_ = game.name + " is off the list. WheelBri's shim is still in its folder: to delete it too, add the "
                   "game again and press Remove shim.\n" + pathToUtf8(game.folder);
    }
}

bool ShimGamesView::addFolder(const std::filesystem::path& folder, std::string& problem) {
    if (const std::string why = gameFolderProblem(folder); !why.empty()) {
        problem = why;
        return false;
    }
    const std::filesystem::path before = list_.selected;
    addShimGame(list_, folder);
    if (!before.empty()) list_.selected = before;   // the file's older "selected" line stays as it was
    save();
    refreshStates();
    WE_LOG_INFO(log(), "shim game folder added: {}", pathToUtf8(folder));
    return true;
}

bool ShimGamesView::chooseFolder(const GameProfile* const game, const std::filesystem::path& start) {
    std::filesystem::path folder = start;
    std::string error;
    const std::string title = game != nullptr ? "Choose " + game->label + "'s folder (the one with " + game->program +
                                                    ")"
                                              : std::string("Choose the game's folder (the one with its .exe)");
    if (!window_.pickFolder(title, folder, error)) {
        if (!error.empty()) problem_ = error;
        return false;
    }
    if (game != nullptr) {
        const std::array<GameProfile, 1> one{*game};
        if (gameInFolder(folder, one) == nullptr) {
            problem_ = "Not added: " + game->program + " is not in that folder.\n" + pathToUtf8(folder);
            return false;
        }
    }
    std::string why;
    if (!addFolder(folder, why)) {
        problem_ = "Not added: " + why + ".\n" + pathToUtf8(folder);
        return false;
    }
    toast_ = "Folder added: " + pathToUtf8(folder.filename());
    return true;
}

void ShimGamesView::setHotSwap(const bool on) {
    if (on == hotSwap_) return;
    hotSwap_ = on;
    if (hotSwap_) ++changes_;   // on again: swapped to the target now
    if (!settings_.stateFile.empty()) writeAppSetting(settings_.stateFile, "shim_hot_swap", hotSwap_ ? "on" : "off");
    WE_LOG_INFO(log(), "shim: hot swap {}", hotSwap_ ? "on (the games' shims follow the target)"
                                                   : "off (the games' shims get the target at Start)");
}

void ShimGamesView::refresh(const bool now) {
    if (now || states_.size() != list_.games.size() || platform::nowMs() - statesReadMs_ >= kStateEveryMs) {
        refreshStates();
    }
}

ShimState ShimGamesView::stateAt(const std::size_t index) const {
    return index < states_.size() ? states_[index] : ShimState::Missing;
}

std::string ShimGamesView::targetAt(const std::size_t index) const {
    return index < targets_.size() ? targets_[index] : std::string();
}

bool ShimGamesView::asiLoaderAt(const std::size_t index) const {
    return index < asiLoaders_.size() && asiLoaders_[index];
}

bool ShimGamesView::chainedAt(const std::size_t index) const { return index < chained_.size() && chained_[index]; }

std::string ShimGamesView::chainProblemAt(const std::size_t index) const {
    return index < chainProblems_.size() ? chainProblems_[index] : std::string();
}

bool ShimGamesView::prepareStart(const std::string& profile, const AxesFor& axesFor,
                                 std::vector<std::filesystem::path>& prepared, std::string& error) {
    prepared.clear();
    if (!hasGame()) {
        error = "No game yet: add one on the Games tab (G).";
        return false;
    }
    std::vector<std::string> others;
    for (const ShimGame& game : list_.games) {
        // Put in, replaced when older, another mod's DLL chained; a chained one checked for its chain line. A shim
        // in place is left as it is (its settings are setShimTarget's below).
        const ShimState state = shimState(game.folder, source(game.folder));
        ShimResult result;
        result.ok = true;
        if (state != ShimState::Installed || shimChained(game.folder)) {
            result = installShim(game.folder, source(game.folder), settings_.sourceSettings);
        }
        if (!result.ok && result.leftAlone) {   // another mod's DLL that cannot be chained: never touched
            WE_LOG_WARN(log(), "shim: {} left out: {} ({})", game.name, result.error, pathToUtf8(game.folder));
            others.push_back(game.name);
            continue;
        }
        if (!result.ok) {
            error = "The shim could not be put into " + displayName(game) + ": " + result.error + ".";
            WE_LOG_WARN(log(), "shim: {}", error);
            return false;
        }
        for (const std::string& file : result.changed) WE_LOG_INFO(log(), "shim: wrote {}", file);
        if (shimChained(game.folder)) {
            WE_LOG_INFO(log(), "shim: {} is chained: DirectInput goes on to {}", game.name, kShimChainDll);
        }
        // The wheel it presents: the target picked on the Mapping tab; and the game's own pedal axes.
        const ShimResult target = setShimTarget(game.folder, profile);
        if (!target.ok) {
            error = "The shim in " + displayName(game) + " could not be set to the " + profile + ": " + target.error +
                    ".";
            WE_LOG_WARN(log(), "shim: {}", error);
            return false;
        }
        for (const std::string& file : target.changed) {
            WE_LOG_INFO(log(), "shim: target_profile = {} in {}", profile, file);
        }
        const std::optional<JoystickLayout> axes = axesFor ? axesFor(game.folder) : std::nullopt;
        const ShimResult written = setShimGameAxes(game.folder, axes);
        if (!written.ok) {
            error = "The pedal axes for " + displayName(game) + " could not be set: " + written.error + ".";
            WE_LOG_WARN(log(), "shim: {}", error);
            return false;
        }
        for (const std::string& file : written.changed) WE_LOG_INFO(log(), "shim: pedal axes in {}", file);
        prepared.push_back(game.folder);
    }
    statesReadMs_ = 0;   // read again
    if (prepared.empty()) {
        error = "No game folder can take the shim: " + others.front() +
                (others.size() > 1 ? " and the others hold" : " holds") +
                " another mod's dinput8.dll that WheelBri cannot chain (see the log).";
        return false;
    }
    WE_LOG_INFO(log(), "shim: ready in {} game folder(s), presenting the {}", prepared.size(), profile);
    return true;
}

bool ShimGamesView::removeFrom(const std::filesystem::path& folder, std::string& error) {
    const ShimResult result = removeShim(folder, source(folder));
    if (!result.ok) {
        error = result.error;
        WE_LOG_WARN(log(), "shim not deleted from {}: {}", pathToUtf8(folder), error);
        return false;
    }
    for (const std::string& file : result.changed) WE_LOG_INFO(log(), "shim: deleted {}", file);
    ++changes_;
    return true;
}

bool ShimGamesView::takeProblem(std::string& problem) {
    if (problem_.empty()) return false;
    problem = std::move(problem_);
    problem_.clear();
    return true;
}

bool ShimGamesView::onClick(const float x, const float y) {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) {
            const auto action = it->action;   // a copy: the list belongs to the last draw
            hits_.clear();                    // stale until the next draw: a second click before it does nothing
            action();
            return true;
        }
    }
    return false;
}

void ShimGamesView::draw(const float x0, const float y, const bool canChange) {
    hits_.clear();
    window_.text(x0, y + 4, "Game", kDim);
    const float fieldX = x0 + 38;
    window_.fill(fieldX, y, kFieldWidth, kRowHeight, kStripe);
    const auto chars = static_cast<std::size_t>((kFieldWidth - 10) / ui::kCharWidth);
    if (!hasGame()) {
        window_.text(fieldX + 5, y + 4, fit("no game: add one on the Games tab", chars), kCaution);
    } else {
        // Every listed game, by name (Start readies them all; whichever starts is the one played).
        std::string shown = list_.games.size() == 1 ? std::string() : std::to_string(list_.games.size()) + ": ";
        for (std::size_t i = 0; i < list_.games.size(); ++i) {
            // The game's own name when WheelBri knows it ("NFS Unbound" for its Excalibur folder).
            const std::string label = labelOf(list_.games[i].folder);
            shown += (i > 0 ? ", " : "") + (label.empty() ? list_.games[i].name : label);
        }
        window_.text(fieldX + 5, y + 4, fit(shown, chars), canChange ? kHeading : kText);
    }
    const float x = fieldX + kFieldWidth + 6;
    const std::string label = "Games";
    style::button(window_, x, y, kGamesWidth, kRowHeight, label, false, canChange);
    if (canChange) {
        hits_.push_back({x, y, kGamesWidth, kRowHeight, [this] {
                             if (settings_.openGames) settings_.openGames();
                         }});
    }
}

}  // namespace wheelbri::service
