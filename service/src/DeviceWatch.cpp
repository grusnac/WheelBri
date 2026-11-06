// SPDX-License-Identifier: GPL-3.0-or-later
#include "DeviceWatch.hpp"

#include "Platform.hpp"

#include "wheelbri/Logging.hpp"

#include <exception>

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.devices");
    return logger;
}

}  // namespace

DeviceWatch::Options DeviceWatch::systemOptions(const bool notificationsAvailable) {
    Options options;
    options.relist = notificationsAvailable ? RelistTimer::Settings{} : RelistTimer::Settings::withoutNotifications();
    return options;
}

DeviceWatch::DeviceWatch(Lister list, ChangeCounter& changes, const Options& options)
    : list_(std::move(list)), changes_(changes), options_(options), thread_([this] {
          platform::nameThisThread("wheelbri device watch");
          run();
      }) {}

DeviceWatch::~DeviceWatch() {
    {
        std::lock_guard guard(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    changes_.wakeWaiters();
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
    changes_.wakeWaiters();
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
    RelistTimer timer(options_.relist);
    try {
        std::unique_lock guard(mutex_);
        while (!quit_) {
            wake_.wait(guard, [this] { return quit_ || active_; });
            if (quit_) break;
            if (activated_) {
                activated_ = false;
                timer = RelistTimer(options_.relist);   // switched on: list at once
            }
            // Read while the state above is: setActive / the destructor change it, then wake. A wake from now on ends
            // the wait below even when it comes before the wait begins (switched off and on during the listing).
            const std::uint64_t wakes = changes_.wakes();
            guard.unlock();

            const std::uint64_t seen = changes_.count();
            std::uint64_t waitMs = 0;
            if (timer.due(platform::nowMs(), seen)) {
                std::optional<std::vector<WheelInfo>> found;
                try {
                    if (list_) found = list_();
                } catch (const std::exception& e) {   // counted as "cannot read": tried again later, the thread lives
                    WE_LOG_ERROR(log(), "listing the game controllers failed: {}", e.what());
                } catch (...) {
                    WE_LOG_ERROR(log(), "listing the game controllers failed (not a standard exception)");
                }
                timer.listed(platform::nowMs(), seen);
                guard.lock();
                unavailable_ = !found.has_value();
                // Switched off (or on again) while listing: this list may be from before; the next pass lists anew.
                if (found && active_ && !activated_) latest_ = Snapshot{std::move(*found), ++generation_};
                guard.unlock();
                if (!found) {
                    timer = RelistTimer(options_.relist);   // due again after the wait below
                    waitMs = options_.retryListerMs;
                }
            }
            if (waitMs == 0) {
                const std::uint64_t now = platform::nowMs();
                waitMs = timer.nextDueMs(now, changes_.count()) - now;
            }
            // Until a change is announced, the timer says so, or setActive / the destructor wake it.
            if (waitMs > 0) (void)changes_.waitForChange(seen, waitMs, wakes);
            guard.lock();
        }
    } catch (const std::exception& e) {   // never std::terminate the app from here
        WE_LOG_ERROR(log(), "device watch stopped: {}", e.what());
    } catch (...) {
        WE_LOG_ERROR(log(), "device watch stopped on an unknown error");
    }
}

}  // namespace wheelbri::service
