#include "wheelbri/FileText.hpp"
#include "wheelbri/EmulatorConfig.hpp"

#include "wheelbri/MappingEdit.hpp"

#include <charconv>
#include <cmath>
#include <optional>
#include <set>
#include <utility>

namespace wheelbri {

namespace {

// Reads typed values from one INI section and records every problem instead of failing fast,
// so a single typo does not hide the other mistakes in the file.
class SectionReader {
public:
    SectionReader(const IniDocument& ini, std::string section, std::vector<std::string>& errors)
        : ini_(ini), section_(std::move(section)), errors_(errors) {}

    void readString(const std::string& key, std::string& out) {
        if (const auto value = lookup(key)) {
            out = *value;
        }
    }

    void readFloat(const std::string& key, float& out, const float min, const float max) {
        const auto value = lookup(key);
        if (!value) {
            return;
        }
        float parsed = 0.0F;
        const auto [end, ec] = std::from_chars(value->data(), value->data() + value->size(), parsed);
        // from_chars reads "nan" and "inf": NaN passes both range checks, so reject non-finite values first.
        if (ec != std::errc{} || end != value->data() + value->size() || !std::isfinite(parsed) || parsed < min ||
            parsed > max) {
            invalid(key, *value, "a number in [" + std::to_string(min) + ", " + std::to_string(max) + "]");
            return;
        }
        out = parsed;
    }

    void readInt(const std::string& key, int& out, const int min, const int max) {
        const auto value = lookup(key);
        if (!value) {
            return;
        }
        int parsed = 0;
        const auto [end, ec] = std::from_chars(value->data(), value->data() + value->size(), parsed);
        if (ec != std::errc{} || end != value->data() + value->size() || parsed < min || parsed > max) {
            invalid(key, *value, "an integer in [" + std::to_string(min) + ", " + std::to_string(max) + "]");
            return;
        }
        out = parsed;
    }

    void readBool(const std::string& key, bool& out) {
        const auto value = lookup(key);
        if (!value) {
            return;
        }
        if (*value == "true" || *value == "yes" || *value == "1") {
            out = true;
        } else if (*value == "false" || *value == "no" || *value == "0") {
            out = false;
        } else {
            invalid(key, *value, "true or false");
        }
    }

    // A source: "2" on the wheel, "044f:b696/2" on an accessory (MappingEdit.hpp), or -1 when `allowNone`.
    void readSource(const std::string& key, int& index, int& device, const int max, const bool allowNone,
                    MapperConfig& mapper) {
        const auto value = lookup(key);
        if (!value) {
            return;
        }
        const std::string expected = std::string(allowNone ? "-1, " : "") + "0.." + std::to_string(max) +
                                     " (the wheel's) or vid:pid/0.." + std::to_string(max) + " (an accessory's)";
        const auto source = parseSource(*value);
        if (!source || source->index > max || (!allowNone && !source->mapped())) {
            invalid(key, *value, expected);
            return;
        }
        const auto number = deviceNumber(mapper, source->device);
        if (!number) {
            errors_.push_back("[" + section_ + "] " + key + " = '" + *value + "': at most " +
                              std::to_string(MapperConfig::kMaxAccessories) +
                              " accessories besides the wheel can be read at once");
            return;
        }
        index = source->index;
        device = *number;
    }

    // A key earlier versions read: accepted (not an unknown key) and ignored, so an older file still loads.
    void retired(const std::string& key) { known_.insert(key); }

    void reportUnknownKeys() const {
        const auto& sections = ini_.sections();
        const auto it = sections.find(section_);
        if (it == sections.end()) {
            return;
        }
        for (const auto& [key, value] : it->second) {
            if (known_.count(key) == 0) {
                errors_.push_back("[" + section_ + "] unknown key '" + key + "'");
            }
        }
    }

private:
    std::optional<std::string> lookup(const std::string& key) {
        known_.insert(key);
        return ini_.get(section_, key);
    }

    void invalid(const std::string& key, const std::string& value, const std::string& expected) {
        errors_.push_back("[" + section_ + "] " + key + " = '" + value + "': expected " + expected);
    }

