// One input to every parser and protocol state machine WheelBri gives data it does not control (configs and profiles
// edited by hand, datagrams on 127.0.0.1, HID reports from games, DirectInput values): the first byte picks which.
// Whatever the bytes, nothing may crash, hang or read out of bounds, what decodes encodes back, and no force
// that reaches the service is NaN or infinite. Used by RobustnessTest (fixed inputs, every build) and by
// wheelbri_fuzz (libFuzzer, -DWE_BUILD_FUZZ=ON with Clang).

#include "FuzzTargets.hpp"

#include "wheelbri/AxisTuning.hpp"
#include "wheelbri/DeviceLabels.hpp"
#include "wheelbri/DiEffect.hpp"
#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/FfbPacket.hpp"
#include "wheelbri/GameProfiles.hpp"
#include "wheelbri/GameRotation.hpp"
#include "wheelbri/GameTuning.hpp"
#include "wheelbri/HelperProtocol.hpp"
#include "wheelbri/HidPid.hpp"
#include "wheelbri/HidPp.hpp"
#include "wheelbri/IniDocument.hpp"
#include "wheelbri/LogFileNames.hpp"
#include "wheelbri/LogLevelPacket.hpp"
#include "wheelbri/LogiSdk.hpp"
#include "wheelbri/MappingEdit.hpp"
#include "wheelbri/Rig.hpp"
#include "wheelbri/ShimGames.hpp"
#include "wheelbri/StatePacket.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace wheelbri::fuzz {
namespace {

#define CHECK(c) \
    do { \
        if (!(c)) throw Broken(std::string(#c) + " (FuzzTargets.cpp:" + std::to_string(__LINE__) + ")"); \
    } while (0)

bool finite(const FfbEffect& e) {
    return std::isfinite(e.constantLevel) && std::isfinite(e.periodic.magnitude) && std::isfinite(e.periodic.offset) &&
           std::isfinite(e.periodic.periodMs) && std::isfinite(e.gain) && std::isfinite(e.condition.positiveCoefficient) &&
           std::isfinite(e.condition.negativeCoefficient) && std::isfinite(e.condition.center);
}

}  // namespace

void runOne(const std::span<const std::uint8_t> input) {
    const std::uint8_t* const data = input.data();
    const std::size_t size = input.size();
    if (size < 1) return;
    const std::uint8_t which = data[0] % kTargets;
    const std::span<const std::uint8_t> bytes(data + 1, size - 1);
    const std::string_view text(reinterpret_cast<const char*>(data + 1), size - 1);
    switch (which) {
        case 0: {   // a config: load, then write the mapping back and load that again
            const auto parsed = IniDocument::parse(text);
            const auto loaded = loadEmulatorConfig(parsed.document);
            const MappingModel model = mappingModel(loaded.config);
            const auto written = writeMapping(text, model);
            const auto again = loadEmulatorConfig(IniDocument::parse(written.text).document);
            (void)again;
            break;
        }
        case 1: {
            if (auto p = decodeStatePacket(bytes)) {
                const auto e = encode(*p);
                auto q = decodeStatePacket(e);
                CHECK(q.has_value());
            }
            break;
        }
        case 2: {
            if (auto p = decodeFfbPacket(bytes)) {
                const auto e = encode(*p);
                CHECK(decodeFfbPacket(e).has_value());
            }
            (void)decodeLogLevelPacket(bytes);
            break;
        }
        case 3: {
            std::string error;
            if (auto r = helper::decodeRequest(bytes, error)) {
                const auto e = helper::encode(*r);
                std::string e2;
                CHECK(helper::decodeRequest(e, e2).has_value());
                (void)helper::requestProblem(*r);
            }
            if (auto r = helper::decodeReply(bytes, error)) {
                const auto e = helper::encode(*r);
                std::string e2;
                CHECK(helper::decodeReply(e, e2).has_value());
            }
            break;
        }
        case 4: {   // HID PID from a game (the driver's input): a stream of reports; every effect sent on is finite
            HidPid pid([](const FfbPacket& p) { CHECK(finite(p.effect)); }, [](LogLevel, const std::string&) {});
            std::int64_t now = 0;
            std::size_t i = 0;
            while (i + 2 <= bytes.size()) {
                const std::uint8_t op = bytes[i];
                const std::uint8_t id = bytes[i + 1];
                const std::size_t len = std::min<std::size_t>(bytes.size() - i - 2, op >> 3);
                const auto chunk = bytes.subspan(i + 2, len);
                switch (op & 3) {
                    case 0: pid.output(id, chunk, now); break;
                    case 1: pid.setFeature(id, chunk, now); break;
                    case 2: { std::uint8_t out[64]; (void)pid.getFeature(id, out); break; }
                    case 3: now += (op >> 2) * 7; pid.tick(now); break;
                }
                i += 2 + len;
            }
            pid.tick(now + 20000);
            break;
        }
        case 5: {
            HidPp pp({"G923", 0xC266, 1, 0}, [](LogLevel, const std::string&) {}, [](int, float) {});
            std::size_t i = 0;
            std::int64_t now = 0;
            while (i < bytes.size()) {
                const std::size_t len = std::min<std::size_t>(bytes.size() - i, 1 + bytes[i] % 70);
                (void)pp.handle(bytes.subspan(i, len), now += 5);
                i += len;
            }
            break;
        }
        case 6: {
            const auto rig = loadRig(IniDocument::parse(text).document);
            const std::string written = writeRig(rig.rig, "x");
            const auto again = loadRig(IniDocument::parse(written).document);
            CHECK(again.rig.devices.size() == rig.rig.devices.size());
            break;
        }
        case 7: {
            (void)parseUserGames(text);
            (void)parseGameProfile("fuzz", text);
            break;
        }
        case 8: {
            (void)parseGameRotation(text);
            (void)tuning::parseNumber(text);
            (void)parseButtonRefs(text);
            (void)parseDeviceId(text);
            (void)logfiles::partsOf(text, "wheelbri-service");
            break;
        }
        case 9: {
            (void)logi::decodeProperties(bytes);
            (void)logi::decodeState(bytes);
            break;
        }
        case 10: {
            (void)loadDeviceLabels(IniDocument::parse(text).document);
            break;
        }
        case 11: {   // DirectInput values from a game
            if (bytes.size() < 24) break;
            std::int32_t v[6];
            std::memcpy(v, bytes.data(), 24);
            const float dir = di::directionAlongX(static_cast<std::uint32_t>(v[0]), v[1]);
            CHECK(std::isfinite(dir));
            CHECK(finite(di::constantForce(v[2], dir, static_cast<std::uint32_t>(v[3]))));
            CHECK(finite(di::condition(FfbEffectType::Spring, v[0], v[1], v[2], static_cast<std::uint32_t>(v[3]), static_cast<std::uint32_t>(v[4]), v[5],
                                       static_cast<std::uint32_t>(v[0]))));
            CHECK(finite(di::periodic(FfbEffectType::Sine, static_cast<std::uint32_t>(v[0]), v[1],
                                      static_cast<std::uint32_t>(v[2]), static_cast<std::uint32_t>(v[3]), dir,
                                      static_cast<std::uint32_t>(v[4]))));
            break;
        }
        case 12: {   // [shim] chain from a wheelbri-shim.ini: never a path, never the shim's own name
            if (validChainName(text)) {
                CHECK(text.find_first_of("\\/:") == std::string_view::npos);
                CHECK(text.size() <= 64);
            }
            break;
        }
    }
}

}  // namespace wheelbri::fuzz
