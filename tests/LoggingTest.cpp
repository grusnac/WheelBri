// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/Logging.hpp"
#include "wheelbri/LocalDate.hpp"
#include "wheelbri/LogFiles.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <cstdlib>
#include <unistd.h>
#endif

namespace wheelbri {
namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

// For a test that sets logging up and shuts it down: the loggers that already exist are put back as they were when
// it ends (their sinks and level, and in spdlog's registry). Code keeps its logger in a function-local static
// (`log()`, fetched once): without this, init() points those loggers at this test's file and shutdown() drops them
// from the registry, so every later test that captures a component's lines (LogLinesCapture) misses them, depending
// on which tests ran before.
class LoggersKept {
public:
    LoggersKept() {
        spdlog::apply_all([this](const logging::Logger& logger) {
            kept_.push_back({logger, logger->sinks(), logger->level()});
        });
    }
    ~LoggersKept() {
        for (const Kept& kept : kept_) {
            kept.logger->sinks() = kept.sinks;
            kept.logger->set_level(kept.level);
            if (spdlog::get(kept.logger->name()) != kept.logger) {
                spdlog::drop(kept.logger->name());   // one made after the shutdown under the same name
                spdlog::register_logger(kept.logger);
            }
        }
    }
    LoggersKept(const LoggersKept&) = delete;
    LoggersKept& operator=(const LoggersKept&) = delete;

private:
    struct Kept {
        logging::Logger logger;
        std::vector<spdlog::sink_ptr> sinks;
        spdlog::level::level_enum level;
    };
    std::vector<Kept> kept_;
};

TEST(Logging, DatedFileNames) {
    EXPECT_EQ(logging::datedFileName("wheelbri-service.log", "2026-09-30"), "wheelbri-service-2026-09-30.log");
    EXPECT_EQ(std::filesystem::path(logging::datedFileName("/logs/x.log", "2026-09-30")),
              std::filesystem::path("/logs/x-2026-09-30.log"));
}

TEST(RateLimiter, AllowsOncePerIntervalAndCountsDrops) {
    logging::RateLimiter limiter(std::chrono::milliseconds(50));
    std::uint64_t suppressed = 99;
    EXPECT_TRUE(limiter.allow(suppressed));
    EXPECT_EQ(suppressed, 0U);
    EXPECT_FALSE(limiter.allow(suppressed));
    EXPECT_FALSE(limiter.allow(suppressed));

    // Past the interval by the clock: on Windows sleep_for can return early (this one once took under 50 ms).
    const auto past = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);
    while (std::chrono::steady_clock::now() < past) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(limiter.allow(suppressed));
    EXPECT_EQ(suppressed, 2U);
}

TEST(CallSampler, FirstNThenEveryNth) {
    logging::CallSampler sampler(3, 10);
    std::vector<std::uint64_t> logged;
    for (int i = 0; i < 25; ++i) {
        if (const auto call = sampler.next(); call != 0) {
            logged.push_back(call);
        }
    }
    EXPECT_EQ(logged, (std::vector<std::uint64_t>{1, 2, 3, 10, 20}));
}

TEST(LogConfig, LevelNames) {
    EXPECT_TRUE(logging::isValidLevel("trace"));
    EXPECT_TRUE(logging::isValidLevel("off"));
    EXPECT_FALSE(logging::isValidLevel("verbose"));
    EXPECT_FALSE(logging::setLevel("verbose"));
}