    const IniDocument& ini_;
    std::string section_;
    std::vector<std::string>& errors_;
    std::set<std::string> known_;
};

constexpr int kMaxAxisIndex = static_cast<int>(RawInputState::kMaxAxes) - 1;
// Longest [button_labels] / [axis_labels] name: it has to fit the mapping window's cells.
constexpr std::size_t kMaxButtonLabel = 12;

// Reads `<prefix>N = name` for N in [0, count) into `labels`, cutting names that are too long.
template <std::size_t N>
void readLabels(const IniDocument& ini, const std::string& section, const std::string& prefix,
                std::array<std::string, N>& labels, std::vector<std::string>& errors) {
    SectionReader reader(ini, section, errors);
    for (std::size_t index = 0; index < N; ++index) {
        const std::string key = prefix + std::to_string(index);
        std::string label;
        reader.readString(key, label);
        if (label.size() > kMaxButtonLabel) {
            errors.push_back("[" + section + "] " + key + " = '" + label + "': at most " +
                             std::to_string(kMaxButtonLabel) + " characters");
            label = truncateUtf8(std::move(label), kMaxButtonLabel);
        }
        labels[index] = std::move(label);
    }
    reader.reportUnknownKeys();
}

void readPedal(const IniDocument& ini, const std::string& section, PedalConfig& pedal, MapperConfig& mapper,
               std::vector<std::string>& errors) {
    SectionReader reader(ini, section, errors);
    reader.readSource("axis", pedal.sourceAxis, pedal.sourceDevice, kMaxAxisIndex, true, mapper);
    reader.readBool("invert", pedal.invert);
    reader.readFloat("deadzone_low", pedal.deadzoneLow, 0.0F, 0.99F);
    reader.readFloat("saturation_high", pedal.saturationHigh, 0.01F, 1.0F);
    reader.readFloat("gamma", pedal.gamma, 0.1F, 10.0F);
    reader.reportUnknownKeys();
    if (pedal.saturationHigh <= pedal.deadzoneLow) {
        errors.push_back("[" + section + "] saturation_high must be greater than deadzone_low");
    }
}

}  // namespace

EmulatorConfigResult loadEmulatorConfig(const IniDocument& ini) {
    EmulatorConfigResult result;
    EmulatorConfig& config = result.config;
    std::vector<std::string>& errors = result.errors;

    {
        SectionReader reader(ini, "emulator", errors);
        reader.readString("source_device", config.sourceDeviceMatch);
        reader.readString("target_profile", config.targetProfile);
        int port = config.statePort;
        reader.readInt("state_port", port, 1024, 65535);
        config.statePort = static_cast<std::uint16_t>(port);
        int ffbPort = config.ffbPort;
        reader.readInt("ffb_port", ffbPort, 1024, 65535);
        config.ffbPort = static_cast<std::uint16_t>(ffbPort);
        reader.readInt("state_rate_hz", config.stateRateHz, 10, 2000);
        // The fixed-rate loop's settings (until 2026-10-07; the loop now waits for events and polls only when it must,
        // EventLoopControl): users' own configs still have them, so they load and are ignored.
        reader.retired("loop_mode");
        reader.retired("loop_hz");
        reader.reportUnknownKeys();
    }
    {
        SectionReader reader(ini, "steering", errors);
        SteeringConfig& steering = config.mapper.steering;
        reader.readSource("axis", steering.sourceAxis, steering.sourceDevice, kMaxAxisIndex, true, config.mapper);
        reader.readFloat("source_rotation_deg", steering.sourceRotationDeg, 90.0F, 2520.0F);
        reader.readFloat("target_rotation_deg", steering.targetRotationDeg, 90.0F, 2520.0F);
        reader.readFloat("deadzone", steering.deadzone, 0.0F, 0.5F);
        reader.readBool("invert", steering.invert);
        reader.reportUnknownKeys();
    }
    readPedal(ini, "throttle", config.mapper.throttle, config.mapper, errors);
    readPedal(ini, "brake", config.mapper.brake, config.mapper, errors);
    readPedal(ini, "clutch", config.mapper.clutch, config.mapper, errors);
    {
        // [buttons] target_N = source button (or -1 to unmap).
        SectionReader reader(ini, "buttons", errors);
        for (std::size_t target = 0; target < config.mapper.buttonMap.size(); ++target) {
            reader.readSource("target_" + std::to_string(target), config.mapper.buttonMap[target],
                              config.mapper.buttonDevice[target], 63, true, config.mapper);
        }
        reader.reportUnknownKeys();
    }
    // [button_labels] button_N / [axis_labels] axis_N = what that control is called on the wheel (display only).
    readLabels(ini, "button_labels", "button_", config.mapper.sourceButtonLabels, errors);
    readLabels(ini, "axis_labels", "axis_", config.mapper.sourceAxisLabels, errors);
    {
        // [hat] up/right/down/left = source button that acts as that D-pad direction.
        SectionReader reader(ini, "hat", errors);
        HatButtons& hat = config.mapper.hatButtons;
        reader.readSource("up", hat.up, hat.devices[0], 63, true, config.mapper);
        reader.readSource("right", hat.right, hat.devices[1], 63, true, config.mapper);
        reader.readSource("down", hat.down, hat.devices[2], 63, true, config.mapper);
        reader.readSource("left", hat.left, hat.devices[3], 63, true, config.mapper);
        reader.reportUnknownKeys();
    }
    {
        SectionReader reader(ini, "ffb", errors);
        reader.readFloat("master_gain", config.ffb.masterGain, 0.0F, 1.0F);
        reader.readFloat("max_magnitude", config.ffb.maxMagnitude, 0.0F, 1.0F);
        reader.readFloat("max_slew_per_second", config.ffb.maxSlewPerSecond, 0.0F, 1000.0F);
        reader.readBool("invert", config.ffbInvert);
        reader.readBool("game_effects", config.gameFfb);
        reader.reportUnknownKeys();
    }

    if (config.ffbPort == config.statePort) {
        errors.push_back("[emulator] ffb_port must differ from state_port");
        config.ffbPort = kDefaultFfbPort;
    }

    {
        SectionReader reader(ini, "logging", errors);
        logging::LogConfig& log = config.logging;
        reader.readString("level", log.level);
        reader.readString("file", log.filePath);
        reader.readBool("console", log.console);
        reader.readString("flush_level", log.flushLevel);
        reader.reportUnknownKeys();
        if (!logging::isValidLevel(log.level)) {
            errors.push_back("[logging] level = '" + log.level + "': expected trace, debug, info, warn, error, critical or off");
            log.level = "warn";
        }
        if (!logging::isValidLevel(log.flushLevel)) {
            errors.push_back("[logging] flush_level = '" + log.flushLevel + "': expected a log level");
            log.flushLevel = "warn";
        }
    }

    {
        SectionReader reader(ini, "telemetry", errors);
        TelemetryConfig& telemetry = config.telemetry;
        reader.readBool("enabled", telemetry.enabled);
        reader.readInt("rate_hz", telemetry.rateHz, 1, 1000);
        reader.readBool("only_changes", telemetry.onlyChanges);
        reader.readInt("heartbeat_s", telemetry.heartbeatSeconds, 0, 3600);
        reader.readFloat("min_change", telemetry.minChange, 0.0F, 0.1F);
        reader.readInt("max_file_mb", telemetry.maxFileMb, 1, 4096);
        reader.readString("directory", telemetry.directory);
        reader.reportUnknownKeys();
    }

    {
        SectionReader reader(ini, "retention", errors);
        RetentionConfig& retention = config.retention;
        std::string action = toString(retention.action);
        reader.readString("old_files", action);
        if (const auto parsed = parseOldFilesAction(action)) {
            retention.action = *parsed;
        } else {
            errors.push_back("[retention] old_files = '" + action + "': expected keep, delete or archive");
        }
        reader.readInt("older_than_days", retention.olderThanDays, 1, 3650);
        reader.readString("archiver", retention.archiver);
        if (!isKnownArchiver(retention.archiver)) {
            errors.push_back("[retention] archiver = '" + retention.archiver + "': expected " + kArchivers);
            retention.archiver = "auto";
        }
        reader.reportUnknownKeys();
    }

    static const std::set<std::string> kKnownSections = {"",         "emulator",      "steering",    "throttle",
                                                         "brake",    "clutch",        "buttons",     "button_labels",
                                                         "axis_labels", "hat",        "ffb",         "logging",
                                                         "telemetry", "retention"};
    for (const auto& [section, keys] : ini.sections()) {
        if (kKnownSections.count(section) == 0) {
            errors.push_back("unknown section [" + section + "]");
        } else if (section.empty() && !keys.empty()) {
            errors.push_back("keys found before the first [section]");
        }
    }
    return result;
}

}  // namespace wheelbri
