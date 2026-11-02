// One input to every parser and protocol state machine WheelBri gives data it does not control (configs and profiles
// edited by hand, datagrams on 127.0.0.1, HID reports from games, DirectInput values): the first byte picks which.
// Whatever the bytes, nothing may crash, hang or read out of bounds, what decodes encodes back, and no force
// that reaches the service is NaN or infinite. Used by RobustnessTest (fixed inputs, every build) and by
// wheelbri_fuzz (libFuzzer, -DWE_BUILD_FUZZ=ON with Clang).

#include "FuzzTargets.hpp"

#include "wheelbri/AxisAudit.hpp"
#include "wheelbri/AxisTuning.hpp"
#include "wheelbri/DeviceLabels.hpp"
#include "wheelbri/DiEffect.hpp"
#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/FfbPacket.hpp"
#include "wheelbri/GameProfiles.hpp"
#include "wheelbri/GameRotation.hpp"
#include "wheelbri/GameSettings.hpp"
#include "wheelbri/GameTuning.hpp"
#include "wheelbri/HelperProtocol.hpp"
#include "wheelbri/HidPid.hpp"
#include "wheelbri/HidPp.hpp"
#include "wheelbri/HidPpFfb.hpp"
#include "wheelbri/TrueForce.hpp"
#include "wheelbri/TrueForceFfb.hpp"
#include "wheelbri/IniDocument.hpp"
#include "wheelbri/LogFileNames.hpp"
#include "wheelbri/LogLevelPacket.hpp"
#include "wheelbri/LogiSdk.hpp"
#include "wheelbri/MappingEdit.hpp"
#include "wheelbri/Nfs2015Settings.hpp"
#include "wheelbri/Rig.hpp"
#include "wheelbri/SenderGate.hpp"
#include "wheelbri/ShimGames.hpp"
#include "wheelbri/StatePacket.hpp"
#ifdef WHEELBRI_G923CAP
#include "g923cap/Analysis.hpp"
#include "g923cap/Pcap.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

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
        case 13: {   // what a game or G HUB writes to the TrueForce interface (the driver's input)
            TrueForceDevice device([](LogLevel, const std::string&) {});
            TrueForceFfb ffb([](const FfbPacket& p) {
                CHECK(std::isfinite(p.effect.constantLevel) && std::abs(p.effect.constantLevel) <= 1.0F);
                CHECK(p.slot == TrueForceFfb::kSlot || p.slot == TrueForceFfb::kDamperSlot ||
                      p.slot == TrueForceFfb::kTextureSlot);
                CHECK(p.effect.condition.positiveCoefficient >= 0.0F && p.effect.condition.positiveCoefficient <= 1.0F);
                CHECK(std::isfinite(p.effect.periodic.magnitude) && p.effect.periodic.magnitude >= 0.0F &&
                      p.effect.periodic.magnitude <= TrueForceFfb::kTextureMax);
                CHECK(p.slot != TrueForceFfb::kTextureSlot ||
                      p.effect.periodic.periodMs <= TrueForceFfb::kTextureMaxPeriodMs);
            });
            std::size_t i = 0;
            std::int64_t now = 0;
            // Each packet: a length byte, then the packet (report ID first), so a 64-byte stream packet with its
            // texture window can come too.
            while (i + 1 < bytes.size()) {
                const std::uint8_t head = bytes[i];
                const std::size_t len = std::min<std::size_t>(bytes.size() - i - 1, 1 + head % 70);
                const auto packet = bytes.subspan(i + 1, len);
                ffb.output(packet, now + head % 8);
                ffb.tick(now + head % 8);
                for (const auto& answer : device.output(packet, now += head % 8)) {
                    CHECK(answer.size() == kTrueForceReportSize && answer[0] == kTrueForceReportId);
                }
                if (const auto status = device.steering(head * 300 - 9000, head * 11, now)) {
                    CHECK(status->size() == kTrueForceReportSize);
                }
                i += 1 + len;
            }
            device.finish(now);
            ffb.stop(now);
            break;
        }
        case 14: {   // HID++ 0x8123 effects from Logitech's force feedback driver: every packet sent on is finite
            HidPpFfb ffb(
                [](const FfbPacket& p) {
                    CHECK(std::isfinite(p.effect.constantLevel) && std::isfinite(p.effect.gain) &&
                          std::isfinite(p.effect.periodic.phaseDeg));
                    CHECK(p.slot < HidPpFfb::kServiceSlots);
                },
                [](LogLevel, const std::string&) {});
            HidPp pp({"G923", 0xC26E, 1, 0x0A}, [](LogLevel, const std::string&) {});
            pp.setForceFeedback(&ffb);
            std::size_t i = 0;
            std::int64_t now = 0;
            while (i < bytes.size()) {
                const std::size_t len = std::min<std::size_t>(bytes.size() - i, 1 + bytes[i] % 70);
                std::vector<std::uint8_t> report(bytes.begin() + static_cast<std::ptrdiff_t>(i),
                                                 bytes.begin() + static_cast<std::ptrdiff_t>(i + len));
                if (report.size() >= 3) {
                    report[0] = HidPp::kLongId;
                    report[2] = 4;   // always ForceFeedback
                }
                (void)pp.handle(report, now += bytes[i] % 20);
                ffb.tick(now);
                i += len;
            }
            ffb.stopAll(now);
            break;
        }
        case 15: {   // a game's own settings file (NFS Unbound's Frostbite PROFILEOPTIONS, NFS 2015's profile)
            const GameSettingText values[] = {{"WheelSpringMagnitude", "0.800000"}, {"WheelMaxRotation", "900.000000"}};
            const GameSettingsEdit edit = editFrostbiteSettings(text, values);
            if (edit.problems.empty()) {
                CHECK(edit.bytes.size() == text.size());   // same-length values only: nothing else in the file moves
                for (const GameSettingsEdit::Change& change : edit.changes) {
                    CHECK(frostbiteSetting(edit.bytes, change.name) == change.after);
                }
            } else {
                CHECK(edit.bytes.empty() && edit.changes.empty());
            }
            (void)frostbiteSetting(text, "WheelSpringMagnitude");
            (void)checkNfs2015Settings(text);
            break;
        }
        case 16: {   // an Escape from a game (the shim's DeviceAudit): the first 4 bytes are the command
            std::uint32_t command = 0;
            if (bytes.size() >= 4) std::memcpy(&command, bytes.data(), 4);
            (void)decodeRevLights(command % 2, bytes.size() >= 4 ? bytes.subspan(4) : bytes);
            break;
        }
        case 17: {   // Linux's /proc/net/udp (the service's force feedback sender check): any port, any owner
            const std::uint16_t port = bytes.size() >= 2 ? static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8)) : 0;
            (void)udpPortUid(text, port);
            (void)udpPortUid(text, 41330);
            break;
        }
#ifdef WHEELBRI_G923CAP
        case 18: {   // a USBPcap capture (pcap or pcapng): frames, URBs, transfers, the devices' descriptors
            const auto file = g923cap::parseCapture(g923cap::Bytes(bytes.begin(), bytes.end()));
            const auto devices = g923cap::findDevices(g923cap::transfersOf(g923cap::urbsOf(file)));
            for (const auto& [key, device] : devices) {
                for (const auto& [interface, raw] : device.reportDescriptors) (void)g923cap::parseReportDescriptor(raw);
            }
            (void)g923cap::parseReportDescriptor(bytes);
            (void)g923cap::parseConfiguration(bytes);
            break;
        }
#endif
    }
}

}  // namespace wheelbri::fuzz
