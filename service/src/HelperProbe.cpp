#include "HelperProbe.hpp"

#include "AppLogLevel.hpp"
#include "Platform.hpp"

#include "wheelbri/Logging.hpp"

#include <chrono>
#include <exception>

#ifdef _WIN32
#include "HelperClient.hpp"
#endif

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.helper");
    return logger;
}

std::uint64_t steadyMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

}  // namespace

namespace {

// The process's own (shared()), or null before its first use. Never destroyed: the window and the app ask it until
// the process ends; stopShared ends its thread.
HelperProbe*& sharedProbe() {
    static HelperProbe* probe = nullptr;
    return probe;
}
std::mutex gSharedMutex;

}  // namespace

HelperProbe& HelperProbe::shared() {
    const std::lock_guard<std::mutex> lock(gSharedMutex);
    if (sharedProbe() == nullptr) {
        sharedProbe() = new HelperProbe(systemCheck(), kMinGapMs, AppLogLevel::shared().current());
    }
    return *sharedProbe();
}

void HelperProbe::stopShared() {
    HelperProbe* probe = nullptr;
    {
        const std::lock_guard<std::mutex> lock(gSharedMutex);
        probe = sharedProbe();
    }
    if (probe == nullptr) return;
    {
        const std::lock_guard<std::mutex> lock(probe->mutex_);
        if (probe->quit_) return;
        probe->quit_ = true;
    }
    probe->wake_.notify_all();
    if (probe->thread_.joinable()) probe->thread_.join();
}

HelperProbe::Check HelperProbe::systemCheck() {
    return [](const Ask& ask) {
        helper::HelperState state;
#ifdef _WIN32
        helper::HelperClient client;
        std::string error;
        switch (client.connect(error)) {
            case helper::HelperClient::Connect::Ok: break;
            case helper::HelperClient::Connect::NoHelper:
                state.kind = helper::HelperState::Kind::NotRunning;
                return state;
            case helper::HelperClient::Connect::Failed:
                state.kind = helper::HelperState::Kind::Failed;
                state.detail = error;
                return state;
        }
        const auto reply = client.call(helper::RequestType::Hello, {logLevelName(ask.level)}, 5000, error);
        state = helper::helperStateFromHello(reply, error, client.serverImage());
        if (ask.driversToo && state.kind == helper::HelperState::Kind::Accepted) {
            std::string setError;
            const auto set = client.call(helper::RequestType::SetLogLevel, {logLevelName(ask.level)}, 5000, setError);
            if (!set) {
                WE_LOG_WARN(log(), "the drivers' log level stays as it was until the next Start: {}", setError);
            } else if (set->status == helper::Status::Unsupported) {
                WE_LOG_DEBUG(log(), "this WheelBri Helper is older: the drivers' log level changes at the next Start");
            } else if (set->status != helper::Status::Ok) {
                WE_LOG_WARN(log(), "the drivers' log level stays as it was until the next Start: {}", set->text);
            }
        }
        return state;
#else
        static_cast<void>(ask);
        state.kind = helper::HelperState::Kind::NotWindows;
        return state;
#endif
    };
}

HelperProbe::HelperProbe(Check check, const std::uint64_t minGapMs, const LogLevel level)
    : check_(std::move(check)), minGapMs_(minGapMs), level_(level), thread_([this] { run(); }) {}

HelperProbe::~HelperProbe() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void HelperProbe::request() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (pending_ || running_) return;
        if (checks_ > 0 && steadyMs() - startedMs_ < minGapMs_) return;
        pending_ = true;
    }
    wake_.notify_all();
}

void HelperProbe::levelChanged(const LogLevel level, const bool driversToo) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        level_ = level;
        driversToo_ = driversToo_ || driversToo;
        pending_ = true;   // a check that runs now ends first, then this one
    }
    wake_.notify_all();
}

helper::HelperState HelperProbe::latest() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

std::uint64_t HelperProbe::checks() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return checks_;
}

void HelperProbe::run() {
    platform::nameThisThread("wheelbri helper probe");
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return quit_ || pending_; });
        if (quit_) return;
        pending_ = false;
        running_ = true;
        startedMs_ = steadyMs();
        const Ask ask{level_, driversToo_};
        driversToo_ = false;
        lock.unlock();
        helper::HelperState state;
        try {
            state = check_ ? check_(ask) : helper::HelperState{};
        } catch (const std::exception& e) {
            state.kind = helper::HelperState::Kind::Failed;
            state.detail = e.what();
        } catch (...) {
            state.kind = helper::HelperState::Kind::Failed;
            state.detail = "an unknown error";
        }
        lock.lock();
        running_ = false;
        const bool changed = state.kind != latest_.kind || state.version != latest_.version;
        latest_ = std::move(state);
        ++checks_;
        if (changed) {
            WE_LOG_INFO(log(), "WheelBri Helper: {}{}", helper::describeHelper(latest_),
                        latest_.detail.empty() ? std::string() : " (" + latest_.detail + ")");
        }
    }
}

}  // namespace wheelbri::service
