// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/IniEditor.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <string>

namespace wheelbri {

namespace {

std::string trim(std::string_view text) {
    constexpr std::string_view kWhitespace = " \t\r\n";
    const auto first = text.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(kWhitespace);
    return std::string(text.substr(first, last - first + 1));
}

// "0.0" and "0", "1.0" and "1": the same number written differently counts as unchanged (the line, and its comment,
// stay as the user wrote them).
// Read as everywhere else (from_chars): never by the locale's decimal mark, and "inf" or "nan" are not numbers here.
bool sameValue(const std::string& a, const std::string& b) {
    if (a == b) return true;
    const auto number = [](const std::string& text) -> std::optional<double> {
        double value = 0.0;
        const char* const end = text.data() + text.size();
        const auto [ptr, error] = std::from_chars(text.data(), end, value, std::chars_format::fixed);
        if (error != std::errc() || ptr != end || !std::isfinite(value)) return std::nullopt;
        return value;
    };
    const std::optional<double> x = number(a);
    const std::optional<double> y = number(b);
    return x && y && *x == *y;
}

}  // namespace

IniEditor::Line IniEditor::parseLine(std::string text, const std::string& section) {
    Line line;
    line.section = section;
    std::string_view content = text;
    const auto comment = content.find_first_of(";#");
    if (comment != std::string_view::npos) content = content.substr(0, comment);
    const std::string stripped = trim(content);
    // A header as IniDocument reads one: "[ ]" (no name) is not one, and leaves the line in the section it is in.
    if (stripped.size() >= 3 && stripped.front() == '[' && stripped.back() == ']' &&
        !trim(std::string_view(stripped).substr(1, stripped.size() - 2)).empty()) {
        line.header = true;
        line.section = trim(std::string_view(stripped).substr(1, stripped.size() - 2));
    } else if (const auto equals = stripped.find('='); equals != std::string::npos) {
        line.key = trim(std::string_view(stripped).substr(0, equals));
        line.value = trim(std::string_view(stripped).substr(equals + 1));
    }
    line.text = std::move(text);
    return line;
}

IniEditor::IniEditor(std::string_view text) {
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") {
        bom_ = true;
        text.remove_prefix(3);
    }
    if (text.find("\r\n") != std::string_view::npos) newline_ = "\r\n";
    std::string section;
    std::size_t position = 0;
    while (position < text.size()) {
        auto end = text.find('\n', position);
        if (end == std::string_view::npos) end = text.size();
        std::string raw(text.substr(position, end - position));
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        Line line = parseLine(std::move(raw), section);
        section = line.section;
        lines_.push_back(std::move(line));
        position = end + 1;
    }
}

// The last line with that key: the one the reader (IniDocument) uses when a key is written twice.
std::ptrdiff_t IniEditor::find(const std::string& section, const std::string& key) const {
    for (std::size_t i = lines_.size(); i-- > 0;) {
        if (!lines_[i].header && lines_[i].section == section && lines_[i].key == key) {
            return static_cast<std::ptrdiff_t>(i);
        }
    }
    return -1;
}

void IniEditor::set(const std::string& section, const std::string& key, const std::string& rawValue) {
    // A line break in a value would end the line and start another (a new key, or a [section]).
    std::string value = rawValue;
    for (char& c : value) {
        if (c == '\r' || c == '\n') c = ' ';
    }
    if (auto at = find(section, key); at >= 0) {
        // Earlier copies of the key never counted (the reader takes the last): they go, so the file says it once.
        for (std::ptrdiff_t i = at - 1; i >= 0; --i) {
            const Line& earlier = lines_[static_cast<std::size_t>(i)];
            if (!earlier.header && earlier.section == section && earlier.key == key) {
                lines_.erase(lines_.begin() + i);
                --at;
            }
        }
        Line& line = lines_[static_cast<std::size_t>(at)];
        if (!sameValue(line.value, value)) line = parseLine(key + " = " + value, section);
        return;
    }
    // After the section's last key (or its header); comments and blank lines after it stay below the new key.
    std::ptrdiff_t after = -1;
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        const Line& line = lines_[i];
        if (line.section != section) {
            if (after >= 0) break;   // the section ended
            continue;
        }
        if (line.header || !line.key.empty()) after = static_cast<std::ptrdiff_t>(i);
    }
    if (after < 0) {
        if (!lines_.empty() && !trim(lines_.back().text).empty()) lines_.push_back(parseLine("", lines_.back().section));
        lines_.push_back(parseLine("[" + section + "]", section));
        lines_.push_back(parseLine(key + " = " + value, section));
        return;
    }
    lines_.insert(lines_.begin() + after + 1, parseLine(key + " = " + value, section));
}

// Every copy of the key: one left behind would become the value the reader uses.
void IniEditor::remove(const std::string& section, const std::string& key) {
    for (auto at = find(section, key); at >= 0; at = find(section, key)) lines_.erase(lines_.begin() + at);
}

std::string IniEditor::text() const {
    std::string out = bom_ ? "\xEF\xBB\xBF" : "";
    for (const Line& line : lines_) {
        out += line.text;
        out += newline_;
    }
    return out;
}

}  // namespace wheelbri
