#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace wheelemu {

// Changes keys in an INI text and keeps everything else as it was: comments, blank lines, the order of sections and
// keys, the line ends (CRLF stays CRLF). For saving what the Wheel setup tab edited into a config the user may also
// edit by hand. Same syntax as IniDocument: [section], key = value, ';' or '#' start a comment anywhere on a line.
class IniEditor {
public:
    explicit IniEditor(std::string_view text);

    // Sets `key` in `section` to `value`. An existing line keeps its comment when the value is already `value`;
    // otherwise it becomes "key = value" (its old comment may no longer be true). A new key goes after the last
    // key of its section; a new section at the end of the text.
    void set(const std::string& section, const std::string& key, const std::string& value);
    // Removes the key's line, if there is one.
    void remove(const std::string& section, const std::string& key);
    // Whether the text has this section (even an empty one).
    [[nodiscard]] bool hasSection(const std::string& section) const;
    // Puts `line` (a comment) first, unless the text already starts with it.
    void prepend(const std::string& line);

    [[nodiscard]] std::string text() const;

private:
    struct Line {
        std::string text;
        std::string section;      // the section it is in ("" before the first header)
        bool header = false;      // a [section] line
        std::string key;          // empty unless a key = value line
        std::string value;        // the value, comment and spaces taken off
    };

    [[nodiscard]] static Line parseLine(std::string text, const std::string& section);
    [[nodiscard]] std::ptrdiff_t find(const std::string& section, const std::string& key) const;

    std::vector<Line> lines_;
    std::string newline_ = "\n";
    bool bom_ = false;
};

}  // namespace wheelemu
