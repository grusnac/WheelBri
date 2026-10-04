#include "wheelbri/ShimGames.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace wheelbri {

namespace {

constexpr const char* kHeader =
    "# WheelBri: the games the in-game shim was put into (the app's Mapping tab, target G920 in-game shim).\n"
    "# game<TAB>name<TAB>folder, selected<TAB>folder. Written by the app.\n";

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Windows folders compare without case.
bool sameFolder(const std::filesystem::path& a, const std::filesystem::path& b) {
    return lower(pathToUtf8(a.lexically_normal())) == lower(pathToUtf8(b.lexically_normal()));
}

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t tab = line.find('\t', start);
        parts.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) break;
        start = tab + 1;
    }
    return parts;
}

bool regularFile(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

bool sameBytes(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    if (std::filesystem::file_size(a, ec) != std::filesystem::file_size(b, ec) || ec) return false;
    std::ifstream fa(a, std::ios::binary);
    std::ifstream fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    return std::equal(std::istreambuf_iterator<char>(fa), std::istreambuf_iterator<char>(),
                      std::istreambuf_iterator<char>(fb));
}

}  // namespace

std::string pathToUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

std::filesystem::path pathFromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

ShimGameList readShimGames(const std::filesystem::path& file) {
    ShimGameList list;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        const std::vector<std::string> parts = split(line);
        if (parts[0] == "game" && parts.size() == 3 && !parts[2].empty()) {
            const std::filesystem::path folder = pathFromUtf8(parts[2]);
            const bool known = std::any_of(list.games.begin(), list.games.end(),
                                           [&](const ShimGame& g) { return sameFolder(g.folder, folder); });
            if (!known) list.games.push_back({parts[1], folder});
        } else if (parts[0] == "selected" && parts.size() == 2) {
            list.selected = pathFromUtf8(parts[1]);
        }
    }
    // A selection that is not in the list is no selection; no selection = the first game.
    const bool listed = std::any_of(list.games.begin(), list.games.end(),
                                    [&](const ShimGame& g) { return sameFolder(g.folder, list.selected); });
    if (!listed) list.selected = list.games.empty() ? std::filesystem::path() : list.games.front().folder;
    return list;
}

bool writeShimGames(const std::filesystem::path& file, const ShimGameList& list) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ostringstream text;
    text << kHeader;
    for (const ShimGame& game : list.games) text << "game\t" << game.name << '\t' << pathToUtf8(game.folder) << '\n';
    if (!list.selected.empty()) text << "selected\t" << pathToUtf8(list.selected) << '\n';
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text.str();
    out.close();
    return static_cast<bool>(out);
}

std::size_t addShimGame(ShimGameList& list, const std::filesystem::path& folder) {
    std::filesystem::path clean = folder.lexically_normal();
    if (!clean.has_filename() && clean.has_parent_path()) clean = clean.parent_path();   // "D:\Games\NFS\"
    for (std::size_t i = 0; i < list.games.size(); ++i) {
        if (sameFolder(list.games[i].folder, clean)) {
            list.selected = list.games[i].folder;
            return i;
        }
    }
    std::string name = pathToUtf8(clean.filename());
    if (name.empty()) name = pathToUtf8(clean);
    for (char& c : name) {
        if (c == '\t') c = ' ';
    }
    list.games.push_back({name, clean});
    list.selected = clean;
    return list.games.size() - 1;
}

std::string gameFolderProblem(const std::filesystem::path& folder) {
    std::error_code ec;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec)) return "that folder does not exist";
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        std::error_code entryError;
        if (lower(pathToUtf8(entry.path().extension())) == ".exe" && entry.is_regular_file(entryError)) return {};
    }
    return "no .exe in that folder: pick the folder that holds the game's .exe";
}

const char* shimStateName(const ShimState state) {
    switch (state) {
        case ShimState::Installed: return "shim in place";
        case ShimState::Outdated: return "shim outdated";
        case ShimState::Missing: return "no shim";
        case ShimState::Foreign: return "another dinput8.dll";
    }
    return "?";
}

ShimState shimState(const std::filesystem::path& folder, const std::filesystem::path& sourceDll) {
    const std::filesystem::path dll = folder / kShimDll;
    if (!regularFile(dll)) return ShimState::Missing;
    const bool haveSource = regularFile(sourceDll);
    const bool same = haveSource && sameBytes(dll, sourceDll);
    if (same) return ShimState::Installed;
    if (!regularFile(folder / kShimSettings)) return ShimState::Foreign;
    return haveSource ? ShimState::Outdated : ShimState::Installed;
}

ShimResult installShim(const std::filesystem::path& folder, const std::filesystem::path& sourceDll,
                       const std::filesystem::path& sourceSettings) {
    ShimResult result;
    if (const std::string problem = gameFolderProblem(folder); !problem.empty()) {
        result.error = problem;
        return result;
    }
    if (!regularFile(sourceDll)) {
        result.error = "WheelBri's dinput8.dll is missing (" + pathToUtf8(sourceDll) + "): reinstall or build the shim";
        return result;
    }
    const ShimState state = shimState(folder, sourceDll);
    if (state == ShimState::Foreign) {
        result.error = "the game folder already has a dinput8.dll that is not WheelBri's (another mod?): left alone";
        return result;
    }
    std::error_code ec;
    if (state != ShimState::Installed) {
        std::filesystem::copy_file(sourceDll, folder / kShimDll, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            result.error = "cannot write " + pathToUtf8(folder / kShimDll) + " (" + ec.message() +
                           "): close the game and try again";
            return result;
        }
        result.changed.push_back(pathToUtf8(folder / kShimDll));
    }
    if (!regularFile(folder / kShimSettings) && regularFile(sourceSettings)) {
        std::filesystem::copy_file(sourceSettings, folder / kShimSettings, ec);
        if (ec) {
            result.error = "cannot write " + pathToUtf8(folder / kShimSettings) + " (" + ec.message() + ")";
            return result;
        }
        result.changed.push_back(pathToUtf8(folder / kShimSettings));
    }
    result.ok = true;
    return result;
}

ShimResult removeShim(const std::filesystem::path& folder, const std::filesystem::path& sourceDll) {
    ShimResult result;
    const ShimState state = shimState(folder, sourceDll);
    if (state == ShimState::Foreign) {
        result.error = "the dinput8.dll there is not WheelBri's: left alone";
        return result;
    }
    std::error_code ec;
    if (state != ShimState::Missing) {
        if (!std::filesystem::remove(folder / kShimDll, ec) || ec) {
            result.error = "cannot delete " + pathToUtf8(folder / kShimDll) + " (" + ec.message() +
                           "): is the game still running? Close it and try again";
            return result;
        }
        result.changed.push_back(pathToUtf8(folder / kShimDll));
    }
    if (regularFile(folder / kShimSettings)) {
        if (!std::filesystem::remove(folder / kShimSettings, ec) || ec) {
            result.error = "cannot delete " + pathToUtf8(folder / kShimSettings) + " (" + ec.message() + ")";
            return result;
        }
        result.changed.push_back(pathToUtf8(folder / kShimSettings));
    }
    result.ok = true;
    return result;
}

}  // namespace wheelbri
