#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wheelemu {

// Minimal INI reader: [section], key = value, ';' or '#' comments. Keys and sections are case-sensitive.
class IniDocument {
public:
    struct ParseResult;

    [[nodiscard]] static ParseResult parse(std::string_view text);

    [[nodiscard]] std::optional<std::string> get(const std::string& section, const std::string& key) const;
    [[nodiscard]] const std::map<std::string, std::map<std::string, std::string>>& sections() const {
        return sections_;
    }

private:
    std::map<std::string, std::map<std::string, std::string>> sections_;
};

struct IniDocument::ParseResult {
    IniDocument document;
    std::vector<std::string> errors;
};

}  // namespace wheelemu
