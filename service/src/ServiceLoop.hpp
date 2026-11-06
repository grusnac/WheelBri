// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The service's loop (runLoop, main.cpp): on its own thread, reads the wheel and its accessories into the Pipeline
// (state to the game), plays the game's force feedback, writes telemetry and hands the window its copy; between
// passes it sleeps until a device or the force feedback socket has news, or until what must run on time is due (core
// EventLoopControl, WakePlanner). Everything it asks of the system goes through LoopSystem and the game's force
// feedback through LoopFfb, so tests/emulators runs this very loop on emulated devices and a virtual clock
// (PushLoopTest).

#include "SessionUi.hpp"

#include "wheelbri/EventLoopControl.hpp"
#include "wheelbri/FfbController.hpp"
#include "wheelbri/FfbIdleWatch.hpp"
#include "wheelbri/GameUseWatch.hpp"
#include "wheelbri/LogLevel.hpp"
#include "wheelbri/LogLevelPacket.hpp"
#include "wheelbri/Pipeline.hpp"
#include "wheelbri/Telemetry.hpp"
#include "wheelbri/WakePlanner.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wheelbri::service {

// runLoop's result when the physical wheel or an accessory stopped answering (unplugged, USB reset): runSession
// waits for it to come back and carries on, keeping the virtual wheel (games keep their controller).
inline constexpr int kWheelLost = 3;

// What the loop's thread shares with the window's, and nothing else: the window's copy of the state (handed over
// under `frameMutex`, never waited for by the loop) and the flags that end each side. The pipeline is shared too,
// through its own thread-safe setters (setMapper, setGameMapper, setPedalRoute, setButtonOrder).
struct LoopShared {
    std::mutex frameMutex;
    UiFrame frame;
    std::atomic<bool> stop{false};   // the window side ended (closed, or failed): the loop ends
    std::atomic<bool> done{false};   // the loop ended: the window side ends
};

// What the loop asks of the system. The service's: the monotonic clock, platform::WaitSet, the stop signals and the
// state socket (main.cpp); the tests': a virtual clock and emulated devices (tests/emulators).
class LoopSystem {
public:
    virtual ~LoopSystem() = default;
    [[nodiscard]] virtual std::uint64_t nowNs() = 0;
    // Until a source the loop was told it can wait for is ready (its index, in the order they were added), the
    // deadline (kWaitTimedOut), or the wait failed (kWaitFailed).
    [[nodiscard]] virtual int waitUntil(std::uint64_t deadlineNs) = 0;
    // Why the last wait failed, for the log.
    [[nodiscard]] virtual std::string waitFailureText() const { return {}; }
    virtual void sleepNs(std::uint64_t ns) = 0;
    // Ctrl+C, the stop file, a signal: the loop ends after its pass.
    [[nodiscard]] virtual bool stopRequested() const = 0;
    // The app's log level to whatever reads the state (LogLevelPacket.hpp).
    virtual void publishLogLevel(LogLevel level) = 0;
    // First thing on the loop's thread (the service raises its priority).
    virtual void threadStarted() {}
    // After each pass that read the devices (`startNs`, what woke it): tests see when each pass ran.
    virtual void passDone(std::uint64_t /*startNs*/, WakeReason /*wokeBy*/) {}
};

// The game's force feedback as the loop drives it (main.cpp's GameFfb; a fake in the tests).
class LoopFfb {
public:
    virtual ~LoopFfb() = default;
    // Every pass: what arrived from the game, then the forces brought up to date (limits, watchdog).
    virtual void pump(std::uint64_t nowNs) = 0;
    [[nodiscard]] virtual const FfbController& controller() const = 0;
    // Every 10 s, and at the end: lines for the log.
    virtual void logStatsIfChanged() {}
    virtual void logUsage() {}
};

class ServiceLoop {
public:
    struct Parts {
        Pipeline& pipeline;
        LoopSystem& system;
        LoopShared& shared;
        std::string targetName;                    // "G920": for the log
        std::vector<std::string> accessoryNames;   // in the pipeline's order: for the log when one is lost
        LoopFfb* ffb = nullptr;                    // null: no game force feedback
        TelemetryRecorder* telemetry = nullptr;    // null: none
        bool print = false;                        // --print: a live line on the console
        bool window = false;                       // a window takes copies (LoopShared::frame)
        EventLoopControl::Settings settings;
    };

    explicit ServiceLoop(Parts parts);

    // Before run(), what the loop can wait for, in the order they were added to the system's wait (its indexes).
    // A device that gives no notification (DirectInput only polls it), or a socket the wait cannot take, makes the
    // loop poll every millisecond as well.
    void addDevice(bool notifies, const std::string& name);
    void addGameFfbSocket(bool waitable);

    // On the loop's thread: until stopped (LoopSystem::stopRequested, LoopShared::stop) with 0, or a device lost with
    // kWheelLost.
    int run();

    [[nodiscard]] const EventLoopControl& control() const { return control_; }
    // When the loop started polling during the run, the wait having failed or notifications being lost (not from
    // the start, for what cannot notify it: control().pollReason() says that).
    [[nodiscard]] std::optional<std::uint64_t> pollingSinceNs() const { return pollingSinceNs_; }

private:
    void logStart() const;
    void watchTheGame();   // GameUseWatch, FfbIdleWatch: said in the log
    void writeTelemetry();
    void handFrame();      // the window's copy, if it is not copying now
    void logStats(std::uint64_t tickEnd, std::uint64_t& windowStart);
    void waitForNews(std::uint64_t tickStart);   // until something new, or what is due
    [[nodiscard]] std::string pollingNote() const;

    Pipeline& pipeline_;
    LoopSystem& system_;
    LoopShared& shared_;
    std::string targetName_;
    std::vector<std::string> accessoryNames_;
    LoopFfb* ffb_;
    TelemetryRecorder* telemetry_;
    bool print_;
    bool window_;
    EventLoopControl control_;
    std::vector<WakeReason> wakeReasons_;   // what each of the wait's sources is

    // Pedals pressed but the game never plays force feedback on the wheel: it drives from another controller.
    GameUseWatch gameUse_;
    // Force feedback loaded but none of it started while you drive (NFS 2015 at the start of a race): alt-tab.
    FfbIdleWatch ffbIdle_;
    LogLevelBeacon levelBeacon_;

    // The loop's own figures, for telemetry and the once-a-minute line.
    WakeStats wakeStats_;
    std::uint64_t lastPeriodNs_ = 0;
    std::uint64_t lastWorkNs_ = 0;
    std::uint64_t lastUsageLogNs_ = 0;
    std::uint64_t lastWakeLogNs_ = 0;
    std::optional<std::uint64_t> pollingSinceNs_;
};

}  // namespace wheelbri::service
