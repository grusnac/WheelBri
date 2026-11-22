// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// A value map: lines WheelBri puts into a game's own XML file and takes out again, all at once, when the user presses
// one button in the Games tab (Race Driver: GRID's ActionMap/actionMapPreset1.xml gets its replay controls on the
// wheel). A game profile's [game_files] section (GameProfiles.hpp):
//   [game_files]
//   file = ActionMap/actionMapPreset1.xml          ; relative to the game's folder, / separated
//   note = what the whole set does
//   insert.1 = <parent key> | <position> | <value>
//   insert.1.note = what this line does in the game (required)
// parent key: Element or Element[attr=value] (Action[actionName=Replay Pause]); it must match exactly one element of
// the file. position: start, end, or after:<child key> (a child of the parent already in the file, not one the map
// adds). value: one element on one line, written as the file writes it. Line based: an inserted line takes the
// file's line ending (that of its first line) and the indentation of the parent's first child.
//
// Pure: no file I/O (GameFiles.hpp reads and writes the game's file). Nothing here trusts the file: anything not as
// expected is a problem, and then nothing is changed at all.
namespace wheelbri {

inline constexpr std::size_t kMaxFileValueLines = 32;
inline constexpr std::size_t kMaxFileValueNote = 300;        // a line's note: a few sentences
inline constexpr std::size_t kMaxFileValueSetNote = 160;     // the whole set's note: one line of the window
inline constexpr std::size_t kMaxFileValueBytes = 4 * 1024 * 1024;   // GRID's preset files are about 10 KB

struct FileValueLine {
    enum class Position { Start, End, After };
    std::string parent;             // Action[actionName=Replay Pause]
    Position position = Position::End;
    std::string after;              // the child key, for Position::After
    std::string value;              // <Axis axisName = "..." ... />
    std::string note;               // what it does in the game
    friend bool operator==(const FileValueLine&, const FileValueLine&) = default;
};

struct FileValueMap {
    std::string file;   // relative to the game's folder, / separated
    std::string note;
    std::vector<FileValueLine> lines;   // insert.N in the order of N
    [[nodiscard]] bool empty() const { return file.empty() && note.empty() && lines.empty(); }
    friend bool operator==(const FileValueMap&, const FileValueMap&) = default;
};

// The section's keys (IniDocument's [game_files]) read into a map: `problems` (each starting "[game_files]") says what
// is wrong; with any, the map is not to be used.
struct ParsedFileValueMap {
    FileValueMap map;
    std::vector<std::string> problems;
};
[[nodiscard]] ParsedFileValueMap parseFileValueMap(const std::map<std::string, std::string>& keys);

// Whether each line of `map` is in `text`: a child line of its parent equal to the value, runs of spaces and tabs
// taken as one. With problems (not readable XML, a parent key matching no element or more than one), `present` is all
// false and the file is not to be changed.
struct FileValueState {
    std::vector<bool> present;   // one per map line
    std::size_t presentCount = 0;
    std::vector<std::string> problems;
    [[nodiscard]] bool all() const { return problems.empty() && presentCount == present.size(); }
    [[nodiscard]] bool none() const { return problems.empty() && presentCount == 0; }
};
[[nodiscard]] FileValueState fileValueState(std::string_view text, const FileValueMap& map);

// `text` with every line of `map` not yet there put in (add), or with every line equal to one of the map's taken out
// of its parent (remove; nothing else). Nothing to do: `text` as it was, `changed` 0. Any problem: `text` empty,
// `changed` 0, `problems` says why.
struct FileValueEdit {
    std::string text;
    std::size_t changed = 0;   // lines put in or taken out
    std::vector<std::string> problems;
};
[[nodiscard]] FileValueEdit addFileValues(std::string_view text, const FileValueMap& map);
[[nodiscard]] FileValueEdit removeFileValues(std::string_view text, const FileValueMap& map);

// "Replay   Pause \t x" -> "Replay Pause x": runs of spaces and tabs as one space, none at either end.
[[nodiscard]] std::string collapseSpaces(std::string_view text);

}  // namespace wheelbri
