#include "wheelbri/KnownGames.hpp"

#include <array>
#include <cctype>

namespace wheelbri {

namespace {

// Only NFS 2015 reads a G920 differently; the others are listed so the log and the window can name them.
constexpr std::array<KnownGame, 4> kGames{{
    {"NFS 2015", "nfs16.exe", PedalLayout::Nfs2015},
    {"NFS Payback", "needforspeedpayback.exe", PedalLayout::Logitech},
    {"NFS Heat", "needforspeedheat.exe", PedalLayout::Logitech},
    {"NFS Unbound", "needforspeedunbound.exe", PedalLayout::Logitech},
}};

}  // namespace

std::span<const KnownGame> knownGames() { return kGames; }

std::string programName(const std::string_view nameOrPath) {
    const std::size_t slash = nameOrPath.find_last_of("/\\");
    const std::string_view file = slash == std::string_view::npos ? nameOrPath : nameOrPath.substr(slash + 1);
    std::string name(file);
    for (char& c : name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return name;
}

const KnownGame* findRunningGame(const std::span<const std::string> programs) {
    for (const KnownGame& game : kGames) {
        for (const std::string& program : programs) {
            if (programName(program) == game.program) {
                return &game;
            }
        }
    }
    return nullptr;
}

}  // namespace wheelbri
