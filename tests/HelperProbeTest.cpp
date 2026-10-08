// HelperProbe: the app's Hello to the WheelBri Helper on its own thread, with a fake check (real threads).

#include "HelperProbe.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace wheelbri::service {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using Kind = helper::HelperState::Kind;

bool eventually(const std::function<bool()>& condition, const std::chrono::milliseconds limit = 5s) {
    const auto end = Clock::now() + limit;
    while (Clock::now() < end) {
        if (condition()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return condition();
}

// Counts its calls; can be held inside a check until released.
class FakeCheck {
public:
    HelperProbe::Check check() {
        return [this] {
            std::unique_lock<std::mutex> lock(mutex_);
            ++calls_;
            inside_ = true;
            changed_.notify_all();
            changed_.wait(lock, [this] { return !hold_; });
            inside_ = false;
            if (throwStd_) throw std::runtime_error("pipe on fire");
            if (throwOther_) throw 7;
            helper::HelperState state;
            state.kind = kind_;
            state.version = "0.9.0";
            return state;
        };
    }
    void hold() {
        const std::lock_guard<std::mutex> lock(mutex_);
        hold_ = true;
    }
    void release() {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            hold_ = false;
        }
        changed_.notify_all();
    }
    bool waitInside() {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, 5s, [this] { return inside_; });
    }
    int calls() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return calls_;
    }
    void answer(const Kind kind) {
        const std::lock_guard<std::mutex> lock(mutex_);
        kind_ = kind;
    }
    void throwStd() {
        const std::lock_guard<std::mutex> lock(mutex_);
        throwStd_ = true;
    }
    void throwOther() {
        const std::lock_guard<std::mutex> lock(mutex_);
        throwOther_ = true;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    int calls_ = 0;
    bool hold_ = false;
    bool inside_ = false;
    bool throwStd_ = false;
    bool throwOther_ = false;
    Kind kind_ = Kind::Accepted;
};

TEST(HelperProbe, ChecksOnceAtStart) {
    FakeCheck fake;
    HelperProbe probe(fake.check(), 0);
    ASSERT_TRUE(eventually([&] { return probe.checks() == 1; }));
    EXPECT_EQ(probe.latest().kind, Kind::Accepted);
    EXPECT_EQ(probe.latest().version, "0.9.0");
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(fake.calls(), 1);   // and nothing more without a request
}

TEST(HelperProbe, NothingIsKnownBeforeTheFirstCheckEnds) {
    FakeCheck fake;
    fake.hold();
    HelperProbe probe(fake.check(), 0);
    ASSERT_TRUE(fake.waitInside());
    EXPECT_EQ(probe.latest().kind, Kind::NotChecked);
    EXPECT_EQ(probe.checks(), 0U);
    fake.release();
    EXPECT_TRUE(eventually([&] { return probe.checks() == 1; }));
}

TEST(HelperProbe, ARequestAfterTheGapChecksAgainAndTakesTheNewAnswer) {
    FakeCheck fake;
    HelperProbe probe(fake.check(), 0);
    ASSERT_TRUE(eventually([&] { return probe.checks() == 1; }));
    fake.answer(Kind::NotRunning);   // the service was stopped
    probe.request();
    ASSERT_TRUE(eventually([&] { return probe.checks() == 2; }));
    EXPECT_EQ(probe.latest().kind, Kind::NotRunning);
}

