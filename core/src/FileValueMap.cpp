// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/FileValueMap.hpp"

#include <algorithm>
#include <optional>
#include <utility>

namespace wheelbri {

namespace {

constexpr std::size_t kMaxDepth = 256;
constexpr std::string_view kSection = "[game_files] ";

bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool isNameChar(const char c) {
    const auto u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '_' || u == '-' ||
           u == '.' || u == ':' || u >= 0x80;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && isSpace(text.front())) text.remove_prefix(1);
    while (!text.empty() && isSpace(text.back())) text.remove_suffix(1);
    return text;
}

bool validName(const std::string_view name) {
    return !name.empty() && std::all_of(name.begin(), name.end(), isNameChar);
}

// One line of the file: its text from `begin` to `end` (without its line ending), the next line from `next`.
struct Line {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t next = 0;
};

struct Attribute {
    std::string name;
    std::string value;
};

// An element as the file has it: where its start tag and its end tag are (the same for <x/>), its parent's index.
struct Element {
    std::string name;
    std::vector<Attribute> attributes;
    std::size_t startOffset = 0;      // its '<'
    std::size_t startTagEnd = 0;      // just past its start tag's '>'
    std::size_t endTagStart = 0;      // its end tag's '<' (<x/>: its '<')
    std::size_t endOffset = 0;        // just past its end tag's '>' (<x/>: past "/>")
    std::size_t startLine = 0;
    std::size_t startTagEndLine = 0;  // the line of its start tag's '>'
    std::size_t endTagStartLine = 0;
    std::size_t endLine = 0;
    int parent = -1;
    bool selfClosing = false;
};

struct Scan {
    std::vector<Line> lines;
    std::vector<Element> elements;
    std::string eol = "\n";
    std::string error;   // empty: read

