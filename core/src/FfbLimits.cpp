#include "wheelbri/FfbLimits.hpp"

#include "wheelbri/IniDocument.hpp"
#include "wheelbri/IniEditor.hpp"

#include <cmath>
#include <cstdlib>

namespace wheelbri {

namespace {

bool near(const float a, const float b) { return std::fabs(a - b) < 1e-4F; }

// The [ffb] value as a number; nullopt when it is missing or not one.
std::optional<float> numberOf(const IniDocument& ini, const std::string& key) {
    const std::optional<std::string> text = ini.get("ffb", key);
    if (!text) return std::nullopt;
    char* end = nullptr;
    const float value = std::strtof(text->c_str(), &end);
    if (end == text->c_str() || *end != '\0' || !std::isfinite(value)) return std::nullopt;
    return value;
}

}  // namespace

bool hasOldLimits(const FfbSafetyConfig& safety) {
    return near(safety.masterGain, kOldMasterGain) && near(safety.maxMagnitude, kOldMaxMagnitude);
}

std::string withWheelAppLimits(const std::string_view iniText) {
    IniEditor ini(iniText);
    ini.set("ffb", "master_gain", "1.0");
    ini.set("ffb", "max_magnitude", "1.0");
    return ini.text();
}

std::optional<std::string> movedFromOldLimits(const std::string_view iniText) {
    const auto parsed = IniDocument::parse(iniText);
    const std::optional<float> gain = numberOf(parsed.document, "master_gain");
    const std::optional<float> cap = numberOf(parsed.document, "max_magnitude");
    if (!gain || !cap || !near(*gain, kOldMasterGain) || !near(*cap, kOldMaxMagnitude)) return std::nullopt;
    IniEditor ini(withWheelAppLimits(iniText));
    // A game's strength or cap that only repeats the older limits goes too: the game plays at 1 / 1 like the others.
    const auto& sections = parsed.document.sections();
    if (const auto tuning = sections.find("game_tuning"); tuning != sections.end()) {
        for (const auto& [key, value] : tuning->second) {
            const bool strength = key.ends_with(".ffb_strength");
            const bool capKey = key.ends_with(".ffb_cap");
            if (!strength && !capKey) continue;
            char* end = nullptr;
            const float number = std::strtof(value.c_str(), &end);
            if (end == value.c_str() || *end != '\0') continue;
            if ((strength && near(number, kOldMasterGain)) || (capKey && near(number, kOldMaxMagnitude))) {
                ini.remove("game_tuning", key);
            }
        }
    }
    return ini.text();
}

}  // namespace wheelbri
