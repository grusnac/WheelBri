// SPDX-License-Identifier: GPL-3.0-or-later
// wheelbri-uhid: the virtual G25, G27, G920 or G923 (--target) on Linux (Steam Deck, any distribution).
//
// The Linux counterpart of the old HIDMaestro bridge (Windows): creates a virtual "Logitech G920" (or "Logitech G25",
// 046D:C299) through the kernel's uhid, feeds it the wheel state wheelbri-service publishes (UDP 127.0.0.1:41329) and
// sends the game's HID PID force feedback back to the service as FfbPackets (UDP 127.0.0.1:41330). The service does
// not change.
//
// For a Proton game to use it, start the game with
//   PROTON_ENABLE_HIDRAW=0x046D/0xC262 SDL_GAMECONTROLLER_IGNORE_DEVICES=0x346E/0x0005 %command%
// (raw HID for the G920, so its force feedback works; 0x046D/0xC299 for the G25; the rig hidden from the game).
// scripts/linux/wheelbri.sh does all of this. Needs access to /dev/uhid (scripts/linux/install-udev-rules.sh, once).

#include "UhidWheel.hpp"
#include "UhidLoop.hpp"

#include "wheelbri/FfbPacket.hpp"
#include "wheelbri/HidWheelDevice.hpp"
#include "wheelbri/Logging.hpp"
#include "wheelbri/StatePacket.hpp"
#include "wheelbri/UdpLoopback.hpp"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
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
#include <vector>

#ifndef WHEELBRI_VERSION
#define WHEELBRI_VERSION "dev"
#endif

namespace {

const wheelbri::logging::Logger& log() {
    static const wheelbri::logging::Logger logger = wheelbri::logging::get("tools.uhid");
    return logger;
}

std::atomic<bool> gStop{false};
void onSignal(int) { gStop = true; }


std::int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? value : "";
}

// Next to the service's log: $XDG_STATE_HOME/wheelbri or ~/.local/state/wheelbri.
std::string defaultLogFile() {
    const std::string state = env("XDG_STATE_HOME");
    const std::filesystem::path directory = state.empty()
                                                ? std::filesystem::path(env("HOME")) / ".local" / "state" / "wheelbri"
                                                : std::filesystem::path(state) / "wheelbri";
    return (directory / "wheelbri-uhid.log").string();
}

bool takeStopFile(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return false;
    std::filesystem::remove(path, ec);
    return true;
}

constexpr std::int64_t kTickMs = 10;        // HidPid's tick: effects that ran out, keep-alives to the service
constexpr int kMaxEventsPerWake = 64;       // uhid events read per wake; the kernel queues at most 32

}  // namespace

