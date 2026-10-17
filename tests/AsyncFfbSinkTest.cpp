#include "wheelbri/AsyncFfbSink.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace wheelbri;

namespace {

// Records every call; can be made slow (as the wheel's driver is) and can refuse plays.
class SlowSink final : public IFfbSink {
public:
    struct Log {
        std::mutex mutex;
        std::vector<std::string> calls;
        std::chrono::milliseconds delay{0};
        bool refuse = false;
        int started = 0;    // calls begun
        bool hold = false;  // calls wait inside the sink until released: the worker is "busy" for as long as the
                            // test needs, whatever the system's sleep granularity (Windows sleeps 1 ms as ~15 ms)
        std::condition_variable changed;

        // While on, every call waits inside the sink; off releases them.
        void holdCalls(const bool on) {
            {
                const std::lock_guard<std::mutex> lock(mutex);
                hold = on;
            }
            changed.notify_all();
        }

        void setRefuse(const bool value) {
            const std::lock_guard<std::mutex> lock(mutex);
            refuse = value;
        }
        // Waits until the worker is inside its first call to the sink.
        void waitForFirstCall() {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [this] { return started > 0; });
        }

        std::vector<std::string> take() {
            const std::lock_guard<std::mutex> lock(mutex);
            return std::exchange(calls, {});
        }
    };

    explicit SlowSink(std::shared_ptr<Log> log) : log_(std::move(log)) {}

    bool play(const std::size_t slot, const FfbEffect& effect) override {
        wait();
        const std::lock_guard<std::mutex> lock(log_->mutex);
        log_->calls.push_back("play " + std::to_string(slot) + " " + std::to_string(effect.constantLevel));
        return !log_->refuse;
    }
    void stop(const std::size_t slot) override {
        wait();
        const std::lock_guard<std::mutex> lock(log_->mutex);
        log_->calls.push_back("stop " + std::to_string(slot));
    }
    void stopAll() override {
        wait();
        const std::lock_guard<std::mutex> lock(log_->mutex);
        log_->calls.push_back("stopAll");
    }

private:
    void wait() {
        std::chrono::milliseconds delay{};
        {
            std::unique_lock<std::mutex> lock(log_->mutex);
            ++log_->started;
            log_->changed.notify_all();
            log_->changed.wait(lock, [this] { return !log_->hold; });
            delay = log_->delay;
        }
        if (delay.count() > 0) std::this_thread::sleep_for(delay);
    }

    std::shared_ptr<Log> log_;
};

FfbEffect constant(const float level) {
    FfbEffect effect;
    effect.type = FfbEffectType::Constant;
    effect.constantLevel = level;
    return effect;
}

// Another kind of effect, told apart in the log by its slot (constantLevel is unused, so it logs 0).
FfbEffect ofType(const FfbEffectType type) {
    FfbEffect effect;
    effect.type = type;
    return effect;
}

}  // namespace

TEST(AsyncFfbSink, PlaysReachTheWheel) {
    auto log = std::make_shared<SlowSink::Log>();
    AsyncFfbSink sink(std::make_unique<SlowSink>(log));
    EXPECT_TRUE(sink.play(0, constant(0.5F)));
    sink.flush();
    sink.stop(1);
    sink.flush();
    EXPECT_EQ(log->take(), (std::vector<std::string>{"play 0 0.500000", "stop 1"}));
}

TEST(AsyncFfbSink, ASlowWheelDoesNotHoldTheCaller) {
    auto log = std::make_shared<SlowSink::Log>();
    log->delay = std::chrono::milliseconds(50);
    AsyncFfbSink sink(std::make_unique<SlowSink>(log));
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) {
        EXPECT_TRUE(sink.play(0, constant(static_cast<float>(i) / 100.0F)));
    }
    const auto spent = std::chrono::steady_clock::now() - start;
    EXPECT_LT(spent, std::chrono::milliseconds(40));   // 20 direct calls would take 1 s
    sink.flush();
    // The worker played the first and then only the newest: the ones in between were replaced while it waited.
    const auto calls = log->take();
    ASSERT_FALSE(calls.empty());
    EXPECT_LT(calls.size(), 20U);
    EXPECT_EQ(calls.back(), "play 0 0.190000");
    EXPECT_GE(sink.longestCallUs(), 40'000U);
}

