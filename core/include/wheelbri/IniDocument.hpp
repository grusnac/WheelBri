#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wheelbri {

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

// `text` made safe to write as an INI value: ';' and '#' (which would start a comment), line breaks (which would end
// the line) and other control characters become spaces. For values that come from outside, such as a device's name.
[[nodiscard]] std::string iniSafeValue(std::string_view text);

struct IniDocument::ParseResult {
    IniDocument document;
    std::vector<std::string> errors;
};

}  // namespace wheelbri
