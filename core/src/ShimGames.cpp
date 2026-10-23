#include "wheelbri/ShimGames.hpp"

#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/IniDocument.hpp"
#include "wheelbri/IniEditor.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <system_error>

namespace wheelbri {

namespace {

// Windows refused because the folder is protected (under Program Files, say): WheelBri runs as a standard user, so
// it says what to do instead of only the system's words.
bool protectedFolder(const std::error_code& ec) {
    return ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted ||
           (ec.category() == std::system_category() && ec.value() == 5);   // Windows' ERROR_ACCESS_DENIED
}

std::string writeProblem(const char* verb, const std::filesystem::path& file, const std::error_code& ec,
                         const char* otherwise) {
    if (protectedFolder(ec)) {
        return std::string("Windows does not let WheelBri ") + verb + " " + pathToUtf8(file) +
               ": close the game if it runs; if it does not, the game folder is protected (under Program Files?): do "
               "it yourself as Administrator (WheelBri's shim folder has the files), or install the game in a folder "
               "you own (Steam libraries are)";
    }
    return std::string("cannot ") + verb + " " + pathToUtf8(file) + " (" + ec.message() + ")" + otherwise;
}


constexpr const char* kHeader =
    "# WheelBri: the games the in-game shim was put into (the app's Mapping tab, Mode Shim).\n"
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

// The tab-separated fields of `line`, at most `maxParts`: the last one keeps the rest of the line, tabs and all (a
// folder's name may hold a tab on Linux).
std::vector<std::string> split(const std::string& line, const std::size_t maxParts) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t tab = parts.size() + 1 < maxParts ? line.find('\t', start) : std::string::npos;
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

// The whole file, or nothing when it is larger than `limit` or cannot be read.
std::string readBytes(const std::filesystem::path& path, const std::uintmax_t limit) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size > limit) return {};
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

Architecture peArchitecture(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    unsigned char dos[64] = {};
    if (!in.read(reinterpret_cast<char*>(dos), sizeof(dos)) || dos[0] != 'M' || dos[1] != 'Z') {
        return Architecture::Unknown;
    }
    std::uint32_t peOffset = 0;   // e_lfanew, little-endian
    for (int i = 3; i >= 0; --i) peOffset = (peOffset << 8U) | dos[0x3C + i];
    unsigned char pe[6] = {};
    in.seekg(static_cast<std::streamoff>(peOffset));
    if (!in.read(reinterpret_cast<char*>(pe), sizeof(pe)) || pe[0] != 'P' || pe[1] != 'E' || pe[2] != 0 || pe[3] != 0) {
        return Architecture::Unknown;
    }
    const unsigned machine = static_cast<unsigned>(pe[4]) | (static_cast<unsigned>(pe[5]) << 8U);
    if (machine == 0x014C) return Architecture::X86;    // IMAGE_FILE_MACHINE_I386
    if (machine == 0x8664) return Architecture::X64;    // IMAGE_FILE_MACHINE_AMD64
    return Architecture::Unknown;
}

Architecture gameArchitecture(const std::filesystem::path& folder) {
    Architecture architecture = Architecture::Unknown;
    std::uintmax_t largest = 0;
    forEachEntry(folder, [&](const std::filesystem::directory_entry& entry) {
        std::error_code ec;
        if (lower(pathToUtf8(entry.path().extension())) != ".exe" || !entry.is_regular_file(ec)) return;
        const std::uintmax_t size = entry.file_size(ec);
        if (ec || size < largest) return;
        if (const Architecture found = peArchitecture(entry.path()); found != Architecture::Unknown) {
            largest = size;
            architecture = found;
        }
    });
    return architecture;
}

bool isAsiLoader(const std::filesystem::path& dll) {
    // Its export's name, in the export table. The loader is a few MB; anything over 64 MB is not it.
    const std::string bytes = readBytes(dll, 64U * 1024U * 1024U);
    return bytes.find("IsUltimateASILoader") != std::string::npos;
}

std::filesystem::path shimDllIn(const std::filesystem::path& folder) {
    const std::filesystem::path dll = folder / kShimDll;
    return regularFile(dll) && isAsiLoader(dll) ? folder / kShimDllHooked : dll;
}

std::filesystem::path shimSourceFor(const std::filesystem::path& folder, const ShimSources& sources) {
    return gameArchitecture(folder) == Architecture::X86 ? sources.x86 : sources.x64;
}

ShimGameList readShimGames(const std::filesystem::path& file) {
    ShimGameList list;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        const std::vector<std::string> parts = split(line, 3);   // the folder is the rest of the line
        if (parts[0] == "game" && parts.size() == 3 && !parts[2].empty()) {
            const std::filesystem::path folder = pathFromUtf8(parts[2]);
            if (folder.empty()) continue;   // not valid UTF-8 (edited by hand in another code page): skipped
            const bool known = std::any_of(list.games.begin(), list.games.end(),
                                           [&](const ShimGame& g) { return sameFolder(g.folder, folder); });
            if (!known) list.games.push_back({parts[1], folder});
        } else if (parts[0] == "selected" && parts.size() >= 2) {
            list.selected = pathFromUtf8(line.substr(parts[0].size() + 1));
        }
    }
    // A selection that is not in the list is no selection; no selection = the first game.
    const bool listed = std::any_of(list.games.begin(), list.games.end(),
                                    [&](const ShimGame& g) { return sameFolder(g.folder, list.selected); });
    if (!listed) list.selected = list.games.empty() ? std::filesystem::path() : list.games.front().folder;
    return list;
}

