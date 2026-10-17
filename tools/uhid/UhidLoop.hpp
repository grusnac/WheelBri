#pragma once

// wheelbri-uhid's decisions besides the uhid protocol (UhidG920), without file descriptors or clocks so
// UhidLoopTest covers them: the command line, and what goes to the virtual G920 when (InputFeed).

#include "UhidG920.hpp"

#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/StatePacket.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wheelbri::uhid {

struct Options {
    std::uint16_t statePort = 41329;
    std::uint16_t ffbPort = 41330;
    std::string stopFile;
    std::string device = "/dev/uhid";
    int parentPid = 0;   // --parent: stop once this process is no longer our parent (0: never)
    std::string target = "g920";   // --target: which wheel (VirtualWheel: g25, g27, g920, g923)
};

struct ParsedOptions {
    std::optional<Options> options;   // nullopt: do not start (help asked for, or `error`)
    std::string error;                // empty with --help
};

// The arguments after the program's name. A port is 1..65535 in digits only (atoi took "abc" as port 0), the two
// ports differ (the device would hear its own force feedback), every option has its value, --target is a wheel
// WheelBri emulates.
[[nodiscard]] ParsedOptions parseOptions(const std::vector<std::string>& args);

inline constexpr const char* kUsage =
    "usage: wheelbri-uhid [--state-port 41329] [--ffb-port 41330] [--stop-file PATH] [--device /dev/uhid]\n"
    "                     [--parent PID] [--target g920|g25|g27|g923]\n"
    "  A virtual Logitech G25, G27, G920 or G923 (uhid) fed by wheelbri-service; stops on Ctrl+C, SIGTERM, when\n"
    "  --stop-file appears or when process PID (--parent, the app that started it) is gone.\n"
    "  Proton games: PROTON_ENABLE_HIDRAW=0x046D/0xC262 (G920), 0x046D/0xC299 (G25), 0x046D/0xC29B (G27) or\n"
    "  0x046D/0xC26E (G923) in the launch options.";

// The wheel's state into the device: each new state (sent when it changed, or as a keep-alive), an input report at
// least every kKeepAliveMs, and after kStaleMs without a state the centred wheel and released pedals until states
// come again (the shim's rule).
class InputFeed {
public:
    static constexpr std::int64_t kStaleMs = 1500;
    static constexpr std::int64_t kKeepAliveMs = 100;

    enum class Change { None, Receiving, Stale };

    explicit InputFeed(std::int64_t nowMs) : lastState_(nowMs - kStaleMs) {}

    // One pass of the loop: `packet` is the newest state read since the last pass, if any. Says when states start
    // coming and when they stopped (for the log).
    Change update(const std::optional<StatePacket>& packet, std::int64_t nowMs, UhidG920& device);

    // Milliseconds until update() has something to send without a new state (keep-alive, going neutral); >= 0.
    [[nodiscard]] std::int64_t msUntilDue(std::int64_t nowMs) const;

    [[nodiscard]] bool receiving() const { return receiving_; }
    [[nodiscard]] const TargetReport& current() const { return current_; }

private:
    bool receiving_ = false;
    bool neutralSent_ = true;   // create() sends the neutral report
    std::int64_t lastState_;
    std::int64_t lastInput_ = 0;
    TargetReport current_ = g920Neutral();
};

}  // namespace wheelbri::uhid