TEST(AsyncFfbSink, NewestCommandPerSlotWins) {
    auto log = std::make_shared<SlowSink::Log>();
    log->holdCalls(true);
    AsyncFfbSink sink(std::make_unique<SlowSink>(log));
    sink.play(7, constant(0.1F));   // keeps the worker busy
    log->waitForFirstCall();
    sink.play(0, constant(0.2F));
    sink.stop(0);                    // play then stop: only the stop
    sink.stop(1);
    sink.play(1, constant(0.3F));   // stop then play: only the play
    log->holdCalls(false);
    sink.flush();
    EXPECT_EQ(log->take(), (std::vector<std::string>{"play 7 0.100000", "stop 0", "play 1 0.300000"}));
}

TEST(AsyncFfbSink, StopAllDropsWhatWaitsAndComesFirst) {
    auto log = std::make_shared<SlowSink::Log>();
    log->holdCalls(true);
    AsyncFfbSink sink(std::make_unique<SlowSink>(log));
    sink.play(7, constant(0.1F));   // keeps the worker busy
    log->waitForFirstCall();
    sink.play(0, constant(0.2F));
    sink.play(2, constant(0.4F));
    sink.stopAll();
    sink.play(3, constant(0.5F));   // after the stopAll: still played
    log->holdCalls(false);
    sink.flush();
    EXPECT_EQ(log->take(), (std::vector<std::string>{"play 7 0.100000", "stopAll", "play 3 0.500000"}));
}

TEST(AsyncFfbSink, ARefusedPlayIsReportedByTheNextOne) {
    auto log = std::make_shared<SlowSink::Log>();
    log->refuse = true;
    AsyncFfbSink sink(std::make_unique<SlowSink>(log));
    EXPECT_TRUE(sink.play(4, constant(0.5F)));
    sink.flush();
    EXPECT_EQ(sink.failures(), 1U);
    // The answer comes now, and the slot is stopped, so nothing of the effect is left on the wheel.
    EXPECT_FALSE(sink.play(4, constant(0.6F)));
    sink.flush();
    EXPECT_EQ(log->take(), (std::vector<std::string>{"play 4 0.500000", "stop 4"}));
    log->setRefuse(false);
    EXPECT_TRUE(sink.play(4, constant(0.7F)));
    sink.flush();
    EXPECT_EQ(log->take(), (std::vector<std::string>{"play 4 0.700000"}));
}

TEST(AsyncFfbSink, ARefusalSettledByAStopIsNotReported) {
    auto log = std::make_shared<SlowSink::Log>();
    log->refuse = true;
    AsyncFfbSink sink(std::make_unique<SlowSink>(log));
    sink.play(4, constant(0.5F));
    sink.flush();
    sink.stop(4);
    EXPECT_TRUE(sink.play(4, constant(0.6F)));
}

TEST(AsyncFfbSink, DestroyingItStopsEverything) {
    auto log = std::make_shared<SlowSink::Log>();
    log->holdCalls(true);
    std::thread releaser;
    {
        AsyncFfbSink sink(std::make_unique<SlowSink>(log));
        sink.play(0, constant(0.5F));
        log->waitForFirstCall();
        sink.play(1, constant(0.6F));   // still waiting when the sink goes: dropped
        // Released only once the destructor has asked the worker to quit (it then waits for the call in progress).
        releaser = std::thread([log] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            log->holdCalls(false);
        });
    }
    releaser.join();
    const auto calls = log->take();
    ASSERT_FALSE(calls.empty());
    EXPECT_EQ(calls.back(), "stopAll");
    for (const auto& call : calls) {
        EXPECT_NE(call, "play 1 0.600000");
    }
}