TEST(Logging, WritesComponentLevelMessageAndSourceToFile) {
    const LoggersKept kept;   // put back the loggers other tests use
    const auto path = std::filesystem::temp_directory_path() / "wheelbri-logging-test" / "test.log";
    std::error_code ignored;
    std::filesystem::remove_all(path.parent_path(), ignored);

    logging::LogConfig config;
    config.level = "debug";
    config.console = false;
    config.flushLevel = "trace";
    config.periodicFlush = false;
    ASSERT_TRUE(logging::init(config, path.string()));

    {
        // Scoped so the last reference to the logger (and its file sink) is gone before the file is
        // read and deleted: Windows cannot delete a file that is still open.
        const auto logger = logging::get("test.component");
        WE_LOG_DEBUG(logger, "debug value {}", 42);
        WE_LOG_TRACE(logger, "trace is below the configured level");

        logging::RateLimiter limiter(std::chrono::seconds(60));
        WE_LOG_WARN_LIMITED(logger, limiter, "limited {}", "first");
        WE_LOG_WARN_LIMITED(logger, limiter, "limited {}", "second");
    }
    const std::string dated = logging::currentLogFile();
    logging::shutdown();

    // One file per run and day: "test.log" is written as "test-YYYY-MM-DD-NN.log".
    EXPECT_EQ(std::filesystem::path(dated).parent_path(), path.parent_path());
    EXPECT_EQ(std::filesystem::path(dated).filename().string().rfind("test-", 0), 0U);
    EXPECT_EQ(dated.size(), path.string().size() + 14);
    const auto parts = logfiles::partsOf(std::filesystem::path(dated).filename().string(), "test");
    ASSERT_TRUE(parts.has_value()) << dated;
    EXPECT_GE(parts->index, 1);
    const std::string text = readFile(dated);
    EXPECT_NE(text.find("[test.component"), std::string::npos);
    EXPECT_NE(text.find("[debug]"), std::string::npos);
    EXPECT_NE(text.find("debug value 42"), std::string::npos);
    EXPECT_NE(text.find("LoggingTest.cpp:"), std::string::npos);        // source location
    EXPECT_EQ(text.find("trace is below"), std::string::npos);
    EXPECT_NE(text.find("limited first (0 similar suppressed)"), std::string::npos);
    EXPECT_EQ(text.find("limited second"), std::string::npos);
    std::filesystem::remove_all(path.parent_path(), ignored);
}

TEST(Logging, UnwritableFileFallsBackToConsole) {
    const LoggersKept kept;   // put back the loggers other tests use
    logging::LogConfig config;
    config.console = false;
    config.periodicFlush = false;
    // A path below an existing regular file can never be created.
    const auto blocker = std::filesystem::temp_directory_path() / "wheelbri-logging-blocker";
    std::ofstream(blocker) << "x";
    EXPECT_FALSE(logging::init(config, (blocker / "sub" / "x.log").string()));
    WE_LOG_ERROR(logging::get("test.fallback"), "still reaches stderr");
    logging::shutdown();
    std::error_code ignored;
    std::filesystem::remove(blocker, ignored);
}

// A listener (the app's warning counter) gets every message logged after it was added, with its level and
// component; with no file and no console the messages still go somewhere (stderr), and a level or flush level that
// is not a level is warn.
TEST(Logging, ListenersGetMessagesAndNothingConfiguredStillLogs) {
    const LoggersKept kept;   // put back the loggers other tests use
    logging::LogConfig config;
    config.level = "loud";
    config.flushLevel = "sometimes";
    config.console = false;
    config.periodicFlush = true;
    EXPECT_TRUE(logging::init(config, ""));   // no file asked for: nothing failed
    EXPECT_EQ(logging::currentLogFile(), "");
    EXPECT_EQ(logging::currentLevel(), spdlog::level::warn);
    struct Heard {
        spdlog::level::level_enum level;
        std::string component;
        std::string message;
    };
    std::vector<Heard> heard;
    const auto logger = logging::get("test.listened");
    logging::addListener([&heard](const spdlog::level::level_enum level, const std::string& component,
                                  const std::string& message) { heard.push_back({level, component, message}); });
    WE_LOG_WARN(logger, "a warning {}", 1);
    WE_LOG_INFO(logger, "below the level");
    WE_LOG_ERROR(logging::get("test.later"), "from a logger made after the listener");
    logging::shutdown();
    ASSERT_EQ(heard.size(), 2U);
    EXPECT_EQ(heard[0].level, spdlog::level::warn);
    EXPECT_EQ(heard[0].component, "test.listened");
    EXPECT_EQ(heard[0].message, "a warning 1");
    EXPECT_EQ(heard[1].component, "test.later");
}

