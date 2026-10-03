#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wheelbri {

// What happens to logs and telemetry older than `olderThanDays` days ([retention] in the service config,
// or --old-files on the command line). Default: delete everything older than 7 days.
enum class OldFilesAction {
    Keep,      // leave everything
    Delete,    // delete old files (and old archives)
    Archive,   // compress each old day into an archive, then delete the originals
};

[[nodiscard]] const char* toString(OldFilesAction action);
[[nodiscard]] std::optional<OldFilesAction> parseOldFilesAction(std::string_view text);

// Compressors the service can use, best compression first. "auto" picks the best one installed.
inline constexpr const char* kArchivers = "auto, 7z, xz, zstd, zip, gzip";
[[nodiscard]] bool isKnownArchiver(std::string_view name);

struct RetentionConfig {
    OldFilesAction action = OldFilesAction::Delete;
    int olderThanDays = 7;             // today and the previous `olderThanDays` days are never touched
    std::string archiver = "auto";
};

// Files the service manages in its folders: wheelbri-service*, wheelbri-shim*, wheelbri-bridge*, telemetry-*,
// ffb-* and the archives made from them; also the same files under the project's old name (wheelemu-*). Anything else in the folder is never touched.
[[nodiscard]] bool isManagedFile(const std::string& fileName);
[[nodiscard]] bool isArchive(const std::string& fileName);

// The day a file belongs to: the date in its name, or else the day it was last written.
[[nodiscard]] std::string fileDay(const std::filesystem::path& file);

// Managed files in `directory` from days before `cutoffDay`, grouped by day (oldest first).
// Archives are included only when `includeArchives` (deleting), so an archive is never archived again.
[[nodiscard]] std::map<std::string, std::vector<std::filesystem::path>> filesBefore(
    const std::filesystem::path& directory, const std::string& cutoffDay, bool includeArchives);

}  // namespace wheelbri
