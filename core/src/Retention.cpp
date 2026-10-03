#include "wheelbri/Retention.hpp"

#include "wheelbri/LocalDate.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <system_error>

namespace wheelbri {

namespace {

bool startsWith(const std::string& text, const std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& text, const std::string_view suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](const unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
    return text;
}

}  // namespace

const char* toString(const OldFilesAction action) {
    switch (action) {
        case OldFilesAction::Keep: return "keep";
        case OldFilesAction::Delete: return "delete";
        case OldFilesAction::Archive: return "archive";
    }
    return "?";
}

std::optional<OldFilesAction> parseOldFilesAction(const std::string_view text) {
    for (const auto action : {OldFilesAction::Keep, OldFilesAction::Delete, OldFilesAction::Archive}) {
        if (text == toString(action)) {
            return action;
        }
    }
    return std::nullopt;
}

bool isKnownArchiver(const std::string_view name) {
    constexpr std::array<std::string_view, 6> kKnown = {"auto", "7z", "xz", "zstd", "zip", "gzip"};
    return std::find(kKnown.begin(), kKnown.end(), name) != kKnown.end();
}

bool isManagedFile(const std::string& fileName) {
    const std::string name = lower(fileName);
    // wheelemu: the project's name before the rename; those files moved along with the folder.
    for (const std::string_view project : {std::string_view("wheelbri"), std::string_view("wheelemu")}) {
        const std::string prefix(project);
        // The app, the shim, the drivers (-driver, -hide), the installers (-install, -hide-install), Linux's
        // uhid and wheelbri.sh: every log is in the one folder.
        if (startsWith(name, prefix + "-service") || startsWith(name, prefix + "-shim") ||
            startsWith(name, prefix + "-bridge") || startsWith(name, prefix + "-logs-") ||
            startsWith(name, prefix + "-driver") || startsWith(name, prefix + "-hide") ||
            startsWith(name, prefix + "-install") || startsWith(name, prefix + "-uhid") ||
            startsWith(name, prefix + "-sh.") || startsWith(name, prefix + "-sh-")) {
            return true;
        }
    }
    return startsWith(name, "telemetry-") || startsWith(name, "ffb-");
}

bool isArchive(const std::string& fileName) {
    const std::string name = lower(fileName);
    return endsWith(name, ".7z") || endsWith(name, ".zip") || endsWith(name, ".xz") || endsWith(name, ".zst") ||
           endsWith(name, ".gz");
}

namespace {

// The file name as UTF-8. path::string() throws on Windows for a name the ANSI code page cannot show (an emoji or
// a foreign letter in some file in the log folder), which would stop the service: never throw here.
std::string nameOf(const std::filesystem::path& file) {
    try {
        const std::u8string name = file.filename().u8string();
        return std::string(name.begin(), name.end());
    } catch (...) {
        return {};
    }
}

}  // namespace

std::string fileDay(const std::filesystem::path& file) {
    if (const auto date = findDate(nameOf(file))) {
        return *date;
    }
    // No date in the name (e.g. an old "wheelbri-service.log"): the day it was last written.
    std::error_code ec;
    const auto written = std::filesystem::last_write_time(file, ec);
    if (ec) {
        return localDate(WallClock::now());  // unknown: treat as today, so it is never removed by mistake
    }
    const auto age = std::filesystem::file_time_type::clock::now() - written;
    return localDate(WallClock::now() - std::chrono::duration_cast<WallClock::duration>(age));
}

std::map<std::string, std::vector<std::filesystem::path>> filesBefore(const std::filesystem::path& directory,
                                                                        const std::string& cutoffDay,
                                                                        const bool includeArchives) {
    std::map<std::string, std::vector<std::filesystem::path>> days;
    std::error_code ec;
    // Advanced with an error code: a folder that changes or goes away during the scan ends it, never throws.
    std::filesystem::directory_iterator it(directory, ec);
    for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        const auto& entry = *it;
        std::error_code typeError;
        if (!entry.is_regular_file(typeError)) {
            continue;
        }
        const std::string name = nameOf(entry.path());
        if (name.empty()) {
            continue;
        }
        if (!isManagedFile(name) || (!includeArchives && isArchive(name))) {
            continue;
        }
        const std::string day = fileDay(entry.path());
        if (day < cutoffDay) {
            days[day].push_back(entry.path());
        }
    }
    for (auto& [day, files] : days) {
        std::sort(files.begin(), files.end());
    }
    return days;
}

}  // namespace wheelbri
