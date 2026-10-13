#include "wheelbri/GameTab.hpp"

#include "wheelbri/AxisTuning.hpp"
#include "wheelbri/GameSettings.hpp"

namespace wheelbri {

std::optional<std::string> GameTabFollower::onFrame(const std::uint64_t starts, const std::string_view id) {
    if (starts == seen_) return std::nullopt;
    seen_ = starts;
    if (id.empty()) return std::nullopt;
    return std::string(id);
}

std::vector<GameProfileChange> describeGameProfile(const GameProfile& game) {
    std::vector<GameProfileChange> changes;
    if (const std::string pedals = describeOverlay(game); !pedals.empty()) {
        changes.push_back({"Pedals", pedals, {}});
    }
    if (game.rotationDeg) {
        changes.push_back({"Steering rotation",
                           tuning::formatNumber(*game.rotationDeg, 1) + " deg (set your wheel's software to it too)",
                           game.rotationNote});
    }
    if (game.ffbGain) {
        changes.push_back({"Force feedback gain",
                           "x" + tuning::formatNumber(*game.ffbGain, 2) + " on its steering force and rumble",
                           game.ffbNote});
    }
    if (!game.settings.empty()) {
        std::string values;
        for (const GameSetting& setting : game.settings) {
            values += (values.empty() ? "" : ", ") + gameSettingLabel(setting.id) + " = " +
                      (setting.value ? tuning::formatNumber(*setting.value, 2) : std::string("the steering rotation"));
        }
        changes.push_back({"Game's own settings", values, game.settingsNote});
    }
    return changes;
}

}  // namespace wheelbri
