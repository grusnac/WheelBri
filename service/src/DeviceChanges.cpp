// platform::DeviceChanges (Platform.hpp): the counting and waiting, the same on every system. The notifications
// themselves are started per system: watchDeviceChanges in PlatformWin32.cpp / PlatformPosix.cpp.

#include "Platform.hpp"

#include "wheelbri/Logging.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>

namespace wheelbri::service::platform {

namespace detail {
// Starts the system's notifications of game controllers coming or going; `onChange` is called (from a system thread)
// for each. False when the system refused them. Defined per system.
bool watchDeviceChanges(std::function<void()> onChange);
}  // namespace detail

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.devices");
    return logger;
}

}  // namespace

struct DeviceChanges::Impl {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<std::uint64_t> count{0};
    std::uint64_t wakeGeneration = 0;   // under mutex
    bool available = false;
};

DeviceChanges& DeviceChanges::shared() {
    static DeviceChanges* const instance = new DeviceChanges();   // never deleted: see the class
    return *instance;
}

DeviceChanges::DeviceChanges() : impl_(new Impl) {
    Impl* const impl = impl_;
    impl_->available = detail::watchDeviceChanges([impl] {
        {
            std::lock_guard guard(impl->mutex);
            impl->count.fetch_add(1, std::memory_order_relaxed);
        }
        impl->changed.notify_all();
    });
    if (impl_->available) {
        WE_LOG_INFO(log(), "devices: listed again when the system says a game controller came or went");
    } else {
        WE_LOG_WARN(log(), "devices: the system gives no notice of game controllers coming or going; they are listed "
                           "every 2 s instead");
    }
}

bool DeviceChanges::available() const { return impl_->available; }

std::uint64_t DeviceChanges::count() const { return impl_->count.load(std::memory_order_relaxed); }

bool DeviceChanges::waitForChange(const std::uint64_t seen, const std::uint64_t timeoutMs) {
    std::unique_lock guard(impl_->mutex);
    const std::uint64_t generation = impl_->wakeGeneration;
    impl_->changed.wait_for(guard, std::chrono::milliseconds(timeoutMs), [&] {
        return impl_->count.load(std::memory_order_relaxed) != seen || impl_->wakeGeneration != generation;
    });
    return impl_->count.load(std::memory_order_relaxed) != seen;
}

void DeviceChanges::wakeWaiters() {
    {
        std::lock_guard guard(impl_->mutex);
        ++impl_->wakeGeneration;
    }
    impl_->changed.notify_all();
}

}  // namespace wheelbri::service::platform