bool writeShimGames(const std::filesystem::path& file, const ShimGameList& list) {
    std::ostringstream text;
    text << kHeader;
    for (const ShimGame& game : list.games) text << "game\t" << game.name << '\t' << pathToUtf8(game.folder) << '\n';
    if (!list.selected.empty()) text << "selected\t" << pathToUtf8(list.selected) << '\n';
    return writeFileReplacing(file, text.str());
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

bool removeShimGame(ShimGameList& list, const std::size_t index) {
    if (index >= list.games.size()) return false;
    const bool wasSelected = sameFolder(list.games[index].folder, list.selected);
    list.games.erase(list.games.begin() + static_cast<std::ptrdiff_t>(index));
    if (wasSelected) list.selected = list.games.empty() ? std::filesystem::path() : list.games.front().folder;
    return true;
}

std::string gameFolderProblem(const std::filesystem::path& folder) {
    std::error_code ec;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec)) return "that folder does not exist";
    bool exe = false;
    forEachEntry(folder, [&exe](const std::filesystem::directory_entry& entry) {
        std::error_code entryError;
        if (lower(pathToUtf8(entry.path().extension())) == ".exe" && entry.is_regular_file(entryError)) exe = true;
    });
    return exe ? std::string() : "no .exe in that folder: pick the folder that holds the game's .exe";
}

ShimState shimState(const std::filesystem::path& folder, const std::filesystem::path& sourceDll) {
    const std::filesystem::path dll = shimDllIn(folder);
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
        const char* bits = gameArchitecture(folder) == Architecture::X86 ? "32-bit " : "";
        result.error = std::string("WheelBri's ") + bits + "dinput8.dll is missing (" +
                       (sourceDll.empty() ? std::string("not found in shim/ next to the program")
                                          : pathToUtf8(sourceDll)) +
                       "): reinstall or build the shim";
        return result;
    }
    const ShimState state = shimState(folder, sourceDll);
    if (state == ShimState::Foreign) {
        result.error = "the game folder already has a dinput8.dll that is not WheelBri's (another mod?): left alone";
        return result;
    }
    std::error_code ec;
    if (state != ShimState::Installed) {
        const std::filesystem::path target = shimDllIn(folder);
        std::filesystem::copy_file(sourceDll, target, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec && regularFile(target)) {
            // Some standard libraries (MinGW's libstdc++) refuse to copy over an existing file even when asked to:
            // take the old one away first (a game that has it loaded keeps it locked, and that error is reported).
            std::error_code removeEc;
            if (std::filesystem::remove(target, removeEc)) {
                ec.clear();
                std::filesystem::copy_file(sourceDll, target, ec);
            }
        }
        if (ec) {
            result.error = writeProblem("write", target, ec, ": close the game and try again");
            return result;
        }
        result.changed.push_back(pathToUtf8(target));
    }
    if (!regularFile(folder / kShimSettings) && regularFile(sourceSettings)) {
        std::filesystem::copy_file(sourceSettings, folder / kShimSettings, ec);
        if (ec) {
            result.error = writeProblem("write", folder / kShimSettings, ec, "");
            return result;
        }
        result.changed.push_back(pathToUtf8(folder / kShimSettings));
    }
    result.ok = true;
    return result;
}

