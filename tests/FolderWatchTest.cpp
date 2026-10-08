// platform::FolderWatch (inotify on Linux; what DeviceChanges watches /dev/input with) on a temporary folder, and
// platform::DeviceChanges' process-wide instance. On Windows FolderWatch is never made (the system's device notices are
// used instead): there the tests check exactly that.

#include "Platform.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace wheelbri::service {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using platform::FolderWatch;

// A folder of its own for each test, removed afterwards.
class TempFolder {
public:
    TempFolder() {
        const auto stamp = std::to_string(Clock::now().time_since_epoch().count());
        path_ = fs::temp_directory_path() / ("wheelbri-folderwatch-" + stamp);
        fs::create_directories(path_);
    }
    ~TempFolder() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

#if defined(__linux__)

bool eventually(const std::function<bool()>& condition, const std::chrono::milliseconds limit = 3s) {
    const auto end = Clock::now() + limit;
    while (Clock::now() < end) {
        if (condition()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return condition();
}

void touch(const fs::path& file) { std::ofstream(file) << "x"; }

TEST(FolderWatch, AFileCreatedIsAChange) {
    TempFolder folder;
    std::atomic<int> calls{0};
    const auto watch = FolderWatch::create(folder.path(), [&] { ++calls; });
    ASSERT_NE(watch, nullptr);
    touch(folder.path() / "event7");
    EXPECT_TRUE(eventually([&] { return calls >= 1; }));
}

TEST(FolderWatch, AFileDeletedIsAChange) {
    TempFolder folder;
    touch(folder.path() / "event7");
    std::atomic<int> calls{0};
    const auto watch = FolderWatch::create(folder.path(), [&] { ++calls; });
    ASSERT_NE(watch, nullptr);
    fs::remove(folder.path() / "event7");
    EXPECT_TRUE(eventually([&] { return calls >= 1; }));
}

TEST(FolderWatch, ChangedPermissionsAreAChange) {
    // udev makes /dev/input/eventN readable a moment after it appears: that is when it can be opened.
    TempFolder folder;
    touch(folder.path() / "event7");
    std::atomic<int> calls{0};
    const auto watch = FolderWatch::create(folder.path(), [&] { ++calls; });
    ASSERT_NE(watch, nullptr);
    ASSERT_EQ(::chmod((folder.path() / "event7").c_str(), 0600), 0);
    EXPECT_TRUE(eventually([&] { return calls >= 1; }));
}

TEST(FolderWatch, AFileMovedInOrOutIsAChange) {
    TempFolder folder;
    TempFolder elsewhere;
    touch(elsewhere.path() / "event8");
    std::atomic<int> calls{0};
    const auto watch = FolderWatch::create(folder.path(), [&] { ++calls; });
    ASSERT_NE(watch, nullptr);
    fs::rename(elsewhere.path() / "event8", folder.path() / "event8");
    ASSERT_TRUE(eventually([&] { return calls >= 1; }));
    const int afterIn = calls;
    fs::rename(folder.path() / "event8", elsewhere.path() / "event8");
    EXPECT_TRUE(eventually([&] { return calls > afterIn; }));
}

TEST(FolderWatch, WritingIntoAFileIsNotAChange) {
    // Only entries coming, going or changing permissions count: a device's data is not a device change.
    TempFolder folder;
    touch(folder.path() / "event7");
    std::atomic<int> calls{0};
    const auto watch = FolderWatch::create(folder.path(), [&] { ++calls; });
    ASSERT_NE(watch, nullptr);
    {
        std::ofstream file(folder.path() / "event7", std::ios::app);
        file << "more data";
    }
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(calls.load(), 0);
}

TEST(FolderWatch, NothingHappeningIsNoCall) {
    TempFolder folder;
    std::atomic<int> calls{0};
    const auto watch = FolderWatch::create(folder.path(), [&] { ++calls; });
    ASSERT_NE(watch, nullptr);
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(calls.load(), 0);
}

TEST(FolderWatch, EachChangeAfterTheLastCallIsCalledAgain) {
    TempFolder folder;
    std::atomic<int> calls{0};
    const auto watch = FolderWatch::create(folder.path(), [&] { ++calls; });
    ASSERT_NE(watch, nullptr);
    for (int i = 0; i < 3; ++i) {
        const int before = calls;
        touch(folder.path() / ("event" + std::to_string(i)));
        ASSERT_TRUE(eventually([&] { return calls > before; })) << "change " << i;
    }
}

TEST(FolderWatch, AFolderThatDoesNotExistCannotBeWatched) {
    TempFolder folder;
    std::atomic<int> calls{0};
    EXPECT_EQ(FolderWatch::create(folder.path() / "missing", [&] { ++calls; }), nullptr);
}

TEST(FolderWatch, AFileIsNotAFolder) {
    TempFolder folder;
    touch(folder.path() / "plain");
    EXPECT_EQ(FolderWatch::create(folder.path() / "plain", [] {}), nullptr);
}

TEST(FolderWatch, NothingToCallIsRefused) {
    TempFolder folder;
    EXPECT_EQ(FolderWatch::create(folder.path(), std::function<void()>{}), nullptr);
}

TEST(FolderWatch, ItEndsAtOnceAndCallsNothingAfterwards) {
    TempFolder folder;
    std::atomic<int> calls{0};
    auto watch = FolderWatch::create(folder.path(), [&] { ++calls; });
    ASSERT_NE(watch, nullptr);
    const auto start = Clock::now();
    watch.reset();   // blocked in poll: the stop pipe ends it
    EXPECT_LT(Clock::now() - start, 1s);
    touch(folder.path() / "event9");
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(calls.load(), 0);
}

TEST(FolderWatch, ItEndsAtOnceEvenRightAfterAChange) {
    TempFolder folder;
    std::atomic<int> calls{0};
    auto watch = FolderWatch::create(folder.path(), [&] { ++calls; });
    ASSERT_NE(watch, nullptr);
    touch(folder.path() / "event1");
    const auto start = Clock::now();
    watch.reset();
    EXPECT_LT(Clock::now() - start, 1s);
}

TEST(FolderWatch, TheFolderGoingAwayEndsTheWatchQuietly) {
    // The watched folder deleted: inotify drops the watch; nothing hangs and the destructor still ends it.
    std::atomic<int> calls{0};
    std::unique_ptr<FolderWatch> watch;
    {
        TempFolder folder;
        watch = FolderWatch::create(folder.path(), [&] { ++calls; });
        ASSERT_NE(watch, nullptr);
    }
    std::this_thread::sleep_for(50ms);
    const auto start = Clock::now();
    watch.reset();
    EXPECT_LT(Clock::now() - start, 1s);
}

#else

TEST(FolderWatch, IsNotMadeOnThisSystem) {
    TempFolder folder;
    EXPECT_EQ(FolderWatch::create(folder.path(), [] {}), nullptr);
}

#endif

// --- DeviceChanges: one per process ---

TEST(DeviceChanges, IsOnePerProcessAndItsCounterWorks) {
    platform::DeviceChanges& changes = platform::DeviceChanges::shared();
    EXPECT_EQ(&changes, &platform::DeviceChanges::shared());
    ChangeCounter& counter = changes.counter();
    const std::uint64_t before = counter.count();
    counter.announce();   // what the system's notice does
    EXPECT_EQ(counter.count(), before + 1);
    EXPECT_TRUE(counter.waitForChange(before, 0));
}

}  // namespace
}  // namespace wheelbri::service
