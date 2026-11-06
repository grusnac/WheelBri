// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/LocalDate.hpp"

#include <cctype>
#include <cstdio>

namespace wheelbri {

std::tm localTime(const std::time_t time) {
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    return local;
}

namespace {

std::tm toLocal(const std::time_t time) { return localTime(time); }

struct Ymd {
    int year = 0;
    int month = 0;
    int day = 0;
};

std::optional<Ymd> parse(const std::string& date) {
    Ymd ymd;
    char dash1 = 0;
    char dash2 = 0;
    if (date.size() != 10 || std::sscanf(date.c_str(), "%4d%c%2d%c%2d", &ymd.year, &dash1, &ymd.month, &dash2,
                                         &ymd.day) != 5 ||
        dash1 != '-' || dash2 != '-' || ymd.month < 1 || ymd.month > 12 || ymd.day < 1 || ymd.day > 31) {
        return std::nullopt;
    }
    return ymd;
}

// mktime for a local calendar time; tm_isdst = -1 lets the C library decide daylight saving.
std::time_t makeLocal(const Ymd& ymd, const int hour) {
    std::tm tm{};
    tm.tm_year = ymd.year - 1900;
    tm.tm_mon = ymd.month - 1;
    tm.tm_mday = ymd.day;
    tm.tm_hour = hour;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

bool digits(const std::string& text, const std::size_t pos, const std::size_t count) {
    for (std::size_t i = pos; i < pos + count; ++i) {
        if (i >= text.size() || std::isdigit(static_cast<unsigned char>(text[i])) == 0) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string localDate(const std::time_t time) {
    const std::tm local = toLocal(time);
    char text[40];
    std::snprintf(text, sizeof(text), "%04d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    return text;
}

std::string localDate(const WallClock::time_point time) {
    return localDate(WallClock::to_time_t(time));
}

std::optional<std::string> addDays(const std::string& date, const int days) {
    const auto ymd = parse(date);
    if (!ymd) {
        return std::nullopt;
    }
    Ymd shifted = *ymd;
    shifted.day += days;  // mktime normalises out-of-range days; noon keeps clear of daylight-saving jumps
    return localDate(makeLocal(shifted, 12));
}

std::optional<WallClock::time_point> startOfDay(const std::string& date) {
    const auto ymd = parse(date);
    if (!ymd) {
        return std::nullopt;
    }
    return WallClock::from_time_t(makeLocal(*ymd, 0));
}

std::optional<std::string> findDate(const std::string& text) {
    for (std::size_t i = 0; i + 8 <= text.size(); ++i) {
        if (i > 0 && std::isdigit(static_cast<unsigned char>(text[i - 1])) != 0) {
            continue;  // must start a number
        }
        std::string candidate;
        if (digits(text, i, 4) && i + 10 <= text.size() && text[i + 4] == '-' && digits(text, i + 5, 2) &&
            text[i + 7] == '-' && digits(text, i + 8, 2)) {
            candidate = text.substr(i, 10);
        } else if (digits(text, i, 8) && !digits(text, i + 8, 1)) {
            candidate = text.substr(i, 4) + "-" + text.substr(i + 4, 2) + "-" + text.substr(i + 6, 2);
        }
        if (!candidate.empty() && parse(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

}  // namespace wheelbri
