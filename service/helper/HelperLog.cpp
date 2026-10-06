#include "HelperLog.hpp"

#include "wheelbri/LocalDate.hpp"
#include "wheelbri/Logging.hpp"

#include <windows.h>

#include <chrono>
#include <cstdio>

namespace wheelbri::helper {

namespace {

// The same look as spdlog's lines in the other logs (the Logs tab reads them alike), without file:line.
std::string formatLine(const spdlog::level::level_enum level, const std::string& component, const std::string& message) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    const auto levelName = spdlog::level::to_string_view(level);
    char head[96];
    std::snprintf(head, sizeof(head), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%-5.*s] [%-14s] [tid %-6lu] ", t.wYear,
                  t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                  static_cast<int>(levelName.size()), levelName.data(), component.c_str(), GetCurrentThreadId());
    return std::string(head) + message + "\r\n";
}

// Runs `work` as the user of `token` on this thread, then puts back whatever this thread was before (it may be
// impersonating a pipe client itself).
template <typename Work>
bool asUser(const HANDLE token, Work&& work) {
    HANDLE previous = nullptr;
    const bool hadToken = OpenThreadToken(GetCurrentThread(), TOKEN_IMPERSONATE, TRUE, &previous) != FALSE;
    if (!SetThreadToken(nullptr, token)) {
        if (hadToken) CloseHandle(previous);
        return false;
    }
    const bool ok = work();
    SetThreadToken(nullptr, hadToken ? previous : nullptr);
    if (hadToken) CloseHandle(previous);
    return ok;
}

}  // namespace

HelperLog& HelperLog::instance() {
    static HelperLog* const log = new HelperLog();   // never destroyed: other threads may log until the end
    return *log;
}

void HelperLog::install() {
    logging::addListener([](const spdlog::level::level_enum level, const std::string& component,
                            const std::string& message) { instance().write(level, component, message); });
}

void HelperLog::attachUser(void* const token, const std::wstring& logsFolder) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (file_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(file_));
        file_ = nullptr;
    }
    if (token_ != nullptr) CloseHandle(static_cast<HANDLE>(token_));
    token_ = token;
    folder_ = logsFolder;
    if (!openLocked(localDate(std::chrono::system_clock::now()))) return;
    if (droppedPending_ > 0) {
        writeLocked(formatLine(spdlog::level::warn, "helper",
                               std::to_string(droppedPending_) + " earlier lines were dropped before a user connected"));
        droppedPending_ = 0;
    }
    while (!pending_.empty()) {
        writeLocked(pending_.front());
        pending_.pop_front();
    }
}

bool HelperLog::openLocked(const std::string& date) {
    if (token_ == nullptr || folder_.empty()) return false;
    const std::wstring path = folder_ + L"\\wheelbri-helper-" + std::wstring(date.begin(), date.end()) + L".log";
    HANDLE file = INVALID_HANDLE_VALUE;
    asUser(static_cast<HANDLE>(token_), [&] {
        // %LOCALAPPDATA%\wheelbri, then logs (the app makes them; after a clean install they may not exist yet).
        const std::size_t cut = folder_.find_last_of(L'\\');
        if (cut != std::wstring::npos) CreateDirectoryW(folder_.substr(0, cut).c_str(), nullptr);
        CreateDirectoryW(folder_.c_str(), nullptr);
        file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return file != INVALID_HANDLE_VALUE;
    });
    if (file == INVALID_HANDLE_VALUE) return false;
    file_ = file;
    fileDate_ = date;
    return true;
}

void HelperLog::writeLocked(const std::string& line) {
    DWORD written = 0;
    WriteFile(static_cast<HANDLE>(file_), line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}

void HelperLog::write(const spdlog::level::level_enum level, const std::string& component, const std::string& message) {
    std::string line = formatLine(level, component, message);
    const std::lock_guard<std::mutex> lock(mutex_);
    if (file_ != nullptr) {
        const std::string today = localDate(std::chrono::system_clock::now());
        if (today != fileDate_) {
            CloseHandle(static_cast<HANDLE>(file_));
            file_ = nullptr;
            openLocked(today);
        }
    }
    if (file_ == nullptr) {
        if (pending_.size() == kPendingLines) {
            pending_.pop_front();
            ++droppedPending_;
        }
        pending_.push_back(std::move(line));
        return;
    }
    writeLocked(line);
}

}  // namespace wheelbri::helper