TEST(AsyncFfbSink, StopsThenConstantThenPeriodicThenConditions) {
    auto log = std::make_shared<SlowSink::Log>();
    log->holdCalls(true);
    AsyncFfbSink sink(std::make_unique<SlowSink>(log));
    sink.play(7, constant(0.1F));   // keeps the worker busy while the rest queues
    log->waitForFirstCall();
    sink.play(0, ofType(FfbEffectType::Spring));
    sink.play(1, ofType(FfbEffectType::Sine));
    sink.play(2, constant(0.4F));
    sink.stop(3);
    log->holdCalls(false);
    sink.flush();
    EXPECT_EQ(log->take(), (std::vector<std::string>{"play 7 0.100000", "stop 3", "play 2 0.400000", "play 1 0.000000",
                                                     "play 0 0.000000"}));
}

TEST(AsyncFfbSink, ACommandThatWaitedLongGoesFirst) {
    auto log = std::make_shared<SlowSink::Log>();
    log->holdCalls(true);
    AsyncFfbSink sink(std::make_unique<SlowSink>(log));
    sink.play(7, constant(0.1F));   // busy for longer than kMaxWait
    log->waitForFirstCall();
    sink.play(0, ofType(FfbEffectType::Spring));   // waits through all of it
    std::this_thread::sleep_for(AsyncFfbSink::kMaxWait + std::chrono::milliseconds(5));
    sink.play(1, constant(0.2F));                  // fresh: would go first if the spring had not waited
    log->holdCalls(false);
    sink.flush();
    EXPECT_EQ(log->take(), (std::vector<std::string>{"play 7 0.100000", "play 0 0.000000", "play 1 0.200000"}));
}

TEST(AsyncFfbSink, TheWorkerStartHookRunsOnTheWorker) {
    auto log = std::make_shared<SlowSink::Log>();
    std::thread::id hookThread;
    std::mutex mutex;
    {
        AsyncFfbSink sink(std::make_unique<SlowSink>(log), [&] {
            const std::lock_guard<std::mutex> lock(mutex);
            hookThread = std::this_thread::get_id();
        });
        sink.play(0, constant(0.5F));
        sink.flush();
    }
    const std::lock_guard<std::mutex> lock(mutex);
    EXPECT_NE(hookThread, std::thread::id{});
    EXPECT_NE(hookThread, std::this_thread::get_id());
}