int main(int argc, char** argv) {
    const auto parsed = wheelbri::uhid::parseOptions(std::vector<std::string>(argv + 1, argv + argc));
    if (!parsed.options) {
        if (!parsed.error.empty()) std::fprintf(stderr, "wheelbri-uhid: %s\n", parsed.error.c_str());
        std::puts(wheelbri::uhid::kUsage);
        return parsed.error.empty() ? 0 : 2;
    }
    const wheelbri::uhid::Options& o = *parsed.options;
    wheelbri::logging::init(wheelbri::logging::LogConfig{}, defaultLogFile());
    WE_LOG_INFO(log(), "wheelbri-uhid {} starting: state from 127.0.0.1:{}, force feedback to 127.0.0.1:{}",
                WHEELBRI_VERSION, o.statePort, o.ffbPort);
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGHUP, onSignal);    // the terminal or the Steam wrapper went away: stop cleanly too
    std::signal(SIGQUIT, onSignal);
    if (!o.stopFile.empty()) {
        std::error_code ec;
        std::filesystem::remove(o.stopFile, ec);   // left over from an earlier run
    }

    const int fd = ::open(o.device.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);   // reads drain the queue
    if (fd < 0) {
        const int error = errno;
        WE_LOG_CRITICAL(log(), "cannot open {}: {}. {}", o.device, std::strerror(error),
                        error == EACCES ? "Run scripts/linux/install-udev-rules.sh once (it puts you in the group "
                                          "wheelbri, which may use uhid), then log out and in."
                        : error == ENOENT ? "Load the module: sudo modprobe uhid (install-udev-rules.sh makes it load "
                                            "at boot)."
                                          : "");
        return 1;
    }

    auto states = wheelbri::UdpLoopbackReceiver::create(o.statePort);
    auto ffbOut = wheelbri::UdpLoopbackSender::create(o.ffbPort);
    if (states == nullptr || ffbOut == nullptr) {
        WE_LOG_CRITICAL(log(), "cannot open the UDP ports (is another wheelbri-uhid, the bridge or the shim using {}?)",
                        o.statePort);
        ::close(fd);
        return 1;
    }

    const auto writeEvent = [fd](const uhid_event& event) {
        const ssize_t written = ::write(fd, &event, sizeof(event));
        if (written != static_cast<ssize_t>(sizeof(event))) {
            static wheelbri::logging::RateLimiter limiter(std::chrono::seconds(5));
            WE_LOG_WARN_LIMITED(log(), limiter, "uhid: write failed: {}", std::strerror(errno));
            return false;
        }
        return true;
    };
    const auto sendFfb = [&ffbOut](const wheelbri::FfbPacket& packet) { ffbOut->send(wheelbri::encode(packet)); };
    const auto logLine = [](const wheelbri::LogLevel level, const std::string& line) {
        switch (level) {
            case wheelbri::LogLevel::Trace: WE_LOG_TRACE(log(), "{}", line); break;
            case wheelbri::LogLevel::Debug: WE_LOG_DEBUG(log(), "{}", line); break;
            case wheelbri::LogLevel::Info: WE_LOG_INFO(log(), "{}", line); break;
            case wheelbri::LogLevel::Warn: WE_LOG_WARN(log(), "{}", line); break;
            default: WE_LOG_ERROR(log(), "{}", line); break;
        }
    };

    const wheelbri::VirtualWheel& wheel = *wheelbri::virtualWheel(o.target);   // checked by parseOptions
    wheelbri::uhid::UhidWheel device(writeEvent, sendFfb, logLine, wheel);
    if (!device.create()) {
        WE_LOG_CRITICAL(log(), "the kernel refused the virtual {}", device.label());
        ::close(fd);
        return 1;
    }
    WE_LOG_INFO(log(), "virtual '{}' created ({:04X}:{:04X}); Ctrl+C{} to stop", device.name(), wheel.vendorId,
                wheel.productId, o.stopFile.empty() ? "" : " or " + o.stopFile);

    wheelbri::uhid::InputFeed feed(nowMs());
    wheelbri::LogLevel appliedLevel = wheelbri::LogLevel::Off;   // none applied yet: the first packet is said
    std::int64_t lastTick = 0;
    std::int64_t lastStopCheck = 0;
    std::int64_t lastStatus = nowMs();
    std::optional<std::tuple<int, std::size_t, bool>> shown;
    const int stateSocket = static_cast<int>(states->nativeSocket());

    while (!gStop) {
        // Wakes for a uhid event (the game's force feedback), a state from the service, or what is due next: the
        // tick, a keep-alive, going neutral. Until 2026-10-08 it woke every 2 ms and read one uhid event each time.
        const std::int64_t before = nowMs();
        const std::int64_t timeout =
            std::clamp<std::int64_t>(std::min(lastTick + kTickMs - before, feed.msUntilDue(before)), 0, kTickMs);
        std::array<pollfd, 2> pfds{{{fd, POLLIN, 0}, {stateSocket, POLLIN, 0}}};
        const int ready = ::poll(pfds.data(), pfds.size(), static_cast<int>(timeout));
        if (ready < 0 && errno != EINTR) {
            WE_LOG_ERROR(log(), "poll: {}", std::strerror(errno));
            break;
        }
        const short uhidFlags = ready > 0 ? pfds[0].revents : 0;
        if ((uhidFlags & (POLLERR | POLLHUP | POLLNVAL)) != 0 && (uhidFlags & POLLIN) == 0) {
            // Would wake poll at once forever (100 % CPU): the device is gone.
            WE_LOG_ERROR(log(), "uhid: {} gave up (poll flags 0x{:x}): stopping", o.device,
                         static_cast<unsigned>(uhidFlags));
            break;
        }
        if (ready > 0 && (pfds[1].revents & POLLNVAL) != 0) {
            WE_LOG_ERROR(log(), "the state socket is closed: stopping");
            break;
        }
        const std::int64_t now = nowMs();
        if ((uhidFlags & POLLIN) != 0) {
            // Every queued event (the kernel's queue holds 32; a burst of force feedback reports would overflow it if
            // only one were read per wake). Bounded, so a flood cannot starve the states.
            bool failed = false;
            for (int i = 0; i < kMaxEventsPerWake; ++i) {
                uhid_event event{};
                const ssize_t got = ::read(fd, &event, sizeof(event));
                if (got > 0) {
                    device.handle(event, now);
                    continue;
                }
                if (got < 0 && errno == EINTR) continue;
                if (got < 0 && errno != EAGAIN) {
                    WE_LOG_ERROR(log(), "uhid read: {}", std::strerror(errno));
                    failed = true;
                }
                break;
            }
            if (failed) break;
        }

        const auto packet = states->pollLatest();
        if (const auto level = states->takeLogLevel(); level && *level != appliedLevel) {
            // The app's Level button, from the service (LogLevelPacket).
            const bool quieter = !wheelbri::logLevelAllows(*level, wheelbri::LogLevel::Info);
            if (!quieter) wheelbri::logging::setLevel(wheelbri::logLevelName(*level));
            WE_LOG_INFO(log(), "log level {} (the app's Level button)", wheelbri::logLevelName(*level));
            if (quieter) wheelbri::logging::setLevel(wheelbri::logLevelName(*level));
            appliedLevel = *level;
        }
        switch (feed.update(packet, now, device)) {
            case wheelbri::uhid::InputFeed::Change::Receiving:
                WE_LOG_INFO(log(), "receiving wheel state from wheelbri-service (packet #{})", packet->sequence);
                break;
            case wheelbri::uhid::InputFeed::Change::Stale:
                WE_LOG_INFO(log(), "no wheel state for {} ms: centred wheel and released pedals until it comes back",
                            wheelbri::uhid::InputFeed::kStaleMs);
                break;
            case wheelbri::uhid::InputFeed::Change::None: break;
        }

        if (now - lastTick >= kTickMs) {
            lastTick = now;
            device.tick(now);
        }
        if (now - lastStopCheck >= 500) {
            lastStopCheck = now;
            if (takeStopFile(o.stopFile)) {
                WE_LOG_INFO(log(), "stop requested ({} appeared)", o.stopFile);
                break;
            }
            // Started by the app (--parent): an app that was killed cannot stop us, so we go when it is gone (we are
            // handed to another parent then).
            if (o.parentPid != 0 && ::getppid() != o.parentPid) {
                WE_LOG_WARN(log(), "the program that started us (pid {}) is gone: stopping", o.parentPid);
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
    WE_LOG_INFO(log(), "virtual wheel removed");
    return 0;
}
