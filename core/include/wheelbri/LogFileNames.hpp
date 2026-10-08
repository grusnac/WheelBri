#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wheelbri::logfiles {

// The names of WheelBri's log files: "<stem>-<date>-<NN><extension>" ("wheelbri-service-2026-10-08-03.log"), a new
// number every time a component starts (and at midnight for one that runs on): one run, one file (2026-10-08, the
// user's choice). Before that a component wrote one file per day, "<stem>-<date>.log"; those read as number 0.
//
// Header only and free of the file system, so the drivers (driver/, MSVC + WDK) use the same rules as everything
// else; the files themselves are in LogFiles.hpp.

inline constexpr int kMaxIndex = 99'999;

// "wheelbri-service", "2026-10-08", 3 -> "wheelbri-service-2026-10-08-03.log" (at least two digits).
[[nodiscard]] inline std::string numberedName(const std::string_view stem, const std::string_view date, const int index,
                                              const std::string_view extension = ".log") {
    std::string number = std::to_string(index < 0 ? 0 : index);
    if (number.size() < 2) number.insert(0, 2 - number.size(), '0');
    std::string name;
    name.reserve(stem.size() + date.size() + number.size() + extension.size() + 2);
    name.append(stem).append("-").append(date).append("-").append(number).append(extension);
    return name;
}

// Whether `date` looks like "YYYY-MM-DD" (digits and dashes in place; the calendar is not checked).
[[nodiscard]] inline bool isDateText(const std::string_view date) {
    if (date.size() != 10 || date[4] != '-' || date[7] != '-') return false;
    for (std::size_t i = 0; i < date.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (date[i] < '0' || date[i] > '9') return false;
    }
    return true;
}

// One of WheelBri's log names taken apart.
struct NameParts {
    std::string date;   // "2026-10-08"
    int index = 0;      // 0: the old daily name without a number
};

// `name` as one of `stem`'s logs: "<stem>-<date>-<NN>.<anything>" or the old "<stem>-<date>.<anything>" (any
// extension, so archives such as ".log.xz" count too). Nullopt for any other name (another stem, "<stem>-x-...", a
// number with signs or too large).
[[nodiscard]] inline std::optional<NameParts> partsOf(const std::string_view name, const std::string_view stem) {
    if (name.size() <= stem.size() + 11 || name.substr(0, stem.size()) != stem || name[stem.size()] != '-') {
        return std::nullopt;
    }
    std::string_view rest = name.substr(stem.size() + 1);
    if (!isDateText(rest.substr(0, 10))) return std::nullopt;
    NameParts parts;
    parts.date = std::string(rest.substr(0, 10));
    rest.remove_prefix(10);
    if (rest.front() == '.') return parts;   // the old daily name
    if (rest.front() != '-') return std::nullopt;
    rest.remove_prefix(1);
    std::size_t digits = 0;
    long long value = 0;
    while (digits < rest.size() && rest[digits] >= '0' && rest[digits] <= '9') {
        value = value * 10 + (rest[digits] - '0');
        if (value > kMaxIndex) return std::nullopt;
        ++digits;
    }
    if (digits == 0 || digits == rest.size() || rest[digits] != '.') return std::nullopt;
    parts.index = static_cast<int>(value);
    return parts;
}

// The number a new run of `stem` takes on `date`, given the names in the folder: one more than the highest there
// (a gap left by a deleted file is not filled, so numbers only grow within a day); 1 when there is none.
[[nodiscard]] inline int nextIndex(const std::vector<std::string>& names, const std::string_view stem,
                                   const std::string_view date) {
    int highest = 0;
    for (const std::string& name : names) {
        const std::optional<NameParts> parts = partsOf(name, stem);
        if (parts && parts->date == date && parts->index > highest) highest = parts->index;
    }
    return highest < kMaxIndex ? highest + 1 : kMaxIndex;
}

// Whether a file of `stem` with number `index` on `date` is among `names` (with any extension).
[[nodiscard]] inline bool hasIndex(const std::vector<std::string>& names, const std::string_view stem,
                                   const std::string_view date, const int index) {
    for (const std::string& name : names) {
        const std::optional<NameParts> parts = partsOf(name, stem);
        if (parts && parts->date == date && parts->index == index) return true;
    }
    return false;
}

// The newest of `stem`'s logs among `names`: the latest date, then the highest number; a plain ".log" before an
// archive of the same run. Nullopt when there is none.
[[nodiscard]] inline std::optional<std::string> newestOf(const std::vector<std::string>& names,
                                                         const std::string_view stem) {
    std::optional<std::string> best;
    NameParts bestParts;
    for (const std::string& name : names) {
        const std::optional<NameParts> parts = partsOf(name, stem);
        if (!parts) continue;
        const bool plain = name.size() >= 4 && name.compare(name.size() - 4, 4, ".log") == 0;
        bool better = !best || parts->date > bestParts.date ||
                      (parts->date == bestParts.date && parts->index > bestParts.index);
        if (best && parts->date == bestParts.date && parts->index == bestParts.index) {
            const bool bestPlain = best->size() >= 4 && best->compare(best->size() - 4, 4, ".log") == 0;
            better = plain && !bestPlain;
        }
        if (better) {
            best = name;
            bestParts = *parts;
        }
    }
    return best;
}

}  // namespace wheelbri::logfiles
