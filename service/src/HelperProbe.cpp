#include "HelperProbe.hpp"

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

HelperProbe& HelperProbe::shared() {
    // Never destroyed: the window and the app ask it until the process ends.
    static HelperProbe* const probe = new HelperProbe(systemCheck(), kMinGapMs);
    return *probe;
}

HelperProbe::Check HelperProbe::systemCheck() {
    return [] {
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
        const auto reply = client.call(helper::RequestType::Hello, {}, 5000, error);
        return helper::helperStateFromHello(reply, error, client.serverImage());
#else
        state.kind = helper::HelperState::Kind::NotWindows;
        return state;
#endif
    };
}

HelperProbe::HelperProbe(Check check, const std::uint64_t minGapMs)
    : check_(std::move(check)), minGapMs_(minGapMs), thread_([this] { run(); }) {}

HelperProbe::~HelperProbe() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    thread_.join();
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
        lock.unlock();
        helper::HelperState state;
        try {
            state = check_ ? check_() : helper::HelperState{};
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
