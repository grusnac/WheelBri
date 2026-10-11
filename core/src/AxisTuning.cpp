#include "wheelbri/AxisTuning.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>

namespace wheelbri::tuning {

namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

}  // namespace

std::optional<float> parseNumber(std::string_view text) {
    text = trim(text);
    if (text.size() >= 3 && text.substr(text.size() - 3) == "deg") {
        text = trim(text.substr(0, text.size() - 3));
    } else if (!text.empty() && text.back() == '%') {
        text = trim(text.substr(0, text.size() - 1));
    }
    if (text.empty()) return std::nullopt;
    // Digits, one '.', an optional leading '-': nothing from_chars would also take (exponents, "inf", "nan").
    bool point = false;
    bool digit = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c >= '0' && c <= '9') {
            digit = true;
        } else if (c == '.' && !point) {
            point = true;
        } else if (!(c == '-' && i == 0)) {
            return std::nullopt;
        }
    }
    if (!digit) return std::nullopt;
    float value = 0.0F;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

std::string formatNumber(const float value, const int decimals) {
    char text[48];
    std::snprintf(text, sizeof(text), "%.*f", decimals, static_cast<double>(value));
    std::string out(text);
    if (out.find('.') != std::string::npos) {
        while (out.back() == '0') out.pop_back();
        if (out.back() == '.') out.pop_back();
    }
    if (out == "-0") out = "0";
    return out;
}

std::string formatPercent(const float fraction) { return formatNumber(fraction * 100.0F, 2) + "%"; }

std::string rotationProblem(const float degrees) {
    if (!std::isfinite(degrees) || degrees < kMinRotationDeg || degrees > kMaxRotationDeg) {
        return "a rotation is " + formatNumber(kMinRotationDeg) + " to " + formatNumber(kMaxRotationDeg) + " degrees";
    }
    return {};
}

std::string steeringDeadzoneProblem(const float deadzone) {
    if (!std::isfinite(deadzone) || deadzone < 0.0F || deadzone > kMaxSteeringDeadzone) {
        return "the steering's dead zone is 0 to " + formatPercent(kMaxSteeringDeadzone);
    }
    return {};
}

std::string pedalProblem(const float deadzone, const float full) {
    if (!std::isfinite(deadzone) || deadzone < 0.0F || deadzone > kMaxPedalDeadzone) {
        return "a pedal's dead zone is 0 to " + formatPercent(kMaxPedalDeadzone);
    }
    if (!std::isfinite(full) || full < kMinPedalFull || full > 1.0F) {
        return "a pedal's full point is " + formatPercent(kMinPedalFull) + " to 100%";
    }
    if (full <= deadzone) return "a pedal's full point must be above its dead zone";
    return {};
}

}  // namespace wheelbri::tuning