    [[nodiscard]] std::size_t lineOf(const std::size_t offset) const {
        const auto it = std::upper_bound(lines.begin(), lines.end(), offset,
                                         [](const std::size_t at, const Line& line) { return at < line.begin; });
        return it == lines.begin() ? 0 : static_cast<std::size_t>(it - lines.begin()) - 1;
    }
};

// The file's lines, and its elements read just well enough to find them: tags, attributes, comments, <? ?>, <! >,
// CDATA. Text between tags is left alone. Not as expected anywhere: `error`.
Scan scan(const std::string_view text) {
    Scan s;
    std::size_t pos = 0;
    while (true) {
        const std::size_t newline = text.find('\n', pos);
        if (newline == std::string_view::npos) {
            if (pos < text.size() || s.lines.empty()) s.lines.push_back({pos, text.size(), text.size()});
            break;
        }
        std::size_t end = newline;
        if (end > pos && text[end - 1] == '\r') --end;
        s.lines.push_back({pos, end, newline + 1});
        pos = newline + 1;
    }
    if (s.lines.front().next > s.lines.front().end) {
        s.eol = std::string(text.substr(s.lines.front().end, s.lines.front().next - s.lines.front().end));
    }

    const auto fail = [&s](const std::size_t at, const std::string& what) {
        s.error = "line " + std::to_string(s.lineOf(at) + 1) + ": " + what;
    };
    const auto skipTo = [&](const std::size_t from, const std::string_view until, std::size_t& i,
                            const std::string& what) {
        const std::size_t e = text.find(until, from);
        if (e == std::string_view::npos) {
            fail(i, what);
            return false;
        }
        i = e + until.size();
        return true;
    };
    std::vector<int> stack;
    std::size_t i = 0;
    while ((i = text.find('<', i)) != std::string_view::npos) {
        const std::string_view rest = text.substr(i);
        if (rest.starts_with("<!--")) {
            if (!skipTo(i + 4, "-->", i, "a comment that does not end")) return s;
            continue;
        }
        if (rest.starts_with("<![CDATA[")) {
            if (!skipTo(i + 9, "]]>", i, "a CDATA section that does not end")) return s;
            continue;
        }
        if (rest.starts_with("<?")) {
            if (!skipTo(i + 2, "?>", i, "a <? that does not end")) return s;
            continue;
        }
        if (rest.starts_with("<!")) {
            if (!skipTo(i + 2, ">", i, "a <! that does not end")) return s;
            continue;
        }
        if (rest.starts_with("</")) {
            std::size_t j = i + 2;
            while (j < text.size() && isNameChar(text[j])) ++j;
            const std::string_view name = text.substr(i + 2, j - i - 2);
            while (j < text.size() && isSpace(text[j])) ++j;
            if (name.empty() || j >= text.size() || text[j] != '>') {
                fail(i, "an end tag that is not as expected");
                return s;
            }
            if (stack.empty() || s.elements[static_cast<std::size_t>(stack.back())].name != name) {
                fail(i, "</" + std::string(name) + "> closes no open element of that name");
                return s;
            }
            Element& open = s.elements[static_cast<std::size_t>(stack.back())];
            open.endTagStart = i;
            open.endOffset = j + 1;
            open.endTagStartLine = s.lineOf(i);
            open.endLine = s.lineOf(j);
            stack.pop_back();
            i = j + 1;
            continue;
        }
        Element element;
        std::size_t j = i + 1;
        while (j < text.size() && isNameChar(text[j])) ++j;
        element.name = std::string(text.substr(i + 1, j - i - 1));
        if (element.name.empty()) {
            fail(i, "a '<' that starts no tag");
            return s;
        }
        while (true) {
            while (j < text.size() && isSpace(text[j])) ++j;
            if (j >= text.size()) {
                fail(i, "<" + element.name + " does not end");
                return s;
            }
            if (text[j] == '>') {
                ++j;
                break;
            }
            if (text[j] == '/') {
                if (j + 1 < text.size() && text[j + 1] == '>') {
                    j += 2;
                    element.selfClosing = true;
                    break;
                }
                fail(j, "<" + element.name + ": a '/' not followed by '>'");
                return s;
            }
            const std::size_t nameStart = j;
            while (j < text.size() && isNameChar(text[j])) ++j;
            Attribute attribute{std::string(text.substr(nameStart, j - nameStart)), {}};
            while (j < text.size() && isSpace(text[j])) ++j;
            if (attribute.name.empty() || j >= text.size() || text[j] != '=') {
                fail(j, "<" + element.name + ": an attribute that is not as expected");
                return s;
            }
            ++j;
            while (j < text.size() && isSpace(text[j])) ++j;
            if (j >= text.size() || (text[j] != '"' && text[j] != '\'')) {
                fail(j, "<" + element.name + ": an attribute value without quotes");
                return s;
            }
            const std::size_t close = text.find(text[j], j + 1);
            if (close == std::string_view::npos) {
                fail(j, "<" + element.name + ": an attribute value that does not end");
                return s;
            }
            attribute.value = std::string(text.substr(j + 1, close - j - 1));
            element.attributes.push_back(std::move(attribute));
            j = close + 1;
        }
        if (stack.size() >= kMaxDepth) {
            fail(i, "elements nested too deep");
            return s;
        }
        element.startOffset = i;
        element.startTagEnd = j;
        element.startLine = s.lineOf(i);
        element.startTagEndLine = s.lineOf(j - 1);
        element.parent = stack.empty() ? -1 : stack.back();
        if (element.selfClosing) {
            element.endTagStart = i;
            element.endOffset = j;
            element.endTagStartLine = element.startLine;
            element.endLine = element.startTagEndLine;
        }
        s.elements.push_back(std::move(element));
        if (!s.elements.back().selfClosing) stack.push_back(static_cast<int>(s.elements.size() - 1));
        i = j;
    }
    if (!stack.empty()) fail(s.elements[static_cast<std::size_t>(stack.back())].startOffset,
                             "<" + s.elements[static_cast<std::size_t>(stack.back())].name + "> is not closed");
    return s;
}

// Element or Element[attr=value].
struct Key {
    std::string name;
    std::string attribute;   // empty: any element of that name
    std::string value;
};

std::optional<Key> parseKey(std::string_view text) {
    text = trim(text);
    const std::size_t open = text.find('[');
    if (open == std::string_view::npos) {
        if (!validName(text)) return std::nullopt;
        return Key{std::string(text), {}, {}};
    }
    if (text.back() != ']') return std::nullopt;
    const std::string_view name = trim(text.substr(0, open));
    const std::string_view inner = text.substr(open + 1, text.size() - open - 2);
    const std::size_t equals = inner.find('=');
    if (equals == std::string_view::npos) return std::nullopt;
    const std::string_view attribute = trim(inner.substr(0, equals));
    const std::string_view value = trim(inner.substr(equals + 1));
    if (!validName(name) || !validName(attribute) || value.empty() ||
        value.find_first_of("[]\"'<>") != std::string_view::npos) {
        return std::nullopt;
    }
    return Key{std::string(name), std::string(attribute), std::string(value)};
}

bool matches(const Element& element, const Key& key) {
    if (element.name != key.name) return false;
    if (key.attribute.empty()) return true;
    for (const Attribute& attribute : element.attributes) {
        if (attribute.name == key.attribute) return attribute.value == key.value;
    }
    return false;
}

std::string_view lineText(const std::string_view text, const Scan& s, const std::size_t line) {
    return text.substr(s.lines[line].begin, s.lines[line].end - s.lines[line].begin);
}

bool onlySpaces(const std::string_view text) { return trim(text).empty(); }

std::string insertName(const std::size_t index) { return "insert." + std::to_string(index + 1); }

// The one element `key` names among those with parent `parent` (-2: anywhere), or -1 and a problem.
int findOne(const Scan& s, const std::string_view keyText, const int parent, const std::string& who,
            std::vector<std::string>& problems) {
    const std::optional<Key> key = parseKey(keyText);
    if (!key) {
        problems.push_back(who + ": '" + std::string(keyText) + "' is not Element or Element[attr=value]");
        return -1;
    }
    int found = -1;
    int count = 0;
    for (std::size_t e = 0; e < s.elements.size(); ++e) {
        if ((parent == -2 || s.elements[e].parent == parent) && matches(s.elements[e], *key)) {
            found = static_cast<int>(e);
            ++count;
        }
    }
    if (count == 0) problems.push_back(who + ": no " + std::string(keyText) + " in the file");
    if (count > 1) problems.push_back(who + ": " + std::to_string(count) + " of " + std::string(keyText) + " in the file");
    return count == 1 ? found : -1;
}

// The lines that are a child of `parent` equal to `value` (spaces collapsed).
std::vector<std::size_t> childLinesEqual(const std::string_view text, const Scan& s, const int parent,
                                         const std::string& value) {
    std::vector<std::size_t> lines;
    for (const Element& element : s.elements) {
        if (element.parent != parent || element.startLine != element.endLine) continue;
        if (collapseSpaces(lineText(text, s, element.startLine)) == value) lines.push_back(element.startLine);
    }
    return lines;
}

// The indentation of `parent`'s first child; with none, the parent's own and one step more: the step from its own
// parent to it, else a tab.
std::string childIndent(const std::string_view text, const Scan& s, const int parent) {
    const auto indentOf = [&](const Element& element) -> std::optional<std::string_view> {
        const std::size_t begin = s.lines[element.startLine].begin;
        const std::string_view before = text.substr(begin, element.startOffset - begin);
        return onlySpaces(before) ? std::optional<std::string_view>(before) : std::nullopt;
    };
    for (const Element& element : s.elements) {
        if (element.parent != parent) continue;
        if (const auto indent = indentOf(element)) return std::string(*indent);
        break;
    }
    const Element& p = s.elements[static_cast<std::size_t>(parent)];
    const std::string_view own = indentOf(p).value_or(std::string_view());
    std::string_view step = "\t";
    if (p.parent >= 0) {
        const auto up = indentOf(s.elements[static_cast<std::size_t>(p.parent)]);
        if (up && own.size() > up->size() && own.starts_with(*up)) step = own.substr(up->size());
    }
    return std::string(own) + std::string(step);
}

bool validRelativeFile(const std::string_view file) {
    if (file.empty() || file.front() == '/' || file.find_first_of(":\\") != std::string_view::npos) return false;
    if (std::any_of(file.begin(), file.end(), [](const char c) { return static_cast<unsigned char>(c) < 0x20; })) {
        return false;
    }
    std::size_t at = 0;
    while (at <= file.size()) {
        const std::size_t slash = file.find('/', at);
        const std::string_view part = file.substr(at, slash == std::string_view::npos ? std::string_view::npos : slash - at);
        if (part.empty() || part == "." || part == "..") return false;
        if (slash == std::string_view::npos) break;
        at = slash + 1;
    }
    return true;
}

// The insert number in "insert.<n>" / "insert.<n>.note": 1..kMaxFileValueLines, no leading 0; 0 when not one.
std::size_t insertNumber(const std::string_view digits) {
    if (digits.empty() || digits.size() > 3 || digits.front() == '0') return 0;
    std::size_t n = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') return 0;
        n = n * 10 + static_cast<std::size_t>(c - '0');
    }
    return n <= kMaxFileValueLines ? n : 0;
}

