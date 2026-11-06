// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What a decoded session says: its pass/fail checks, summary.md, the day's report.md; and the two steps the capture
// sessions run on raw captures (locate, keep).

#include "g923cap/Analysis.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wheelbri::g923cap {

using Check = std::pair<bool, std::string>;

// The session's pass criteria, by its kind (the folder's S1..S5 prefix); a rehearsal (G923_TARGET) checks only what
// any wheel shows.
[[nodiscard]] std::vector<Check> checksOf(const SessionResult& result);

// `featureNames`: HID++ feature ids known from other sessions (S1 has G HUB's discovery), by index.
[[nodiscard]] std::string summaryMarkdown(const SessionResult& result,
                                          const std::map<int, std::uint16_t>& featureNames = {});
[[nodiscard]] std::string dayReport(const std::vector<SessionResult>& results,
                                    const std::map<int, std::uint16_t>& featureMap);

// A session folder, or every session folder (S1..S5) in a day folder: decodes, writes summary.md (and stream.tsv),
// and for a day report.md. The results come back.
[[nodiscard]] std::vector<SessionResult> decode(const std::filesystem::path& path,
                                                std::optional<int> address = std::nullopt);

// Where the wheel is in short captures of each root hub.
struct Located {
    std::filesystem::path file;
    std::uint16_t bus = 0;
    std::uint16_t address = 0;
    std::uint16_t product = 0;
};
[[nodiscard]] std::vector<Located> locate(const std::vector<std::filesystem::path>& captures);

// Rewrites each raw-*.pcap in a session folder as capture[-N].pcap with only the wheel's frames (every address it
// had: it can enumerate again in a session), then deletes the raw file: another device on the same hub (a keyboard)
// must not stay in the capture. A raw file without the wheel is deleted, but only when another raw file has it; if
// none has, nothing is deleted. Lines saying what was done come back.
[[nodiscard]] std::vector<std::string> keepWheelOnly(const std::filesystem::path& folder);

}  // namespace wheelbri::g923cap
