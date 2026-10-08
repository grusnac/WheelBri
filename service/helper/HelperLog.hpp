#pragma once

#include "wheelbri/DailyLogFile.hpp"

#include <spdlog/common.h>

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

namespace wheelbri::helper {

// The helper's log, in WheelBri's user's log folder with every other log
// (%LOCALAPPDATA%\wheelbri\logs\wheelbri-helper-<date>.log): the user's choice, 2026-10-06; it follows whoever
// last ran WheelBri (HelperServer.hpp: an accepted app, or WheelBri from another folder saying Hello).
//
// The helper runs as SYSTEM and that folder belongs to the user, who could put a link in it pointing anywhere. So
// the file is created and opened only while impersonating that user (with the token of the pipe's client): Windows
// then checks the user's own rights, and a link leads nowhere the user could not write anyway. The handle, once
// open, is written as SYSTEM.
//
// Before anyone has run WheelBri (the helper starts at boot) the lines wait in memory and are written when it does;
// a new file every day, opened with the same user's token; a file that cannot be opened or written is tried again
// a minute later, the lines waiting meanwhile (DailyLogFile.hpp). With --console the lines also go to the console
// (spdlog's own sink).
class HelperLog {
public:
    static constexpr std::size_t kPendingLines = 2000;
    static constexpr std::uint64_t kRetryMs = 60'000;

    static HelperLog& instance();

    // Hooks into wheelbri::logging (call once, after logging::init).
    void install();

    // From now on writes into `logsFolder` (UTF-16 path, created if missing) as the user of `token`, an impersonation
    // token the log keeps (the previous one is closed). `token` is a Windows HANDLE.
    void attachUser(void* token, const std::wstring& logsFolder);

    void write(spdlog::level::level_enum level, const std::string& component, const std::string& message);

    // One line as the other logs write theirs (the Logs tab reads them alike), with "\r\n".
    [[nodiscard]] static std::string formatLine(spdlog::level::level_enum level, const std::string& component,
                                                const std::string& message);

    // The daily file wheelbri-helper-<date>.log in `logsFolder`, created and opened as the user of `token` (a HANDLE
    // it takes and closes). For attachUser, and for the tests.
    [[nodiscard]] static std::unique_ptr<LogTarget> userFile(void* token, const std::wstring& logsFolder);

private:
    HelperLog();

    std::mutex mutex_;
    DailyLogFile file_;
};

}  // namespace wheelbri::helper
