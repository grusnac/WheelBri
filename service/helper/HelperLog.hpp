#pragma once

#include <spdlog/common.h>

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>

namespace wheelbri::helper {

// The helper's log, in the connected user's log folder with every other log
// (%LOCALAPPDATA%\wheelbri\logs\wheelbri-helper-<date>.log): the user's choice, 2026-10-06.
//
// The helper runs as SYSTEM and that folder belongs to the user, who could put a link in it pointing anywhere. So
// the file is created and opened only while impersonating that user (with the token of the pipe's client): Windows
// then checks the user's own rights, and a link leads nowhere the user could not write anyway. The handle, once
// open, is written as SYSTEM.
//
// Before any user has connected (the helper starts at boot) the lines wait in memory (at most kPendingLines; the
// oldest go first, counted) and are written at the first connection. A new file every day, opened again with the
// same user's token. With --console the lines also go to the console (spdlog's own sink).
class HelperLog {
public:
    static constexpr std::size_t kPendingLines = 2000;

    static HelperLog& instance();

    // Hooks into wheelbri::logging (call once, after logging::init).
    void install();

    // From now on writes into `logsFolder` (UTF-16 path, created if missing) as the user of `token`, an impersonation
    // token the log keeps (the previous one is closed). `token` is a Windows HANDLE.
    void attachUser(void* token, const std::wstring& logsFolder);

    void write(spdlog::level::level_enum level, const std::string& component, const std::string& message);

private:
    HelperLog() = default;
    bool openLocked(const std::string& date);   // under mutex_
    void writeLocked(const std::string& line);   // under mutex_, file open

    std::mutex mutex_;
    std::deque<std::string> pending_;
    std::size_t droppedPending_ = 0;
    void* token_ = nullptr;   // HANDLE
    std::wstring folder_;
    void* file_ = nullptr;    // HANDLE; nullptr: none
    std::string fileDate_;
};

}  // namespace wheelbri::helper
