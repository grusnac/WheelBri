// Every parser and protocol state machine fed what it does not expect (FuzzTargets.cpp): the shipped configs and
// game profiles with bytes changed, cut short or repeated, and random bytes for the packets, HID reports and
// DirectInput values. Fixed seeds, so a failure always comes back the same way; wheelbri_fuzz (-DWE_BUILD_FUZZ=ON)
// searches far longer.

#include "fuzz/FuzzTargets.hpp"

#include "wheelbri/FfbPacket.hpp"
#include "wheelbri/FileText.hpp"
#include "wheelbri/StatePacket.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace wheelbri::fuzz {
namespace {

using Bytes = std::vector<std::uint8_t>;

void run(const std::uint8_t target, const Bytes& body) {
    Bytes input;
    input.reserve(body.size() + 1);
    input.push_back(target);
    input.insert(input.end(), body.begin(), body.end());
    try {
        runOne(input);
    } catch (const Broken& broken) {
        ADD_FAILURE() << "target " << int{target} << ", " << body.size() << " bytes: " << broken.what();
    }
}

// `text` changed a little, as a hand edit or a damaged file would: bytes replaced, taken out, put in, or the end cut.
Bytes mutated(const std::string& text, std::mt19937& random) {
    Bytes bytes(text.begin(), text.end());
    static constexpr std::string_view kPieces = "[]=;#\n\r -.0123456789|:,\x00\xFF";
    const int changes = 1 + static_cast<int>(random() % 8);
    for (int i = 0; i < changes && !bytes.empty(); ++i) {
        const std::size_t at = random() % bytes.size();
        const auto piece = static_cast<std::uint8_t>(kPieces[random() % kPieces.size()]);
        switch (random() % 4) {
            case 0: bytes[at] = piece; break;
            case 1: bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(at)); break;
            case 2: bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at), piece); break;
            default: bytes.resize(at); break;
        }
    }
    return bytes;
}

Bytes randomBytes(std::mt19937& random, const std::size_t most) {
    Bytes bytes(random() % (most + 1));
    for (std::uint8_t& b : bytes) b = static_cast<std::uint8_t>(random());
    return bytes;
}

std::vector<std::string> shipped(const std::filesystem::path& folder) {
    std::vector<std::string> texts;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        if (entry.path().extension() != ".ini") continue;
        if (const auto text = readFileText(entry.path())) texts.push_back(*text);
    }
    return texts;
}

// The shipped configs as they are and changed: every config is read, its mapping written back and read again.
TEST(Robustness, ChangedConfigsAndProfilesAreReadWithoutHarm) {
    std::mt19937 random(2026);
    const std::filesystem::path config(WHEELBRI_CONFIG_DIR);
    const auto configs = shipped(config);
    const auto profiles = shipped(config / "games");
    ASSERT_FALSE(configs.empty());
    ASSERT_FALSE(profiles.empty());
    for (const std::string& text : configs) {
        run(0, Bytes(text.begin(), text.end()));
        for (int i = 0; i < 60; ++i) run(0, mutated(text, random));
    }
    for (const std::string& text : profiles) {
        run(7, Bytes(text.begin(), text.end()));
        for (int i = 0; i < 100; ++i) run(7, mutated(text, random));
    }
    const std::string rig = "[device_1]\nname = Wheel\nid = 346e:0005\nkind = wheel\naxis_0 = Wheel | steering\n"
                            "button_1 = A | a\n[device_2]\nid = 30b7:1001\naxis_1 = Gas | throttle\n";
    const std::string labels = "[device]\nname = Pedals\nid = 044f:b696\nkind = pedals\n[axis_labels]\naxis_2 = Gas\n";
    for (int i = 0; i < 300; ++i) {
        run(6, mutated(rig, random));
        run(10, mutated(labels, random));
        run(8, mutated("ax2+ 0.5 | 3 | 346e:0005 | 540 | wheelbri-service-2026-10-23-01.log", random));
    }
}

// Random bytes, and packets that were valid before a byte changed, for everything that reads binary data.
TEST(Robustness, RandomBytesAreRefusedOrReadWithoutHarm) {
    std::mt19937 random(41329);
    StatePacket state;
    state.sequence = 7;
    state.report.steering = 1000;
    const auto stateBytes = encode(state);
    FfbPacket ffb;
    ffb.kind = FfbPacket::Kind::SlotState;
    ffb.playing = true;
    ffb.effect.constantLevel = 0.5F;
    const auto ffbBytes = encode(ffb);
    for (int i = 0; i < 400; ++i) {
        for (const std::uint8_t target : {1, 2, 3, 4, 5, 9, 11}) run(target, randomBytes(random, 300));
        Bytes s(stateBytes.begin(), stateBytes.end());
        s[random() % s.size()] = static_cast<std::uint8_t>(random());
        run(1, s);
        Bytes f(ffbBytes.begin(), ffbBytes.end());
        f[random() % f.size()] = static_cast<std::uint8_t>(random());
        run(2, f);
    }
}

}  // namespace
}  // namespace wheelbri::fuzz