// The map's lines and their parents, resolved in `s`: -1 for a line whose parent is not found once.
std::vector<int> resolveParents(const Scan& s, const FileValueMap& map, std::vector<std::string>& problems) {
    std::vector<int> parents;
    for (std::size_t i = 0; i < map.lines.size(); ++i) {
        parents.push_back(findOne(s, map.lines[i].parent, -2, insertName(i), problems));
    }
    return parents;
}

struct Prepared {
    Scan scan;
    std::vector<int> parents;
    FileValueState state;
};

Prepared prepare(const std::string_view text, const FileValueMap& map) {
    Prepared p;
    p.state.present.assign(map.lines.size(), false);
    if (text.size() > kMaxFileValueBytes) {
        p.state.problems.push_back("larger than such a file (" + std::to_string(text.size()) + " bytes)");
        return p;
    }
    p.scan = scan(text);
    if (!p.scan.error.empty()) {
        p.state.problems.push_back("not readable as XML: " + p.scan.error);
        return p;
    }
    p.parents = resolveParents(p.scan, map, p.state.problems);
    if (!p.state.problems.empty()) return p;
    for (std::size_t i = 0; i < map.lines.size(); ++i) {
        if (!childLinesEqual(text, p.scan, p.parents[i], collapseSpaces(map.lines[i].value)).empty()) {
            p.state.present[i] = true;
            ++p.state.presentCount;
        }
    }
    return p;
}