TEST(AsyncFfbSink, EveryCallIsMeasured) {
    auto log = std::make_shared<SlowSink::Log>();
    log->delay = std::chrono::milliseconds(10);   // each call takes at least this
    log->holdCalls(true);
    AsyncFfbSink sink(std::make_unique<SlowSink>(log));
    sink.play(7, constant(0.1F));
    log->waitForFirstCall();
    sink.play(0, ofType(FfbEffectType::Damper));   // waits for the call before it, held for 10 ms or more
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    log->holdCalls(false);
    sink.flush();
    sink.stopAll();
    sink.flush();

    std::array<AsyncFfbSink::Applied, AsyncFfbSink::kAppliedCapacity> out{};
    ASSERT_EQ(sink.drainApplied(out), 3U);
    EXPECT_EQ(out[0].kind, AsyncFfbSink::Applied::Kind::Play);
    EXPECT_EQ(out[0].slot, 7);
    EXPECT_GE(out[0].callUs, 9'000U);
    EXPECT_EQ(out[1].slot, 0);
    EXPECT_EQ(out[1].type, FfbEffectType::Damper);
    EXPECT_GE(out[1].waitUs, 5'000U);
    EXPECT_GE(out[1].callUs, 9'000U);
    EXPECT_TRUE(out[1].ok);
    EXPECT_EQ(out[2].kind, AsyncFfbSink::Applied::Kind::StopAll);
    EXPECT_EQ(sink.drainApplied(out), 0U);   // drained

    FfbDelayStats stats;
    for (std::size_t i = 0; i < 3; ++i) stats.record(out[i]);
    EXPECT_EQ(stats.count(FfbDelayStats::Group::Constant), 1U);
    EXPECT_EQ(stats.count(FfbDelayStats::Group::Condition), 1U);
    EXPECT_EQ(stats.count(FfbDelayStats::Group::Stop), 1U);
    EXPECT_EQ(stats.line(FfbDelayStats::Group::Periodic), "");
    EXPECT_NE(stats.line(FfbDelayStats::Group::Constant).find("1 calls"), std::string::npos);
}

TEST(AsyncFfbSink, TooManyUndrainedCallsDropTheOldest) {
    auto log = std::make_shared<SlowSink::Log>();
    AsyncFfbSink sink(std::make_unique<SlowSink>(log));
    for (std::size_t i = 0; i < AsyncFfbSink::kAppliedCapacity + 5; ++i) {
        sink.play(0, constant(static_cast<float>(i) / 1000.0F));
        sink.flush();
    }
    std::array<AsyncFfbSink::Applied, AsyncFfbSink::kAppliedCapacity> out{};
    EXPECT_EQ(sink.drainApplied(out), AsyncFfbSink::kAppliedCapacity);
    EXPECT_EQ(sink.droppedApplied(), 5U);
}

TEST(FfbDelayStats, SharesWithin) {
    FfbDelayStats stats;
    AsyncFfbSink::Applied a;
    a.waitUs = 500;
    a.callUs = 3'000;   // 3.5 ms
    stats.record(a);
    a.callUs = 9'000;   // 9.5 ms
    stats.record(a);
    a.callUs = 30'000;  // 30.5 ms
    stats.record(a);
    EXPECT_NEAR(stats.shareWithin(FfbDelayStats::Group::Constant, 5), 1.0 / 3.0, 1e-9);
    EXPECT_NEAR(stats.shareWithin(FfbDelayStats::Group::Constant, 10), 2.0 / 3.0, 1e-9);
    EXPECT_NEAR(stats.shareWithin(FfbDelayStats::Group::Constant, 50), 1.0, 1e-9);
    EXPECT_EQ(stats.shareWithin(FfbDelayStats::Group::Stop, 5), 0.0);
}

// Slot numbers past the last one (a caller's bug): a play is refused, a stop ignored, the wheel is not called.
TEST(AsyncFfbSink, SlotsPastTheLastAreRefused) {
    auto log = std::make_shared<SlowSink::Log>();
    AsyncFfbSink sink(std::make_unique<SlowSink>(log));
    EXPECT_FALSE(sink.play(IFfbSink::kMaxSlots, constant(0.5F)));
    sink.stop(IFfbSink::kMaxSlots);
    sink.stop(1000);
    sink.flush();
    EXPECT_TRUE(log->take().empty());
}

// A sink created without a wheel behind it (none could be opened) can still be destroyed: there is nothing to stop.
TEST(AsyncFfbSink, OneWithoutAWheelCanBeDestroyed) {
    { const AsyncFfbSink sink(nullptr); }
    SUCCEED();
}

// Two stops of everything while the wheel is busy are one call, not two.
TEST(AsyncFfbSink, StopAllTwiceWhileBusyIsOneCall) {
    auto log = std::make_shared<SlowSink::Log>();
    log->holdCalls(true);
    AsyncFfbSink sink(std::make_unique<SlowSink>(log));
    sink.play(7, constant(0.1F));
    log->waitForFirstCall();
    sink.stopAll();
    sink.stopAll();
    log->holdCalls(false);
    sink.flush();
    EXPECT_EQ(log->take(), (std::vector<std::string>{"play 7 0.100000", "stopAll"}));
}

// Two commands of the same kind waiting: the one that waited longer goes first, whatever its slot number. (Queued in
// the order they should come out, so a slow machine that lets them all wait past kMaxWait gives the same order.)
TEST(AsyncFfbSink, OfTheSameKindTheOlderGoesFirst) {
    auto log = std::make_shared<SlowSink::Log>();
    log->holdCalls(true);
    AsyncFfbSink sink(std::make_unique<SlowSink>(log));
    sink.play(7, constant(0.1F));
    log->waitForFirstCall();
    sink.stop(6);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    sink.stop(2);
    sink.play(1, constant(0.2F));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    sink.play(5, constant(0.5F));
    log->holdCalls(false);
    sink.flush();
    EXPECT_EQ(log->take(), (std::vector<std::string>{"play 7 0.100000", "stop 6", "stop 2", "play 1 0.200000",
                                                     "play 5 0.500000"}));
}

// The wheel refusing a play while a stop for that slot already waits: the stop settles it, nothing to report.
TEST(AsyncFfbSink, ARefusalWithAStopWaitingIsNotReported) {
    auto log = std::make_shared<SlowSink::Log>();
    log->refuse = true;
    log->holdCalls(true);
    AsyncFfbSink sink(std::make_unique<SlowSink>(log));
    sink.play(4, constant(0.5F));
    log->waitForFirstCall();
    sink.stop(4);
    log->holdCalls(false);
    sink.flush();
    EXPECT_EQ(sink.failures(), 1U);
    EXPECT_EQ(log->take(), (std::vector<std::string>{"play 4 0.500000", "stop 4"}));
    log->setRefuse(false);
    EXPECT_TRUE(sink.play(4, constant(0.6F)));
}

// The wheel refusing a play while a stop of everything waits: the stop settles it, nothing to report.
TEST(AsyncFfbSink, ARefusalWithAStopAllWaitingIsNotReported) {
    auto log = std::make_shared<SlowSink::Log>();
    log->refuse = true;
    log->holdCalls(true);
    AsyncFfbSink sink(std::make_unique<SlowSink>(log));
    sink.play(4, constant(0.5F));
    log->waitForFirstCall();
    sink.stopAll();
    log->holdCalls(false);
    sink.flush();
    EXPECT_EQ(log->take(), (std::vector<std::string>{"play 4 0.500000", "stopAll"}));
    log->setRefuse(false);
    EXPECT_TRUE(sink.play(4, constant(0.6F)));
}

// Every group has its name; one this build does not know shows as "?". Stops of one slot and of all are stops.
TEST(FfbDelayStats, GroupsAndNames) {
    EXPECT_STREQ(FfbDelayStats::groupName(FfbDelayStats::Group::Constant), "constant");
    EXPECT_STREQ(FfbDelayStats::groupName(FfbDelayStats::Group::Periodic), "periodic");
    EXPECT_STREQ(FfbDelayStats::groupName(FfbDelayStats::Group::Condition), "spring etc.");
    EXPECT_STREQ(FfbDelayStats::groupName(FfbDelayStats::Group::Stop), "stops");
    EXPECT_STREQ(FfbDelayStats::groupName(static_cast<FfbDelayStats::Group>(9)), "?");
    AsyncFfbSink::Applied a;
    a.type = FfbEffectType::Triangle;
    EXPECT_EQ(FfbDelayStats::groupOf(a), FfbDelayStats::Group::Periodic);
    a.type = FfbEffectType::Inertia;
    EXPECT_EQ(FfbDelayStats::groupOf(a), FfbDelayStats::Group::Condition);
    a.kind = AsyncFfbSink::Applied::Kind::Stop;
    EXPECT_EQ(FfbDelayStats::groupOf(a), FfbDelayStats::Group::Stop);
    a.kind = AsyncFfbSink::Applied::Kind::StopAll;
    EXPECT_EQ(FfbDelayStats::groupOf(a), FfbDelayStats::Group::Stop);
}

// A call slower than the last bucket (a wheel's driver stuck for 80 ms) counts, outside every share.
TEST(FfbDelayStats, CallsSlowerThanTheLastBucketCount) {
    FfbDelayStats stats;
    EXPECT_FALSE(stats.any());
    AsyncFfbSink::Applied a;
    a.type = FfbEffectType::Sine;
    a.waitUs = 0xFFFFFFFFU;   // the longest wait there is: no overflow in the total
    a.callUs = 0xFFFFFFFFU;
    stats.record(a);
    a.waitUs = 0;
    a.callUs = 1'000;
    stats.record(a);
    EXPECT_TRUE(stats.any());
    EXPECT_EQ(stats.count(FfbDelayStats::Group::Periodic), 2U);
    EXPECT_NEAR(stats.shareWithin(FfbDelayStats::Group::Periodic, 50), 0.5, 1e-9);
    EXPECT_NEAR(stats.shareWithin(FfbDelayStats::Group::Periodic, 1), 0.5, 1e-9);
    EXPECT_NEAR(stats.shareWithin(FfbDelayStats::Group::Periodic, 0), 0.0, 1e-9);
    EXPECT_NE(stats.line(FfbDelayStats::Group::Periodic).find("periodic"), std::string::npos);
}
