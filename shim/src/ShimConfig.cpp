#include "ShimConfig.hpp"

#include "DiFormat.hpp"
#include "ShimLogging.hpp"

#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/IniDocument.hpp"

#include <charconv>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

namespace wheelbri::shim {

namespace {

std::optional<std::uint32_t> parseHex(const std::string& text) {
    std::uint32_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (ec != std::errc{} || end != text.data() + text.size() || value > 0xFFFF) {
        return std::nullopt;
    }
    return value;
}

ShimConfig load(const logging::Logger& log) {
    ShimConfig config;
    config.statePort = kDefaultStatePort;

    std::wstring path = dllDirectory() + L"wheelbri-shim.ini";
    std::ifstream file(path.c_str());
    if (!file) {
        // The name before the project's rename (wheelemu): a game folder set up then keeps its settings.
        const std::wstring old = dllDirectory() + L"wheelemu-shim.ini";
        file.open(old.c_str());
        if (file) path = old;
    }
    if (!file) {
        WE_LOG_INFO(log, "no '{}', using defaults", toUtf8(path.c_str()));
        return config;
    }
    std::stringstream text;
    text << file.rdbuf();
    const auto parsed = IniDocument::parse(text.str());
    std::vector<std::string> problems = parsed.errors;

    const auto value = [&](const char* key) { return parsed.document.get("shim", key); };
    const auto readBool = [&](const char* key, bool& out) {
        if (const auto v = value(key)) {
            if (*v == "true" || *v == "1" || *v == "yes") {
                out = true;
            } else if (*v == "false" || *v == "0" || *v == "no") {
                out = false;
            } else {
                problems.push_back(std::string(key) + " = '" + *v + "': expected true or false");
            }
        }
    };
    readBool("emulate", config.emulate);
    if (const auto v = value("force_feedback")) {
        if (*v == "service" || *v == "true") {
            config.forceFeedback = FfbMode::Service;
        } else if (*v == "passthrough") {
            config.forceFeedback = FfbMode::Passthrough;
        } else if (*v == "off" || *v == "false") {
            config.forceFeedback = FfbMode::Off;
        } else {
            problems.push_back("force_feedback = '" + *v + "': expected service, passthrough or off");
        }
    }
    if (const auto v = value("ffb_port")) {
        int port = 0;
        const auto [end, ec] = std::from_chars(v->data(), v->data() + v->size(), port);
        if (ec == std::errc{} && end == v->data() + v->size() && port >= 1024 && port <= 65535) {
            config.ffbPort = static_cast<std::uint16_t>(port);
        } else {
            problems.push_back("ffb_port = '" + *v + "': expected 1024..65535");
        }
    }
    readBool("invert_pedals", config.invertPedals);
    const auto readAxis = [&](const char* key, std::optional<ObjectGuid>& out) {
        if (const auto v = value(key)) {
            if (const auto axis = parseAxisName(*v)) {
                out = *axis;
            } else {
                problems.push_back(std::string(key) + " = '" + *v + "': expected x, y, z, rx, ry, rz, slider or none");
            }
        }
    };
    readAxis("throttle_axis", config.throttleAxis);
    readAxis("brake_axis", config.brakeAxis);
    readAxis("clutch_axis", config.clutchAxis);
    if (const auto v = value("source_vid")) {
        if (const auto vid = parseHex(*v)) {
            config.sourceVendorId = static_cast<std::uint16_t>(*vid);
        } else {
            problems.push_back("source_vid = '" + *v + "': expected 4 hex digits");
        }
    }
    if (const auto v = value("source_pid"); v && *v != "any") {
        if (const auto pid = parseHex(*v)) {
            config.sourceProductId = static_cast<std::uint16_t>(*pid);
        } else {
            problems.push_back("source_pid = '" + *v + "': expected 4 hex digits or any");
        }
    }
    if (const auto v = value("target_profile")) {
        if (makeProfile(*v) != nullptr) {
            config.targetProfile = *v;
        } else {
            problems.push_back("target_profile = '" + *v + "': expected one of " + std::string(kKnownProfiles));
        }
    }
    if (const auto v = value("state_port")) {
        int port = 0;
        const auto [end, ec] = std::from_chars(v->data(), v->data() + v->size(), port);
        if (ec == std::errc{} && end == v->data() + v->size() && port >= 1024 && port <= 65535) {
            config.statePort = static_cast<std::uint16_t>(port);
        } else {
            problems.push_back("state_port = '" + *v + "': expected 1024..65535");
        }
    }
    if (const auto v = value("stale_after_ms")) {
        int ms = 0;
        const auto [end, ec] = std::from_chars(v->data(), v->data() + v->size(), ms);
        if (ec == std::errc{} && end == v->data() + v->size() && ms >= 20 && ms <= 5000) {
            config.staleAfterMs = ms;
        } else {
            problems.push_back("stale_after_ms = '" + *v + "': expected 20..5000");
        }
    }
    if (!isValid(config.layout(0))) {
        problems.push_back("throttle_axis / brake_axis / clutch_axis: two pedals share an axis, a pedal uses x "
                           "(the wheel), or throttle/brake is none; using the " + config.targetProfile + " layout");
        config.throttleAxis.reset();
        config.brakeAxis.reset();
        config.clutchAxis.reset();
    }
    for (const std::string& problem : problems) {
        WE_LOG_ERROR(log, "wheelbri-shim.ini: {} (default kept)", problem);
    }
    WE_LOG_INFO(log, "loaded '{}'", toUtf8(path.c_str()));
    return config;
}

}  // namespace

const char* toString(const FfbMode mode) {
    switch (mode) {
        case FfbMode::Service: return "service";
        case FfbMode::Passthrough: return "passthrough";
        case FfbMode::Off: return "off";
    }
    return "?";
}

const ShimConfig& shimConfig() {
    static ShimConfig config;
    static std::once_flag once;
    std::call_once(once, [] {
        const logging::Logger log = logger("shim.config");
        config = load(log);
        WE_LOG_INFO(log, "emulate={} source={:04x}:{} target_profile={} state_port={} stale_after_ms={} "
                         "force_feedback={} ffb_port={}",
                    config.emulate, config.sourceVendorId,
                    config.sourceProductId ? fmt::format("{:04x}", *config.sourceProductId) : std::string("any"),
                    config.targetProfile, config.statePort, config.staleAfterMs, toString(config.forceFeedback),
                    config.ffbPort);
        const JoystickLayout layout = config.layout(0);
        WE_LOG_INFO(log, "axes: wheel=x throttle={} brake={} clutch={} invert_pedals={}", axisName(layout.throttle),
                    axisName(layout.brake), axisName(layout.clutch), layout.invertPedals);
        for (const std::string& problem : gameProfileProblems()) {
            WE_LOG_ERROR(log, "game profile {}: not used", problem);
        }
        // The game this DLL is loaded into: its profile, if it has one, laid over the layout above.
        wchar_t exe[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const GameProfile* const game = length > 0 && length < MAX_PATH ? gameProfileFor(toUtf8(exe)) : nullptr;
        if (game != nullptr && !profileFitsTarget(*game, config.targetProfile)) {
            WE_LOG_INFO(log, "game profile {} ({}) is for the {}, the target is {}: not used", game->id, game->label,
                        game->target, config.targetProfile);
        } else if (game != nullptr) {
            ShimConfig withGame = config;
            withGame.gameProfile = game;
            if (!isValid(withGame.layout(0))) {
                WE_LOG_ERROR(log, "game profile {} ({}) over this file's axes puts two pedals on one axis or a pedal "
                                  "on x: not used",
                             game->id, describeOverlay(*game));
            } else {
                config.gameProfile = game;
                const JoystickLayout used = config.layout(0);
                WE_LOG_INFO(log, "game profile {} ({}) laid over the defaults: {}; axes now throttle={} brake={} "
                                 "clutch={}",
                            game->id, game->label, describeOverlay(*game), axisName(used.throttle),
                            axisName(used.brake), axisName(used.clutch));
                if ((game->throttleAxis && config.throttleAxis) || (game->brakeAxis && config.brakeAxis) ||
                    (game->clutchAxis && config.clutchAxis)) {
                    WE_LOG_WARN(log, "wheelbri-shim.ini sets pedal axes the game profile {} also sets: the profile's "
                                     "are used",
                                game->id);
                }
            }
        }
        if (config.emulate && config.forceFeedback == FfbMode::Passthrough) {
            WE_LOG_INFO(log, "force feedback goes straight to the physical wheel: its strength is capped by the "
                             "wheel's own settings (Pit House), not by wheelbri-service");
        }
    });
    return config;
}

}  // namespace wheelbri::shim
