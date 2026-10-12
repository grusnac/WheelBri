#include "wheelbri/IniDocument.hpp"

namespace wheelbri {

namespace {

std::string_view trim(std::string_view text) {
    constexpr std::string_view kWhitespace = " \t\r\n";
    const auto first = text.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(kWhitespace);
    return text.substr(first, last - first + 1);
}

std::string_view stripComment(std::string_view line) {
    const auto comment = line.find_first_of(";#");
    return comment == std::string_view::npos ? line : line.substr(0, comment);
}

}  // namespace

IniDocument::ParseResult IniDocument::parse(const std::string_view text) {
    ParseResult result;
    std::string section;
    std::size_t lineNumber = 0;
    std::size_t position = 0;
    // Notepad may save the file with a UTF-8 byte order mark: not part of the first line.
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") {
        position = 3;
    }

    while (position <= text.size()) {
        const auto end = text.find('\n', position);
        const std::string_view rawLine =
            text.substr(position, end == std::string_view::npos ? std::string_view::npos : end - position);
        position = end == std::string_view::npos ? text.size() + 1 : end + 1;
        ++lineNumber;

        const std::string_view line = trim(stripComment(rawLine));
        if (line.empty()) {
            continue;
        }

        if (line.front() == '[') {
            if (line.back() != ']' || line.size() < 3) {
                result.errors.push_back("line " + std::to_string(lineNumber) + ": malformed section header");
                continue;
            }
            section = std::string(trim(line.substr(1, line.size() - 2)));
            result.document.sections_[section];
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string_view::npos) {
            result.errors.push_back("line " + std::to_string(lineNumber) + ": expected key = value");
            continue;
        }
        const std::string key(trim(line.substr(0, equals)));
        if (key.empty()) {
            result.errors.push_back("line " + std::to_string(lineNumber) + ": empty key");
            continue;
        }
        result.document.sections_[section][key] = std::string(trim(line.substr(equals + 1)));
    }
    return result;
}

std::optional<std::string> IniDocument::get(const std::string& section, const std::string& key) const {
    const auto sectionIt = sections_.find(section);
    if (sectionIt == sections_.end()) {
        return std::nullopt;
    }
    const auto keyIt = sectionIt->second.find(key);
    if (keyIt == sectionIt->second.end()) {
        return std::nullopt;
    }
    return keyIt->second;
}

std::string iniSafeValue(const std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c == ';' || c == '#' || (static_cast<unsigned char>(c) < 0x20 && c != '\t') || c == 0x7F) c = ' ';
    }
    return out;
}

}  // namespace wheelbri
