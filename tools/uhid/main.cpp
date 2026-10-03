// wheelemu-uhid: Phase 4, the virtual G920 on Linux (Steam Deck, any distribution).
//
// The Linux counterpart of tools/vbridge (Windows, HIDMaestro): creates a virtual "Logitech G920" through the
// kernel's uhid, feeds it the wheel state wheelemu-service publishes (UDP 127.0.0.1:41329) and sends the game's
// HID PID force feedback back to the service as FfbPackets (UDP 127.0.0.1:41330). The service does not change.
//
// For a Proton game to use it, start the game with
//   PROTON_ENABLE_HIDRAW=0x046D/0xC262 SDL_GAMECONTROLLER_IGNORE_DEVICES=0x346E/0x0005 %command%
// (raw HID for the G920, so its force feedback works; the R3 hidden from the game). scripts/linux/wheelemu.sh
// does all of this. Needs access to /dev/uhid (scripts/linux/install-udev-rules.sh, once).

#include "UhidG920.hpp"

#include "wheelemu/FfbPacket.hpp"
#include "wheelemu/HidG920.hpp"
#include "wheelemu/Logging.hpp"
#include "wheelemu/StatePacket.hpp"
#include "wheelemu/UdpLoopback.hpp"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <tuple>

#ifndef WHEELEMU_VERSION
#define WHEELEMU_VERSION "dev"
#endif

namespace {

const wheelemu::logging::Logger& log() {
    static const wheelemu::logging::Logger logger = wheelemu::logging::get("tools.uhid");
    return logger;
}

std::atomic<bool> gStop{false};
void onSignal(int) { gStop = true; }

constexpr std::int64_t kStaleMs = 1500;       // same as the shim and the bridge
constexpr std::int64_t kKeepAliveMs = 100;    // input report at least this often

std::int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? value : "";
}

// Next to the service's log: $XDG_STATE_HOME/wheelemu or ~/.local/state/wheelemu.
std::string defaultLogFile() {
    const std::string state = env("XDG_STATE_HOME");
    const std::filesystem::path directory = state.empty()
                                                ? std::filesystem::path(env("HOME")) / ".local" / "state" / "wheelemu"
                                                : std::filesystem::path(state) / "wheelemu";
    return (directory / "wheelemu-uhid.log").string();
}

struct Options {
    std::uint16_t statePort = 41329;
    std::uint16_t ffbPort = 41330;
    std::string stopFile;
    std::string device = "/dev/uhid";
};

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--state-port" && i + 1 < argc) {
            o.statePort = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--ffb-port" && i + 1 < argc) {
            o.ffbPort = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--stop-file" && i + 1 < argc) {
            o.stopFile = argv[++i];
        } else if (arg == "--device" && i + 1 < argc) {
            o.device = argv[++i];
        } else {
            std::puts(
                "usage: wheelemu-uhid [--state-port 41329] [--ffb-port 41330] [--stop-file PATH] [--device /dev/uhid]\n"
                "  A virtual Logitech G920 (uhid) fed by wheelemu-service; stops on Ctrl+C, SIGTERM or when\n"
                "  --stop-file appears. Proton games: PROTON_ENABLE_HIDRAW=0x046D/0xC262 in the launch options.");
            return false;
        }
    }
    return true;
}

