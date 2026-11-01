#include "g923cap/Probe.hpp"

#include "g923cap/Protocol.hpp"
#include "wheelbri/TrueForce.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <sstream>

namespace wheelbri::g923cap {

bool safeHidpp(const std::uint16_t featureId, const std::uint8_t function) {
    switch (featureId) {
        case 0x0000: return function <= 1;   // Root: getFeature, getProtocolVersion
        case 0x0001: return function <= 1;   // FeatureSet: getCount, getFeatureId
        case 0x0003: return function <= 1;   // DeviceFwVersion: getEntityCount, getFwInfo
        case 0x0005: return function <= 2;   // DeviceNameType: getCount, getDeviceName, getDeviceType
        default: return false;
    }
}

Bytes trueForceRequest(const std::uint8_t type, const std::uint8_t seq, const std::span<const std::uint8_t> payload,
                       const std::size_t length, const bool seal) {
    if (std::find(kSafeTrueForceTypes.begin(), kSafeTrueForceTypes.end(), type) == kSafeTrueForceTypes.end()) {
        char b[48];
        std::snprintf(b, sizeof(b), "TrueForce type 0x%02X is not a read", type);
        throw Unsafe(b);
    }
    Bytes d(std::max<std::size_t>(length, 7 + payload.size()), 0);
    d[0] = 0x01;   // report ID; then 00 00 00, as the SDK sends them to a G923 (2026-11-01)
    d[4] = type;
    d[5] = seq;
    std::copy(payload.begin(), payload.end(), d.begin() + 6);
    if (seal) {
        const std::size_t at = 6 + payload.size();
        unsigned sum = 0;
        for (std::size_t i = 4; i < at; ++i) sum += d[i];
        d[at] = static_cast<std::uint8_t>((0x100U - (sum & 0xFFU)) & 0xFFU);
    }
    return d;
}

Bytes hidppRequest(const std::uint8_t featureIndex, const std::uint16_t featureId, const std::uint8_t function,
                   const std::span<const std::uint8_t> params, const std::size_t length) {
    if (!safeHidpp(featureId, function)) {
        char b[80];
        std::snprintf(b, sizeof(b), "HID++ feature 0x%04X function %u is not on the read-only list", featureId, function);
        throw Unsafe(b);
    }
    Bytes d(length, 0);
    d[0] = length == 20 ? 0x11 : length == 64 ? 0x12 : 0x10;
    d[1] = kDeviceIndex;
    d[2] = featureIndex;
    d[3] = static_cast<std::uint8_t>(function << 4 | kSoftwareId);
    for (std::size_t i = 0; i < params.size() && 4 + i < length; ++i) d[4 + i] = params[i];
    return d;
}

namespace {

template <class Match>
std::optional<Bytes> readAnswer(Transport& t, const double timeoutS, Match match, std::vector<Bytes>* seen) {
    using Clock = std::chrono::steady_clock;
    const auto end = Clock::now() + std::chrono::duration<double>(timeoutS);
    for (;;) {
        const double left = std::chrono::duration<double>(end - Clock::now()).count();
        if (left <= 0) return std::nullopt;
        auto d = t.read(left);
        if (!d) return std::nullopt;
        if (match(*d)) return d;
        if (seen != nullptr && seen->size() < 2000) seen->push_back(std::move(*d));
    }
}

}  // namespace

TrueForceProbe probeTrueForce(Transport& t, const double timeoutS, const int first, const int last) {
    TrueForceProbe res;
    std::vector<Bytes> seen;
    std::uint8_t seq = 0xA0;
    for (int idx = first; idx <= last; ++idx) {
        const std::uint8_t payload[] = {static_cast<std::uint8_t>(idx)};
        std::optional<Bytes> answer;
        std::vector<bool> tries = res.sealedRequests ? std::vector<bool>{*res.sealedRequests} : std::vector<bool>{false, true};
        for (const bool seal : tries) {
            const std::uint8_t s = ++seq;
            t.write(trueForceRequest(0x05, s, payload, t.outputLength(), seal));
            answer = readAnswer(t, timeoutS, [s](const Bytes& d) { return d.size() > 5 && d[4] == 0x05 && d[5] == s; }, &seen);
            if (answer) {
                res.sealedRequests = seal;
                break;
            }
        }
        if (!answer || answer->size() < 14) {
            res.noAnswer.push_back(idx);
            continue;
        }
        const Bytes& d = *answer;
        res.parameters[d[6]] = {u32At(d, 9), d[7] == 0x09 && d[8] == d[6] && sealedAt(d, 13),
                                hexOf(std::span<const std::uint8_t>(d).subspan(6, 8), 8)};
    }
    for (const Bytes& d : seen) {
        if (d.size() > 5) ++res.otherReports[d[4]];
    }
    return res;
}

HidppProbe probeHidpp(Transport& t, const double timeoutS) {
    HidppProbe res;
    const auto ask = [&](const std::uint8_t index, const std::uint16_t fid, const std::uint8_t fn,
                         const std::vector<std::uint8_t>& params) -> std::optional<Bytes> {
        const Bytes q = hidppRequest(index, fid, fn, params, t.outputLength());
        t.write(q);
        auto a = readAnswer(t, timeoutS, [&q](const Bytes& d) { return hidppAnswers(q, d); }, nullptr);
        if (a && isHidppError(*a)) {
            char b[64];
            std::snprintf(b, sizeof(b), "feature 0x%04X function %u: ", fid, fn);
            res.errors.push_back(b + hidppErrorName((*a)[5]));
            return std::nullopt;
        }
        return a;
    };
    if (auto a = ask(0, 0x0000, 1, {0, 0, 0x5A})) res.protocol = std::to_string((*a)[4]) + "." + std::to_string((*a)[5]);
    const auto indexOf = [&](const std::uint16_t fid) -> std::optional<std::uint8_t> {
        auto r = ask(0, 0x0000, 0, {static_cast<std::uint8_t>(fid >> 8), static_cast<std::uint8_t>(fid & 0xFF)});
        if (!r || (*r)[4] == 0) return std::nullopt;
        res.features[fid] = {(*r)[4], (*r)[5], (*r)[6]};
        return (*r)[4];
    };
    if (const auto fs = indexOf(0x0001)) {
        if (auto c = ask(*fs, 0x0001, 0, {})) {
            for (int i = 1; i <= (*c)[4]; ++i) {
                if (auto r = ask(*fs, 0x0001, 1, {static_cast<std::uint8_t>(i)})) {
                    const auto fid = static_cast<std::uint16_t>((*r)[4] << 8 | (*r)[5]);
                    res.features.try_emplace(fid, HidppFeature{i, (*r)[6], r->size() > 7 ? std::optional<int>((*r)[7]) : std::nullopt});
                }
            }
        }
    }
    if (const auto fw = indexOf(0x0003)) {
        if (auto c = ask(*fw, 0x0003, 0, {})) {
            for (int e = 0; e < (*c)[4]; ++e) {
                if (auto r = ask(*fw, 0x0003, 1, {static_cast<std::uint8_t>(e)}); r && r->size() >= 12) {
                    char b[96];
                    std::snprintf(b, sizeof(b), "entity %d: type %u %s %02X.%02X build %02X%02X", e, (*r)[4],
                                  asciiOf(std::span<const std::uint8_t>(*r).subspan(5, 3)).c_str(), (*r)[8], (*r)[9],
                                  (*r)[10], (*r)[11]);
                    res.firmware.emplace_back(b);
                }
            }
        }
    }
    if (const auto nm = indexOf(0x0005)) {
        if (auto c = ask(*nm, 0x0005, 0, {})) {
            const std::size_t n = (*c)[4];
            while (res.name.size() < n) {
                auto r = ask(*nm, 0x0005, 1, {static_cast<std::uint8_t>(res.name.size())});
                if (!r) break;
                std::string chunk;
                for (std::size_t i = 4; i < r->size() && (*r)[i] != 0; ++i) chunk += static_cast<char>((*r)[i]);
                if (chunk.empty()) break;
                res.name += chunk;
            }
            if (res.name.size() > n) res.name.resize(n);
        }
        if (auto r = ask(*nm, 0x0005, 2, {})) res.deviceType = (*r)[4];
    }
    return res;
}

std::string probeMarkdown(const ProbeRun& run) {
    std::ostringstream o;
    o << "# Probe\n\n" << run.when << "; G HUB: " << run.ghub << "\n\n";
    for (const auto& c : run.collections) o << "- " << c << "\n";
    if (run.trueForce) {
        const TrueForceProbe& t = *run.trueForce;
        o << "\n## TrueForce parameters (0x05)\n\nRequests "
          << (t.sealedRequests.value_or(false) ? "with" : "without") << " a checksum got answers. No answer for:";
        if (t.noAnswer.empty()) o << " none";
        for (const int i : t.noAnswer) {
            char b[4];
            std::snprintf(b, sizeof(b), "%02X", i);
            o << " " << b;
        }
        o << ". Other reports while asking:";
        if (t.otherReports.empty()) o << " none";
        for (const auto& [type, n] : t.otherReports) {
            char b[8];
            std::snprintf(b, sizeof(b), "0x%02X", type);
            o << " " << b << " x" << n;
        }
        o << ".\n\n| Index | Real G923 | WheelBri | Raw |\n|---|---|---|---|\n";
        for (int i = 0; i < 0x40; ++i) {
            const std::uint32_t ours = TrueForceDevice::parameterBits(static_cast<std::uint8_t>(i));
            const auto it = t.parameters.find(i);
            char idx[8];
            std::snprintf(idx, sizeof(idx), "0x%02X", i);
            const std::string ourText = numberText(static_cast<double>(std::bit_cast<float>(ours)));
            if (it == t.parameters.end()) {
                o << "| " << idx << " | no answer | " << ourText << " | |\n";
                continue;
            }
            const std::string mark = it->second.bits != ours ? "**" : "";
            o << "| " << idx << " | " << mark << numberText(static_cast<double>(std::bit_cast<float>(it->second.bits)))
              << mark << " | " << ourText << " | `" << it->second.raw << "` |\n";
        }
    }
    if (run.hidpp) {
        const HidppProbe& h = *run.hidpp;
        o << "\n## HID++\n\nProtocol " << h.protocol.value_or("None") << "; name '" << h.name << "'; device type "
          << (h.deviceType ? std::to_string(*h.deviceType) : "None") << "\n\n";
        std::vector<std::pair<std::uint16_t, HidppFeature>> features(h.features.begin(), h.features.end());
        std::sort(features.begin(), features.end(), [](const auto& a, const auto& b) { return a.second.index < b.second.index; });
        for (const auto& [fid, f] : features) {
            o << "- index " << f.index << ": " << featureName(fid) << " (type " << f.type << ", version "
              << (f.version ? std::to_string(*f.version) : "None") << ")\n";
        }
        for (const auto& fw : h.firmware) o << "- firmware " << fw << "\n";
        for (const auto& e : h.errors) o << "- error: " << e << "\n";
    }
    for (const auto& p : run.problems) o << "- problem: " << p << "\n";
    return o.str();
}

}  // namespace wheelbri::g923cap
