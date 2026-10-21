// DeviceWatch (service/src/DeviceWatch.cpp): its thread with a fake lister, a ChangeCounter the test announces on, and
// short times. Real threads and the real clock, so every check waits for what it expects (with a generous limit) and
// checks for what must not happen over a short while.

#include "DeviceWatch.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace wheelbri::service {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// Waits until `condition` holds, at most `limit`.
bool eventually(const std::function<bool()>& condition, const std::chrono::milliseconds limit = 3s) {
    const auto end = Clock::now() + limit;
    while (Clock::now() < end) {
        if (condition()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return condition();
}

// What the system's game-controller API would answer, settable by the test, and how often it was asked.
class FakeLister {
public:
    enum class Answer { Devices, Unavailable, Throws, ThrowsSomethingElse };

    DeviceWatch::Lister lister() {
        return [this]() -> std::optional<std::vector<WheelInfo>> {
            std::lock_guard guard(mutex_);
            ++calls_;
            if (answer_ == Answer::Throws) throw std::runtime_error("DirectInput fell over");
            if (answer_ == Answer::ThrowsSomethingElse) throw 42;   // not a std::exception
            if (answer_ == Answer::Unavailable) return std::nullopt;
            return devices_;
        };
    }
    void answer(const Answer answer) {
        std::lock_guard guard(mutex_);
        answer_ = answer;
    }
    void devices(std::vector<std::string> names) {
        std::lock_guard guard(mutex_);
        devices_.clear();
        for (std::string& name : names) devices_.push_back(WheelInfo{std::move(name), 0x346e, 0x0005, {}});
    }
    [[nodiscard]] int calls() const {
        std::lock_guard guard(mutex_);
        return calls_;
    }

private:
    mutable std::mutex mutex_;
    Answer answer_ = Answer::Devices;
    std::vector<WheelInfo> devices_{WheelInfo{"Moza R3", 0x346e, 0x0005, {}}};
    int calls_ = 0;
};

DeviceWatch::Options fast() {
    DeviceWatch::Options options;
    options.relist.settleMs = 10;
    options.relist.followUpMs = 150;
    options.relist.safetyMs = 60'000;   // never within a test unless it says otherwise
    options.retryListerMs = 40;
    return options;
}

std::vector<std::string> names(const std::optional<DeviceWatch::Snapshot>& snapshot) {
    std::vector<std::string> result;
    if (snapshot) {
        for (const WheelInfo& info : snapshot->devices) result.push_back(info.name);
    }
    return result;
}

// --- switched on and off ---

TEST(DeviceWatch, InactiveItNeverLists) {
    FakeLister fake;
    ChangeCounter changes;
    DeviceWatch watch(fake.lister(), changes, fast());
    changes.announce();
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(fake.calls(), 0);
    EXPECT_FALSE(watch.latest().has_value());
    EXPECT_FALSE(watch.unavailable());
}

TEST(DeviceWatch, SwitchedOnItListsAtOnce) {
    FakeLister fake;
    ChangeCounter changes;
    DeviceWatch watch(fake.lister(), changes, fast());
    watch.setActive(true);
    ASSERT_TRUE(eventually([&] { return watch.latest().has_value(); }));
    EXPECT_EQ(names(watch.latest()), std::vector<std::string>{"Moza R3"});
    EXPECT_EQ(watch.latest()->generation, 1U);
    EXPECT_EQ(fake.calls(), 1);
}

TEST(DeviceWatch, WithoutAChangeItDoesNotListAgain) {
    FakeLister fake;
    ChangeCounter changes;
    DeviceWatch watch(fake.lister(), changes, fast());
    watch.setActive(true);
    ASSERT_TRUE(eventually([&] { return fake.calls() == 1; }));
    std::this_thread::sleep_for(200ms);   // longer than settle and follow-up together
    EXPECT_EQ(fake.calls(), 1);
}

TEST(DeviceWatch, SwitchingOnTwiceListsOnce) {
    FakeLister fake;
    ChangeCounter changes;
    DeviceWatch watch(fake.lister(), changes, fast());
    watch.setActive(true);
    watch.setActive(true);   // no change: ignored
    ASSERT_TRUE(eventually([&] { return fake.calls() == 1; }));
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(fake.calls(), 1);
}

TEST(DeviceWatch, SwitchedOffItDropsTheListAndStopsListing) {
    FakeLister fake;
    ChangeCounter changes;
    DeviceWatch watch(fake.lister(), changes, fast());
    watch.setActive(true);
    ASSERT_TRUE(eventually([&] { return watch.latest().has_value(); }));
    watch.setActive(false);
    EXPECT_FALSE(watch.latest().has_value());   // at once, not at the next listing
    const int before = fake.calls();
    changes.announce();
    std::this_thread::sleep_for(250ms);   // past settle and follow-up
    EXPECT_EQ(fake.calls(), before);
    EXPECT_FALSE(watch.latest().has_value());
}

TEST(DeviceWatch, SwitchedOnAgainItListsAtOnceEvenWithoutAChange) {
    FakeLister fake;
    ChangeCounter changes;
    DeviceWatch watch(fake.lister(), changes, fast());
    watch.setActive(true);
    ASSERT_TRUE(eventually([&] { return fake.calls() == 1; }));
    watch.setActive(false);
    fake.devices({"Moza R3", "Moza SR-P pedals"});   // plugged in while a session ran (no notice reaches the list)
    watch.setActive(true);
    ASSERT_TRUE(eventually([&] { return names(watch.latest()).size() == 2; }));
    EXPECT_EQ(fake.calls(), 2);
    EXPECT_EQ(watch.latest()->generation, 2U);
}

// --- changes ---

TEST(DeviceWatch, AChangeIsListedOnceItSettledThenFollowedUpOnce) {
    FakeLister fake;
    ChangeCounter changes;
    DeviceWatch watch(fake.lister(), changes, fast());
    watch.setActive(true);
    ASSERT_TRUE(eventually([&] { return fake.calls() == 1; }));
    fake.devices({"Moza R3", "Button box"});
    changes.announce();
    ASSERT_TRUE(eventually([&] { return fake.calls() == 2; }));
    EXPECT_EQ(names(watch.latest()), (std::vector<std::string>{"Moza R3", "Button box"}));
    EXPECT_EQ(watch.latest()->generation, 2U);
    ASSERT_TRUE(eventually([&] { return fake.calls() == 3; }));   // the follow-up
    std::this_thread::sleep_for(300ms);                         // and no follow-up of the follow-up
    EXPECT_EQ(fake.calls(), 3);
}

TEST(DeviceWatch, AListingDueAgainAtOnceRunsWithoutWaiting) {
    // A follow-up time of 0: the follow-up is due the moment the listing ends; the watch lists it straight away,
    // then waits (no endless listing).
    FakeLister fake;
    ChangeCounter changes;
    DeviceWatch::Options options = fast();
    options.relist.followUpMs = 0;
    DeviceWatch watch(fake.lister(), changes, options);
    watch.setActive(true);
    ASSERT_TRUE(eventually([&] { return fake.calls() == 1; }));
    changes.announce();
    ASSERT_TRUE(eventually([&] { return fake.calls() == 3; }));   // the change, and its follow-up at once
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(fake.calls(), 3);
}

TEST(DeviceWatch, ABurstOfChangesIsListedOnce) {
    FakeLister fake;
    ChangeCounter changes;
    DeviceWatch::Options options = fast();
    options.relist.settleMs = 60;   // the whole burst arrives within it
    DeviceWatch watch(fake.lister(), changes, options);
    watch.setActive(true);
    ASSERT_TRUE(eventually([&] { return fake.calls() == 1; }));
    for (int i = 0; i < 10; ++i) changes.announce();   // one device's interfaces arriving
    ASSERT_TRUE(eventually([&] { return fake.calls() >= 2; }));
    std::this_thread::sleep_for(50ms);   // before the follow-up (150 ms)
    EXPECT_EQ(fake.calls(), 2);
}

TEST(DeviceWatch, AChangeWhileInactiveIsNotListedLaterWithoutBeingSwitchedOn) {
    FakeLister fake;
    ChangeCounter changes;
    DeviceWatch watch(fake.lister(), changes, fast());
    for (int i = 0; i < 5; ++i) changes.announce();
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(fake.calls(), 0);
    watch.setActive(true);   // then the one listing, at once
    ASSERT_TRUE(eventually([&] { return fake.calls() == 1; }));
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(fake.calls(), 1);   // the changes before were not a change after this listing
}

TEST(DeviceWatch, TheSafetyListingRunsWithoutAnyChange) {
    FakeLister fake;
    ChangeCounter changes;
    DeviceWatch::Options options = fast();
    options.relist.safetyMs = 40;
    DeviceWatch watch(fake.lister(), changes, options);
    watch.setActive(true);
    EXPECT_TRUE(eventually([&] { return fake.calls() >= 4; }));
}

// --- a lister that cannot list ---

TEST(DeviceWatch, AnUnavailableApiIsReportedAndTriedAgain) {
    FakeLister fake;
    fake.answer(FakeLister::Answer::Unavailable);
    ChangeCounter changes;
    DeviceWatch watch(fake.lister(), changes, fast());
    watch.setActive(true);
    ASSERT_TRUE(eventually([&] { return watch.unavailable(); }));
    EXPECT_FALSE(watch.latest().has_value());
    ASSERT_TRUE(eventually([&] { return fake.calls() >= 2; }));   // retried after retryListerMs, without a change
    fake.answer(FakeLister::Answer::Devices);
    ASSERT_TRUE(eventually([&] { return watch.latest().has_value(); }));
    EXPECT_FALSE(watch.unavailable());
}

TEST(DeviceWatch, ALatestListSurvivesALaterFailureUntilItWorksAgain) {
    FakeLister fake;
    ChangeCounter changes;
    DeviceWatch watch(fake.lister(), changes, fast());
    watch.setActive(true);
    ASSERT_TRUE(eventually([&] { return watch.latest().has_value(); }));
    fake.answer(FakeLister::Answer::Unavailable);
    changes.announce();
    ASSERT_TRUE(eventually([&] { return watch.unavailable(); }));
    EXPECT_EQ(names(watch.latest()), std::vector<std::string>{"Moza R3"});   // the last good list is kept
}

TEST(DeviceWatch, AListerThatThrowsCountsAsUnavailableAndTheWatchLives) {
    FakeLister fake;
    fake.answer(FakeLister::Answer::Throws);
    ChangeCounter changes;
    DeviceWatch watch(fake.lister(), changes, fast());
    watch.setActive(true);
    ASSERT_TRUE(eventually([&] { return watch.unavailable(); }));
    ASSERT_TRUE(eventually([&] { return fake.calls() >= 2; }));   // the thread is still running: it tries again
    fake.answer(FakeLister::Answer::Devices);
    EXPECT_TRUE(eventually([&] { return watch.latest().has_value() && !watch.unavailable(); }));
}

TEST(DeviceWatch, AListerThatThrowsAnythingElseCountsAsUnavailableToo) {
    FakeLister fake;
    fake.answer(FakeLister::Answer::ThrowsSomethingElse);
    ChangeCounter changes;
    DeviceWatch watch(fake.lister(), changes, fast());
    watch.setActive(true);
    ASSERT_TRUE(eventually([&] { return watch.unavailable(); }));
    ASSERT_TRUE(eventually([&] { return fake.calls() >= 2; }));   // still running
    fake.answer(FakeLister::Answer::Devices);
    EXPECT_TRUE(eventually([&] { return watch.latest().has_value() && !watch.unavailable(); }));
}

TEST(DeviceWatch, NoListerAtAllIsUnavailable) {
    ChangeCounter changes;
    DeviceWatch watch(DeviceWatch::Lister{}, changes, fast());
    watch.setActive(true);
    EXPECT_TRUE(eventually([&] { return watch.unavailable(); }));
    EXPECT_FALSE(watch.latest().has_value());
}

TEST(DeviceWatch, AnEmptyListIsAListNotAFailure) {
    FakeLister fake;
    fake.devices({});
    ChangeCounter changes;
    DeviceWatch watch(fake.lister(), changes, fast());
    watch.setActive(true);
    ASSERT_TRUE(eventually([&] { return watch.latest().has_value(); }));
    EXPECT_TRUE(watch.latest()->devices.empty());
    EXPECT_FALSE(watch.unavailable());
}

// --- switched off during a listing ---

TEST(DeviceWatch, AListFinishedAfterSwitchingOffIsNotKept) {
    // The session starts while DirectInput is still listing: that list must not appear once the app is stopped
    // again, it may be from before.
    ChangeCounter changes;
    std::atomic<bool> release{false};
    std::atomic<int> calls{0};
    DeviceWatch watch(
        [&]() -> std::optional<std::vector<WheelInfo>> {
            ++calls;
            while (calls == 1 && !release) std::this_thread::sleep_for(1ms);   // the first listing is slow
            return std::vector<WheelInfo>{WheelInfo{"Moza R3", 0x346e, 0x0005, {}}};
        },
        changes, fast());
    watch.setActive(true);
    ASSERT_TRUE(eventually([&] { return calls == 1; }));
    watch.setActive(false);
    release = true;
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(watch.latest().has_value());
}

TEST(DeviceWatch, OffAndOnAgainDuringAListingListsAgain) {
    ChangeCounter changes;
    std::atomic<bool> release{false};
    std::atomic<int> calls{0};
    DeviceWatch watch(
        [&]() -> std::optional<std::vector<WheelInfo>> {
            ++calls;
            while (calls == 1 && !release) std::this_thread::sleep_for(1ms);
            return std::vector<WheelInfo>{WheelInfo{calls == 1 ? "before" : "after", 0, 0, {}}};
        },
        changes, fast());
    watch.setActive(true);
    ASSERT_TRUE(eventually([&] { return calls == 1; }));
    watch.setActive(false);
    watch.setActive(true);
    release = true;
    ASSERT_TRUE(eventually([&] { return calls == 2 && watch.latest().has_value(); }));
    EXPECT_EQ(names(watch.latest()), std::vector<std::string>{"after"});
}

// --- ending ---

TEST(DeviceWatch, ItEndsAtOnceWhileWaitingForAChange) {
    FakeLister fake;
    ChangeCounter changes;
    auto watch = std::make_unique<DeviceWatch>(fake.lister(), changes, fast());   // safety: a minute
    watch->setActive(true);
    ASSERT_TRUE(eventually([&] { return fake.calls() == 1; }));
    const auto start = Clock::now();
    watch.reset();
    EXPECT_LT(Clock::now() - start, 1s);
}

TEST(DeviceWatch, ItEndsAtOnceWhileWaitingToRetryTheApi) {
    FakeLister fake;
    fake.answer(FakeLister::Answer::Unavailable);
    ChangeCounter changes;
    DeviceWatch::Options options = fast();
    options.retryListerMs = 60'000;
    auto watch = std::make_unique<DeviceWatch>(fake.lister(), changes, options);
    watch->setActive(true);
    ASSERT_TRUE(eventually([&] { return watch->unavailable(); }));
    const auto start = Clock::now();
    watch.reset();
    EXPECT_LT(Clock::now() - start, 1s);
}

TEST(DeviceWatch, ItEndsAtOnceWhenNeverSwitchedOn) {
    FakeLister fake;
    ChangeCounter changes;
    const auto start = Clock::now();
    { DeviceWatch watch(fake.lister(), changes, fast()); }
    EXPECT_LT(Clock::now() - start, 1s);
    EXPECT_EQ(fake.calls(), 0);
}

// --- the app's times ---

TEST(DeviceWatch, TheSystemOptionsFollowWhetherThereAreNotices) {
    EXPECT_EQ(DeviceWatch::systemOptions(true).relist.safetyMs, RelistTimer::Settings{}.safetyMs);
    EXPECT_EQ(DeviceWatch::systemOptions(false).relist.safetyMs,
              RelistTimer::Settings::withoutNotifications().safetyMs);
}

#if defined(__linux__)
// The app's own lister on this machine: the system's backend, started on the first call and kept for the next; what
// it lists is whatever is plugged in (here possibly nothing), never "cannot read".
TEST(DeviceWatch, TheSystemListerStartsTheBackendOnce) {
    const DeviceWatch::Lister lister = DeviceWatch::systemLister();
    const auto first = lister();
    const auto second = lister();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first->size(), second->size());
}
#endif

}  // namespace
}  // namespace wheelbri::service
