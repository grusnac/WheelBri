#include "Housekeeping.hpp"

#include "Platform.hpp"
#include "wheelbri/Logging.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <vector>

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.files");
    return logger;
}

// UTF-8 for the process starter (which converts back on Windows), whatever the ANSI code page is.
std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

bool isFile(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

// The folders on PATH. Not when running as Administrator (only possible on Windows): PATH can hold folders any program
// of the user can write to, and a planted "xz.exe" would then run as Administrator. A standard user runs what is on
// their own PATH anyway.
std::vector<std::filesystem::path> pathFolders() {
    std::vector<std::filesystem::path> folders;
    if (platform::isElevated()) {
#if defined(_WIN32)
        return folders;
#endif
    }
#if defined(_WIN32)
    const wchar_t* const value = _wgetenv(L"PATH");
    const std::wstring path = value != nullptr ? value : L"";
    constexpr wchar_t kSeparator = L';';
#else
    const char* const value = std::getenv("PATH");
    const std::string path = value != nullptr ? value : "";
    constexpr char kSeparator = ':';
#endif
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const std::size_t end = path.find(kSeparator, begin);
        const auto folder = path.substr(begin, end == decltype(path)::npos ? decltype(path)::npos : end - begin);
        if (!folder.empty()) folders.emplace_back(folder);
        if (end == decltype(path)::npos) break;
        begin = end + 1;
    }
    return folders;
}

// First `name` (with .exe on Windows) in pathFolders().
std::optional<std::filesystem::path> onPath(const std::string& name) {
#if defined(_WIN32)
    const std::string file = name + ".exe";
#else
    const std::string& file = name;
#endif
    for (const std::filesystem::path& folder : pathFolders()) {
        const std::filesystem::path candidate = folder / file;
        if (isFile(candidate)) return candidate;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> findSevenZip() {
    for (const char* name : {"7z", "7zz", "7za"}) {
        if (auto found = onPath(name)) {
            return found;
        }
    }
#if defined(_WIN32)
    // 7-Zip's installer does not add itself to PATH: its own folder, from Windows (not from environment variables).
    for (const KNOWNFOLDERID& id : {FOLDERID_ProgramFilesX64, FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86}) {
        PWSTR root = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &root)) && root != nullptr) {
            const std::filesystem::path candidate = std::filesystem::path(std::wstring(root)) / "7-Zip" / "7z.exe";
            CoTaskMemFree(root);
            if (isFile(candidate)) return candidate;
        } else if (root != nullptr) {
            CoTaskMemFree(root);
        }
    }
#endif
    return std::nullopt;
}

std::optional<Archiver> find(const Archiver::Kind kind) {
    std::optional<std::filesystem::path> exe;
    switch (kind) {
        case Archiver::Kind::SevenZip: exe = findSevenZip(); break;
        case Archiver::Kind::Xz: exe = onPath("xz"); break;
        case Archiver::Kind::Zstd: exe = onPath("zstd"); break;
        case Archiver::Kind::InfoZip: exe = onPath("zip"); break;
        case Archiver::Kind::Gzip: exe = onPath("gzip"); break;
        case Archiver::Kind::WindowsZip:
#if defined(_WIN32)
            // Windows 10 1803+ ships bsdtar, which writes .zip; use it by full path, not an MSYS2 tar on PATH.
            if (const auto tar = platform::systemFolder() / "tar.exe"; isFile(tar)) {
                exe = tar;
            }
#endif
            break;
    }
    if (!exe) {
        return std::nullopt;
    }
    return Archiver{kind, *exe};
}

std::uint64_t totalSize(const std::vector<std::filesystem::path>& files) {
    std::uint64_t total = 0;
    for (const auto& file : files) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(file, ec);
        total += ec ? 0 : static_cast<std::uint64_t>(size);
    }
    return total;
}

std::string sizeText(const std::uint64_t bytes) {
    char text[32];
    if (bytes >= 1'000'000) {
        std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / 1e6);
    } else {
        std::snprintf(text, sizeof(text), "%.1f KB", static_cast<double>(bytes) / 1e3);
    }
    return text;
}

void removeAll(const std::vector<std::filesystem::path>& files) {
    for (const auto& file : files) {
        std::error_code ec;
        std::filesystem::remove(file, ec);
    }
}

}  // namespace

const char* Archiver::name() const {
    switch (kind) {
        case Kind::SevenZip: return "7z";
        case Kind::Xz: return "xz";
        case Kind::Zstd: return "zstd";
        case Kind::WindowsZip:
        case Kind::InfoZip: return "zip";
        case Kind::Gzip: return "gzip";
    }
    return "?";
}

