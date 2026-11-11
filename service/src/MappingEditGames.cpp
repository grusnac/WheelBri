// SPDX-License-Identifier: GPL-3.0-or-later
// MappingEditView: the Games page (games list, folder, profile, the game's own values and settings file).

#include "MappingEditView.hpp"
#include "MappingEditViewParts.hpp"
#include "UiStyle.hpp"
#include "GameWatch.hpp"
#include "HoverText.hpp"

#include "Platform.hpp"
#include "ServiceLogging.hpp"
#include "Version.hpp"

#include "wheelbri/AutoMap.hpp"
#include "wheelbri/AxisTuning.hpp"
#include "wheelbri/AntiDeadZone.hpp"
#include "wheelbri/CentringSpring.hpp"
#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/DeviceMatch.hpp"
#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/FileText.hpp"
#include "wheelbri/GameProfiles.hpp"
#include "wheelbri/GameRotation.hpp"
#include "wheelbri/GameSettings.hpp"
#include "wheelbri/GameTab.hpp"
#include "wheelbri/GameTuning.hpp"
#include "wheelbri/IniDocument.hpp"
#include "wheelbri/Logging.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <utility>

namespace wheelbri::service {

using namespace editview;

// --- the Games page ---------------------------------------------------------------------------------------------

namespace {

constexpr std::uint64_t kFileStateEveryMs = 2000;

// The list shows a game by its name (an added game's: its program's folder); a name another game has too is flagged.
[[nodiscard]] bool nameShared(const std::span<const GameProfile> games, const std::size_t index) {
    for (std::size_t g = 0; g < games.size(); ++g) {
        if (g != index && samePath(games[g].label, games[index].label)) return true;
    }
    return false;
}
constexpr const char* kNotUnique = "! not unique";

std::string clockText(const std::uint64_t ms) {
    const std::uint64_t seconds = ms / 1000;
    char text[32];
    std::snprintf(text, sizeof(text), "%llu:%02llu:%02llu", static_cast<unsigned long long>(seconds / 3600),
                  static_cast<unsigned long long>(seconds / 60 % 60), static_cast<unsigned long long>(seconds % 60));
    return text;
}

}  // namespace

void MappingEditView::pressListBar() {
    const std::uint64_t now = nowMs();
    if (gripPressMs_ != 0 && now - gripPressMs_ < kDoubleClickMs) {   // the second press: fit the longest name
        gripPressMs_ = 0;
        listDragging_ = false;
        gameListW_ = std::clamp(std::ceil(fitListW_), kGameListMinW, maxGameListW());
        keepListWidth();
        return;
    }
    gripPressMs_ = now;
    listGrab_ = clickX_ - gameListW();
    gameListW_ = gameListW();   // what is shown, if the canvas made the kept width narrower
    listDragging_ = true;
}

void MappingEditView::keepListWidth() {
    const std::string value = std::to_string(static_cast<int>(std::lround(gameListW())));
    if (stateFile_.empty() || readAppSetting(stateFile_, "games_list_width") == value) return;
    if (!writeAppSetting(stateFile_, "games_list_width", value)) {
        status_ = "Could not keep the games list's width in " + pathToUtf8(stateFile_) + ".";
        statusError_ = true;
    }
}

void MappingEditView::selectGame(const std::string& id) {
    for (const GameProfile& game : games_) {
        if (game.id == id) {
            if (selectedGame_ != id) field_ = Field::None;
            selectedGame_ = id;
            selectedFolderOnly_.clear();
            return;
        }
    }
}

std::size_t MappingEditView::selectedGameIndex() const {
    const std::span<const GameProfile> games = games_;
    for (std::size_t g = 0; g < games.size(); ++g) {
        if (games[g].id == selectedGame_) return g;
    }
    // None picked yet: the one that runs, else the first for this target, else the first.
    for (std::size_t g = 0; g < games.size(); ++g) {
        if (games[g].id == now_.id) return g;
    }
    for (std::size_t g = 0; g < games.size(); ++g) {
        if (profileFitsTarget(games[g], targetName_)) return g;
    }
    return 0;
}

void MappingEditView::drawGames() {
    const std::span<const GameProfile> games = games_;
    float y = 46;
    if (games.empty()) {
        window_.text(4, y, "No game profiles in this build.", kWarning);
        return;
    }
    const std::size_t picked = selectedGameIndex();

    // The list: the built-in profiles, the games the user added, then the folders of games WheelBri does not know; the
    // one that runs now in green. Each says what is in its folder.
    refreshGameFolders();
    if (!gameListRead_) {
        gameListRead_ = true;
        if (const std::string kept = readAppSetting(stateFile_, "games_list_width"); !kept.empty()) {
            gameListW_ = static_cast<float>(std::atoi(kept.c_str()));
        }
    }
    const float listW = gameListW();
    fitListW_ = kGameListMinW;
    // A row's name, cut only where its tag (`tagW` wide at the right) leaves no room; when cut, its hover text has it
    // whole. What the row needs to show it whole joins fitListW_ (a double-click on the bar).
    const auto drawName = [this, listW](const float rowY, const std::string& name, const float tagW,
                                        const std::string& tag, const ui::Colour colour) {
        const float room = listW - 6 - 8 - tagW - 6;
        const auto chars = static_cast<std::size_t>(std::max(4.0F, room / ui::kCharWidth));
        window_.text(10, rowY + 5, fit(name, chars), colour);
        fitListW_ = std::max(fitListW_, 6 + static_cast<float>(name.size()) * ui::kCharWidth + 8 + tagW + 6);
        if (name.size() > chars) tip(4, rowY, listW, 18, name + " (" + tag + ")");
    };
    style::headingBand(window_, 0, y, gameListW() + 4);
    style::heading(window_, 8, y, "GAMES");
    y += 16;
    const auto folderTag = [this](const std::size_t folder, ui::Colour& colour) {
        const ShimState state = shims_->stateAt(folder);
        if (state == ShimState::Foreign) {   // amber only when Start cannot chain it
            colour = shims_->chainProblemAt(folder).empty() ? kDim : kWarning;
            return std::string(shims_->oldAsiLoaderAt(folder) ? "old ASI" : "other mod");
        }
        if (state == ShimState::Missing) {
            colour = style::kFaint;
            return std::string("no shim");
        }
        colour = kActive;
        return "shim " + shims_->targetAt(folder) +
               (shims_->chainedAt(folder) ? " +mod" : shims_->asiLoaderAt(folder) ? " +ASI" : "");
    };
    for (std::size_t g = 0; g < games.size(); ++g) {
        if (y > contentH() - 150) break;   // keep the buttons and the hint clear
        const GameProfile& game = games[g];
        const bool on = g == picked && selectedFolderOnly_.empty();
        const bool runs = game.id == now_.id;
        const auto folder = gameFolders_.find(game.id);
        const bool hasFolder = shims_ != nullptr && folder != gameFolders_.end();
        // A game that does not support the target says so in the list too (amber).
        const bool unfit = targetAdvice(game, targetName_).has_value();
        window_.fill(4, y, listW, 18, on ? kHeading : kPanel);
        if (runs) {
            window_.fill(4 + listW - 64, y + 3, 58, 12, kActive);
            window_.text(4 + listW - 60, y + 5, "running", kDark);
            drawName(y, game.label, 58, "running", on ? kDark : kText);
        } else {
            ui::Colour colour = style::kFaint;
            std::string tag = userGame(game.id) ? "yours" : "built in";
            if (nameShared(games, g)) {
                tag = kNotUnique;
                colour = kCaution;
            } else if (unfit) {
                tag = "! not " + upperName(targetName_);
                colour = kCaution;
            } else if (hasFolder) {
                tag = folderTag(folder->second, colour);
            }
            const float tagW = static_cast<float>(tag.size()) * ui::kCharWidth;
            window_.text(4 + listW - 6 - tagW, y + 5, tag, on ? kDark : colour);
            drawName(y, game.label, tagW, tag, on ? kDark : kText);
        }
        const std::string id = game.id;
        hit(4, y, listW, 18, [this, id] { selectGame(id); });
        y += 22;
    }
    for (const std::size_t folder : otherFolders_) {
        if (y > contentH() - 150 || shims_ == nullptr || folder >= shims_->folders().size()) break;
        const ShimGame& entry = shims_->folders()[folder];
        const bool on = selectedFolderOnly_ == entry.folder;
        window_.fill(4, y, listW, 18, on ? kHeading : kStripe);
        ui::Colour colour = style::kFaint;
        const std::string tag = folderTag(folder, colour);
        const float tagW = static_cast<float>(tag.size()) * ui::kCharWidth;
        window_.text(4 + listW - 6 - tagW, y + 5, tag, on ? kDark : colour);
        drawName(y, entry.name, tagW, tag, on ? kDark : kText);
        const std::filesystem::path path = entry.folder;
        hit(4, y, listW, 18, [this, path] {
            selectedFolderOnly_ = path;
            field_ = Field::None;
        });
        y += 22;
    }
    y += 4;
    button(4, y, "Add game...", [this] { addGame(); }, false, !locked_ && canPickFile(), 104);
    tip(4, y, 104, 16,
        canPickFile()
            ? "Pick a game's .exe: WheelBri finds the game by that name while it runs, and its folder joins the list "
              "(no shim put in)."
            : "No file window here yet: add games in games.ini next to app.ini.");
    const std::string pickedId = games[picked].id;
    button(4 + 112, y, "Remove", [this, pickedId] { removeGame(pickedId); }, false,
           !locked_ && userGame(pickedId) && selectedFolderOnly_.empty(), 84);
    tip(4 + 112, y, 84, 16, "Takes a game you added off the list, with its own values. Built-in games stay.");
    // The bar between the list and the game: dragged, the list is wider or narrower; double-clicked, it fits.
    {
        const float barX = gameX() - 12;
        const float barH = contentH() - 100;
        const bool active = listDragging_ || (hoverInside_ && hoverX_ >= barX && hoverX_ < barX + 9 &&
                                              hoverY_ >= 42 && hoverY_ < 42 + barH);
        window_.fill(barX, 42, 9, barH, style::kSplit);
        window_.outline(barX, 42, 9, barH, active ? kHeading : style::kSplitEdge);
        const float midY = 42 + std::floor(barH / 2);
        for (const float dy : {-8.0F, 0.0F, 8.0F}) {
            window_.fill(barX + 4, midY + dy - 1, 2, 2, active ? kHeading : kDim);
        }
        hit(barX, 42, 9, barH, [this] { pressListBar(); });
        tip(barX, 42, 9, barH, "Drag to make the games list wider or narrower; double-click to fit its longest name.");
    }

    // A folder whose game WheelBri does not know: its folder and the shim there, and Add as your game for its values.
    std::optional<std::size_t> otherFolder;
    if (!selectedFolderOnly_.empty() && shims_ != nullptr) {
        for (const std::size_t folder : otherFolders_) {
            if (shims_->folders()[folder].folder == selectedFolderOnly_) otherFolder = folder;
        }
        if (!otherFolder) selectedFolderOnly_.clear();
    }
    if (otherFolder) {
        const ShimGame& entry = shims_->folders()[*otherFolder];
        window_.text(gameX(), 46, entry.name, kHeading);
        const std::string unknown =
            "WheelBri does not know the game in this folder: add it as yours to set its values.";
        window_.text(gameX(), 62, fit(unknown, gameChars()), kDim);
        float top = drawGameFolder(nullptr, otherFolder, 80);
        button(gameX(), top + 4, "Add as your game...", [this, entry] { addGame(entry.folder); }, false,
               !locked_ && canPickFile(), 168);
    } else {
        // The picked game: its name and whether it runs, its folder, its sub-tabs, then the one shown.
        float top = drawGameProfile(picked, 46);
        const GameProfile& game = games[picked];
        const auto folder = gameFolders_.find(game.id);
        top = drawGameFolder(&game, folder != gameFolders_.end() ? std::optional<std::size_t>(folder->second)
                                                                 : std::nullopt,
                             top);
        if (gameTab_ == GameTab::File && game.settings.empty()) gameTab_ = GameTab::Controls;
        float x = gameX();
        const auto tab = [&](const GameTab which, const char* label) {
            x += button(x, top + 4, label, [this, which] { gameTab_ = which; }, gameTab_ == which, true) + 6;
        };
        // The game's axes and its buttons on one page; its force feedback is the Live tab's (nothing to set).
        tab(GameTab::Controls, "Axes and buttons");
        if (!game.settings.empty()) tab(GameTab::File, "Game file");
        top += 28;
        switch (gameTab_) {
            case GameTab::Controls: drawGameButtons(picked, drawGameAxes(picked, top) + 12); break;
            case GameTab::File: drawGameSettings(picked, top); break;
        }
    }

    // One status line: what was just done, else when the values apply.
    const float status = contentH() - 24;
    if (!status_.empty()) {
        window_.text(4, status, fit(status_, 118 + extraChars()), statusError_ ? kWarning : kActive);
    } else if (!locked_) {
        window_.text(4, status, "Saved as you change them: used from the next Start, or at once while WheelBri runs.",
                     kDim);
    } else if (path_ == sessionPath_) {
        window_.text(4, status,
                     fit("Running: changes apply at once (pedal axes in Mode Shim, and Add game: from the next Start).",
                         118 + extraChars()),
                     kActive);
    } else {
        window_.text(4, status,
                     fit("Running with WheelBri's default mapping: your changes are kept in your own mapping and used "
                         "from the next Start.",
                         118 + extraChars()),
                     kCaution);
    }
}

void MappingEditView::refreshGameFolders() {
    if (shims_ == nullptr) return;
    shims_->refresh();
    const std::uint64_t now = nowMs();
    if (now - gameFoldersReadMs_ < 2000 && gameFoldersCount_ == shims_->folders().size() + games_.size()) return;
    gameFoldersReadMs_ = now;
    gameFoldersCount_ = shims_->folders().size() + games_.size();
    gameFolders_.clear();
    otherFolders_.clear();
    for (std::size_t f = 0; f < shims_->folders().size(); ++f) {
        const GameProfile* const known = gameInFolder(shims_->folders()[f].folder, games_);
        if (known == nullptr) {
            otherFolders_.push_back(f);
        } else if (gameFolders_.count(known->id) == 0) {
            gameFolders_[known->id] = f;
        }
    }
}

float MappingEditView::drawGameFolder(const GameProfile* const game, const std::optional<std::size_t> folder, float y) {
    if (shims_ == nullptr) return y;
    const bool shimMode = picker_.way() == ConfigPicker::Way::Shim;
    style::headingBand(window_, gameX() - 4, y + 4, canvasW() - gameX());
    style::heading(window_, gameX() + 4, y + 4, "FOLDER");
    const float x = gameX() + 64;
    if (!folder || *folder >= shims_->folders().size()) {
        // No folder yet: only Mode Shim needs one (the shim goes there).
        window_.text(x, y + 4,
                     fit(shimMode ? "none yet: Mode Shim puts its shim in the game's folder"
                                  : "none needed in Mode Virtual (Mode Shim puts its shim there)",
                         gameChars() - 26),
                     shimMode ? kCaution : kDim);
        // The folder window starts next to the last listed game's folder (games are often side by side).
        const std::filesystem::path start =
            shims_->folders().empty() ? std::filesystem::path() : shims_->folders().back().folder.parent_path();
        button(canvasW() - 4 - 152, y, "Choose folder...", [this, game, start] { shims_->chooseFolder(game, start); },
               false, !locked_, 152);
        return y + 24;
    }
    const std::size_t index = *folder;
    const ShimGame& entry = shims_->folders()[index];
    const ShimState state = shims_->stateAt(index);
    const bool ours = state == ShimState::Installed || state == ShimState::Outdated;
    const bool chained = ours && shims_->chainedAt(index);
    const std::string chainProblem = state == ShimState::Foreign ? shims_->chainProblemAt(index) : std::string();
    const bool oldLoader = state == ShimState::Foreign && shims_->oldAsiLoaderAt(index);
    const std::string where = chained ? ", chained to another mod's DLL"
                              : ours && shims_->asiLoaderAt(index) ? " next to the ASI Loader"
                                                                   : "";
    const std::string theirs = oldLoader ? "an old ASI Loader as dinput8.dll" : "another mod's dinput8.dll";
    const std::string stateText =
        state == ShimState::Installed  ? "shim in place" + where
        : state == ShimState::Outdated ? "older shim" + where + " (Start replaces it)"
        : state == ShimState::Foreign  ? (!chainProblem.empty() ? theirs + " (" + chainProblem + "): Start leaves it"
                                          : oldLoader           ? theirs + ": Start moves it to d3d9.dll"
                                                                : theirs + ": Start chains it")
                                       : "no shim (Start puts it in)";
    const std::string target = shims_->targetAt(index);
    const std::string presents =
        (state == ShimState::Installed || state == ShimState::Outdated) && !target.empty()
            ? ", presents the " + upperName(target)
            : std::string();
    const std::size_t pathChars = gameChars() > 60 ? gameChars() - 52 : 8;
    window_.text(x, y + 4, fit(pathToUtf8(entry.folder), pathChars), kDim);
    const float stateX = x + static_cast<float>(pathChars + 2) * ui::kCharWidth;
    window_.text(stateX, y + 4, stateText + presents,
                 !chainProblem.empty() ? kWarning : state == ShimState::Missing || state == ShimState::Foreign ? kDim
                                                                                                             : kActive);
    tip(x, y, static_cast<float>(pathChars) * ui::kCharWidth, 16, pathToUtf8(entry.folder));
    if (chained) {
        tip(stateX, y, static_cast<float>((stateText + presents).size()) * ui::kCharWidth, 16,
            "DirectInput goes: game -> WheelBri's shim -> dinput8-chain.dll (the mod) -> Windows.");
    }
    y += 20;
    // What to do with it: put the shim in or take it out, take the folder off the list.
    float bx = x;
    if (state == ShimState::Missing || state == ShimState::Outdated) {
        bx += button(bx, y, "Put shim", [this, index] { shims_->put(index); }, false, !locked_, 96) + 6;
    } else if (state == ShimState::Foreign && chainProblem.empty()) {
        tip(bx, y, 96, 16,
            oldLoader ? "Renames the ASI Loader to d3d9.dll (the game loads it under that name too) and puts WheelBri's "
                        "shim in its place. Remove shim undoes it."
                      : "Renames the mod's dinput8.dll to dinput8-chain.dll and puts WheelBri's shim in front of it. "
                        "Remove shim undoes it.");
        bx += button(bx, y, oldLoader ? "Put shim" : "Chain mod", [this, index] { shims_->put(index); }, false,
                     !locked_, 96) + 6;
    } else if (state == ShimState::Installed) {
        if (chained) tip(bx, y, 112, 16, "Deletes WheelBri's shim and gives the mod its dinput8.dll back.");
        bx += button(bx, y, "Remove shim", [this, index] { shims_->remove(index); }, false, !locked_, 112) + 6;
    }
    button(bx, y, "Forget folder", [this, index] { shims_->forget(index); }, false, !locked_, 120);
    tip(bx, y, 120, 16, "Takes the folder off the list; its files stay as they are.");
    return y + 24;
}

float MappingEditView::drawGameProfile(const std::size_t index, float y) {
    const GameProfile& game = games_[index];
    window_.text(gameX(), y, game.label, kHeading);
    window_.text(gameX() + static_cast<float>(game.label.size() + 2) * ui::kCharWidth, y,
                 fit("program " + game.program, gameChars() - game.label.size() - 2),
                 kDim);
    y += 16;
    if (nameShared(games_, index)) {
        const std::string where = game.path.empty() ? game.program : game.path;
        for (const std::string& line :
             wrap("Not unique: another game in the list has this name too. Rename this game's folder (" + where +
                      ") so its name is unique, then add it again.",
                  gameChars(), 3)) {
            window_.text(gameX(), y, line, kCaution);
            y += 12;
        }
    }
    // A game that works one way only ([game] ways): a session run the other way tells the user when it starts.
    if (!game.virtualDevice || !game.shim) {
        const std::string only = game.shim ? "Mode Shim only" : "Mode Virtual only";
        for (const std::string& line : wrap(only + ": " + game.waysNote + ".", gameChars(), 2)) {
            window_.text(gameX(), y, line, kCaution);
            y += 12;
        }
    }

    // A game that does not support the target ([game] not_for): what goes wrong and what to use, in an amber box. The
    // target is changed on the Mapping tab only.
    if (const auto advice = targetAdvice(game, targetName_)) {
        const std::vector<std::string> lines = wrap("! " + *advice, gameChars() - 2, 3);
        const float boxH = 8 + 12 * static_cast<float>(lines.size());
        window_.fill(gameX(), y, canvasW() - gameX() - 4, boxH, style::kCautionBack);
        window_.outline(gameX(), y, canvasW() - gameX() - 4, boxH, kCaution);
        float line = y + 5;
        for (const std::string& text : lines) {
            window_.text(gameX() + 8, line, text, kCaution);
            line += 12;
        }
        y += boxH + 6;
    }

    // Now: the game that runs (while a session does), else how it is found.
    if (now_.id == game.id) {
        const std::string line =
            "RUNNING, pid " + std::to_string(now_.pid) + ", for " + clockText(now_.forMs) +
            (now_.profileApplies ? ": its profile and your values are in use" : ": your values are in use");
        window_.text(gameX(), y, fit(line, gameChars()), kActive);
        y += 12;
        std::string applied;
        if (!now_.rotation.empty()) applied += "steering " + now_.rotation;
        if (!describeOverlay(game).empty()) applied += (applied.empty() ? "" : ", ") + std::string("pedals as below");
        if (!applied.empty()) {
            window_.text(gameX(), y, fit("now: " + applied, gameChars()), kText);
            y += 12;
        }
        if (!now_.warning.empty()) {
            window_.text(gameX(), y, fit(now_.warning, gameChars()), kWarning);
            y += 12;
        }
    } else if (now_.session) {
        window_.text(gameX(), y,
                     fit(now_.id.empty() ? std::string("not running: WheelBri looks for it every 2 s")
                                         : "not running (another game with a profile runs)",
                         gameChars()),
                     kDim);
        y += 12;
    } else {
        window_.text(gameX(), y,
                     fit("While WheelBri runs (Start) it finds the game when it starts, and uses this profile.",
                         gameChars()),
                     kDim);
        y += 12;
    }
    return y;
}

// A section's heading on the Games page: its name, and what it holds (dim).
void MappingEditView::gameSection(const float y, const std::string& name, const std::string& about) {
    style::headingBand(window_, gameX() - 4, y, canvasW() - gameX());
    style::heading(window_, gameX() + 4, y, name);
    window_.text(gameX() + 160, y, fit(about, gameChars() - 20), kDim);
}

float MappingEditView::drawGameSettings(const std::size_t index, float y) {
    const GameProfile& game = games_[index];
    const std::string id = game.id;
    if (game.settings.empty()) return y;   // nothing of its own to change: no Game config section at all
    gameSection(y, "GAME CONFIG", "the game's own settings: written into its file only by Save to file");
    y += 16;
    window_.text(gameX(), y, fit(game.settingsNote, gameChars()), kDim);
    y += 16;

    // What the file holds now against the values below (as shown, saved or not), read every 2 s. Where the file is
    // (app.ini, read and parsed): read again with it, or when app.ini changed since (a look at its time and size only;
    // two writes within one tick of the file system's clock can share a time).
    const std::uint64_t readNow = nowMs();
    const bool due = !fileState_ || readNow - fileStateReadMs_ >= kFileStateEveryMs;
    std::error_code statError;
    const std::filesystem::file_time_type stateWritten = std::filesystem::last_write_time(stateFile_, statError);
    const std::uintmax_t stateSize = std::filesystem::file_size(stateFile_, statError);
    if (due || filePlaceGame_ != id || stateWritten != filePlaceStateWritten_ || stateSize != filePlaceStateSize_) {
        filePlace_ = gameFilePlace(game);
        filePlaceGame_ = id;
        filePlaceStateWritten_ = stateWritten;
        filePlaceStateSize_ = stateSize;
    }
    const GameFilePlace& place = filePlace_;
    const std::vector<GameSettingText> wanted = gameValues(game);
    std::string key = id + "|" + pathToUtf8(place.root) + "|" + place.relative;
    for (const GameSettingText& value : wanted) key += "|" + value.name + "=" + value.text;
    if (due || key != fileStateKey_) {
        fileState_ = readGameSettingsState(place.root, place.relative, wanted);
        fileStateKey_ = key;
        fileStateReadMs_ = readNow;
    }
    const GameSettingsFileState& file = *fileState_;
    using Status = GameSettingsFileState::Status;
    window_.text(gameX(), y, "File", kText);
    const std::string where = place.located           ? pathToUtf8(place.root / pathFromUtf8(place.relative))
                              : !place.root.empty()   ? "Documents/" + game.settingsFile
                                                      : game.settingsFile;
    window_.text(gameValueX(), y, fit(where, valueChars()), place.located ? kText : kDim);
    y += 12;
    std::string fileLine;
    Colour fileColour = kDim;
    switch (file.status) {
        case Status::NotHere:
            fileLine = "not found here: Locate it";
            fileColour = kCaution;
            break;
        case Status::NoFile:
            fileLine = place.located ? "no longer there: Locate it again"
                                     : "not found: start the game once (it writes it), or Locate it";
            fileColour = kCaution;
            break;
        case Status::Unreadable:
            fileLine = "cannot be read: " + file.why;
            fileColour = kWarning;
            break;
        case Status::Read:
            if (file.asSet()) {
                fileLine = "holds the values below";
                fileColour = kActive;
            } else {
                fileLine = "not the values below yet: Save to file writes them (game closed)";
                fileColour = kCaution;
            }
            break;
    }
    window_.text(gameValueX(), y, fit(fileLine, valueChars()), fileColour);
    if (!place.confirmed) {
        y += 12;
        window_.text(gameValueX(), y,
                     fit("not seen written by " + game.label + " yet: play it once while WheelBri runs", valueChars()),
                     kCaution);
        tip(gameX(), y - 12, canvasW() - gameX() - 4, 24,
            "WheelBri knows a game's settings file when it sees the game write it (when the game ends). Another game "
            "can have a file of the same name with the same settings (NFS Heat's for NFS Unbound): until then Save to "
            "file asks first.");
    }
    y += 16;
    // The file found: Save to file. Not found yet: Locate it first (the button then says Save to file).
    const bool found = file.status == Status::Read || file.status == Status::Unreadable;
    if (found) {
        button(gameValueX(), y, "Save to file", [this, index] { saveGameFile(index); }, !file.asSet(),
               !locked_ && file.status == Status::Read, 120);
        button(gameValueX() + 128, y, "Locate...", [this, index] { locateGameFile(index); }, false,
               !locked_ && canPickFile(), 88);
        window_.text(gameValueX() + 224, y + 4, fit("keeps the original as .before-wheelbri", valueChars() - 28),
                     kDim);
    } else {
        button(gameValueX(), y, "Locate file", [this, index] { locateGameFile(index); }, true,
               !locked_ && canPickFile(), 120);
        const std::string name = pathToUtf8(pathFromUtf8(game.settingsFile).filename());
        window_.text(gameValueX() + 128, y + 4,
                     fit(canPickFile() ? "find the game's " + name + " file; then Save to file"
                                                 : "no file dialog on this system yet",
                         valueChars() - 16),
                     kDim);
    }
    y += 22;
    if (file.status == Status::Read) window_.text(fileColumnX(), y - 2, "in the file", kDim);
    y += 10;

    for (std::size_t s = 0; s < game.settings.size() && s < kMaxGameSettings; ++s) {
        if (y > contentH() - 52 - 20) break;   // keep clear of the hint
        const GameSetting& setting = game.settings[s];
        // A "steering" setting is the steering rotation: named and edited as in Mapping above, one value.
        window_.text(gameX(), y + 4, fit(setting.value ? gameSettingLabel(setting.id) : "steering rotation", 19),
                     kText);
        const std::optional<std::string> inFile =
            s < file.entries.size() ? file.entries[s].inFile : std::optional<std::string>();
        // What the file holds, right: amber while it is not what the next Start writes.
        if (file.status == Status::Read) {
            const bool asWanted = s < wanted.size() && inFile == wanted[s].text;
            window_.text(fileColumnX(), y + 4, fit(inFile.value_or(std::string("not found")), 13),
                         asWanted ? kDim : kCaution);
        }
        tip(gameX(), y, fileColumnX() - gameX(), 16,
            setting.name + " in the game's settings file" +
                (setting.value ? ": profile " + tuning::formatNumber(*setting.value, 2) + ", 0 to 1"
                               : ": the steering rotation, in degrees"));
        if (!setting.value) {
            // The same value as Mapping's steering rotation: changing either changes both.
            const bool ownRotation = edit_.gameRotations.count(id) != 0;
            const bool rotationChanged =
                ownRotation != (saved_.gameRotations.count(id) != 0) ||
                (ownRotation && !(edit_.gameRotations.at(id) == saved_.gameRotations.at(id)));
            const float deg = rotationWhileRunning(game, edit_.gameRotations, profileFitsTarget(game, targetName_))
                                   .value_or(edit_.steeringRotationDeg);
            drawField(gameValueX(), y, 80, Field::GameSteering, degrees(deg), rotationChanged, index);
            window_.text(gameValueX() + 84, y + 4, "deg", kDim);
            window_.text(gameValueX() + 248, y + 4, fit("same as in Mapping", 26), kDim);
            y += 20;
            continue;
        }
        const auto ownOf = [&](const GameSettingOverrides& values) -> std::optional<float> {
            const auto byGame = values.find(id);
            if (byGame == values.end()) return std::nullopt;
            const auto it = byGame->second.find(setting.id);
            return it == byGame->second.end() ? std::nullopt : std::optional<float>(it->second);
        };
        const std::optional<float> own = ownOf(edit_.gameSettings);
        const std::optional<float> saved = ownOf(saved_.gameSettings);
        const bool changed = own.has_value() != saved.has_value() || (own && std::fabs(*own - *saved) > 1e-5F);
        drawField(gameValueX(), y, 80, Field::GameSetting, tuning::formatNumber(own.value_or(*setting.value), 2),
                  changed, index * kMaxGameSettings + s);
        const std::string settingId = setting.id;
        button(gameValueX() + 168, y, "Default", [this, id, settingId] {
            if (const auto byGame = edit_.gameSettings.find(id); byGame != edit_.gameSettings.end()) {
                byGame->second.erase(settingId);
                if (byGame->second.empty()) edit_.gameSettings.erase(byGame);
            }
        }, false, editable() && own.has_value(), 72);
        window_.text(gameValueX() + 248, y + 4,
                     fit("profile " + tuning::formatNumber(*setting.value, 2) + ", 0 to 1", 26), kDim);
        y += 20;
    }
    return y;
}

// --- the picked game's own values (GameTuning.hpp) ----------------------------------------------------------------

namespace {

// The axes a pedal can be read from, in the order < and > go through them (clutch also none).
constexpr std::array<ObjectGuid, 6> kPedalAxes = {ObjectGuid::YAxis,  ObjectGuid::ZAxis,  ObjectGuid::RxAxis,
                                                  ObjectGuid::RyAxis, ObjectGuid::RzAxis, ObjectGuid::Slider};
constexpr std::array<const char*, 3> kPedalNames = {"Throttle", "Brake", "Clutch"};
}  // namespace

const GameTuning* MappingEditView::tuningOf(const std::string& id) const {
    const auto found = edit_.gameTunings.find(id);
    return found == edit_.gameTunings.end() ? nullptr : &found->second;
}

GameTuning& MappingEditView::tuningFor(const std::string& id) { return edit_.gameTunings[id]; }

void MappingEditView::dropEmptyTuning(const std::string& id) {
    if (const auto found = edit_.gameTunings.find(id); found != edit_.gameTunings.end() && found->second.empty()) {
        edit_.gameTunings.erase(found);
    }
}

bool MappingEditView::tuningChanged(const std::string& id) const {
    const auto a = edit_.gameTunings.find(id);
    const auto b = saved_.gameTunings.find(id);
    const GameTuning none;
    return !((a == edit_.gameTunings.end() ? none : a->second) == (b == saved_.gameTunings.end() ? none : b->second));
}

bool MappingEditView::userGame(const std::string& id) const {
    // One in games.ini with a built-in profile's id (a hand edit) is that profile, not the user's (reloadGames).
    const auto same = [&id](const GameProfile& g) { return g.id == id; };
    return std::any_of(userGames_.begin(), userGames_.end(), same) &&
           std::none_of(gameProfiles().begin(), gameProfiles().end(), same);
}

void MappingEditView::reloadGames() {
    userGames_ = loadUserGames();
    games_.assign(gameProfiles().begin(), gameProfiles().end());
    for (const GameProfile& game : userGames_) {
        const bool builtIn = std::any_of(gameProfiles().begin(), gameProfiles().end(), [&game](const GameProfile& g) {
            return g.id == game.id;
        });
        if (!builtIn) games_.push_back(game);
    }
}

void MappingEditView::addGame(const std::filesystem::path& start) {
    const std::optional<std::filesystem::path> picked =
        pickFile("Add a game: pick its program (.exe)", start, "");
    if (!picked) return;   // cancelled
    // Its folder joins the game folders (for Mode Shim; no shim put in until Put shim or Start).
    const auto addFolder = [this, &picked] {
        if (shims_ == nullptr) return;
        for (const ShimGame& entry : shims_->folders()) {
            std::error_code ec;
            if (std::filesystem::equivalent(entry.folder, picked->parent_path(), ec)) return;   // listed already
        }
        std::string problem;
        if (!shims_->addFolder(picked->parent_path(), problem)) {
            WE_LOG_WARN(log(), "games: {}'s folder not added: {}", pathToUtf8(picked->parent_path()), problem);
        }
        gameFoldersReadMs_ = 0;
    };
    const std::string program = programName(pathToUtf8(picked->filename()));
    if (program.size() < 5 || program.compare(program.size() - 4, 4, ".exe") != 0) {
        status_ = "Not added: pick the game's program, a .exe file.";
        statusError_ = true;
        return;
    }
    // Kept only if games.ini can hold it as it is (iniSafeValue blanks ; and #); else found by its name, as before.
    std::string path = pathToUtf8(*picked);
    if (iniSafeValue(path) != path) {
        WE_LOG_WARN(log(), "games: {} has ; or # in its path: found by its program's name only", path);
        path.clear();
    }
    // The same game: the same program file (its absolute path); without a path, a game you added by name only.
    for (const GameProfile& game : games_) {
        if (!userGame(game.id) || game.program != program) continue;
        if (game.path.empty() ? path.empty() : samePath(game.path, path)) {
            addFolder();
            selectGame(game.id);
            status_ = game.label + " is in the list already (" + (game.path.empty() ? program : game.path) + ").";
            statusError_ = false;
            return;
        }
    }
    // Named after its folder ("NFS Most Wanted"): several games' programs share a name (speed.exe).
    std::string folderName = pathToUtf8(picked->parent_path().filename());
    if (folderName.empty()) folderName = pathToUtf8(picked->stem());
    std::string base = gameIdFor(folderName);
    if (base.empty()) base = gameIdFor(program);
    if (base.empty()) base = "game";
    std::string id = base;
    const auto taken = [this](const std::string& candidate) {
        return std::any_of(games_.begin(), games_.end(), [&candidate](const GameProfile& g) { return g.id == candidate; });
    };
    for (int n = 2; taken(id); ++n) id = base + std::to_string(n);
    GameProfile game;
    game.id = id;
    game.label = folderName;   // the program's parent folder, as it is
    game.program = program;
    game.path = path;
    std::vector<GameProfile> next = userGames_;
    next.push_back(game);
    const std::filesystem::path file = userGamesFile();
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (!writeFileReplacing(file, formatUserGames(next))) {
        status_ = "Not added: cannot write " + pathToUtf8(file) + ".";
        statusError_ = true;
        return;
    }
    WE_LOG_INFO(log(), "games: added {} ({}) to {}", game.label, game.program, pathToUtf8(file));
    reloadGames();
    addFolder();
    selectGame(id);
    gameTab_ = GameTab::Controls;
    status_ = game.label + " added: WheelBri finds " + program + " while it runs. Set its own values here.";
    statusError_ = false;
    for (std::size_t g = 0; g < games_.size(); ++g) {
        if (games_[g].id == id && nameShared(games_, g)) {
            status_ = game.label + " added, but its name is not unique: rename its folder so the name is unique.";
            statusError_ = true;
        }
    }
}

void MappingEditView::removeGame(const std::string& id) {
    std::vector<GameProfile> next;
    std::string label;
    for (const GameProfile& game : userGames_) {
        if (game.id == id) {
            label = game.label;
        } else {
            next.push_back(game);
        }
    }
    if (label.empty()) return;   // a built-in profile: never removed
    // Its own values go first (saved as they change), so the mapping never names a game that is gone: the config
    // would then have a problem (a [game_rotation] line for no game). (Only this save's result stops it: an error the
    // page showed before, for something else, does not.)
    edit_.gameTunings.erase(id);
    edit_.gameRotations.erase(id);
    edit_.gameSettings.erase(id);
    if (gameValuesChanged()) {
        saveGameValues();
        if (statusError_) {
            WE_LOG_WARN(log(), "games: {} not removed: its values could not be saved ({})", label, status_);
            return;
        }
    }
    if (!writeFileReplacing(userGamesFile(), formatUserGames(next))) {
        status_ = "Not removed: cannot write " + pathToUtf8(userGamesFile()) + ".";
        statusError_ = true;
        WE_LOG_WARN(log(), "games: {} not removed: {}", label, status_);
        return;
    }
    WE_LOG_INFO(log(), "games: removed {} ({})", label, id);
    status_ = label + " removed, with its own values.";
    statusError_ = false;
    // Add game put its folder on the list: it goes too (its files stay), unless WheelBri's shim is in it (the folder
    // stays listed, so Remove shim can delete it).
    if (const auto folder = gameFolders_.find(id); shims_ != nullptr && folder != gameFolders_.end() &&
                                                   folder->second < shims_->folders().size()) {
        const ShimState state = shims_->stateAt(folder->second);
        if (state == ShimState::Installed || state == ShimState::Outdated) {
            status_ = label + " removed, with its own values. Its folder stays listed: WheelBri's shim is in it " +
                      "(Remove shim, then Forget folder).";
        } else {
            shims_->forget(folder->second);
        }
    }
    reloadGames();
    gameFoldersCount_ = 0;   // the folders are matched to the games again at the next draw
    selectedGame_.clear();
    selectedFolderOnly_.clear();
}

SourceRef MappingEditView::pickedSource() const {
    if (pickKind_ == Slot::GameButton) {
        const std::size_t picked = selectedGameIndex();
        if (picked < games_.size()) {
            if (const GameTuning* const own = tuningOf(games_[picked].id)) {
                if (const auto it = own->buttons.find(static_cast<int>(pickIndex_)); it != own->buttons.end()) {
                    return it->second;
                }
            }
        }
        return edit_.buttons[pickIndex_];   // as mapped
    }
    return const_cast<MappingEditView*>(this)->slot(pickKind_, pickIndex_);
}

void MappingEditView::setPicked(const SourceRef& source) {
    if (pickKind_ != Slot::GameButton) {
        slot(pickKind_, pickIndex_) = source;
        // Picked: the user's own from now on, written in the file, no longer the Rig's.
        AutoInputs& autos = edit_.autoInputs;
        if (pickKind_ == Slot::Axis) autos.axes[pickIndex_] = false;
        if (pickKind_ == Slot::Hat) autos.hat[pickIndex_] = false;
        if (pickKind_ == Slot::Button && pickIndex_ < autos.buttons.size()) autos.buttons[pickIndex_] = false;
        return;
    }
    const std::size_t picked = selectedGameIndex();
    if (picked >= games_.size()) return;
    const std::string id = games_[picked].id;
    if (source == edit_.buttons[pickIndex_]) {
        // The mapping's own: no value of the game's needed (the main follows the Mapping tab).
        tuningFor(id).buttons.erase(static_cast<int>(pickIndex_));
        dropEmptyTuning(id);
    } else {
        tuningFor(id).buttons[static_cast<int>(pickIndex_)] = source;
    }
}

float MappingEditView::antiDeadZoneWhileRunning(const GameProfile& game) const {
    const GameTuning* const own = tuningOf(game.id);
    if (own != nullptr && own->antiDeadZonePercent) return *own->antiDeadZonePercent;
    if (game.antiDeadZonePercent && profileFitsTarget(game, targetName_)) return *game.antiDeadZonePercent;
    return edit_.antiDeadZonePercent;
}

float MappingEditView::drawGameAxes(const std::size_t index, float y) {
    const GameProfile& game = games_[index];
    const std::string id = game.id;
    const GameTuning* const own = tuningOf(id);
    const GameTuning* const saved = saved_.gameTunings.count(id) != 0 ? &saved_.gameTunings.at(id) : nullptr;
    const std::string target = profile_ != nullptr ? profile_->identity().shortName : targetName_;
    gameSection(y, "AXES", "what the game reads; starts from its profile and your Mapping tab");
    y += 16;
    // The columns: name, the axis the game reads, its direction, the degrees (steering), Default, where it comes from.
    const float axisX = gameValueX() - 52;
    const float directionX = axisX + 136;
    const float degreesX = directionX + 96;
    const float defaultX = degreesX + 72;
    const float fromX = defaultX + 80;
    window_.text(axisX, y, "game reads", kDim);
    tip(axisX - 2, y - 2, 10 * ui::kCharWidth + 4, 12,
        "The axis the game reads each pedal from. How they feel (dead zones, curves): your rig's own software.");
    window_.text(directionX, y, "direction", kDim);
    window_.text(degreesX, y, "degrees", kDim);
    window_.text(fromX, y, "comes from", kDim);
    y += 14;
    const auto from = [&](const float rowY, const bool mine, const std::string& source) {
        window_.text(fromX, rowY + 4, fit(mine ? "yours" : source, valueChars() - 40), mine ? kMine : kDim);
    };

    // Steering: the wheel's axis (games read it there), and the rotation the game is played with ([game_rotation]):
    // yours, else the profile's, else the Mapping tab's. The wheel's own software must be set to the same.
    window_.text(gameX(), y + 4, "Steering", kText);
    window_.fill(axisX, y, 128, 16, kStripe);
    window_.text(axisX + 6, y + 4, "x, the wheel", kDim);
    const bool ownRotation = edit_.gameRotations.count(id) != 0;
    const bool fits = profileFitsTarget(game, targetName_);
    const std::optional<float> rotation = rotationWhileRunning(game, edit_.gameRotations, fits);
    const bool rotationChanged = ownRotation != (saved_.gameRotations.count(id) != 0) ||
                                 (ownRotation && !(edit_.gameRotations.at(id) == saved_.gameRotations.at(id)));
    const float degreesNow = rotation.value_or(edit_.steeringRotationDeg);
    drawField(degreesX, y, 64, Field::PerGame, degrees(degreesNow), rotationChanged, index);
    button(defaultX, y, "Default", [this, id] { edit_.gameRotations.erase(id); }, false, editable() && ownRotation,
           72);
    from(y, ownRotation, fits && game.rotationDeg ? "profile" : "Mapping tab");
    y += 20;

    // Anti-dead zone (AntiDeadZone.hpp): yours, else the profile's, else the Mapping tab's.
    {
        window_.text(gameX(), y + 4, "Anti-dead zone", kText);
        tip(gameX() - 2, y, 14 * ui::kCharWidth + 4, 16,
            "For a game that ignores the first part of the turn each way: as soon as the wheel leaves the centre, the "
            "game gets this much steering. Set it at or just under the game's dead zone; 0 = off.");
        const bool ownAntiDeadZone = own != nullptr && own->antiDeadZonePercent.has_value();
        const bool antiDeadZoneChanged =
            ownAntiDeadZone != (saved != nullptr && saved->antiDeadZonePercent.has_value()) ||
            (ownAntiDeadZone && saved != nullptr && saved->antiDeadZonePercent != own->antiDeadZonePercent);
        window_.fill(axisX, y, 128, 16, kStripe);
        window_.text(axisX + 6, y + 4, "x, from the centre", kDim);
        drawField(degreesX, y, 64, Field::GameAntiDeadZone, percent(antiDeadZoneWhileRunning(game)),
                  antiDeadZoneChanged, index);
        button(defaultX, y, "Default", [this, id] {
            tuningFor(id).antiDeadZonePercent.reset();
            dropEmptyTuning(id);
        }, false, editable() && ownAntiDeadZone, 72);
        from(y, ownAntiDeadZone, fits && game.antiDeadZonePercent ? "profile" : "Mapping tab");
        y += 20;
    }

    // Pedals: the axis the game reads each one from (the profile's layout over the target's, then yours), and its
    // direction (the Mapping tab's invert, then yours).
    const JoystickLayout device = layoutFor(targetName_, 0);
    const JoystickLayout profileLayout = fits ? overlay(device, game) : device;
    const JoystickLayout shown = own != nullptr ? tunedLayout(profileLayout, *own) : profileLayout;
    const std::array<ObjectGuid, 3> axes = {shown.throttle, shown.brake, shown.clutch};
    const std::array<ObjectGuid, 3> deviceAxes = {device.throttle, device.brake, device.clutch};
    const std::array<ObjectGuid, 3> profileAxes = {profileLayout.throttle, profileLayout.brake, profileLayout.clutch};
    for (std::size_t p = 0; p < 3; ++p) {
        window_.text(gameX(), y + 4, kPedalNames[p], kText);
        const bool ownAxis = own != nullptr && own->pedalAxes[p].has_value();
        const bool axisChanged = ownAxis != (saved != nullptr && saved->pedalAxes[p].has_value()) ||
                                 (ownAxis && saved != nullptr && saved->pedalAxes[p] != own->pedalAxes[p]);
        // < and > go through the axes (the clutch also through none).
        const auto step = [this, id, p, axes](const int by) {
            std::vector<ObjectGuid> choices(kPedalAxes.begin(), kPedalAxes.end());
            if (p == 2) choices.push_back(ObjectGuid::Any);
            const auto at = std::find(choices.begin(), choices.end(), axes[p]);
            const auto i = at == choices.end() ? 0 : static_cast<int>(at - choices.begin());
            const auto n = static_cast<int>(choices.size());
            tuningFor(id).pedalAxes[p] = choices[static_cast<std::size_t>(((i + by) % n + n) % n)];
        };
        button(axisX, y, "<", [step] { step(-1); }, false, editable(), 24);
        window_.fill(axisX + 28, y, 72, 16, kStripe);
        window_.outline(axisX + 28, y, 72, 16, axisChanged ? kMine : style::kFieldEdge);
        window_.text(axisX + 34, y + 4, std::string(wheelbri::axisName(axes[p])), kText);
        button(axisX + 104, y, ">", [step] { step(1); }, false, editable(), 24);
        const bool ownInvert = own != nullptr && own->pedalInvert[p].has_value();
        const bool invert = ownInvert ? *own->pedalInvert[p] : edit_.invert[p + 1];
        button(directionX, y, invert ? "Inverted" : "Normal", [this, id, p, invert] {
            tuningFor(id).pedalInvert[p] = !invert;
        }, invert, editable(), 88);
        button(defaultX, y, "Default", [this, id, p] {
            tuningFor(id).pedalAxes[p].reset();
            tuningFor(id).pedalInvert[p].reset();
            dropEmptyTuning(id);
        }, false, editable() && (ownAxis || ownInvert), 72);
        from(y, ownAxis || ownInvert, fits && profileAxes[p] != deviceAxes[p] ? "profile" : "Mapping tab");
        y += 20;
    }

    // Combined pedals (Logitech's "Report Combined Pedals"): yours, else the profile's, else the Mapping tab's.
    {
        window_.text(gameX(), y + 4, "Combined", kText);
        tip(gameX() - 2, y, 8 * ui::kCharWidth + 4, 16,
            "Throttle and brake on one axis (the throttle's) while this game runs, as Logitech's \"Report Combined "
            "Pedals\": for old games whose menus scroll by themselves with separate pedals.");
        const bool ownCombined = own != nullptr && own->combinedPedals.has_value();
        const bool profileCombined = fits && game.combinedPedals.has_value();
        const bool combined = ownCombined       ? *own->combinedPedals
                              : profileCombined ? *game.combinedPedals
                                                : edit_.combinedPedals;
        const bool combinedChanged =
            ownCombined != (saved != nullptr && saved->combinedPedals.has_value()) ||
            (ownCombined && saved != nullptr && saved->combinedPedals != own->combinedPedals);
        window_.fill(axisX, y, 128, 16, kStripe);
        window_.outline(axisX, y, 128, 16, combinedChanged ? kMine : style::kFieldEdge);
        window_.text(axisX + 6, y + 4, combined ? "one axis" : "separate", kText);
        button(directionX, y, combined ? "On" : "Off", [this, id, combined] {
            tuningFor(id).combinedPedals = !combined;
        }, combined, editable(), 88);
        button(defaultX, y, "Default", [this, id] {
            tuningFor(id).combinedPedals.reset();
            dropEmptyTuning(id);
        }, false, editable() && ownCombined, 72);
        from(y, ownCombined, profileCombined ? "profile" : "Mapping tab");
        y += 20;
    }
    y += 6;
    window_.text(axisX, y, fit("Default: back to where it came from. Violet: yours, for this game.", valueChars() + 6),
                 kDim);
    y += 12;
    if (rotation && std::fabs(*rotation - edit_.steeringRotationDeg) >= 0.5F) {
        window_.text(axisX, y,
                     fit("Set your wheel's own software to " + degrees(*rotation) + " deg while you play this game.",
                         valueChars() + 6),
                     kCaution);
        y += 12;
    }
    // Two pedals on one axis, or (Mode Virtual) an axis the virtual wheel has not: said, and what happens then.
    if (!isValid(shown)) {
        window_.text(axisX, y, fit("Two pedals on one axis: pick another axis for one of them (not used until then).",
                                   valueChars() + 6),
                     kWarning);
        y += 12;
    } else if (picker_.virtualDevice() && !routePedals(device, shown)) {
        const std::string deviceAxesText = std::string(wheelbri::axisName(device.throttle)) + ", " +
                                           std::string(wheelbri::axisName(device.brake)) + " and " +
                                           std::string(wheelbri::axisName(device.clutch));
        for (const std::string& line : wrap("Mode Virtual: the virtual " + target + " has only " + deviceAxesText +
                                                " for pedals, each once; these are used in Mode Shim only.",
                                            valueChars() + 6, 2)) {
            window_.text(axisX, y, line, kCaution);
            y += 12;
        }
    }
    return y;
}

float MappingEditView::drawGameButtons(const std::size_t index, float y) {
    // Only the buttons changed for this game, under the target's own groups (in the Mapping tab's order); Add button...
    // adds one. Each target keeps its own (my-<target>.ini), so this is the shown target's list.
    const GameProfile& game = games_[index];
    const std::string id = game.id;
    const GameTuning* const own = tuningOf(id);
    const std::string target = profile_ != nullptr ? profile_->identity().shortName : targetName_;
    gameSection(y, "BUTTONS", "only the ones you change for this game; the rest: Mapping tab");
    y += 16;
    if (profile_ == nullptr) return y;

    // The changed buttons, group by group; one in no group (or with no groups at all) at the end.
    struct Group {
        std::string name;
        std::vector<std::size_t> buttons;
    };
    std::vector<Group> groups;
    std::vector<bool> shown(profile_->buttonCount(), false);
    // Rows added by Add button... and not changed yet: this game's and this target's only.
    const std::string pendingKey = id + "/" + target;
    if (pendingKey_ != pendingKey) {
        pendingKey_ = pendingKey;
        pendingButtons_.clear();
    }
    const auto changed = [this, own](const std::size_t b) {
        const auto key = static_cast<int>(b);
        return (own != nullptr && (own->buttons.count(key) != 0 || own->secondaries.count(key) != 0)) ||
               std::find(pendingButtons_.begin(), pendingButtons_.end(), b) != pendingButtons_.end();
    };
    for (const ButtonGroup& group : profile_->buttonGroups()) {
        Group mine{group.name, {}};
        for (const std::size_t b : group.buttons) {
            if (b < shown.size() && !shown[b] && changed(b)) {
                mine.buttons.push_back(b);
                shown[b] = true;
            }
        }
        if (!mine.buttons.empty()) groups.push_back(std::move(mine));
    }
    Group rest{profile_->buttonGroups().empty() ? std::string() : std::string("OTHER"), {}};
    for (std::size_t b = 0; b < shown.size(); ++b) {
        if (!shown[b] && changed(b)) rest.buttons.push_back(b);
    }
    if (!rest.buttons.empty()) groups.push_back(std::move(rest));

    const float labelW = 120;
    const float removeW = 72;
    // Three fields: the Mapping tab's (main and its secondaries, shown), this game's main and this game's secondaries.
    const float fieldW = std::floor((canvasW() - gameX() - 4 - labelW - removeW - 24) / 3);
    const float mappingX = gameX() + labelW;
    const float gameColumnX = mappingX + fieldW + 8;
    const float altColumnX = gameColumnX + fieldW + 8;
    const auto chars = static_cast<std::size_t>((fieldW - 12) / ui::kCharWidth);
    if (groups.empty()) {
        for (const std::string& line : wrap("No button changed for this game: every " + target +
                                                " button comes from the Mapping tab.",
                                            gameChars(), 2)) {
            window_.text(gameX(), y + 4, line, kDim);
            y += 12;
        }
        y += 12;
    } else {
        window_.text(gameX(), y, target + " button", kDim);
        window_.text(mappingX, y, "Mapping tab", kDim);
        window_.text(gameColumnX, y, "this game", kDim);
        window_.text(altColumnX, y, "secondaries", kDim);
        y += 14;
    }
    // A source's text and then its secondaries as chips, in a field `w` wide at `x`.
    const auto sourceAndChips = [&](const float x, const float rowY, const float w, const std::string& text,
                                    const ui::Colour colour, const std::vector<ButtonRef>& alts, const bool dim,
                                    const std::function<void()>& action) {
        const float room = w - 12;
        const float chipsW = alts.empty() ? 0.0F
                                          : std::min(chipsWidth(alts),
                                                     std::max(room - textWidth(text) - 8, 5 * ui::kCharWidth));
        const std::string shownText =
            fit(text, static_cast<std::size_t>(std::max(0.0F, room - (chipsW > 0 ? chipsW + 8 : 0.0F)) /
                                               ui::kCharWidth));
        window_.text(x + 6, rowY + 4, shownText, colour);
        drawChips(x + 6 + textWidth(shownText) + (shownText.empty() ? 0 : 8), rowY, chipsW, alts, std::nullopt, dim,
                  action);
    };
    const std::uint64_t now = nowMs();
    const float bottom = contentH() - 52 - 64;   // room for Add button... and the notes under it
    std::size_t hidden = 0;
    for (const Group& group : groups) {
        if (!group.name.empty()) {
            if (y > bottom) {
                hidden += group.buttons.size();
                continue;
            }
            window_.text(gameX(), y + 4, group.name, kDim);
            y += kButtonHeadingH;
        }
        for (const std::size_t b : group.buttons) {
            if (y > bottom) {
                ++hidden;
                continue;
            }
            const auto key = static_cast<int>(b);
            char label[32];
            std::snprintf(label, sizeof(label), "%2d %.10s", gameNumber(b),
                          std::string(profile_->buttonName(b)).c_str());
            window_.text(gameX() + 4, y + 4, label, kText);
            if (b == pendingFlash_ && now < pendingFlashUntilMs_) {
                window_.outline(gameX(), y - 2, altColumnX + fieldW + 8 + removeW - gameX() + 2, 20, kHeading);
            }
            // What the Mapping tab gives it, its secondaries too: shown, not changed here.
            const SourceRef mapped = edit_.buttons[b];
            const std::vector<ButtonRef>& mappedAlts = edit_.secondaries[b];
            window_.fill(mappingX, y, fieldW, 16, kStripe);
            if (mapped.mapped()) tip(mappingX, y, fieldW, 16, describeFull(mapped, false));
            sourceAndChips(mappingX, y, fieldW, describe(mapped, false), kDim, mappedAlts, true, {});
            // This game's main: its own (violet), else the Mapping tab's; click to change it.
            const SourceRef* ownMain = nullptr;
            const std::vector<ButtonRef>* ownAlts = nullptr;
            if (own != nullptr) {
                if (const auto it = own->buttons.find(key); it != own->buttons.end()) ownMain = &it->second;
                if (const auto it = own->secondaries.find(key); it != own->secondaries.end()) ownAlts = &it->second;
            }
            const bool hasMain = ownMain != nullptr;
            const SourceRef source = hasMain ? *ownMain : mapped;
            window_.fill(gameColumnX, y, fieldW, 16, kStripe);
            window_.outline(gameColumnX, y, fieldW, 16, hasMain ? kMine : style::kFieldEdge);
            window_.text(gameColumnX + 6, y + 4, fit(source.mapped() ? describe(source, false) : "nothing", chars),
                         source.mapped() ? kText : kDim);
            tip(gameColumnX, y, fieldW, 16,
                (source.mapped() ? describeFull(source, false) : std::string("Nothing")) +
                    (hasMain ? ". This game's own: click to change it." : ". As the Mapping tab: click to change it "
                                                                          "for this game."));
            if (editable()) hit(gameColumnX, y, fieldW, 16, [this, b] { openPicker(Slot::GameButton, b); });
            // This game's secondaries: its own (violet), else the Mapping tab's; click to change them.
            const bool hasAlts = ownAlts != nullptr;
            const std::vector<ButtonRef>& alts = hasAlts ? *ownAlts : mappedAlts;
            window_.fill(altColumnX, y, fieldW, 16, kStripe);
            window_.outline(altColumnX, y, fieldW, 16, hasAlts ? kMine : style::kFieldEdge);
            const std::function<void()> openAlts = [this, b] { openGameAltPicker(b); };
            if (editable()) hit(altColumnX, y, fieldW, 16, openAlts);
            if (alts.empty()) {
                window_.text(altColumnX + 6, y + 4, fit("(none: click to add)", chars), kDim);
                tip(altColumnX, y, fieldW, 16,
                    std::string(hasAlts ? "None in this game" : "None, as the Mapping tab") +
                        ": click to add controls that activate it too in this game (a button, or a lever past a "
                        "point).");
            } else {
                sourceAndChips(altColumnX, y, fieldW, "", kText, alts, false,
                               editable() ? openAlts : std::function<void()>{});
            }
            button(altColumnX + fieldW + 8, y, "Remove", [this, id, b, key] {
                GameTuning& tuning = tuningFor(id);
                tuning.buttons.erase(key);
                tuning.secondaries.erase(key);
                dropEmptyTuning(id);
                pendingButtons_.erase(std::remove(pendingButtons_.begin(), pendingButtons_.end(), b),
                                      pendingButtons_.end());
            }, false, editable(), removeW);
            tip(altColumnX + fieldW + 8, y, removeW, 16, "Back to the Mapping tab: its main and its secondaries.");
            y += kButtonRowH;
        }
    }
    if (hidden > 0) {
        window_.text(gameX(), y + 4, std::to_string(hidden) + " more not shown: a bigger window shows them.", kCaution);
        y += 16;
    }
    y += 8;
    const std::size_t named = layoutButtons(*profile_).cells.size();
    button(mappingX, y, "Add button...", [this] { mode_ = Mode::AddButton; }, false, editable() && named > 0, 120);
    const std::size_t helpChars = gameChars() - 15;   // from mappingX
    window_.text(mappingX + 132, y + 4,
                 fit("pick a " + target + " button, then click its main or secondaries", helpChars - 16), kDim);
    y += 28;
    window_.text(mappingX, y,
                 fit("Click a game field to change it. Remove: back to the Mapping tab.", helpChars),
                 kDim);
    window_.text(mappingX, y + 12,
                 fit("Grouped as in the Mapping tab. Each target keeps its own list.", helpChars), kDim);
    return y + 24;
}

void MappingEditView::drawAddButton() {
    hit(0, 0, canvasW(), contentH(), [] {});   // clicks outside the popup do nothing
    window_.fill(kPopupX, kPopupY, popupW(), popupH(), style::kBox);
    window_.outline(kPopupX, kPopupY, popupW(), popupH(), kHeading);
    const float left = kPopupX + 12;
    const std::size_t picked = selectedGameIndex();
    if (profile_ == nullptr || picked >= games_.size()) {
        mode_ = Mode::Idle;
        return;
    }
    const GameProfile& game = games_[picked];
    const GameTuning* const own = tuningOf(game.id);
    const std::string target = profile_->identity().shortName;
    window_.text(left, kPopupY + 10, "Add button: which " + target + " button do you want to change in " + game.label +
                                         "?", kHeading);
    button(kPopupX + popupW() - 100, kPopupY + 6, "Cancel", [this] { mode_ = Mode::Idle; }, false, true, 88);
    window_.text(left, kPopupY + 32,
                 "Then click its main or its secondaries to change them. Violet: already yours for this game.", kDim);

    // The target's buttons where the Mapping tab has them: two columns of groups, the shifter's gears below.
    const ButtonLayout layout = layoutButtons(*profile_);
    const float columnW = (popupW() - 24) / 2;
    const float top = kPopupY + 56;
    for (const ButtonCell& cell : layout.cells) {
        const float x = left + static_cast<float>(cell.column) * columnW;
        const float y = top + cell.y;
        if (y > kPopupY + popupH() - 24) break;
        if (cell.heading) {
            window_.text(x, y + 3, cell.text, kDim);
            continue;
        }
        const std::size_t b = cell.button;
        const bool mine = own != nullptr && (own->buttons.count(static_cast<int>(b)) != 0 ||
                                             own->secondaries.count(static_cast<int>(b)) != 0);
        const float w = columnW - 12;
        window_.fill(x, y, w, 16, kPanel);
        if (mine) window_.outline(x, y, w, 16, kMine);
        const std::string text =
            std::to_string(gameNumber(b)) + " " + std::string(profile_->buttonName(b)) + (mine ? "  yours" : "");
        window_.text(x + 4, y + 4, fit(text, static_cast<std::size_t>((w - 8) / ui::kCharWidth)),
                     mine ? kMine : kText);
        tip(x, y, w, 16, "Mapping tab: " + (edit_.buttons[b].mapped() ? describeFull(edit_.buttons[b], false)
                                                                      : std::string("nothing")));
        hit(x, y, w, 16, [this, b, mine] {
            // Its row on the sub-tab (shown while unchanged too), outlined for a moment to show where it is.
            mode_ = Mode::Idle;
            if (!mine && std::find(pendingButtons_.begin(), pendingButtons_.end(), b) == pendingButtons_.end()) {
                pendingButtons_.push_back(b);
            }
            pendingFlash_ = b;
            pendingFlashUntilMs_ = nowMs() + 2000;
        });
    }
}

MappingEditView::GameFilePlace MappingEditView::gameFilePlace(const GameProfile& game) const {
    // The one the user located (app state: game_file.<id>) unless the game was seen writing another, else the one
    // it was seen writing, else the profile's under Documents (Windows only).
    const std::string seen = readAppSetting(stateFile_, "game_file_seen." + game.id);
    if (const std::string located = readAppSetting(stateFile_, "game_file." + game.id); !located.empty()) {
        const std::filesystem::path file = pathFromUtf8(located);
        const bool confirmed = settingsFileFits(file, game.settingsFile) ||
                               (!seen.empty() && sameSettingsFile(file, pathFromUtf8(seen)));
        // One the game never wrote gives way to the one it did (NFS Heat's, located for NFS Unbound by mistake).
        if (confirmed || seen.empty()) return {file.parent_path(), pathToUtf8(file.filename()), true, confirmed};
    }
    const std::filesystem::path documents = documentsFolder();
    if (!seen.empty() && !(!documents.empty() && sameSettingsFile(pathFromUtf8(seen),
                                                                  documents / pathFromUtf8(game.settingsFile)))) {
        const std::filesystem::path file = pathFromUtf8(seen);
        return {file.parent_path(), pathToUtf8(file.filename()), true, true};
    }
    return {documents, game.settingsFile, false, true};
}

void MappingEditView::setGameNow(GameNow now) {
    if (now.id != watchedGame_) {   // a game ended (or the session did), or one started
        if (!watchedGame_.empty()) learnGameFile(watchedGame_, watchedSince_);
        watchedGame_ = now.id;
        // Seen every 2 s: from a little before it was first seen running.
        watchedSince_ = std::filesystem::file_time_type::clock::now() -
                        std::chrono::milliseconds(static_cast<std::int64_t>(now.forMs)) - std::chrono::seconds(5);
    }
    now_ = std::move(now);
}

void MappingEditView::learnGameFile(const std::string& id, const std::filesystem::file_time_type since) {
    const auto game = std::find_if(games_.begin(), games_.end(), [&id](const GameProfile& g) { return g.id == id; });
    if (game == games_.end() || game->settings.empty() || game->settingsFile.empty()) return;
    // Still running (the session stopped, not the game): it writes its file when it ends, not yet.
    if (const auto programs = runningPrograms()) {
        for (const RunningProgram& program : *programs) {
            if (programName(program.name) == game->program) return;
        }
    }
    const std::string located = readAppSetting(stateFile_, "game_file." + id);
    const std::filesystem::path locatedFile = located.empty() ? std::filesystem::path() : pathFromUtf8(located);
    const std::vector<std::filesystem::path> written =
        settingsFilesWrittenBetween(documentsFolder(), game->settingsFile, locatedFile, since,
                                    std::filesystem::file_time_type::clock::now() + std::chrono::seconds(5));
    if (written.size() != 1) {
        WE_LOG_INFO(log(), "game settings: {} ended; {} settings files written while it ran: none taken as its own",
                    game->label, written.size());
        return;
    }
    const std::string file = pathToUtf8(written.front());
    if (readAppSetting(stateFile_, "game_file_seen." + id) == file) return;
    if (!writeAppSetting(stateFile_, "game_file_seen." + id, file)) {
        WE_LOG_WARN(log(), "game settings: could not keep {}'s settings file in {}", game->label,
                    pathToUtf8(stateFile_));
        return;
    }
    fileState_.reset();
    WE_LOG_INFO(log(), "game settings: {} wrote {} while it ran: its settings file", game->label, file);
}

std::vector<GameSettingText> MappingEditView::gameValues(const GameProfile& game) const {
    // As shown (saved or not): the game's own rotation if it has one, else the Mapping tab's.
    const float steeringDeg = rotationWhileRunning(game, edit_.gameRotations, profileFitsTarget(game, targetName_))
                                  .value_or(edit_.steeringRotationDeg);
    return gameSettingTexts(game, edit_.gameSettings, steeringDeg);
}

void MappingEditView::locateGameFile(const std::size_t index) {
    const GameProfile& game = games_[index];
    const std::filesystem::path name = pathFromUtf8(game.settingsFile).filename();
    std::filesystem::path start = documentsFolder();
    if (!start.empty()) {
        // Its folder under Documents when that is there, else Documents.
        std::error_code ec;
        const std::filesystem::path folder = (start / pathFromUtf8(game.settingsFile)).parent_path();
        if (std::filesystem::is_directory(folder, ec)) start = folder;
    }
    const std::optional<std::filesystem::path> picked =
        pickFile("Locate " + game.label + "'s settings file (" + pathToUtf8(name) + ")", start,
                           pathToUtf8(name));
    if (!picked) return;   // cancelled
    // Only a file that holds the game's settings: every one of them found in it, once.
    const std::vector<GameSettingText> values = gameValues(game);
    const GameSettingsFileState state =
        readGameSettingsState(picked->parent_path(), pathToUtf8(picked->filename()), values);
    const bool all = state.status == GameSettingsFileState::Status::Read &&
                     std::all_of(state.entries.begin(), state.entries.end(),
                                 [](const GameSettingsFileState::Entry& e) { return e.inFile.has_value(); });
    if (!all) {
        status_ = "Not " + game.label + "'s settings file: " +
                  (state.status == GameSettingsFileState::Status::Read ? std::string("its settings are not in it")
                                                                       : state.why) + ".";
        statusError_ = true;
        WE_LOG_WARN(log(), "game settings: {}: {} is not its settings file", game.label, pathToUtf8(*picked));
        return;
    }
    if (!writeAppSetting(stateFile_, "game_file." + game.id, pathToUtf8(*picked))) {
        status_ = "Could not remember the file in " + pathToUtf8(stateFile_) + ".";
        statusError_ = true;
        return;
    }
    fileState_.reset();
    saveAnyway_.clear();
    // Its own for sure only if the game was seen writing it (or it is where its profile says): another game's file
    // may have the same name and settings (NFS Heat's for NFS Unbound).
    if (gameFilePlace(game).confirmed) {
        status_ = "Found " + game.label + "'s settings file: Save to file writes the values into it.";
        statusError_ = false;
    } else {
        status_ = "Kept, but " + game.label + " has not been seen writing this file (it may be another game's): play " +
                  game.label + " once while WheelBri runs, or Save to file asks first.";
        statusError_ = true;
    }
    WE_LOG_INFO(log(), "game settings: {}: settings file located: {} ({})", game.label, pathToUtf8(*picked),
                gameFilePlace(game).confirmed ? "its own" : "not seen written by the game yet");
}

void MappingEditView::saveGameFile(const std::size_t index) {
    const GameProfile& game = games_[index];
    // The game reads the file when it starts and writes its own values back when it ends: only with it closed.
    if (const auto programs = runningPrograms()) {
        for (const RunningProgram& program : *programs) {
            if (programName(program.name) == game.program) {
                status_ = game.label + " is running: close it first, then Save to file.";
                statusError_ = true;
                return;
            }
        }
    }
    const GameFilePlace place = gameFilePlace(game);
    const std::string where = pathToUtf8(place.root / pathFromUtf8(place.relative));
    if (!place.confirmed && saveAnyway_ != where) {   // maybe another game's: the second press writes it
        saveAnyway_ = where;
        status_ = game.label + " has not been seen writing this file: it may be another game's (NFS Heat's holds the " +
                  "same settings). Save to file again to write it anyway.";
        statusError_ = true;
        return;
    }
    saveAnyway_.clear();
    const GameSettingsFileResult result = writeGameSettingsFile(place.root, place.relative, gameValues(game));
    fileState_.reset();
    using Outcome = GameSettingsFileResult::Outcome;
    switch (result.outcome) {
        case Outcome::Written: {
            std::string written;
            for (const GameSettingsEdit::Change& change : result.changes) {
                written += (written.empty() ? "" : ", ") + change.name + " " + change.before + " -> " + change.after;
            }
            WE_LOG_INFO(log(), "game settings: {}: written to {}: {} (the original is kept as {})", game.label,
                        pathToUtf8(result.file), written, pathToUtf8(result.backup));
            status_ = "Written to " + game.label + "'s settings file.";
            statusError_ = false;
            break;
        }
        case Outcome::AsSet:
            status_ = game.label + "'s settings file already holds these values.";
            statusError_ = false;
            break;
        case Outcome::NoFile:
            status_ = "No settings file there: start the game once, or Locate it.";
            statusError_ = true;
            break;
        case Outcome::Refused:
        case Outcome::Failed:
            status_ = "Not written: " + result.why + ".";
            statusError_ = true;
            WE_LOG_WARN(log(), "game settings: {}: not written to {}: {}", game.label, pathToUtf8(result.file),
                        result.why);
            break;
    }
}

}  // namespace wheelbri::service
