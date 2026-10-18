#include "ShimGamesView.hpp"

#include "Platform.hpp"

#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/FileText.hpp"
#include "wheelbri/IniDocument.hpp"
#include "wheelbri/Logging.hpp"

#include <algorithm>
#include <utility>

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.shim");
    return logger;
}

using Colour = ui::Colour;

// The same colours as the rest of the window (App.cpp, ConfigPicker.cpp).
constexpr Colour kPanel{44, 48, 56};
constexpr Colour kStripe{30, 33, 39};
constexpr Colour kText{222, 226, 232};
constexpr Colour kDim{120, 126, 136};
constexpr Colour kHeading{120, 190, 255};
constexpr Colour kCaution{206, 164, 58};
constexpr Colour kWarning{214, 72, 64};
constexpr Colour kDark{16, 18, 20};

constexpr Colour kActive{64, 190, 100};
constexpr Colour kPopupColour{36, 40, 48};

constexpr float kRowHeight = 16;
constexpr float kFieldWidth = 230;
constexpr float kGamesWidth = 96;
// The games panel, over the window.
// The games panel: 24 in from the sides and 40 from the top, as wide as the canvas (and as tall, at most) less those
// margins; 912 x 660 in the 960 x 748 design.
constexpr float kPanelX = 24;
constexpr float kPanelY = 40;
constexpr std::uint64_t kStateEveryMs = 2000;

std::string fit(std::string text, const std::size_t chars) {
    if (text.size() > chars) text = text.substr(0, chars - 2) + "..";
    return text;
}

}  // namespace

ShimGamesView::ShimGamesView(ui::UiWindow& window, Settings settings)
    : window_(window), settings_(std::move(settings)), list_(readShimGames(settings_.listFile)) {
    if (!settings_.stateFile.empty()) hotSwap_ = readAppSetting(settings_.stateFile, "shim_hot_swap") != "off";
    refreshState();
    const auto where = [](const std::filesystem::path& dll) {
        return dll.empty() ? std::string("not found") : pathToUtf8(dll);
    };
    WE_LOG_INFO(log(), "shim games: {} in {}; WheelBri's shim: 64-bit {}, 32-bit {}", list_.games.size(),
                pathToUtf8(settings_.listFile), where(settings_.sources.x64), where(settings_.sources.x86));
}

std::filesystem::path ShimGamesView::source(const std::filesystem::path& folder) const {
    return shimSourceFor(folder, settings_.sources);
}

std::string ShimGamesView::selectedName() const {
    for (const ShimGame& game : list_.games) {
        if (game.folder == list_.selected) return game.name;
    }
    return {};
}

void ShimGamesView::refreshState() {
    state_ = hasGame() ? shimState(list_.selected, source(list_.selected)) : ShimState::Missing;
    stateReadMs_ = platform::nowMs();
}

std::vector<std::string> ShimGamesView::swapShims(const std::string& target, const AxesFor& axesFor) {
    std::vector<std::string> problems;
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
    targets_.clear();   // the panel reads them again
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
    for (const ShimGame& game : list_.games) {
        states_.push_back(shimState(game.folder, source(game.folder)));
        const auto text = readFileText(game.folder / kShimSettings);
        targets_.push_back(text ? IniDocument::parse(*text).document.get("shim", "target_profile").value_or("g920")
                                : std::string());
    }
    refreshState();
}

void ShimGamesView::pick(const std::size_t index) {
    if (index >= list_.games.size()) return;
    list_.selected = list_.games[index].folder;
    save();
    refreshState();
    WE_LOG_INFO(log(), "shim game: {} ({})", selectedName(), pathToUtf8(list_.selected));
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
    ++changes_;   // a new shim in a folder presents the default target: hot swap sets it
    refreshStates();
}