bool takeStopFile(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return false;
    std::filesystem::remove(path, ec);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, o)) {
        return 2;
    }
    wheelemu::logging::init(wheelemu::logging::LogConfig{}, defaultLogFile());
    WE_LOG_INFO(log(), "wheelemu-uhid {} starting: state from 127.0.0.1:{}, force feedback to 127.0.0.1:{}",
                WHEELEMU_VERSION, o.statePort, o.ffbPort);
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGHUP, onSignal);    // the terminal or the Steam wrapper went away: stop cleanly too
    std::signal(SIGQUIT, onSignal);
    if (!o.stopFile.empty()) {
        std::error_code ec;
        std::filesystem::remove(o.stopFile, ec);   // left over from an earlier run
    }

    const int fd = ::open(o.device.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        const int error = errno;
        WE_LOG_CRITICAL(log(), "cannot open {}: {}. {}", o.device, std::strerror(error),
                        error == EACCES ? "Run scripts/linux/install-udev-rules.sh once (it lets you use uhid)."
                        : error == ENOENT ? "Load the module: sudo modprobe uhid (install-udev-rules.sh makes it load "
                                            "at boot)."
                                          : "");
        return 1;
    }

    auto states = wheelemu::UdpLoopbackReceiver::create(o.statePort);
    auto ffbOut = wheelemu::UdpLoopbackSender::create(o.ffbPort);
    if (states == nullptr || ffbOut == nullptr) {
        WE_LOG_CRITICAL(log(), "cannot open the UDP ports (is another wheelemu-uhid, the bridge or the shim using {}?)",
                        o.statePort);
        ::close(fd);
        return 1;
    }

    const auto writeEvent = [fd](const uhid_event& event) {
        const ssize_t written = ::write(fd, &event, sizeof(event));
        if (written != static_cast<ssize_t>(sizeof(event))) {
            static wheelemu::logging::RateLimiter limiter(std::chrono::seconds(5));
            WE_LOG_WARN_LIMITED(log(), limiter, "uhid: write failed: {}", std::strerror(errno));
            return false;
        }
        return true;
    };
    const auto sendFfb = [&ffbOut](const wheelemu::FfbPacket& packet) { ffbOut->send(wheelemu::encode(packet)); };
    const auto logLine = [](const std::string& line) { WE_LOG_INFO(log(), "{}", line); };

    wheelemu::uhid::UhidG920 device(writeEvent, sendFfb, logLine);
    if (!device.create()) {
        WE_LOG_CRITICAL(log(), "the kernel refused the virtual G920");
        ::close(fd);
        return 1;
    }
    WE_LOG_INFO(log(), "virtual '{}' created (046D:C262); Ctrl+C{} to stop", wheelemu::uhid::UhidG920::kName,
                o.stopFile.empty() ? "" : " or " + o.stopFile);

    bool receiving = false;
    bool neutralSent = true;
    std::int64_t lastState = nowMs() - kStaleMs;
    std::int64_t lastInput = 0;
    std::int64_t lastTick = 0;
    std::int64_t lastStopCheck = 0;
    std::int64_t lastStatus = nowMs();
    std::optional<std::tuple<int, std::size_t, bool>> shown;
    wheelemu::TargetReport current = wheelemu::g920Neutral();

    while (!gStop) {
        pollfd pfd{fd, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, 2);
        if (ready < 0 && errno != EINTR) {
            WE_LOG_ERROR(log(), "poll: {}", std::strerror(errno));
            break;
        }
        if (ready > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 && (pfd.revents & POLLIN) == 0) {
            // Would wake poll at once forever (100 % CPU): the device is gone.
            WE_LOG_ERROR(log(), "uhid: {} gave up (poll flags 0x{:x}): stopping", o.device,
                         static_cast<unsigned>(pfd.revents));
            break;
        }
        const std::int64_t now = nowMs();
        if (ready > 0 && (pfd.revents & POLLIN) != 0) {
            uhid_event event{};
            const ssize_t got = ::read(fd, &event, sizeof(event));
            if (got > 0) {
                device.handle(event, now);
            } else if (got < 0 && errno != EAGAIN && errno != EINTR) {
                WE_LOG_ERROR(log(), "uhid read: {}", std::strerror(errno));
                break;
            }
        }

        if (const auto packet = states->pollLatest()) {
            if (!receiving) {
                receiving = true;
                WE_LOG_INFO(log(), "receiving wheel state from wheelemu-service (packet #{})", packet->sequence);
            }
            lastState = now;
            neutralSent = false;
            current = packet->report;
            device.submit(current, now - lastInput >= kKeepAliveMs);
            lastInput = now;
        } else if (now - lastState > kStaleMs && !neutralSent) {
            // Same rule as the shim: hold the last input for 1.5 s, then centred wheel, released pedals.
            if (receiving) {
                WE_LOG_INFO(log(), "no wheel state for {} ms: centred wheel and released pedals until it comes back",
                            kStaleMs);
            }
            receiving = false;
            neutralSent = true;
            current = wheelemu::g920Neutral();
            device.submit(current, true);
            lastInput = now;
        } else if (now - lastInput >= kKeepAliveMs) {
            device.submit(current, true);
            lastInput = now;
        }

        if (now - lastTick >= 10) {
            lastTick = now;
            device.tick(now);
        }
        if (now - lastStopCheck >= 500) {
            lastStopCheck = now;
            if (takeStopFile(o.stopFile)) {
                WE_LOG_INFO(log(), "stop requested ({} appeared)", o.stopFile);
                break;
            }
        }
        if (now - lastStatus >= 10'000) {
            lastStatus = now;
            const auto state = std::make_tuple(device.openCount(), device.activeSlots(), device.reportsHandled() > 0);
            if (state != shown) {
                shown = state;
                WE_LOG_INFO(log(), "status: {} program(s) have the G920 open, {} effects playing, {} force feedback "
                                   "reports from the game, {} packets to the service",
                            device.openCount(), device.activeSlots(), device.reportsHandled(), device.packetsSent());
            }
        }
    }

    WE_LOG_INFO(log(), "stopping: {} force feedback reports from the game, {} packets to the service",
                device.reportsHandled(), device.packetsSent());
    device.stopForces(nowMs());
    device.destroy();
    ::close(fd);
    WE_LOG_INFO(log(), "virtual G920 removed");
    return 0;
}