std::optional<Archiver> findArchiver(const std::string& preference) {
    using Kind = Archiver::Kind;
    if (preference == "7z") return find(Kind::SevenZip);
    if (preference == "xz") return find(Kind::Xz);
    if (preference == "zstd") return find(Kind::Zstd);
    if (preference == "gzip") return find(Kind::Gzip);
    if (preference == "zip") {
        auto zip = find(Kind::WindowsZip);
        return zip ? zip : find(Kind::InfoZip);
    }
    for (const Kind kind : {Kind::SevenZip, Kind::Xz, Kind::Zstd, Kind::WindowsZip, Kind::InfoZip, Kind::Gzip}) {
        if (auto found = find(kind)) {
            return found;
        }
    }
    return std::nullopt;
}

Housekeeping::Housekeeping(RetentionConfig config, std::vector<std::filesystem::path> folders)
    : config_(std::move(config)), folders_(std::move(folders)) {}

Housekeeping::Housekeeping(RetentionConfig config, std::vector<std::filesystem::path> folders, Archiver archiver)
    : config_(std::move(config)), folders_(std::move(folders)), archiver_(std::move(archiver)),
      archiverSearched_(true) {}

Housekeeping::~Housekeeping() {
    abortRunning();
}

void Housekeeping::run(const WallClock::time_point now) {
    if (config_.action == OldFilesAction::Keep) {
        return;
    }
    // Today and the previous `olderThanDays` days stay as they are.
    const auto cutoff = addDays(localDate(now), -config_.olderThanDays);
    if (!cutoff) {
        return;
    }
    if (config_.action == OldFilesAction::Delete) {
        deleteOld(*cutoff);
    } else {
        queueArchives(*cutoff);
    }
}

void Housekeeping::deleteOld(const std::string& cutoff) {
    std::uint64_t deleted = 0;
    std::uint64_t bytes = 0;
    for (const auto& folder : folders_) {
        for (const auto& [day, files] : filesBefore(folder, cutoff, true)) {
            for (const auto& file : files) {
                std::error_code ec;
                const auto size = std::filesystem::file_size(file, ec);
                if (std::filesystem::remove(file, ec)) {
                    ++deleted;
                    bytes += ec ? 0 : static_cast<std::uint64_t>(size);
                } else if (ec) {
                    WE_LOG_WARN(log(), "cannot delete {}: {}", file.string(), ec.message());
                }
            }
        }
    }
    filesDeleted_ += deleted;
    if (deleted > 0) {
        WE_LOG_INFO(log(), "deleted {} file(s) from before {} ({})", deleted, cutoff, sizeText(bytes));
    }
}

void Housekeeping::queueArchives(const std::string& cutoff) {
    if (!archiverSearched_) {
        archiverSearched_ = true;
        archiver_ = findArchiver(config_.archiver);
        if (archiver_) {
            WE_LOG_INFO(log(), "old files are archived with {} ({})", archiver_->name(), archiver_->executable.string());
        }
    }
    std::size_t queued = 0;
    for (const auto& folder : folders_) {
        for (auto& [day, files] : filesBefore(folder, cutoff, false)) {
            if (!archiver_) {
                break;
            }
            bool alreadyQueued = running_ && running_->day == day;
            for (const Job& job : queue_) {
                alreadyQueued = alreadyQueued || job.day == day;
            }
            if (!alreadyQueued) {
                queue_.push_back(makeJob(day, folder, std::move(files)));
                ++queued;
            }
        }
    }
    if (!archiver_) {
        failed_ = true;
        lastProblem_ = config_.archiver == "auto" ? "no compressor found (install 7-Zip), old files kept"
                                                  : "'" + config_.archiver + "' not found, old files kept";
        WE_LOG_WARN(log(), "cannot archive old files: {}", lastProblem_);
        return;
    }
    if (queued > 0) {
        WE_LOG_INFO(log(), "archiving {} old day(s) from before {} in the background", queued, cutoff);
    }
    pump();
}