// The config's own file wins over the program's default one; with the day's numbers used up the file is the old
// daily name instead, so logging never stops for want of a number.
TEST(Logging, TheConfigsFileWinsAndNoFreeNumberFallsBackToTheDailyName) {
    const LoggersKept kept;   // put back the loggers other tests use
    const auto dir = std::filesystem::temp_directory_path() / "wheelbri-logging-ownfile";
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
    std::filesystem::create_directories(dir);
    const std::string today = localDate(WallClock::now());
    std::ofstream(dir / ("own-" + today + "-99999.log")) << "the last number";
    logfiles::forgetRun();   // this run has no number for today yet: the next free one would be 100000
    logging::LogConfig config;
    config.filePath = (dir / "own.log").string();
    config.console = true;
    config.level = "warn";
    config.periodicFlush = false;
    ASSERT_TRUE(logging::init(config, (dir / "default.log").string()));
    const std::string file = logging::currentLogFile();
    WE_LOG_WARN(logging::get("test.own"), "into the daily file");
    logging::shutdown();
    logfiles::forgetRun();
    EXPECT_EQ(std::filesystem::path(file), dir / ("own-" + today + ".log"));
    EXPECT_NE(readFile(file).find("into the daily file"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(dir / ("default-" + today + "-01.log")));
    std::filesystem::remove_all(dir, ignored);
}

// A sampler with no "every N" logs only its first calls.
TEST(CallSampler, WithoutEveryNOnlyTheFirstCalls) {
    logging::CallSampler sampler(2, 0);
    std::vector<std::uint64_t> logged;
    for (int i = 0; i < 10; ++i) {
        if (const auto call = sampler.next(); call != 0) logged.push_back(call);
    }
    EXPECT_EQ(logged, (std::vector<std::uint64_t>{1, 2}));
}

// The level is set by name only for real level names.
TEST(LogConfig, SetLevelTakesOnlyLevelNames) {
    const LoggersKept kept;   // put back the loggers other tests use
    logging::LogConfig config;
    config.console = false;
    config.periodicFlush = false;
    ASSERT_TRUE(logging::init(config, ""));
    EXPECT_TRUE(logging::setLevel("error"));
    EXPECT_EQ(logging::currentLevel(), spdlog::level::err);
    EXPECT_TRUE(logging::setLevel("warn"));
    EXPECT_EQ(logging::currentLevel(), spdlog::level::warn);
    EXPECT_FALSE(logging::setLevel("WARN"));
    EXPECT_FALSE(logging::setLevel(""));
    EXPECT_EQ(logging::currentLevel(), spdlog::level::warn);
    logging::shutdown();
}

#ifndef _WIN32
// A log file named without a folder goes in the current folder, no folder made for it. In a child process, whose
// current folder and logging can change without touching the tests'.
TEST(LoggingDeathTest, ABareFileNameIsWrittenWhereWeAre) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const auto dir = std::filesystem::temp_directory_path() / "wheelbri-logging-bare";
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
    std::filesystem::create_directories(dir);
    const std::string folder = dir.string();
    EXPECT_EXIT(
        {
            logging::LogConfig config;
            config.console = false;
            const bool ok = ::chdir(folder.c_str()) == 0 && logging::init(config, "bare.log");
            WE_LOG_INFO(logging::get("test.bare"), "here");
            logging::shutdown();
            std::exit(ok ? 0 : 1);
        },
        ::testing::ExitedWithCode(0), "");
    bool found = false;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        found = found || entry.path().filename().string().rfind("bare", 0) == 0;
    }
    EXPECT_TRUE(found);
    std::filesystem::remove_all(dir, ignored);
}
#endif

}  // namespace
}  // namespace wheelbri
