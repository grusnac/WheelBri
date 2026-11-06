// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <ctime>
#include <optional>
#include <string>

namespace wheelbri {

// Calendar days in the local time zone, as "YYYY-MM-DD" (sorts in date order as text).
// Used to split logs and telemetry into one file per day and to find old files.

using WallClock = std::chrono::system_clock;

[[nodiscard]] std::string localDate(WallClock::time_point time);
// `time` broken down in the local time zone (localtime_s on Windows, localtime_r elsewhere: both thread-safe).
[[nodiscard]] std::tm localTime(std::time_t time);
[[nodiscard]] std::string localDate(std::time_t time);

// The day `days` after (or before, if negative) `date`. Returns nullopt if `date` is not a valid date.
[[nodiscard]] std::optional<std::string> addDays(const std::string& date, int days);

// Local midnight at the start of `date` (handles daylight saving: some days last 23 or 25 hours).
[[nodiscard]] std::optional<WallClock::time_point> startOfDay(const std::string& date);

// "2026-09-30" or "20260930" found in `text` (the first one), normalised to "2026-09-30".
[[nodiscard]] std::optional<std::string> findDate(const std::string& text);

}  // namespace wheelbri
