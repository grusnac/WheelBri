#include "HelperLog.hpp"

#include "wheelbri/LocalDate.hpp"
#include "wheelbri/LogFiles.hpp"
#include "wheelbri/Logging.hpp"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>

namespace wheelbri::helper {

namespace {

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

class UserFile final : public LogTarget {
public:
    UserFile(const HANDLE token, std::wstring folder) : token_(token), folder_(std::move(folder)) {}
    ~UserFile() override {
        close();
        if (token_ != nullptr) CloseHandle(token_);
    }
    UserFile(const UserFile&) = delete;
    UserFile& operator=(const UserFile&) = delete;

    // This run's file of `date` (LogFiles.hpp): the first open of a day makes wheelbri-helper-<date>-<NN>.log with the
    // next free number; opening the same day again (after a failed write) appends to it.
    bool open(const std::string& date) override {
        close();
        if (token_ == nullptr || folder_.empty()) return false;
        HANDLE file = INVALID_HANDLE_VALUE;
        std::wstring path;
        asUser(token_, [&] {
            // %LOCALAPPDATA%\wheelbri, then logs (the app makes them; after a clean install they may not exist yet).
            const std::size_t cut = folder_.find_last_of(L'\\');
            if (cut != std::wstring::npos && cut > 0) CreateDirectoryW(folder_.substr(0, cut).c_str(), nullptr);
            CreateDirectoryW(folder_.c_str(), nullptr);
            if (date == date_ && !path_.empty()) {
                path = path_;
                file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                return file != INVALID_HANDLE_VALUE;
            }
            // Listed as the user too: the helper learns no more of the folder than the user may see.
            int index = logfiles::nextIndex(logfiles::namesIn(std::filesystem::path(folder_)), kStem, date);
            for (int attempt = 0; attempt < 100 && index <= logfiles::kMaxIndex; ++attempt, ++index) {
                const std::string name = logfiles::numberedName(kStem, date, index);
                path = folder_ + L"\\" + std::wstring(name.begin(), name.end());
                file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) break;
            }
            return file != INVALID_HANDLE_VALUE;
        });
        if (file == INVALID_HANDLE_VALUE) return false;
        file_ = file;
        path_ = path;
        date_ = date;
        return true;
    }

    bool write(const std::string_view line) override {
        if (file_ == nullptr) return false;
        DWORD written = 0;
        return WriteFile(file_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) != FALSE &&
               written == line.size();
    }

    void close() override {
        if (file_ != nullptr) CloseHandle(file_);
        file_ = nullptr;
    }

private:
    static constexpr const char* kStem = "wheelbri-helper";

    HANDLE token_;
    std::wstring folder_;
    HANDLE file_ = nullptr;
    std::wstring path_;   // this run's file of date_
    std::string date_;
};

std::string today() {
    return localDate(std::chrono::system_clock::now());
}

}  // namespace

HelperLog::HelperLog()
    : file_(DailyLogFile::Settings{kPendingLines, kRetryMs},
            [](const std::string& message) { return formatLine(spdlog::level::warn, "helper", message); }) {}

HelperLog& HelperLog::instance() {
    static HelperLog* const log = new HelperLog();   // never destroyed: other threads may log until the end
    return *log;
}

void HelperLog::install() {
    logging::addListener([](const spdlog::level::level_enum level, const std::string& component,
                            const std::string& message) { instance().write(level, component, message); });
}

std::string HelperLog::formatLine(const spdlog::level::level_enum level, const std::string& component,
                                  const std::string& message) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char stamp[40];
    std::snprintf(stamp, sizeof(stamp), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ", t.wYear, t.wMonth, t.wDay, t.wHour,
                  t.wMinute, t.wSecond, t.wMilliseconds);
    std::string levelName(spdlog::level::to_string_view(level).data(), spdlog::level::to_string_view(level).size());
    if (levelName.size() < 5) levelName.resize(5, ' ');
    std::string name = component;
    if (name.size() < 14) name.resize(14, ' ');
    std::string tid = std::to_string(GetCurrentThreadId());
    if (tid.size() < 6) tid.resize(6, ' ');
    return std::string(stamp) + "[" + levelName + "] [" + name + "] [tid " + tid + "] " + message + "\r\n";
}

std::unique_ptr<LogTarget> HelperLog::userFile(void* const token, const std::wstring& logsFolder) {
    return std::make_unique<UserFile>(static_cast<HANDLE>(token), logsFolder);
}

void HelperLog::attachUser(void* const token, const std::wstring& logsFolder) {
    std::unique_ptr<LogTarget> target = userFile(token, logsFolder);
    const std::lock_guard<std::mutex> lock(mutex_);
    // The folder names the place: the same user's folder again (their app's Hello every 5 minutes, a second app)
    // keeps this run's file instead of starting a new, empty one; `target`, dropped, closes its token.
    const std::string where(reinterpret_cast<const char*>(logsFolder.data()), logsFolder.size() * sizeof(wchar_t));
    file_.attach(std::move(target), today(), GetTickCount64(), where);
}

void HelperLog::write(const spdlog::level::level_enum level, const std::string& component, const std::string& message) {
    std::string line = formatLine(level, component, message);
    const std::lock_guard<std::mutex> lock(mutex_);
    file_.write(std::move(line), today(), GetTickCount64());
}

}  // namespace wheelbri::helper
