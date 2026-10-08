#include "wheelbri/LogFiles.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <system_error>

#if defined(_WIN32)
#include <fcntl.h>      // the C runtime's _O_* flags
#include <io.h>         // _wopen, _close
#include <sys/stat.h>   // _S_IREAD, _S_IWRITE
#endif

namespace wheelbri::logfiles {

namespace {

std::mutex gMutex;
std::map<std::string, std::filesystem::path> gFiles;   // folder|stem|date|extension -> this run's file
std::map<std::string, int> gRunNumbers;                 // date -> this run's number (the first file made that day)

enum class Made { Yes, Exists, Failed };

// A path as UTF-8 text (string() would throw on a name the code page cannot hold).
std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

// Creates `path` only if there is nothing of that name yet (exclusive). Windows: the C runtime's _wopen with _O_EXCL
// (every msvcrt and the UCRT have it; fopen's "x" is UCRT only); elsewhere C11 fopen "wx".
Made createNew(const std::filesystem::path& path, std::string& error) {
#if defined(_WIN32)
    const int handle = _wopen(path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (handle >= 0) {
        _close(handle);
        return Made::Yes;
    }
#else
    std::FILE* file = std::fopen(path.c_str(), "wx");
    if (file != nullptr) {
        std::fclose(file);
        return Made::Yes;
    }
#endif
    if (errno == EEXIST) return Made::Exists;
    error = "cannot create " + utf8(path) + ": " + std::strerror(errno);
    return Made::Failed;
}

}  // namespace

std::vector<std::string> namesIn(const std::filesystem::path& folder) {
    std::vector<std::string> names;
    std::error_code ec;
    std::filesystem::directory_iterator it(folder.empty() ? std::filesystem::path(".") : folder, ec);
    for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        // Every name WheelBri writes is ASCII; another name only has to fail to match.
        const std::u8string name = it->path().filename().u8string();
        names.emplace_back(name.begin(), name.end());
    }
    return names;
}

std::filesystem::path runFile(const std::filesystem::path& folder, const std::string_view stem,
                              const std::string_view date, const std::string_view extension, std::string& error) {
    const std::lock_guard<std::mutex> lock(gMutex);
    const std::string key = utf8(folder) + "|" + std::string(stem) + "|" + std::string(date) + "|" +
                            std::string(extension);
    if (const auto known = gFiles.find(key); known != gFiles.end()) return known->second;

    std::error_code ec;
    if (!folder.empty()) std::filesystem::create_directories(folder, ec);
    const std::vector<std::string> names = namesIn(folder);
    const auto runNumber = gRunNumbers.find(std::string(date));
    int index = nextIndex(names, stem, date);
    if (runNumber != gRunNumbers.end() && !hasIndex(names, stem, date, runNumber->second)) index = runNumber->second;
    int made = 0;
    const std::filesystem::path path = createNumbered(folder, stem, date, extension, index, error, &made);
    if (path.empty()) return {};
    gFiles.emplace(key, path);
    gRunNumbers.emplace(std::string(date), made);   // kept when this run has a number for the day already
    return path;
}

std::filesystem::path createNumbered(const std::filesystem::path& folder, const std::string_view stem,
                                     const std::string_view date, const std::string_view extension,
                                     const int firstIndex, std::string& error, int* const made) {
    int index = firstIndex < 1 ? 1 : firstIndex;
    for (int attempt = 0; attempt < 100 && index <= kMaxIndex; ++attempt, ++index) {
        const std::filesystem::path path = folder / numberedName(stem, date, index, extension);
        switch (createNew(path, error)) {
            case Made::Yes:
                error.clear();
                if (made != nullptr) *made = index;
                return path;
            case Made::Exists: continue;   // another program took that number a moment ago: the next one
            case Made::Failed: return {};
        }
    }
    error = "no free number for " + std::string(stem) + "-" + std::string(date) + " in " + utf8(folder);
    return {};
}

std::filesystem::path newestFile(const std::filesystem::path& folder, const std::string_view stem) {
    const std::optional<std::string> name = newestOf(namesIn(folder), stem);
    return name ? folder / *name : std::filesystem::path();
}

void forgetRun() {
    const std::lock_guard<std::mutex> lock(gMutex);
    gFiles.clear();
    gRunNumbers.clear();
}

}  // namespace wheelbri::logfiles