void ShimGamesView::remove(const std::size_t index) {
    if (index >= list_.games.size()) return;
    const ShimGame game = list_.games[index];
    std::string error;
    if (!removeFrom(game.folder, error)) {
        problem_ = "The shim could not be deleted from " + game.name + ": " + error + ".\n" + pathToUtf8(game.folder);
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

void ShimGamesView::add() {
    std::filesystem::path folder = list_.selected;   // the window starts next to the last game
    if (!folder.empty()) folder = folder.parent_path();
    std::string error;
    if (!window_.pickFolder("Choose the game's folder (the one with its .exe): WheelBri puts the shim there", folder,
                            error)) {
        if (!error.empty()) problem_ = error;
        return;
    }
    if (const std::string problem = gameFolderProblem(folder); !problem.empty()) {
        problem_ = "Not added: " + problem + ".\n" + pathToUtf8(folder);
        return;
    }
    const ShimResult result = installShim(folder, source(folder), settings_.sourceSettings);
    if (!result.ok) {
        problem_ = "Not added: " + result.error + ".\n" + pathToUtf8(folder);
        WE_LOG_WARN(log(), "shim not put into {}: {}", pathToUtf8(folder), result.error);
        return;
    }
    addShimGame(list_, folder);
    save();
    refreshStates();
    for (const std::string& file : result.changed) WE_LOG_INFO(log(), "shim: wrote {}", file);
    WE_LOG_INFO(log(), "shim game added: {} ({})", selectedName(), pathToUtf8(list_.selected));
}

bool ShimGamesView::prepareStart(const std::string& profile, std::string& error) {
    if (!hasGame()) {
        error = "No game yet: press Add next to Game and choose the game's folder.";
        return false;
    }
    refreshState();
    if (state_ != ShimState::Installed) {
        const ShimResult result = installShim(list_.selected, source(list_.selected), settings_.sourceSettings);
        refreshState();
        if (!result.ok) {
            error = "The shim could not be put into " + selectedName() + ": " + result.error + ".";
            WE_LOG_WARN(log(), "shim: {}", error);
            return false;
        }
        for (const std::string& file : result.changed) WE_LOG_INFO(log(), "shim: wrote {}", file);
    }
    // The wheel it presents: the target picked on the Mapping tab.
    const ShimResult target = setShimTarget(list_.selected, profile);
    if (!target.ok) {
        error = "The shim in " + selectedName() + " could not be set to the " + profile + ": " + target.error + ".";
        WE_LOG_WARN(log(), "shim: {}", error);
        return false;
    }
    for (const std::string& file : target.changed) WE_LOG_INFO(log(), "shim: target_profile = {} in {}", profile, file);
    return true;
}

bool ShimGamesView::removeFrom(const std::filesystem::path& folder, std::string& error) {
    const ShimResult result = removeShim(folder, source(folder));
    refreshState();
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
    if (platform::nowMs() - stateReadMs_ >= kStateEveryMs) refreshState();   // the user may have deleted it by hand
    window_.text(x0, y + 4, "Game", kDim);
    const float fieldX = x0 + 38;
    window_.fill(fieldX, y, kFieldWidth, kRowHeight, kStripe);
    const auto chars = static_cast<std::size_t>((kFieldWidth - 10) / ui::kCharWidth);
    if (!hasGame()) {
        window_.text(fieldX + 5, y + 4, fit("no game: Games, Add game", chars), kCaution);
    } else {
        const bool missing = state_ == ShimState::Missing;
        const bool foreign = state_ == ShimState::Foreign;
        const std::string mark = missing ? " (no shim)" : foreign ? " (not ours)" : "";
        const std::string name = fit(selectedName(), chars - mark.size());
        window_.text(fieldX + 5, y + 4, name + mark, foreign ? kWarning : missing ? kCaution : canChange ? kHeading : kText);
    }
    const float x = fieldX + kFieldWidth + 6;
    const std::string label = "Games (" + std::to_string(list_.games.size()) + ")";
    window_.fill(x, y, kGamesWidth, kRowHeight, canChange ? kPanel : kDark);
    window_.outline(x, y, kGamesWidth, kRowHeight, canChange ? kDim : kPanel);
    window_.text(x + (kGamesWidth - static_cast<float>(label.size()) * ui::kCharWidth) / 2, y + 4, label,
                 canChange ? kText : kDim);
    if (canChange) {
        hits_.push_back({x, y, kGamesWidth, kRowHeight, [this] {
                             refreshStates();
                             panelOpen_ = true;
                         }});
    }
}

void ShimGamesView::onPanelClick(const float x, const float y) {
    // A click outside the panel closes it, and does nothing else (not Start, not a tab under it).
    if (x < panelX_ || x >= panelX_ + panelW_ || y < panelY_ || y >= panelY_ + panelH_) {
        panelHits_.clear();
        panelOpen_ = false;
        return;
    }
    for (auto it = panelHits_.rbegin(); it != panelHits_.rend(); ++it) {
        if (x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) {
            const auto action = it->action;   // a copy: the list belongs to the last draw
            panelHits_.clear();               // stale until the next draw
            action();
            return;
        }
    }
}

void ShimGamesView::drawPanel() {
    panelHits_.clear();
    const float kPanelW = window_.width() - 2 * kPanelX;
    const float kPanelMaxH = window_.height() - kPanelY - 48;
    const auto extraChars = static_cast<std::size_t>(std::max(0.0F, kPanelW - 912) / ui::kCharWidth);
    if (states_.size() != list_.games.size() || targets_.size() != list_.games.size()) refreshStates();
    const float rows = static_cast<float>(std::max<std::size_t>(list_.games.size(), 1));
    const float h = std::min(kPanelMaxH, 80 + rows * 22 + 30);
    panelX_ = kPanelX;
    panelY_ = kPanelY;
    panelW_ = kPanelW;
    panelH_ = h;
    window_.fill(kPanelX, kPanelY, kPanelW, h, kPopupColour);
    window_.outline(kPanelX, kPanelY, kPanelW, h, kHeading);
    const float left = kPanelX + 12;
    window_.text(left, kPanelY + 10, "GAMES WITH THE SHIM", kHeading);
    const auto button = [&](const float bx, const float by, const float w, const std::string& label,
                            std::function<void()> action) {
        window_.fill(bx, by, w, kRowHeight, kPanel);
        window_.outline(bx, by, w, kRowHeight, kDim);
        window_.text(bx + (w - static_cast<float>(label.size()) * ui::kCharWidth) / 2, by + 4, label, kText);
        panelHits_.push_back({bx, by, w, kRowHeight, std::move(action)});
    };
    button(kPanelX + kPanelW - 196, kPanelY + 6, 92, "Add game", [this] { add(); });
    // Hot swap: on (blue), the shims in the games' folders follow the Mapping tab's target.
    const float swapX = kPanelX + kPanelW - 196 - 8 - 132;
    window_.text(left + 168, kPanelY + 10,
                 fit("Start runs the one picked; Add game puts the shim in",
                     static_cast<std::size_t>(std::max(0.0F, swapX - 8 - (left + 168)) / ui::kCharWidth)),
                 kDim);
    window_.fill(swapX, kPanelY + 6, 132, kRowHeight, hotSwap_ ? kHeading : kPanel);
    window_.outline(swapX, kPanelY + 6, 132, kRowHeight, hotSwap_ ? kHeading : kDim);
    const std::string swapLabel = hotSwap_ ? "Hot swap: on" : "Hot swap: off";
    window_.text(swapX + (132 - static_cast<float>(swapLabel.size()) * ui::kCharWidth) / 2, kPanelY + 10, swapLabel,
                 hotSwap_ ? kDark : kText);
    panelHits_.push_back({swapX, kPanelY + 6, 132, kRowHeight, [this] {
        hotSwap_ = !hotSwap_;
        if (hotSwap_) ++changes_;   // on again: swapped to the target now
        if (!settings_.stateFile.empty()) {
            writeAppSetting(settings_.stateFile, "shim_hot_swap", hotSwap_ ? "on" : "off");
        }
        WE_LOG_INFO(log(), "shim: hot swap {}", hotSwap_ ? "on (the games' shims follow the target)"
                                                       : "off (only the picked game's shim, at Start)");
    }});
    button(kPanelX + kPanelW - 100, kPanelY + 6, 88, "Close", [this] { panelOpen_ = false; });

    float y = kPanelY + 36;
    if (list_.games.empty()) {
        window_.text(left, y + 5, "No game yet: Add game, then choose the folder that holds the game's .exe.", kCaution);
    }
    for (std::size_t i = 0; i < list_.games.size(); ++i) {
        if (y + 18 > kPanelY + h - 30) break;
        const ShimGame& game = list_.games[i];
        const bool on = game.folder == list_.selected;
        const ShimState state = states_[i];
        const Colour stateColour = state == ShimState::Foreign   ? kWarning
                                   : state == ShimState::Missing ? kCaution
                                                                 : kActive;
        window_.fill(left, y, kPanelW - 24, 18, on ? kHeading : kPanel);
        panelHits_.push_back({left, y, kPanelW - 24 - 210, 18, [this, i] { pick(i); }});
        window_.text(left + 6, y + 5, fit(game.name, 18), on ? kDark : kText);
        // The folder gets what a wider window adds.
        window_.text(left + 158, y + 5, fit(pathToUtf8(game.folder), 39 + extraChars), on ? kDark : kDim);
        // The state, then the wheel the shim presents, left of Forget.
        const float stateX = left + 484 + static_cast<float>(extraChars) * ui::kCharWidth;
        const std::string stateText = shimStateName(state);
        window_.text(stateX, y + 5, stateText, on ? kDark : stateColour);
        if ((state == ShimState::Installed || state == ShimState::Outdated) && i < targets_.size()) {
            // The wheel its shim presents.
            window_.text(stateX + static_cast<float>(stateText.size() + 2) * ui::kCharWidth, y + 5, targets_[i],
                         on ? kDark : kDim);
        }
        button(kPanelX + kPanelW - 210, y + 1, 74, "Forget", [this, i] { forget(i); });
        if (state == ShimState::Missing || state == ShimState::Outdated) {
            button(kPanelX + kPanelW - 130, y + 1, 110, "Put shim", [this, i] { put(i); });
        } else if (state == ShimState::Installed) {
            button(kPanelX + kPanelW - 130, y + 1, 110, "Remove shim", [this, i] { remove(i); });
        }
        y += 22;
    }
    window_.text(left, kPanelY + h - 20,
                 hotSwap_ ? "Hot swap on: the shims in these folders present the Mapping tab's target. Click a game to "
                            "pick it."
                          : "Hot swap off: only the picked game's shim is set to the target, at Start.",
                 kDim);
}

}  // namespace wheelbri::service
