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
//   set.1 = <element key> | <attribute> | <from> | <to>
//   set.1.note = what this change does in the game (required)
// A set line changes one attribute of one element the file already has (the element key matches exactly one): `from`
// is what the game ships, `to` what WheelBri puts there; taking it out puts `from` back. The attribute holding neither
// is a problem (nothing is changed). Race Driver: GRID's forcefeedback/devicesetup.xml: the RGT's baseFriction.
// A profile may have several sets ([game_files], [game_files.<name>], GameProfiles.hpp), each with `target = <target
// profile>` when it is for one target only.
//
// Pure: no file I/O (GameFiles.hpp reads and writes the game's file). Nothing here trusts the file: anything not as
// expected is a problem, and then nothing is changed at all.
namespace wheelbri {

inline constexpr std::size_t kMaxFileValueLines = 32;
inline constexpr std::size_t kMaxFileValueNote = 300;        // a line's note: a few sentences
inline constexpr std::size_t kMaxFileValueSetNote = 160;     // the whole set's note: one line of the window
inline constexpr std::size_t kMaxFileValueBytes = 4 * 1024 * 1024;   // GRID's preset files are about 10 KB

inline constexpr std::size_t kMaxFileValueAttribute = 64;   // a set line's from / to

struct FileValueLine {
    enum class Kind { Insert, Set };
    enum class Position { Start, End, After };
    Kind kind = Kind::Insert;
    std::string parent;             // Action[actionName=Replay Pause]; for a set line, the element it changes
    Position position = Position::End;
    std::string after;              // the child key, for Position::After
    std::string value;              // <Axis axisName = "..." ... />
    std::string attribute;          // a set line's: baseFriction
    std::string from;               // a set line's value as the game ships it: -25.0
    std::string to;                 // and as WheelBri puts it: 0.0
    std::string note;               // what it does in the game
    friend bool operator==(const FileValueLine&, const FileValueLine&) = default;
};

struct FileValueMap {
    std::string file;   // relative to the game's folder, / separated
    std::string note;
    std::string target;   // the target profile the set is for ("tm_rgt"); empty: every target
    std::vector<FileValueLine> lines;   // insert.N in the order of N, then set.N in the order of N
    [[nodiscard]] bool empty() const { return file.empty() && note.empty() && lines.empty(); }
    friend bool operator==(const FileValueMap&, const FileValueMap&) = default;
};

// "insert.3" or "set.1": the line's key in its section.
[[nodiscard]] std::string fileValueLineName(const FileValueMap& map, std::size_t line);
// How the Games tab shows a line: an insert's value; a set line's "<element key> <attribute> <from> -> <to>".
[[nodiscard]] std::string shownFileValueLine(const FileValueLine& line);

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
// of its parent (remove; nothing else). A set line: its attribute changed to `to` (add) or back to `from`
// (remove). Nothing to do: `text` as it was, `changed` 0. Any problem: `text` empty,
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