ShimResult setShimTarget(const std::filesystem::path& folder, const std::string& profile) {
    ShimResult result;
    if (makeProfile(profile) == nullptr) {   // never a name the shim would refuse (it would present nothing)
        result.error = "'" + profile + "' is not a wheel WheelBri emulates (" + std::string(kKnownProfiles) + ")";
        return result;
    }
    std::error_code ec;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec)) {   // never makes the game's folder
        result.error = "the game folder " + pathToUtf8(folder) + " is not there";
        return result;
    }
    const std::filesystem::path file = folder / kShimSettings;
    if (std::filesystem::is_directory(file, ec)) {
        result.error = pathToUtf8(file) + " is a folder, not the shim's settings";
        return result;
    }
    const std::optional<std::string> text = regularFile(file) ? readFileText(file) : std::optional<std::string>("");
    if (!text) {
        result.error = "cannot read " + pathToUtf8(file);
        return result;
    }
    if (IniDocument::parse(*text).document.get("shim", "target_profile") == profile) {
        result.ok = true;
        return result;
    }
    IniEditor editor(*text);
    editor.set("shim", "target_profile", profile);
    // IniEditor sets the line the shim reads (the last in [shim]) and drops earlier copies.
    if (!writeFileReplacing(file, editor.text())) {
        result.error = "cannot write " + pathToUtf8(file) +
                       ": close the game and try again (a game folder under Program Files is protected: install the "
                       "game in a folder you own, or set target_profile there yourself as Administrator)";
        return result;
    }
    result.changed.push_back(pathToUtf8(file));
    result.ok = true;
    return result;
}

ShimResult setShimGameAxes(const std::filesystem::path& folder, const std::optional<JoystickLayout>& axes) {
    ShimResult result;
    if (axes && !isValid(*axes)) {
        result.error = "two pedals on one axis, a pedal on x (the wheel), or the throttle or brake on none";
        return result;
    }
    std::error_code ec;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec)) {
        result.error = "the game folder " + pathToUtf8(folder) + " is not there";
        return result;
    }
    const std::filesystem::path file = folder / kShimSettings;
    if (std::filesystem::is_directory(file, ec)) {
        result.error = pathToUtf8(file) + " is a folder, not the shim's settings";
        return result;
    }
    const std::optional<std::string> text = regularFile(file) ? readFileText(file) : std::optional<std::string>("");
    if (!text) {
        result.error = "cannot read " + pathToUtf8(file);
        return result;
    }
    const IniDocument before = IniDocument::parse(*text).document;
    const bool ours = before.get("shim", kShimGameAxesKey) == std::string(kShimGameAxesByWheelBri);
    if (!axes && !ours) {
        result.ok = true;   // nothing of ours there
        return result;
    }
    IniEditor editor(*text);
    if (axes) {
        editor.set("shim", "throttle_axis", std::string(axisName(axes->throttle)));
        editor.set("shim", "brake_axis", std::string(axisName(axes->brake)));
        editor.set("shim", "clutch_axis", std::string(axisName(axes->clutch)));
        editor.set("shim", kShimGameAxesKey, kShimGameAxesByWheelBri);
    } else {
        for (const char* key : {"throttle_axis", "brake_axis", "clutch_axis", kShimGameAxesKey}) {
            editor.remove("shim", key);
        }
    }
    if (editor.text() == *text) {
        result.ok = true;
        return result;
    }
    if (!writeFileReplacing(file, editor.text())) {
        result.error = "cannot write " + pathToUtf8(file) + ": close the game and try again";
        return result;
    }
    result.changed.push_back(pathToUtf8(file));
    result.ok = true;
    return result;
}

const GameProfile* gameInFolder(const std::filesystem::path& folder, const std::span<const GameProfile> games) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string name = programName(pathToUtf8(it->path().filename()));
        for (const GameProfile& game : games) {
            if (game.program == name) return &game;
        }
    }
    return nullptr;
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
        const std::filesystem::path dll = shimDllIn(folder);   // never the ASI Loader itself
        if (!std::filesystem::remove(dll, ec) || ec) {
            result.error = writeProblem("delete", dll, ec, ": is the game still running? Close it and try again");
            return result;
        }
        result.changed.push_back(pathToUtf8(dll));
    }
    if (regularFile(folder / kShimSettings)) {
        if (!std::filesystem::remove(folder / kShimSettings, ec) || ec) {
            result.error = writeProblem("delete", folder / kShimSettings, ec, "");
            return result;
        }
        result.changed.push_back(pathToUtf8(folder / kShimSettings));
    }
    result.ok = true;
    return result;
}

}  // namespace wheelbri
