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
    const std::optional<float> wheel = degrees(std::string_view(value).substr(0, slash));
    const std::optional<float> game =
        slash == std::string::npos ? wheel : degrees(std::string_view(value).substr(slash + 1));
    if (!wheel || !game) return std::nullopt;
    return GameRotationOverride{false, {*wheel, *game}};
}

std::string formatGameRotation(const GameRotationOverride& value) {
    if (value.off) return "off";
    const std::string wheel = tuning::formatNumber(value.rotation.wheelDeg);
    const std::string game = tuning::formatNumber(value.rotation.gameDeg);
    return wheel == game ? wheel : wheel + "/" + game;
}

std::string describeRotation(const SteeringRotation& rotation) {
    const std::string wheel = tuning::formatNumber(rotation.wheelDeg);
    const std::string game = tuning::formatNumber(rotation.gameDeg);
    return wheel == game ? wheel + " deg" : "wheel " + wheel + " / game " + game + " deg";
}

std::optional<SteeringRotation> rotationWhileRunning(const GameProfile& game, const GameRotations& overrides) {
    if (const auto it = overrides.find(game.id); it != overrides.end()) {
        if (it->second.off) return std::nullopt;
        return it->second.rotation;
    }
    if (!game.rotationDeg) return std::nullopt;
    return SteeringRotation{*game.rotationDeg, *game.rotationDeg};
}

}  // namespace wheelbri
