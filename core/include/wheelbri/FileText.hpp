// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace wheelbri {

// Text files and paths, done the same way everywhere (rigs, configs, app.ini, the shim's games list).

// Paths as UTF-8 text and back (folder names are often not ASCII). Neither throws: text that is not valid UTF-8, or a
// path that cannot be converted, gives an empty result.
[[nodiscard]] std::string pathToUtf8(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path pathFromUtf8(const std::string& text);

// The whole file, or nothing when it cannot be read.
[[nodiscard]] std::optional<std::string> readFileText(const std::filesystem::path& path);

// Replaces `path` with `text` so that it is never left half written: the text goes to `<path>.tmp` first, which is
// renamed over `path` only once it is complete. The folder is created when missing. False (and `path` untouched)
// if anything fails. A link at `path` is replaced by the file, never followed.
bool writeFileReplacing(const std::filesystem::path& path, std::string_view text);

// Calls `visit` for each entry of `folder` (not recursive). Never throws: an unreadable folder or entry ends the
// listing early; a name that cannot be converted is skipped.
void forEachEntry(const std::filesystem::path& folder,
                  const std::function<void(const std::filesystem::directory_entry&)>& visit);

// The bytes kept from `text` when it may hold at most `maxBytes`: cut at a UTF-8 character boundary, so a
// multi-byte character is never split.
[[nodiscard]] std::string truncateUtf8(std::string text, std::size_t maxBytes);

}  // namespace wheelbri
