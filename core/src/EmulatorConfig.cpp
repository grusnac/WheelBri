#include "wheelemu/EmulatorConfig.hpp"

#include <charconv>
#include <optional>
#include <set>
#include <utility>

namespace wheelemu {

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
        if (ec != std::errc{} || end != value->data() + value->size() || parsed < min || parsed > max) {
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

void readPedal(const IniDocument& ini, const std::string& section, PedalConfig& pedal,
               std::vector<std::string>& errors) {
    SectionReader reader(ini, section, errors);
    reader.readInt("axis", pedal.sourceAxis, kUnmapped, kMaxAxisIndex);
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
        reader.readInt("loop_hz", config.loopHz, 100, 2000);
        reader.reportUnknownKeys();
    }
    {
        SectionReader reader(ini, "steering", errors);
        SteeringConfig& steering = config.mapper.steering;
        reader.readInt("axis", steering.sourceAxis, 0, kMaxAxisIndex);
        reader.readFloat("source_rotation_deg", steering.sourceRotationDeg, 90.0F, 2520.0F);
        reader.readFloat("target_rotation_deg", steering.targetRotationDeg, 90.0F, 2520.0F);
        reader.readFloat("deadzone", steering.deadzone, 0.0F, 0.5F);
        reader.readBool("invert", steering.invert);
        reader.reportUnknownKeys();
    }
    readPedal(ini, "throttle", config.mapper.throttle, errors);
    readPedal(ini, "brake", config.mapper.brake, errors);
    readPedal(ini, "clutch", config.mapper.clutch, errors);
    {
        // [buttons] target_N = source index (or -1 to unmap).
        SectionReader reader(ini, "buttons", errors);
        for (std::size_t target = 0; target < config.mapper.buttonMap.size(); ++target) {
            reader.readInt("target_" + std::to_string(target), config.mapper.buttonMap[target], kUnmapped, 63);
        }
        reader.reportUnknownKeys();
    }
    {
        // [hat] up/right/down/left = source button that acts as that D-pad direction.
        SectionReader reader(ini, "hat", errors);
        HatButtons& hat = config.mapper.hatButtons;
        reader.readInt("up", hat.up, kUnmapped, 63);
        reader.readInt("right", hat.right, kUnmapped, 63);
        reader.readInt("down", hat.down, kUnmapped, 63);
        reader.readInt("left", hat.left, kUnmapped, 63);
        reader.reportUnknownKeys();
    }
    {
        SectionReader reader(ini, "ffb", errors);
        reader.readFloat("master_gain", config.ffb.masterGain, 0.0F, 1.0F);
        reader.readFloat("max_magnitude", config.ffb.maxMagnitude, 0.0F, 1.0F);
        reader.readFloat("max_slew_per_second", config.ffb.maxSlewPerSecond, 0.0F, 1000.0F);
        reader.readBool("invert", config.ffbInvert);
        reader.reportUnknownKeys();
    }

    {
        SectionReader reader(ini, "logging", errors);
        logging::LogConfig& log = config.logging;
        reader.readString("level", log.level);
        reader.readString("file", log.filePath);
        int maxFileSizeMb = static_cast<int>(log.maxFileSizeBytes / (1024 * 1024));
        reader.readInt("max_file_size_mb", maxFileSizeMb, 1, 1024);
        log.maxFileSizeBytes = static_cast<std::size_t>(maxFileSizeMb) * 1024 * 1024;
        int maxFiles = static_cast<int>(log.maxFiles);
        reader.readInt("max_files", maxFiles, 0, 100);
        log.maxFiles = static_cast<std::size_t>(maxFiles);
        reader.readBool("console", log.console);
        reader.readString("flush_level", log.flushLevel);
        reader.reportUnknownKeys();
        if (!logging::isValidLevel(log.level)) {
            errors.push_back("[logging] level = '" + log.level + "': expected trace, debug, info, warn, error, critical or off");
            log.level = "info";
        }
        if (!logging::isValidLevel(log.flushLevel)) {
            errors.push_back("[logging] flush_level = '" + log.flushLevel + "': expected a log level");
            log.flushLevel = "warn";
        }
    }

    static const std::set<std::string> kKnownSections = {
        "", "emulator", "steering", "throttle", "brake", "clutch", "buttons", "hat", "ffb", "logging"};
    for (const auto& [section, keys] : ini.sections()) {
        if (kKnownSections.count(section) == 0) {
            errors.push_back("unknown section [" + section + "]");
        } else if (section.empty() && !keys.empty()) {
            errors.push_back("keys found before the first [section]");
        }
    }
    return result;
}

}  // namespace wheelemu