Housekeeping::Job Housekeeping::makeJob(const std::string& day, const std::filesystem::path& folder,
                                        std::vector<std::filesystem::path> files) const {
    Job job;
    job.day = day;
    job.inputs = std::move(files);
    const Archiver& a = *archiver_;
    job.args.push_back(utf8(a.executable));
    if (a.onePerDay()) {
        // One archive per day and folder; never overwrite an existing one.
        const std::string extension = a.kind == Archiver::Kind::SevenZip ? ".7z" : ".zip";
        std::filesystem::path archive = folder / ("wheelbri-logs-" + day + extension);
        std::error_code ec;   // an unreadable folder must not throw (it would end the session)
        for (int n = 2; std::filesystem::exists(archive, ec) && n < 1000; ++n) {
            archive = folder / ("wheelbri-logs-" + day + "-" + std::to_string(n) + extension);
        }
        job.outputs.push_back(archive);
        switch (a.kind) {
            case Archiver::Kind::SevenZip:
                // LZMA2 at the highest level on one thread, so a running game is not slowed down.
                job.args.insert(job.args.end(), {"a", "-t7z", "-mx=9", "-mmt=1", "-bd", "-y", utf8(archive)});
                for (const auto& file : job.inputs) {
                    job.args.push_back(utf8(file));
                }
                break;
            case Archiver::Kind::WindowsZip:
                // bsdtar picks the format from the .zip suffix (-a); -C keeps paths out of the archive.
                job.args.insert(job.args.end(), {"-a", "-c", "-f", utf8(archive), "-C", utf8(folder)});
                for (const auto& file : job.inputs) {
                    job.args.push_back(utf8(file.filename()));
                }
                break;
            default:  // Info-ZIP
                job.args.insert(job.args.end(), {"-9", "-j", "-q", utf8(archive)});
                for (const auto& file : job.inputs) {
                    job.args.push_back(utf8(file));
                }
                break;
        }
        return job;
    }
    // Each file compressed on its own, next to the original (kept until the compressor succeeded).
    std::string suffix;
    switch (a.kind) {
        case Archiver::Kind::Xz:
            job.args.insert(job.args.end(), {"-9e", "-T1", "-k", "-f"});
            suffix = ".xz";
            break;
        case Archiver::Kind::Zstd:
            job.args.insert(job.args.end(), {"-19", "--long=27", "-T1", "-q", "-f"});
            suffix = ".zst";
            break;
        default:  // gzip
            job.args.insert(job.args.end(), {"-9", "-k", "-f"});
            suffix = ".gz";
            break;
    }
    for (const auto& file : job.inputs) {
        job.args.push_back(utf8(file));
        job.outputs.push_back(std::filesystem::path(file.string() + suffix));
    }
    return job;
}

void Housekeeping::pump() {
    if (process_ != nullptr) {
        const std::optional<int> exitCode = process_->poll();
        if (!exitCode) {
            return;  // still compressing
        }
        process_.reset();
        finish(*exitCode);
    }
    if (!queue_.empty() && !failed_) {
        Job job = std::move(queue_.front());
        queue_.pop_front();
        start(std::move(job));
    }
}

void Housekeeping::start(Job job) {
    std::string error;
    process_ = platform::ChildProcess::start(job.args, error);
    if (process_ == nullptr) {
        failed_ = true;
        lastProblem_ = std::string("cannot start ") + archiver_->name() + ": " + error;
        WE_LOG_ERROR(log(), "{}; old files kept", lastProblem_);
        queue_.clear();
        return;
    }
    WE_LOG_DEBUG(log(), "archiving {} ({} file(s)) with {}", job.day, job.inputs.size(), archiver_->name());
    running_ = std::move(job);
}

void Housekeeping::finish(const int exitCode) {
    Job job = std::move(*running_);
    running_.reset();
    bool ok = exitCode == 0;
    for (const auto& output : job.outputs) {
        ok = ok && isFile(output);
    }
    if (!ok) {
        failed_ = true;
        lastProblem_ = std::string(archiver_->name()) + " failed on " + job.day + " (exit code " +
                       std::to_string(exitCode) + ")";
        WE_LOG_ERROR(log(), "{}; its files are kept and archiving stops until the next start", lastProblem_);
        removeAll(job.outputs);  // partial output; the originals are untouched
        queue_.clear();
        return;
    }
    const std::uint64_t before = totalSize(job.inputs);
    const std::uint64_t after = totalSize(job.outputs);
    removeAll(job.inputs);
    ++daysArchived_;
    WE_LOG_INFO(log(), "archived {}: {} file(s), {} -> {} ({:.0f}x smaller)", job.day, job.inputs.size(),
                sizeText(before), sizeText(after),
                after > 0 ? static_cast<double>(before) / static_cast<double>(after) : 0.0);
}

void Housekeeping::abortRunning() {
    if (process_ == nullptr) {
        return;
    }
    // Quitting mid-archive: stop the compressor and remove its partial output; the next start redoes the day.
    process_->kill();
    process_.reset();
    if (running_) {
        removeAll(running_->outputs);
        WE_LOG_INFO(log(), "archiving {} interrupted; it will be redone next time", running_->day);
        running_.reset();
    }
}

std::string Housekeeping::status() const {
    const std::string days = std::to_string(config_.olderThanDays);
    switch (config_.action) {
        case OldFilesAction::Keep:
            return "kept forever ([retention] old_files = keep)";
        case OldFilesAction::Delete:
            return "deleted after " + days + " days, " + std::to_string(filesDeleted_) +
                   " so far (or [retention] old_files = archive)";
        case OldFilesAction::Archive:
            break;
    }
    if (failed_) {
        return "archive after " + days + " days: " + lastProblem_;
    }
    std::string text = std::string("archived with ") + (archiver_ ? archiver_->name() : "?") + " after " + days +
                       " days";
    if (running_) {
        text += ": compressing " + running_->day + (queue_.empty() ? "" : ", " + std::to_string(queue_.size()) + " more");
    } else if (daysArchived_ > 0) {
        text += " (" + std::to_string(daysArchived_) + " day(s) archived)";
    }
    return text;
}

}  // namespace wheelbri::service
