#include "Settings.hpp"

#include "LogFolder.hpp"

#include "wheelbri/FileText.hpp"
#include "wheelbri/IniDocument.hpp"
#include "wheelbri/LogConfig.hpp"

#include <filesystem>
#include <mutex>

namespace wheelbri::logi {

namespace {

Settings read() {
    Settings result;
    const std::filesystem::path file = std::filesystem::path(gamelog::dllDirectory()) / L"wheelbri-logi.ini";
    const std::optional<std::string> text = readFileText(file);
    if (!text) return result;
    IniDocument::ParseResult ini = IniDocument::parse(*text);
    for (std::string& error : ini.errors) result.problems.push_back("wheelbri-logi.ini: " + std::move(error));
    for (const auto& [section, keys] : ini.document.sections()) {
        if (section != "logi") {
            result.problems.push_back("wheelbri-logi.ini: [" + section + "]: unknown section (only [logi])");
            continue;
        }
        for (const auto& [key, value] : keys) {
            if (key == "log_level") {
                if (logging::isValidLevel(value)) {
                    result.logLevel = value;
                } else {
                    result.problems.push_back("wheelbri-logi.ini: log_level = '" + value +
                                              "': expected trace, debug, info, warn, error, critical or off");
                }
            } else if (key == "real_library") {
                result.realLibrary = std::filesystem::path(std::u8string(value.begin(), value.end())).wstring();
            } else {
                result.problems.push_back("wheelbri-logi.ini: [logi] " + key + ": unknown key");
            }
        }
    }
    return result;
}

}  // namespace

const Settings& settings() {
    static std::once_flag once;
    static Settings result;
    std::call_once(once, [] { result = read(); });
    return result;
}

}  // namespace wheelbri::logi
