#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace wheelbri {

// Mode Shim: whether the shim in the game's folder works. Once a known game runs, the shim should be heard from (its
// first force feedback message, or its log written) within kQuietMs; if not, the emulation is stalled or the shim is
// not in that game's folder, and the app says so once per start of the game. Heard once, the session is fine: no
// warning any more, and Stop may ask what to do with the shims. Allocation-free apart from the game's id.
class ShimWatch {
public:
    static constexpr std::uint64_t kQuietMs = 120'000;   // two minutes of the game running without the shim

    void reset() { *this = ShimWatch{}; }
    // Every frame of a shim session: `gameId` the known game running now (empty: none), `gameStarts` how many known
    // games started this session (a new start restarts the wait), `heard` whether the shim has been heard from.
    // Returns the game's id when the warning is due (once per start), nullopt otherwise.
    std::optional<std::string> update(std::uint64_t nowMs, const std::string& gameId, std::uint64_t gameStarts,
                                      bool heard);
    [[nodiscard]] bool heard() const { return heard_; }

private:
    bool heard_ = false;
    std::string game_;          // the game being waited for, empty when none runs
    std::uint64_t since_ = 0;   // when it was seen starting
    std::uint64_t starts_ = 0;
    bool warned_ = false;
};

}  // namespace wheelbri
