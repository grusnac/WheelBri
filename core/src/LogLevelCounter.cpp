// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/LogLevelCounter.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>

namespace wheelbri {

void LogLevelCounter::setFile(const std::filesystem::path& path) {
    if (path == path_) return;
    path_ = path;
    startOver();
}

void LogLevelCounter::startOver() {
    offset_ = 0;
    partial_.clear();
    count_ = LogLevelCount{};
}

bool LogLevelCounter::poll() {
    if (path_.empty()) return false;
    const LogLevelCount before = count_;
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path_, ec);
    if (ec || size < offset_) {
        // Gone, or shorter than what was read (replaced): what it says now is all there is.
        startOver();
        if (ec) return before.any();
    }
    if (size > offset_) {
        std::ifstream in(path_, std::ios::binary);
        if (in) {
            in.seekg(static_cast<std::streamoff>(offset_));
            std::string chunk(static_cast<std::size_t>(std::min(size - offset_, kMaxReadBytes)), '\0');
            in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            chunk.resize(static_cast<std::size_t>(in.gcount()));
            offset_ += chunk.size();
            std::string text = std::move(partial_);
            text += chunk;
            partial_.clear();
            std::size_t start = 0;
            for (std::size_t end = text.find('\n'); end != std::string::npos; end = text.find('\n', start)) {
                take(text.substr(start, end - start));
                start = end + 1;
            }
            partial_ = text.substr(start);
            if (partial_.size() > LiveLog::kMaxLineBytes) {   // a line that never ends: counted as it is
                take(partial_);
                partial_.clear();
            }
        }
    }
    return count_.warnings != before.warnings || count_.errors != before.errors;
}

void LogLevelCounter::take(const std::string& line) {
    const LiveLogLine parsed = parseLogLine(line, 0);
    if (parsed.level == LiveLogLevel::Warn) {
        ++count_.warnings;
    } else if (parsed.level == LiveLogLevel::Error) {
        ++count_.errors;
    } else {
        return;
    }
    count_.lastTime = parsed.time;
    count_.lastMessage = parsed.message;
}

}  // namespace wheelbri