FileValueEdit refused(std::vector<std::string> problems) {
    FileValueEdit edit;
    edit.problems = std::move(problems);
    return edit;
}

}  // namespace

std::string collapseSpaces(const std::string_view text) {
    std::string out;
    bool space = false;
    for (const char c : text) {
        if (c == ' ' || c == '\t') {
            space = true;
            continue;
        }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

ParsedFileValueMap parseFileValueMap(const std::map<std::string, std::string>& keys) {
    ParsedFileValueMap result;
    FileValueMap& map = result.map;
    std::vector<std::string>& problems = result.problems;
    struct Raw {
        std::optional<std::string> insert;
        std::optional<std::string> note;
    };
    std::map<std::size_t, Raw> raw;
    for (const auto& [key, value] : keys) {
        if (key == "file") {
            map.file = value;
        } else if (key == "note") {
            map.note = value;
        } else if (key.starts_with("insert.")) {
            std::string_view rest = std::string_view(key).substr(7);
            const bool isNote = rest.ends_with(".note");
            if (isNote) rest.remove_suffix(5);
            const std::size_t n = insertNumber(rest);
            if (n == 0) {
                problems.push_back(std::string(kSection) + key + ": insert.<1.." + std::to_string(kMaxFileValueLines) +
                                   "> or insert.<n>.note");
                continue;
            }
            (isNote ? raw[n].note : raw[n].insert) = value;
        } else {
            problems.push_back(std::string(kSection) + key + ": unknown key");
        }
    }
    if (map.file.empty() || map.note.empty() || raw.empty()) {
        problems.push_back(std::string(kSection) + "file, note and at least one insert.<n> go together");
    }
    if (!map.file.empty() && !validRelativeFile(map.file)) {
        problems.push_back(std::string(kSection) + "file = '" + map.file +
                           "': a path relative to the game's folder, / separated, without . or ..");
    }
    if (map.note.size() > kMaxFileValueSetNote) {
        problems.push_back(std::string(kSection) + "note: at most " + std::to_string(kMaxFileValueSetNote) + " characters");
    }
    std::size_t expected = 1;
    for (const auto& [n, line] : raw) {
        const std::string who = std::string(kSection) + insertName(n - 1);
        if (n != expected) problems.push_back(who + ": the inserts are numbered 1, 2, 3 ... without gaps");
        expected = n + 1;
        if (!line.insert || !line.note || line.note->empty()) {
            problems.push_back(who + " and " + insertName(n - 1) + ".note go together: the line and what it does");
            continue;
        }
        if (line.note->size() > kMaxFileValueNote) {
            problems.push_back(who + ".note: at most " + std::to_string(kMaxFileValueNote) + " characters");
        }
        const std::string& text = *line.insert;
        const std::size_t bar1 = text.find('|');
        const std::size_t bar2 = bar1 == std::string::npos ? std::string::npos : text.find('|', bar1 + 1);
        if (bar2 == std::string::npos) {
            problems.push_back(who + ": expected <parent key> | <position> | <value>");
            continue;
        }
        FileValueLine out;
        out.parent = std::string(trim(std::string_view(text).substr(0, bar1)));
        const std::string_view position = trim(std::string_view(text).substr(bar1 + 1, bar2 - bar1 - 1));
        out.value = std::string(trim(std::string_view(text).substr(bar2 + 1)));
        out.note = *line.note;
        if (!parseKey(out.parent)) {
            problems.push_back(who + ": parent '" + out.parent + "' is not Element or Element[attr=value]");
        }
        if (position == "start") {
            out.position = FileValueLine::Position::Start;
        } else if (position == "end") {
            out.position = FileValueLine::Position::End;
        } else if (position.starts_with("after:") && parseKey(position.substr(6))) {
            out.position = FileValueLine::Position::After;
            out.after = std::string(trim(position.substr(6)));
        } else {
            problems.push_back(who + ": position '" + std::string(position) +
                               "': start, end or after:<Element or Element[attr=value]>");
        }
        // One element on one line, nothing else: no comment, no <? ?>, no second element after it.
        bool oneElement = !out.value.empty() && out.value.find_first_of("\r\n") == std::string::npos &&
                          out.value.find("<!") == std::string::npos && out.value.find("<?") == std::string::npos;
        if (oneElement) {
            const Scan s = scan(out.value);
            oneElement = s.error.empty() && !s.elements.empty() && s.elements.front().startOffset == 0 &&
                         s.elements.front().endOffset == out.value.size();
        }
        if (!oneElement) problems.push_back(who + ": the value must be one element on one line (<Axis ... />)");
        for (const FileValueLine& before : map.lines) {
            if (before.parent == out.parent && collapseSpaces(before.value) == collapseSpaces(out.value)) {
                problems.push_back(who + ": the same line twice in one parent");
            }
        }
        map.lines.push_back(std::move(out));
    }
    return result;
}

FileValueState fileValueState(const std::string_view text, const FileValueMap& map) {
    FileValueState state = prepare(text, map).state;
    if (!state.problems.empty()) {
        state.present.assign(map.lines.size(), false);
        state.presentCount = 0;
    }
    return state;
}

FileValueEdit addFileValues(const std::string_view text, const FileValueMap& map) {
    const Prepared p = prepare(text, map);
    if (!p.state.problems.empty()) return refused(p.state.problems);
    FileValueEdit edit;
    if (p.state.all()) {
        edit.text = std::string(text);
        return edit;
    }
    const Scan& s = p.scan;
    struct Insert {
        std::size_t beforeLine;
        std::string line;
    };
    std::vector<Insert> inserts;
    std::vector<std::string> problems;
    for (std::size_t i = 0; i < map.lines.size(); ++i) {
        if (p.state.present[i]) continue;
        const FileValueLine& line = map.lines[i];
        const int parent = p.parents[i];
        const Element& pe = s.elements[static_cast<std::size_t>(parent)];
        const std::string who = insertName(i);
        if (pe.selfClosing || pe.startTagEndLine == pe.endTagStartLine) {
            problems.push_back(who + ": " + line.parent + " has no line between its start and its end");
            continue;
        }
        std::size_t before = 0;
        switch (line.position) {
            case FileValueLine::Position::Start:
                if (!onlySpaces(text.substr(pe.startTagEnd, s.lines[pe.startTagEndLine].end - pe.startTagEnd))) {
                    problems.push_back(who + ": something after " + line.parent + "'s start on its line");
                    continue;
                }
                before = pe.startTagEndLine + 1;
                break;
            case FileValueLine::Position::End:
                if (!onlySpaces(text.substr(s.lines[pe.endTagStartLine].begin,
                                            pe.endTagStart - s.lines[pe.endTagStartLine].begin))) {
                    problems.push_back(who + ": something before " + line.parent + "'s end on its line");
                    continue;
                }
                before = pe.endTagStartLine;
                break;
            case FileValueLine::Position::After: {
                const int child = findOne(s, line.after, parent, who + " (after)", problems);
                if (child < 0) continue;
                const Element& ce = s.elements[static_cast<std::size_t>(child)];
                if (!onlySpaces(text.substr(ce.endOffset, s.lines[ce.endLine].end - ce.endOffset))) {
                    problems.push_back(who + ": something after " + line.after + " on its line");
                    continue;
                }
                before = ce.endLine + 1;
                break;
            }
        }
        inserts.push_back({before, childIndent(text, s, parent) + std::string(trim(line.value)) + s.eol});
    }
    if (!problems.empty()) return refused(std::move(problems));
    std::stable_sort(inserts.begin(), inserts.end(),
                     [](const Insert& a, const Insert& b) { return a.beforeLine < b.beforeLine; });
    std::string out;
    out.reserve(text.size() + inserts.size() * 128);
    std::size_t k = 0;
    for (std::size_t l = 0; l <= s.lines.size(); ++l) {
        while (k < inserts.size() && inserts[k].beforeLine == l) out += inserts[k++].line;
        if (l == s.lines.size()) break;
        out += text.substr(s.lines[l].begin, s.lines[l].next - s.lines[l].begin);
        // The last line without a line ending, and lines to go after it: it gets one.
        if (l + 1 == s.lines.size() && s.lines[l].next == s.lines[l].end && k < inserts.size()) out += s.eol;
    }
    const FileValueState after = fileValueState(out, map);
    if (!after.all()) return refused({"the file would not hold every line afterwards: nothing changed"});
    edit.text = std::move(out);
    edit.changed = inserts.size();
    return edit;
}

FileValueEdit removeFileValues(const std::string_view text, const FileValueMap& map) {
    const Prepared p = prepare(text, map);
    if (!p.state.problems.empty()) return refused(p.state.problems);
    FileValueEdit edit;
    const Scan& s = p.scan;
    std::vector<bool> drop(s.lines.size(), false);
    for (std::size_t i = 0; i < map.lines.size(); ++i) {
        for (const std::size_t line : childLinesEqual(text, s, p.parents[i], collapseSpaces(map.lines[i].value))) {
            if (!drop[line]) ++edit.changed;
            drop[line] = true;
        }
    }
    if (edit.changed == 0) {
        edit.text = std::string(text);
        return edit;
    }
    std::string out;
    out.reserve(text.size());
    for (std::size_t l = 0; l < s.lines.size(); ++l) {
        if (!drop[l]) out += text.substr(s.lines[l].begin, s.lines[l].next - s.lines[l].begin);
    }
    const FileValueState after = fileValueState(out, map);
    if (!after.none()) return refused({"the file would still hold some of the lines afterwards: nothing changed"});
    edit.text = std::move(out);
    return edit;
}

}  // namespace wheelbri
