#include "wheelbri/GameRotation.hpp"

#include "wheelbri/AxisTuning.hpp"

#include <cctype>

namespace wheelbri {

namespace {

std::string lowerTrimmed(const std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c != ' ' && c != '\t') out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::optional<float> degrees(const std::string_view text) {
    const std::optional<float> value = tuning::parseNumber(text);
    if (!value || !tuning::rotationProblem(*value).empty()) return std::nullopt;
    return value;
}

}  // namespace

std::optional<GameRotationOverride> parseGameRotation(const std::string_view text) {
    const std::string value = lowerTrimmed(text);
    if (value == "off") return GameRotationOverride{true, {}};
    const std::size_t slash = value.find('/');
    if (slash != std::string::npos && !degrees(std::string_view(value).substr(0, slash))) return std::nullopt;
    // "360/180" (older files): the wheel's rotation, then the game's. The game's is the one: the wheel is set to it.
    const std::optional<float> deg =
        degrees(slash == std::string::npos ? std::string_view(value) : std::string_view(value).substr(slash + 1));
    if (!deg) return std::nullopt;
    return GameRotationOverride{false, *deg};
}

std::string formatGameRotation(const GameRotationOverride& value) {
    return value.off ? std::string("off") : tuning::formatNumber(value.deg);
}

std::string describeRotation(const float deg) { return tuning::formatNumber(deg) + " deg"; }

std::optional<float> rotationWhileRunning(const GameProfile& game, const GameRotations& overrides,
                                         const bool profileApplies) {
    if (const auto it = overrides.find(game.id); it != overrides.end()) {
        if (it->second.off) return std::nullopt;
        return it->second.deg;
    }
    return profileApplies ? game.rotationDeg : std::nullopt;
}

}  // namespace wheelbri