TEST(HelperProbe, ARequestWithinTheGapIsIgnored) {
    FakeCheck fake;
    HelperProbe probe(fake.check(), 60'000);
    ASSERT_TRUE(eventually([&] { return probe.checks() == 1; }));
    for (int i = 0; i < 10; ++i) probe.request();   // the Logs tab asks every second
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(fake.calls(), 1);
}

TEST(HelperProbe, TheGapRunsFromWhenTheLastCheckBegan) {
    FakeCheck fake;
    HelperProbe probe(fake.check(), 100);
    ASSERT_TRUE(eventually([&] { return probe.checks() == 1; }));
    probe.request();   // within 100 ms
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(fake.calls(), 1);
    std::this_thread::sleep_for(150ms);
    probe.request();   // after it
    EXPECT_TRUE(eventually([&] { return probe.checks() == 2; }));
}

TEST(HelperProbe, ARequestWhileTheFirstCheckRunsIsNotASecondOne) {
    FakeCheck fake;
    fake.hold();
    HelperProbe probe(fake.check(), 0);
    ASSERT_TRUE(fake.waitInside());
    probe.request();   // the first one has not ended: it is the answer
    probe.request();
    fake.release();
    ASSERT_TRUE(eventually([&] { return probe.checks() == 1; }));
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(fake.calls(), 1);
}

TEST(HelperProbe, ARequestWhileALaterCheckRunsIsNotQueued) {
    FakeCheck fake;
    HelperProbe probe(fake.check(), 0);
    ASSERT_TRUE(eventually([&] { return probe.checks() == 1; }));
    fake.hold();
    probe.request();
    ASSERT_TRUE(fake.waitInside());
    probe.request();   // running: ignored
    fake.release();
    ASSERT_TRUE(eventually([&] { return probe.checks() == 2; }));
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(fake.calls(), 2);
}

TEST(HelperProbe, ACheckThatThrowsIsAFailureAndTheProbeGoesOn) {
    FakeCheck fake;
    fake.throwStd();
    HelperProbe probe(fake.check(), 0);
    ASSERT_TRUE(eventually([&] { return probe.checks() == 1; }));
    EXPECT_EQ(probe.latest().kind, Kind::Failed);
    EXPECT_EQ(probe.latest().detail, "pipe on fire");
}

TEST(HelperProbe, ACheckThatThrowsSomethingElseIsAFailureToo) {
    FakeCheck fake;
    fake.throwOther();
    HelperProbe probe(fake.check(), 0);
    ASSERT_TRUE(eventually([&] { return probe.checks() == 1; }));
    EXPECT_EQ(probe.latest().kind, Kind::Failed);
    EXPECT_EQ(probe.latest().detail, "an unknown error");
}

TEST(HelperProbe, NoCheckAtAllIsNotChecked) {
    HelperProbe probe(HelperProbe::Check{}, 0);
    ASSERT_TRUE(eventually([&] { return probe.checks() == 1; }));
    EXPECT_EQ(probe.latest().kind, Kind::NotChecked);
}

TEST(HelperProbe, ItEndsPromptlyWhenIdle) {
    FakeCheck fake;
    auto probe = std::make_unique<HelperProbe>(fake.check(), 0);
    ASSERT_TRUE(eventually([&] { return probe->checks() == 1; }));
    const auto start = Clock::now();
    probe.reset();
    EXPECT_LT(Clock::now() - start, 1s);
}

TEST(HelperProbe, ItWaitsForARunningCheckBeforeEnding) {
    FakeCheck fake;
    fake.hold();
    auto probe = std::make_unique<HelperProbe>(fake.check(), 0);
    ASSERT_TRUE(fake.waitInside());
    std::thread releaser([&] {
        std::this_thread::sleep_for(50ms);
        fake.release();
    });
    probe.reset();   // returns once the check ended, not before (the check uses the probe's state)
    releaser.join();
    EXPECT_EQ(fake.calls(), 1);
}

TEST(HelperProbe, ManyThreadsAskingAndReadingAtOnce) {
    FakeCheck fake;
    HelperProbe probe(fake.check(), 0);
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            while (!stop) {
                probe.request();
                (void)probe.latest();
                (void)probe.checks();
            }
        });
    }
    std::this_thread::sleep_for(100ms);
    stop = true;
    for (std::thread& thread : threads) thread.join();
    EXPECT_GE(probe.checks(), 1U);
    EXPECT_EQ(probe.latest().kind, Kind::Accepted);
}

TEST(HelperProbe, TheProcessHasOneAndItChecksAtOnce) {
    HelperProbe& probe = HelperProbe::shared();
    EXPECT_EQ(&probe, &HelperProbe::shared());
    ASSERT_TRUE(eventually([&] { return probe.checks() >= 1; }, 10s));   // a Hello waits at most 5 s
    EXPECT_NE(probe.latest().kind, Kind::NotChecked);
}

TEST(HelperProbe, TheSystemsCheckOnThisSystem) {
    const helper::HelperState state = HelperProbe::systemCheck()();
#if defined(_WIN32)
    // Whatever this PC has: a helper or none, but never "not Windows" and never "not checked".
    EXPECT_NE(state.kind, Kind::NotWindows);
    EXPECT_NE(state.kind, Kind::NotChecked);
#else
    EXPECT_EQ(state.kind, Kind::NotWindows);
#endif
}

}  // namespace
}  // namespace wheelbri::service
