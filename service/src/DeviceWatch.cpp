#include "DeviceWatch.hpp"

#include "Platform.hpp"

#include "wheelbri/Logging.hpp"

#include <chrono>
#include <exception>

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.devices");
    return logger;
}

constexpr auto kListEvery = std::chrono::seconds(2);
constexpr auto kRetryBackend = std::chrono::seconds(10);

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
    if (thread_.joinable()) thread_.join();
}

void DeviceWatch::setActive(const bool active) {
    {
        std::lock_guard guard(mutex_);
        if (active_ == active) return;
        active_ = active;
        if (!active) latest_.reset();
    }
    wake_.notify_all();
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
    std::unique_ptr<WheelBackend> backend;
    try {
        std::unique_lock guard(mutex_);
        while (!quit_) {
            wake_.wait(guard, [this] { return quit_ || active_; });
            if (quit_) break;
            guard.unlock();
            if (backend == nullptr) backend = startWheelBackend();   // logs why when it cannot
            std::optional<std::vector<WheelInfo>> found;
            if (backend != nullptr) found = backend->list();
            guard.lock();
            unavailable_ = backend == nullptr;
            if (found && active_) latest_ = Snapshot{std::move(*found), ++generation_};
            // Every 2 s while active, or a new try at the API every 10 s; at once to the top when switched off.
            wake_.wait_for(guard, backend != nullptr ? kListEvery : kRetryBackend, [this] { return quit_ || !active_; });
        }
    } catch (const std::exception& e) {   // never std::terminate the app from here
        WE_LOG_ERROR(log(), "device watch stopped: {}", e.what());
    } catch (...) {
        WE_LOG_ERROR(log(), "device watch stopped on an unknown error");
    }
}

}  // namespace wheelbri::service
