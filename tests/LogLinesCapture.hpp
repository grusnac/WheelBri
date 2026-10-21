#pragma once

// For tests: what one component logs while this object lives (every level), as "level: message" lines.
//
// It adds a sink to the component's logger in spdlog's registry, which is the one the code keeps in its
// function-local static (`log()`) as long as no test leaves logging shut down: LoggingTest's own set-up and
// shut-down put the existing loggers back (LoggersKept), so this sees the lines whatever ran before.

#include "wheelbri/Logging.hpp"

#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>

namespace wheelbri::testing_support {

class LogLinesCapture {
public:
    explicit LogLinesCapture(const std::string& component)
        : logger_(logging::get(component)), sink_(std::make_shared<spdlog::sinks::ostream_sink_mt>(stream_)),
          level_(logger_->level()) {
        sink_->set_pattern("%l: %v");
        sink_->set_level(spdlog::level::trace);
        logger_->sinks().push_back(sink_);
        logger_->set_level(spdlog::level::trace);
    }
    ~LogLinesCapture() {
        auto& sinks = logger_->sinks();
        sinks.erase(std::remove(sinks.begin(), sinks.end(), sink_), sinks.end());
        logger_->set_level(level_);
    }
    LogLinesCapture(const LogLinesCapture&) = delete;
    LogLinesCapture& operator=(const LogLinesCapture&) = delete;

    [[nodiscard]] std::string text() {
        logger_->flush();
        return stream_.str();
    }
    // Whether a line holds `part`.
    [[nodiscard]] bool has(const std::string& part) { return text().find(part) != std::string::npos; }

private:
    std::ostringstream stream_;
    logging::Logger logger_;
    std::shared_ptr<spdlog::sinks::ostream_sink_mt> sink_;
    spdlog::level::level_enum level_;
};

}  // namespace wheelbri::testing_support
