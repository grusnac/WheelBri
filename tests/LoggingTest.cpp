#include "wheelemu/Logging.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>

namespace wheelemu {
namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

TEST(Logging, DatedFileNames) {
    EXPECT_EQ(logging::datedFileName("wheelemu-service.log", "2026-09-30"), "wheelemu-service-2026-09-30.log");
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

    std::this_thread::sleep_for(std::chrono::milliseconds(60));
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
    const auto path = std::filesystem::temp_directory_path() / "wheelemu-logging-test" / "test.log";
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

    // One file per day: "test.log" is written as "test-YYYY-MM-DD.log".
    EXPECT_EQ(std::filesystem::path(dated).parent_path(), path.parent_path());
    EXPECT_EQ(std::filesystem::path(dated).filename().string().rfind("test-", 0), 0U);
    EXPECT_EQ(dated.size(), path.string().size() + 11);
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
    logging::LogConfig config;
    config.console = false;
    config.periodicFlush = false;
    // A path below an existing regular file can never be created.
    const auto blocker = std::filesystem::temp_directory_path() / "wheelemu-logging-blocker";
    std::ofstream(blocker) << "x";
    EXPECT_FALSE(logging::init(config, (blocker / "sub" / "x.log").string()));
    WE_LOG_ERROR(logging::get("test.fallback"), "still reaches stderr");
    logging::shutdown();
    std::error_code ignored;
    std::filesystem::remove(blocker, ignored);
}

}  // namespace
}  // namespace wheelemu
