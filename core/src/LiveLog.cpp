#include "wheelemu/LiveLog.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>

namespace wheelemu {

namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

// "[...]" at the start of `text`: its inside, and `text` moved past it and the spaces after it.
bool takeBracket(std::string_view& text, std::string_view& inside) {
    if (text.empty() || text.front() != '[') return false;
    const auto close = text.find(']');
    if (close == std::string_view::npos) return false;
    inside = text.substr(1, close - 1);
    text.remove_prefix(close + 1);
    text = trim(text);
    return true;
}

LiveLogLevel levelFrom(const std::string_view name) {
    if (name == "trace") return LiveLogLevel::Trace;
    if (name == "debug") return LiveLogLevel::Debug;
    if (name == "info") return LiveLogLevel::Info;
    if (name == "warn" || name == "warning") return LiveLogLevel::Warn;
    if (name == "error" || name == "critical") return LiveLogLevel::Error;
    return LiveLogLevel::Unknown;
}

// "2026-10-02 22:49:33.488" -> "22:49:33.488".
bool isTimeStamp(const std::string_view stamp) {
    return stamp.size() >= 19 && stamp[4] == '-' && stamp[7] == '-' && stamp[10] == ' ' && stamp[13] == ':' &&
           stamp[16] == ':';
}

}  // namespace

LiveLogLine parseLogLine(std::string_view line, const int source) {
    LiveLogLine parsed;
    parsed.source = source;
    line = trim(line);
    std::string_view rest = line;
    std::string_view stamp;
    if (!takeBracket(rest, stamp) || !isTimeStamp(stamp)) {
        parsed.message = std::string(line);
        return parsed;
    }
    parsed.time = std::string(stamp.substr(11));

    // spdlog: [level] [logger] [tid N] message  (File.cpp:12)
    std::string_view level;
    std::string_view probe = rest;
    if (takeBracket(probe, level) && levelFrom(trim(level)) != LiveLogLevel::Unknown) {
        parsed.level = levelFrom(trim(level));
        rest = probe;
        std::string_view logger;
        if (takeBracket(probe, logger)) {
            parsed.logger = std::string(trim(logger));
            rest = probe;
            std::string_view thread;
            if (takeBracket(probe, thread) && thread.substr(0, 4) == "tid ") {
                rest = probe;
            }
        }
        // The source location at the end: two spaces, then "(File.cpp:12)".
        if (const auto at = rest.rfind("  ("); at != std::string_view::npos && rest.back() == ')') {
            rest = trim(rest.substr(0, at));
        }
    } else {
        // The drivers and the bridge: no level, so read one from the words they use.
        const std::string lower = [&] {
            std::string s(rest.substr(0, 24));
            std::transform(s.begin(), s.end(), s.begin(), [](const unsigned char c) { return std::tolower(c); });
            return s;
        }();
        parsed.level = lower.find("error") != std::string::npos || lower.find("failed") != std::string::npos
                           ? LiveLogLevel::Error
                       : lower.find("warn") != std::string::npos ? LiveLogLevel::Warn
                                                                 : LiveLogLevel::Info;
    }
    parsed.message = std::string(rest);
    return parsed;
}

void LiveLog::setFile(const int source, const std::filesystem::path& path) {
    const auto it = std::find_if(files_.begin(), files_.end(), [source](const File& f) { return f.source == source; });
    if (path.empty()) {
        if (it != files_.end()) files_.erase(it);
        return;
    }
    if (it != files_.end()) {
        if (it->path == path) return;
        *it = File{source, path, 0, false, {}};
        return;
    }
    files_.push_back(File{source, path, 0, false, {}});
}

bool LiveLog::poll() {
    std::vector<LiveLogLine> fresh;
    bool initial = false;
    for (File& file : files_) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(file.path, ec);
        if (ec) continue;   // not written yet today
        if (size < file.offset) {
            // Shorter than before: cut or replaced. Start again from its tail.
            file.offset = 0;
            file.started = false;
            file.partial.clear();
        }
        bool skipFirst = false;
        if (!file.started) {
            file.started = true;
            initial = true;
            if (size > kInitialTailBytes) {
                file.offset = size - kInitialTailBytes;
                skipFirst = true;   // it starts in the middle of a line
            }
        }
        if (size == file.offset) continue;
        const std::uintmax_t count = std::min(size - file.offset, kMaxReadBytes);
        std::ifstream in(file.path, std::ios::binary);
        if (!in) continue;
        in.seekg(static_cast<std::streamoff>(file.offset));
        std::string chunk(static_cast<std::size_t>(count), '\0');
        in.read(chunk.data(), static_cast<std::streamsize>(count));
        chunk.resize(static_cast<std::size_t>(in.gcount()));
        file.offset += chunk.size();

        std::string text = std::move(file.partial);
        text += chunk;
        file.partial.clear();
        std::size_t start = 0;
        while (true) {
            const auto end = text.find('\n', start);
            if (end == std::string::npos) {
                file.partial = text.substr(start);
                // A "line" that never ends (a binary file, a program writing without newlines) is shown cut
                // rather than kept and copied again on every poll.
                if (file.partial.size() > kMaxLineBytes) {
                    if (!skipFirst) fresh.push_back(parseLogLine(file.partial.substr(0, kMaxLineBytes), file.source));
                    file.partial.clear();
                    skipFirst = false;
                }
                break;
            }
            const std::string_view line(text.data() + start, end - start);
            if (skipFirst) {
                skipFirst = false;
            } else if (!trim(line).empty()) {
                fresh.push_back(parseLogLine(line, file.source));
            }
            start = end + 1;
        }
    }
    if (fresh.empty()) return false;
    if (initial) {
        // The files' tails, read together: put them in time order (stamps are the same format in every file).
        std::stable_sort(fresh.begin(), fresh.end(),
                         [](const LiveLogLine& a, const LiveLogLine& b) { return a.time < b.time; });
    }
    for (LiveLogLine& line : fresh) add(std::move(line));
    return true;
}

void LiveLog::add(LiveLogLine line) {
    lines_.push_back(std::move(line));
    ++added_;
    while (lines_.size() > kMaxLines) lines_.pop_front();
}

}  // namespace wheelemu
