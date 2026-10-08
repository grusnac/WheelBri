#include "DeviceWatch.hpp"

#include "Platform.hpp"

#include "wheelbri/Logging.hpp"
#include "wheelbri/RelistTimer.hpp"

#include <exception>

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.devices");
    return logger;
}

constexpr std::uint64_t kRetryBackendMs = 10'000;   // a new try at the game-controller API

}  // namespace

DeviceWatch::DeviceWatch() : thread_([this] {
    platform::nameThisThread("wheelbri device watch");
    run();
}) {}

DeviceWatch::~DeviceWatch() {
    {
        std::lock_guard guard(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    platform::DeviceChanges::shared().wakeWaiters();
    if (thread_.joinable()) thread_.join();
}

void DeviceWatch::setActive(const bool active) {
    {
        std::lock_guard guard(mutex_);
        if (active_ == active) return;
        active_ = active;
        if (active) activated_ = true;
        if (!active) latest_.reset();
    }
    wake_.notify_all();
    platform::DeviceChanges::shared().wakeWaiters();
}

std::optional<DeviceWatch::Snapshot> DeviceWatch::latest() const {
    std::lock_guard guard(mutex_);
    return latest_;
}

bool DeviceWatch::unavailable() const {
    std::lock_guard guard(mutex_);
    return unavailable_;
}

void DeviceWatch::run() {
    platform::DeviceChanges& changes = platform::DeviceChanges::shared();
    const RelistTimer::Settings settings =
        changes.available() ? RelistTimer::Settings{} : RelistTimer::Settings::withoutNotifications();
    RelistTimer timer(settings);
    std::unique_ptr<WheelBackend> backend;
    try {
        std::unique_lock guard(mutex_);
        while (!quit_) {
            wake_.wait(guard, [this] { return quit_ || active_; });
            if (quit_) break;
            if (activated_) {
                activated_ = false;
                timer = RelistTimer(settings);   // switched on: list at once
            }
            guard.unlock();

            const std::uint64_t seen = changes.count();
            std::uint64_t waitMs = 0;
            if (timer.due(platform::nowMs(), seen)) {
                if (backend == nullptr) backend = startWheelBackend();   // logs why when it cannot
                std::optional<std::vector<WheelInfo>> found;
                if (backend != nullptr) found = backend->list();
                timer.listed(platform::nowMs(), seen);
                guard.lock();
                unavailable_ = backend == nullptr;
                if (found && active_) latest_ = Snapshot{std::move(*found), ++generation_};
                guard.unlock();
                if (backend == nullptr) {
                    timer = RelistTimer(settings);   // due again after the wait below
                    waitMs = kRetryBackendMs;
                }
            }
            if (waitMs == 0) {
                const std::uint64_t now = platform::nowMs();
                waitMs = timer.nextDueMs(now, changes.count()) - now;
            }
            // Until a device comes or goes, the timer says so, or setActive / the destructor wake it.
            if (waitMs > 0) (void)changes.waitForChange(seen, waitMs);
            guard.lock();
        }
    } catch (const std::exception& e) {   // never std::terminate the app from here
        WE_LOG_ERROR(log(), "device watch stopped: {}", e.what());
    } catch (...) {
        WE_LOG_ERROR(log(), "device watch stopped on an unknown error");
    }
}

}  // namespace wheelbri::service
