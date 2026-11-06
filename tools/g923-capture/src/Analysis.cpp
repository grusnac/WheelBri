// SPDX-License-Identifier: GPL-3.0-or-later
#include "g923cap/Analysis.hpp"

#include "g923cap/Protocol.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <regex>
#include <set>
#include <sstream>

namespace wheelbri::g923cap {

namespace {

constexpr double kAnswerWindowS = 0.25;
const std::vector<std::string> kCaptureSuffixes = {".pcap", ".pcapng"};

std::string hex2(const int v) {
    char b[8];
    std::snprintf(b, sizeof(b), "0x%02X", v & 0xFF);
    return b;
}

template <class T>
std::vector<std::pair<std::string, std::size_t>> top(const std::map<T, std::size_t>& counts, const std::size_t n,
                                                     const std::function<std::string(const T&)>& name) {
    std::vector<std::pair<std::string, std::size_t>> v;
    for (const auto& [k, c] : counts) v.emplace_back(name(k), c);
    std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (v.size() > n) v.resize(n);
    return v;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    const double hi = v[mid];
    if (v.size() % 2 == 1) return hi;
    const double lo = *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid));
    return (lo + hi) / 2;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

std::vector<std::string> split(const std::string& s, const char sep) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t at = s.find(sep, start);
        parts.push_back(s.substr(start, at == std::string::npos ? std::string::npos : at - start));
        if (at == std::string::npos) break;
        start = at + 1;
    }
    return parts;
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    std::string text = s.str();
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    return text;
}

}  // namespace

const char* kindName(const Kind kind) {
    switch (kind) {
        case Kind::Input: return "input";
        case Kind::Hidpp: return "hidpp";
        case Kind::TrueForce: return "truforce";
        case Kind::Vendor: return "vendor";
        case Kind::Mixed: return "mixed";
        case Kind::InputHidpp: return "input+hidpp";
        default: return "?";
    }
}

// --- Devices --------------------------------------------------------------------------------------------------------

std::string Device::ident() const {
    char b[96];
    if (!descriptor) {
        std::snprintf(b, sizeof(b), "bus %u address %u (no descriptor seen)", bus, address);
    } else {
        std::snprintf(b, sizeof(b), "%04X:%04X on bus %u address %u", descriptor->vendor, descriptor->product, bus,
                      address);
    }
    return b;
}

std::vector<Collection> Device::collections(const int interface) const {
    const auto it = reportDescriptors.find(interface);
    return it == reportDescriptors.end() ? std::vector<Collection>{} : parseReportDescriptor(it->second);
}

std::optional<int> Device::interfaceOfEndpoint(const std::uint8_t endpoint) const {
    for (const Interface& i : interfaces) {
        for (const Endpoint& e : i.endpoints) {
            if (e.address == endpoint) return i.number;
        }
    }
    return std::nullopt;
}

namespace {

Kind kindOfPage(const std::uint16_t page) {
    if (page == 0xFFFD) return Kind::TrueForce;
    if (page == 0xFF43) return Kind::Hidpp;
    return page >= 0xFF00 ? Kind::Vendor : Kind::Input;
}

}  // namespace

Kind Device::kindOfInterface(const std::optional<int> interface) const {
    if (!interface) return Kind::Unknown;
    std::set<Kind> kinds;
    for (const Collection& c : collections(*interface)) kinds.insert(kindOfPage(c.usagePage));
    if (kinds.empty()) return Kind::Unknown;
    return kinds.size() == 1 ? *kinds.begin() : Kind::Mixed;
}

Kind Device::kindOfReport(const std::optional<int> interface, const std::uint8_t reportId) const {
    if (!interface) return Kind::Unknown;
    for (const Collection& c : collections(*interface)) {
        if (c.reports.count(reportId) != 0) return kindOfPage(c.usagePage);
    }
    return Kind::Unknown;
}

std::map<DeviceKey, Device> findDevices(const std::vector<Transfer>& transfers) {
    std::map<DeviceKey, Device> devices;
    for (const Transfer& t : transfers) {
        Device& d = devices[{t.bus, t.device}];
        d.bus = t.bus;
        d.address = t.device;
        ++d.transfers;
        if (t.kind != TransferKind::Control && t.data.size() == 64 && isTrueForceReport(t.data)) ++d.trueForceLike;
        if (t.kind != TransferKind::Control || t.setup.empty()) continue;
        const int kind = t.value() >> 8;
        if (t.requestType() == 0x80 && t.request() == 0x06 && !t.data.empty()) {
            if (kind == 0x01) {
                if (auto parsed = parseDeviceDescriptor(t.data)) {
                    d.descriptor = std::move(parsed);
                    d.descriptorFrame = t.frame;
                }
            } else if (kind == 0x02 && t.data.size() >= 9) {
                const std::size_t total = t.data[2] | t.data[3] << 8;
                if (t.data.size() >= total || d.interfaces.empty()) {
                    d.interfaces = parseConfiguration(t.data);
                    d.configurationFrame = t.frame;
                }
            }
        } else if (t.requestType() == 0x81 && t.request() == 0x06 && kind == 0x22 && !t.data.empty()) {
            d.reportDescriptors[t.index()] = t.data;
            d.reportDescriptorFrames[t.index()] = t.frame;
        }
    }
    return devices;
}

Pick pickTarget(const std::map<DeviceKey, Device>& devices, const std::optional<int> address) {
    if (address) {
        for (const auto& [key, d] : devices) {
            if (d.address == *address) return {&d, "address " + std::to_string(*address) + ", as asked"};
        }
    }
    if (rehearsalTarget()) {
        for (const auto& [key, d] : devices) {
            if (d.descriptor && isTarget(d.descriptor->vendor, d.descriptor->product)) {
                char b[64];
                std::snprintf(b, sizeof(b), "its device descriptor (%04X:%04X, rehearsal)", d.descriptor->vendor,
                              d.descriptor->product);
                return {&d, b};
            }
        }
        return {nullptr, "the rehearsal wheel is not in this capture"};
    }
    for (const auto& [key, d] : devices) {
        if (d.descriptor && d.descriptor->vendor == kLogitech && d.descriptor->product == kG923) {
            return {&d, "its device descriptor (046D:C26E)"};
        }
    }
    for (const auto& [key, d] : devices) {
        if (d.descriptor && d.descriptor->vendor == kLogitech && otherG923Name(d.descriptor->product) != nullptr) {
            return {&d, std::string("its device descriptor (") + otherG923Name(d.descriptor->product) + ")"};
        }
    }
    const Device* best = nullptr;
    for (const auto& [key, d] : devices) {
        if (d.trueForceLike > 0 && (best == nullptr || d.trueForceLike > best->trueForceLike)) best = &d;
    }
    if (best != nullptr) return {best, "the most TrueForce-looking traffic (no descriptor in this capture)"};
    return {nullptr, "no G923 in this capture"};
}

// --- Reports --------------------------------------------------------------------------------------------------------

std::vector<Report> reportsOf(const Device& device, const std::vector<Transfer>& transfers) {
    std::vector<Report> out;
    for (const Transfer& t : transfers) {
        if (t.bus != device.bus || t.device != device.address) continue;
        if (t.kind == TransferKind::In || t.kind == TransferKind::Out) {
            out.push_back({t.ts, t.kind == TransferKind::Out, device.interfaceOfEndpoint(t.endpoint), t.data, t.frame,
                           "interrupt"});
        } else if (t.requestType() == 0x21 && t.request() == 0x09 && !t.outData.empty()) {
            out.push_back({t.ts, true, t.index(), t.outData, t.frame, "SET_REPORT"});
        } else if (t.requestType() == 0xA1 && t.request() == 0x01 && !t.data.empty()) {
            out.push_back({t.ts, false, t.index(), t.data, t.frame, "GET_REPORT"});
        }
    }
    return out;
}

std::map<int, Kind> interfaceKinds(const Device& device, const std::vector<Report>& reports) {
    struct Votes {
        std::size_t trueForce = 0, hidpp = 0, input = 0, hidppOut = 0;
    };
    std::map<int, Votes> votes;
    for (const Report& r : reports) {
        if (!r.interface) continue;
        Votes& v = votes[*r.interface];
        if (r.out && isHidpp(r.data)) ++v.hidppOut;
        if (r.data.size() == 64 && isTrueForceReport(r.data)) ++v.trueForce;
        else if (isHidpp(r.data)) ++v.hidpp;
        else ++v.input;
    }
    std::map<int, Kind> kinds;
    for (const auto& [interface, v] : votes) {
        const Kind known = device.kindOfInterface(interface);
        if (known != Kind::Unknown) {
            kinds[interface] = known;
            continue;
        }
        const std::size_t total = v.trueForce + v.hidpp + v.input;
        Kind kind = Kind::Input;
        if (v.trueForce * 2 > total) kind = Kind::TrueForce;
        else if (v.hidpp * 2 > total) kind = Kind::Hidpp;
        // HID++ sharing an interface with the game reports: the host writes HID++ there, a game report never looks
        // like that, so its HID++-shaped reports are HID++ all the same.
        if (kind == Kind::Input && v.hidppOut > 0) kind = Kind::InputHidpp;
        kinds[interface] = kind;
    }
    return kinds;
}

Kind classify(const Device& device, const Report& r, const std::map<int, Kind>& kinds) {
    Kind kind = device.kindOfInterface(r.interface);
    if (kind == Kind::Mixed && !r.data.empty()) kind = device.kindOfReport(r.interface, r.data[0]);
    if (kind == Kind::Unknown && r.interface) {
        const auto it = kinds.find(*r.interface);
        if (it != kinds.end()) {
            kind = it->second;
            if (kind == Kind::InputHidpp) return isHidpp(r.data) ? Kind::Hidpp : Kind::Input;
        }
    }
    if (kind == Kind::TrueForce) return Kind::TrueForce;
    if ((kind == Kind::Hidpp || kind == Kind::Vendor) && isHidpp(r.data)) return Kind::Hidpp;
    if (kind == Kind::Input) return Kind::Input;
    // No report descriptor for that interface: by the look of the report.
    if (r.data.size() == 64 && isTrueForceReport(r.data)) return Kind::TrueForce;
    if (isHidpp(r.data)) return Kind::Hidpp;
    return Kind::Input;
}

// --- Notes and shim logs ----------------------------------------------------------------------------------------------

Notes readNotes(const std::filesystem::path& folder) {
    Notes notes;
    const std::filesystem::path path = folder / "notes.txt";
    if (!std::filesystem::exists(path)) return notes;
    std::map<std::string, std::pair<double, double>> open;
    std::istringstream lines(readText(path));
    std::string line;
    while (std::getline(lines, line)) {
        const auto parts = split(trim(line), '\t');
        if (parts.size() < 4 || parts[0].empty() ||
            !std::all_of(parts[0].begin(), parts[0].end(), [](const char c) { return c >= '0' && c <= '9'; })) {
            continue;
        }
        const double ts = std::stod(parts[0]) / 1000.0;
        const std::string kind = trim(parts[2]);
        const std::string text = trim(parts[3]);
        notes.lines.push_back({ts, parts[1], kind, text});
        if (kind == "HOLD_START") {
            const std::size_t bar = text.find('|');
            if (bar == std::string::npos) continue;
            try {
                open[text.substr(0, bar)] = {ts, std::stod(text.substr(bar + 1))};
            } catch (const std::exception&) {
                // not a number after the bar: that line is skipped
            }
        } else if (kind == "HOLD_END") {
            const auto it = open.find(text);
            if (it != open.end()) {
                notes.holds.push_back({text, it->second.second, it->second.first, ts});
                open.erase(it);
            }
        }
    }
    return notes;
}

ShimLog readShimLogs(const std::filesystem::path& folder) {
    ShimLog log;
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("wheelbri-shim-", 0) == 0 && entry.path().extension() == ".log") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    static const std::regex kLine(R"(^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\].*?\b(logi[A-Za-z0-9_]+) #(\d+))");
    for (const auto& f : files) {
        log.files.push_back(f.filename().string());
        std::istringstream lines(readText(f));
        std::string line;
        while (std::getline(lines, line)) {
            if (line.find(" #") == std::string::npos) continue;   // quick reject before the regex
            std::smatch m;
            if (std::regex_search(line, m, kLine)) {
                ++log.calls[m[2]];
                log.first.emplace(m[2], m[1]);
            }
        }
    }
    return log;
}

// --- TrueForce ------------------------------------------------------------------------------------------------------

double TfParameter::value() const { return static_cast<double>(std::bit_cast<float>(bits)); }

TrueForceResult analyseTrueForce(const std::vector<const Report*>& reports, const std::vector<Hold>& holds,
                                 const std::optional<std::filesystem::path>& streamTsv) {
    TrueForceResult res;
    std::vector<const Report*> outs, ins;
    for (const Report* r : reports) (r->out ? outs : ins).push_back(r);
    for (const Report* r : outs) {
        if (r->data.size() > 5) ++res.requests[r->data[4]];
    }
    for (const Report* r : ins) {
        if (r->data.size() > 5) ++res.answers[r->data[4]];
    }
    for (const Report* r : outs) {
        if (r->data.size() > 5 && r->data[4] != 0x01) {
            auto& ex = res.examples[r->data[4]];
            if (ex.size() < 6) ex.push_back({r->frame, hexOf(r->data, 24), r->via});
        }
    }

    // Answers by (type, sequence) and, but for the plain status, by sequence alone: in time order.
    std::map<std::pair<int, int>, std::vector<const Report*>> byTypeSeq;
    std::map<int, std::vector<const Report*>> bySeq;
    for (const Report* r : ins) {
        if (r->data.size() <= 5) continue;
        byTypeSeq[{r->data[4], r->data[5]}].push_back(r);
        if (r->data[4] != 0x02) bySeq[r->data[5]].push_back(r);
    }
    const auto firstAfter = [](const std::vector<const Report*>& list, const double ts) -> const Report* {
        auto it = std::lower_bound(list.begin(), list.end(), ts - 0.002,
                                   [](const Report* r, const double t) { return r->ts < t; });
        if (it != list.end() && (*it)->ts <= ts + kAnswerWindowS) return *it;
        return nullptr;
    };

    std::map<int, std::vector<double>> delays;
    for (const Report* r : outs) {
        if (r->data.size() < 6 || r->data[4] == 0x01) continue;
        const int type = r->data[4];
        const int seq = r->data[5];
        const auto expected = expectedAnswers(static_cast<std::uint8_t>(type));
        const Report* answer = nullptr;
        if (!expected.empty()) {
            for (const std::uint8_t at : expected) {
                const auto it = byTypeSeq.find({at, seq});
                if (it == byTypeSeq.end()) continue;
                answer = firstAfter(it->second, r->ts);
                if (answer != nullptr) break;
            }
        } else {
            const auto it = byTypeSeq.find({type, seq});
            if (it != byTypeSeq.end()) answer = firstAfter(it->second, r->ts);
            if (answer == nullptr) {
                const auto s = bySeq.find(seq);
                if (s != bySeq.end()) answer = firstAfter(s->second, r->ts);
            }
        }
        TfPair& p = res.pairs[type];
        ++p.sent;
        if (!p.firstRequest) p.firstRequest = Example{r->frame, hexOf(r->data, 16), r->via};
        if (answer != nullptr) {
            ++p.answered;
            ++p.answerTypes[answer->data[4]];
            delays[type].push_back((answer->ts - r->ts) * 1000.0);
            if (!p.firstAnswer) {
                p.firstAnswer = Example{answer->frame, hexOf(answer->data, 34), answer->via};
                p.sealAt = sealOf(answer->data[4]);
                if (!p.sealAt) p.sealAt = findSeal(answer->data);
                p.sealed = p.sealAt && sealedAt(answer->data, *p.sealAt);
            }
        } else if (res.unanswered.size() < 30) {
            res.unanswered.push_back({r->frame, hexOf(r->data, 16), r->via});
        }
        if (type == 0x05 && r->data.size() > 6) res.parameterRequests.push_back(r->data[6]);
    }
    for (auto& [type, d] : delays) {
        if (!d.empty()) res.pairs[type].medianDelayMs = std::round(median(d) * 100.0) / 100.0;
    }

    // Parameters, from every 0x05 answer: 05 s idx 09 idx <f32> <seal>.
    for (const Report* r : ins) {
        const Bytes& d = r->data;
        if (d.size() < 14 || d[4] != 0x05) continue;
        const std::uint32_t bits = u32At(d, 9);
        auto [it, added] = res.parameters.try_emplace(d[6]);
        TfParameter& p = it->second;
        if (added) {
            p.bits = bits;
            p.frame = r->frame;
            p.wellFormed = d[7] == 0x09 && d[8] == d[6] && sealedAt(d, 13);
            p.raw = hexOf(std::span<const std::uint8_t>(d).subspan(6, 8), 8);
        }
        if (std::find(p.bitsSeen.begin(), p.bitsSeen.end(), bits) == p.bitsSeen.end()) p.bitsSeen.push_back(bits);
    }

    for (const Report* r : ins) {
        const Bytes& d = r->data;
        if (d.size() > 25 && d[4] == 0x07 && res.slots.size() < 3) {
            TfSlots s{r->frame, d[6], {}, hexOf(d, 30), sealedAt(d, 25)};
            for (int k = 0; k < std::min(static_cast<int>(d[6]), 18) && 10 + k * 3 <= static_cast<int>(d.size()); ++k) {
                s.slots.emplace_back(d.begin() + 7 + k * 3, d.begin() + 10 + k * 3);
            }
            res.slots.push_back(std::move(s));
        }
    }

    // Status reports.
    std::vector<const Report*> statuses;
    for (const Report* r : ins) {
        if (r->data.size() >= 34 && isStatusType(r->data[4])) statuses.push_back(r);
    }
    for (const int type : {0x02, 0x10, 0x11, 0x14}) {
        std::vector<const Report*> these;
        for (const Report* r : statuses) {
            if (r->data[4] == type) these.push_back(r);
        }
        if (these.empty()) continue;
        TfStatus s;
        s.count = these.size();
        const double span = std::max(these.back()->ts - these.front()->ts, 1e-9);
        if (these.size() > 1) s.ratePerS = std::round(static_cast<double>(these.size()) / span * 10.0) / 10.0;
        s.positionMin = UINT32_MAX;
        std::map<std::string, std::size_t> at9, at21, countRange, torque1, torque2;
        std::vector<double> clockSteps, gaps;
        std::uint32_t lastClock = 0;
        double lastTs = 0;
        s.speedMin = INT64_MAX;
        s.speedMax = INT64_MIN;
        for (std::size_t i = 0; i < these.size(); ++i) {
            const Bytes& d = these[i]->data;
            const std::uint32_t pos = u24At(d, 6);
            s.positionMin = std::min(s.positionMin, pos);
            s.positionMax = std::max(s.positionMax, pos);
            ++at9[hexOf(std::span<const std::uint8_t>(d).subspan(9, 4), 4)];
            ++at21[std::to_string(d[21])];
            ++countRange[std::to_string(u24At(d, 22))];
            const std::uint32_t t1 = u32At(d, 25);
            const std::uint32_t t2 = u32At(d, 29);
            ++torque1[numberText(std::round((static_cast<double>(t1) - 2147483648.0) / kTorqueUnit * 1e4) / 1e4)];
            ++torque2[numberText(std::round((static_cast<double>(t2) - 2147483648.0) / kTorqueUnit * 1e4) / 1e4)];
            if (sealedAt(d, 33)) ++s.sealed;
            const std::uint32_t clock = u32At(d, 13);
            if (i > 0) {
                if (clock >= lastClock) clockSteps.push_back(static_cast<double>(clock - lastClock));
                gaps.push_back((these[i]->ts - lastTs) * 1e6);
            }
            lastClock = clock;
            lastTs = these[i]->ts;
            const std::int64_t speed = static_cast<std::int64_t>(u32At(d, 17)) - 0x80000000LL;
            s.speedMin = std::min(s.speedMin, speed);
            s.speedMax = std::max(s.speedMax, speed);
            if (s.at17Examples.size() < 3) {
                char b[16];
                std::snprintf(b, sizeof(b), "0x%08X", u32At(d, 17));
                s.at17Examples.emplace_back(b);
            }
        }
        const std::function<std::string(const std::string&)> same = [](const std::string& k) { return k; };
        s.at9 = top(at9, 4, same);
        s.at21 = top(at21, 3, same);
        s.countRange = top(countRange, 3, same);
        s.torque1 = top(torque1, 3, same);
        s.torque2 = top(torque2, 3, same);
        char raw[32];
        std::snprintf(raw, sizeof(raw), "0x%08X 0x%08X", u32At(these.front()->data, 25), u32At(these.front()->data, 29));
        s.torqueRaw = raw;
        if (!clockSteps.empty()) s.clockStepUs = median(clockSteps);
        if (!gaps.empty()) s.hostGapUs = std::round(median(gaps) * 10.0) / 10.0;
        s.first = {these.front()->frame, hexOf(these.front()->data, 36), these.front()->via};
        res.status[type] = std::move(s);
    }

    // Held angles: the median position in each hold, its first second left out (the hand settles).
    for (const Hold& h : holds) {
        std::vector<double> inside;
        auto it = std::lower_bound(statuses.begin(), statuses.end(), h.start + 1.0,
                                   [](const Report* r, const double t) { return r->ts < t; });
        for (; it != statuses.end() && (*it)->ts <= h.end; ++it) inside.push_back(u24At((*it)->data, 6));
        TfHold th{h.label, h.degrees, inside.size(), std::nullopt, std::nullopt, std::nullopt};
        if (!inside.empty()) th.position = median(inside);
        res.holds.push_back(std::move(th));
    }
    for (const TfHold& h : res.holds) {
        if (h.degrees == 0 && h.position) {
            res.centre = h.position;
            break;
        }
    }
    for (TfHold& h : res.holds) {
        if (res.centre && h.position && h.degrees != 0) {
            const double perDegree = (*h.position - *res.centre) / h.degrees;
            h.perDegree = std::round(perDegree * 1000.0) / 1000.0;
            h.perTurn = std::round(perDegree * 360.0 * 10.0) / 10.0;
        }
    }

    // The stream: [6..7] cur (offset binary, 0x8000 = none), [10] new samples, [11] 0x0d = window valid, 13 texture
    // slots of 4 bytes (left, right) from [12].
    std::vector<const Report*> stream;
    for (const Report* r : outs) {
        if (r->data.size() >= 12 && r->data[4] == 0x01) stream.push_back(r);
    }
    if (!stream.empty()) {
        TfStream s;
        s.count = stream.size();
        s.ratePerS = std::round(static_cast<double>(stream.size()) /
                                std::max(stream.back()->ts - stream.front()->ts, 1e-9) * 10.0) / 10.0;
        s.firstFrame = stream.front()->frame;
        std::map<std::string, std::size_t> b1011, b89;
        double curMin = 1e9, curMax = -1e9;
        bool anyCur = false;
        int peak = -1;
        std::ofstream tsv;
        if (streamTsv) {
            tsv.open(*streamTsv, std::ios::binary | std::ios::trunc);
            tsv << "time_s\tframe\tseq\tcur_raw\tcur\tnew\twindow\ttexture_lr_hex\n";
        }
        const double t0 = stream.front()->ts;
        for (const Report* r : stream) {
            const Bytes& d = r->data;
            const int cur = d[6] | d[7] << 8;
            const double curValue = cur != 0 ? (cur - 0x8000) / 32768.0 : 0.0;
            if (cur != 0) {
                anyCur = true;
                curMin = std::min(curMin, curValue);
                curMax = std::max(curMax, curValue);
            }
            if (cur == 0x8000) ++s.curNone;
            char b[16];
            std::snprintf(b, sizeof(b), "%02x/%02x", d[10], d[11]);
            ++b1011[b];
            ++b89[hexPlain(std::span<const std::uint8_t>(d).subspan(8, 2))];
            ++s.via[r->via];
            std::string texture;
            if (d[11] == 0x0D && d.size() >= 12 + 13 * 4) {
                for (std::size_t slot = 0; slot < 13; ++slot) {
                    const std::size_t at = 12 + slot * 4;
                    const int left = d[at] | d[at + 1] << 8;
                    const int right = d[at + 2] | d[at + 3] << 8;
                    peak = std::max({peak, std::abs(left - 0x8000), std::abs(right - 0x8000)});
                    if (tsv.is_open()) {
                        char t[16];
                        std::snprintf(t, sizeof(t), "%s%04x/%04x", slot ? " " : "", left, right);
                        texture += t;
                    }
                }
            }
            if (tsv.is_open()) {
                char line[96];
                std::snprintf(line, sizeof(line), "%.6f\t%zu\t%d\t%d\t%.5f\t%d\t%02x\t", r->ts - t0, r->frame, d[5], cur,
                              curValue, d[10], d[11]);
                tsv << line << texture << '\n';
            }
        }
        if (anyCur) s.curRange = std::make_pair(std::round(curMin * 1e4) / 1e4, std::round(curMax * 1e4) / 1e4);
        const std::function<std::string(const std::string&)> same = [](const std::string& k) { return k; };
        s.bytes10and11 = top(b1011, 8, same);
        s.bytes8and9 = top(b89, 4, same);
        if (peak >= 0) s.texturePeak = peak;
        res.stream = std::move(s);
    }
    return res;
}

// --- HID++ ----------------------------------------------------------------------------------------------------------

HidppResult analyseHidpp(const std::vector<const Report*>& reports) {
    HidppResult res;
    std::vector<const Report*> outs, ins;
    for (const Report* r : reports) (r->out ? outs : ins).push_back(r);
    std::vector<bool> used(ins.size(), false);
    std::vector<std::pair<const Report*, std::optional<std::size_t>>> pairs;   // request, answer index in ins
    for (const Report* q : outs) {
        std::optional<std::size_t> answer;
        auto start = std::lower_bound(ins.begin(), ins.end(), q->ts - 0.002,
                                      [](const Report* r, const double t) { return r->ts < t; });
        for (auto it = start; it != ins.end() && (*it)->ts <= q->ts + 1.0; ++it) {
            const auto k = static_cast<std::size_t>(it - ins.begin());
            if (!used[k] && hidppAnswers(q->data, (*it)->data)) {
                used[k] = true;
                answer = k;
                break;
            }
        }
        pairs.emplace_back(q, answer);
    }
    res.requests = static_cast<int>(outs.size());
    res.featureMap[0] = 0x0000;
    for (const auto& [q, a] : pairs) {
        if (!a) continue;
        ++res.answered;
        const Bytes& ad = ins[*a]->data;
        if (isHidppError(ad)) continue;
        if (q->data[2] == 0x00 && q->data[3] >> 4 == 0 && q->data.size() >= 6 && ad[4] != 0) {
            res.featureMap[ad[4]] = static_cast<std::uint16_t>(q->data[4] << 8 | q->data[5]);
        }
    }
    // FeatureSet getFeatureId answers fill in the rest.
    std::optional<int> featureSet;
    for (const auto& [i, f] : res.featureMap) {
        if (f == 0x0001) featureSet = i;
    }
    if (featureSet) {
        for (const auto& [q, a] : pairs) {
            if (a && !isHidppError(ins[*a]->data) && q->data[2] == *featureSet && q->data[3] >> 4 == 1) {
                res.featureMap[q->data[4]] = static_cast<std::uint16_t>(ins[*a]->data[4] << 8 | ins[*a]->data[5]);
            }
        }
    }
    std::map<std::pair<int, int>, HidppRow> rows;
    std::map<int, Bytes> nameChunks;   // offset -> the first answer for it (G HUB asks more than once)
    for (const auto& [q, a] : pairs) {
        const auto key = std::make_pair(static_cast<int>(q->data[2]), q->data[3] >> 4);
        HidppRow& row = rows[key];
        row.featureIndex = q->data[2];
        row.function = static_cast<std::uint8_t>(q->data[3] >> 4);
        ++row.count;
        ++row.softwareIds[q->data[3] & 0x0F];
        ++row.reportIds[q->data[0]];
        Bytes params(q->data.begin() + 4, q->data.end());
        while (!params.empty() && params.back() == 0) params.pop_back();
        HidppExample ex{q->frame, hexOf(params, 64), std::nullopt, std::nullopt};
        if (a) {
            ++row.answered;
            const Bytes& ad = ins[*a]->data;
            if (isHidppError(ad)) ++row.errors[hidppErrorName(ad[5])];
            Bytes ap(ad.begin() + 4, ad.end());
            while (!ap.empty() && ap.back() == 0) ap.pop_back();
            ex.answer = hexOf(ap, 64);
            ex.answerFrame = ins[*a]->frame;
            const auto fid = res.featureMap.find(q->data[2]);
            if (!isHidppError(ad) && fid != res.featureMap.end()) {
                if (fid->second == 0x0003 && q->data[3] >> 4 == 1 && ad.size() >= 20) {
                    char num[8], rev[8], build[8];
                    std::snprintf(num, sizeof(num), "%02X", ad[8]);
                    std::snprintf(rev, sizeof(rev), "%02X", ad[9]);
                    std::snprintf(build, sizeof(build), "%02X%02X", ad[10], ad[11]);
                    Firmware fw{q->data[4], ad[4], asciiOf(std::span<const std::uint8_t>(ad).subspan(5, 3)), num, rev,
                                build, hexOf(std::span<const std::uint8_t>(ad).subspan(4, 16), 16)};
                    // G HUB asks again and again: each entity once.
                    if (std::none_of(res.firmware.begin(), res.firmware.end(),
                                     [&fw](const Firmware& f) { return f.entity == fw.entity && f.raw == fw.raw; })) {
                        res.firmware.push_back(std::move(fw));
                    }
                }
                if (fid->second == 0x0005 && q->data[3] >> 4 == 1) nameChunks.try_emplace(q->data[4], ad.begin() + 4, ad.end());
            }
        } else if (res.unanswered.size() < 20) {
            res.unanswered.push_back({q->frame, hexOf(q->data, 20), q->via});
        }
        if (row.examples.size() < 4 &&
            std::none_of(row.examples.begin(), row.examples.end(), [&ex](const auto& e) { return e.params == ex.params; })) {
            row.examples.push_back(std::move(ex));
        }
        ++res.via[q->via];
    }
    for (auto& [key, row] : rows) res.rows.push_back(std::move(row));
    // Each answer carries the name from its offset on (16 characters in a long report, all of it in a very long one).
    for (const auto& [offset, chunk] : nameChunks) {
        for (std::size_t i = 0; i < chunk.size() && chunk[i] != 0; ++i) {
            const std::size_t at = static_cast<std::size_t>(offset) + i;
            if (res.name.size() <= at) res.name.resize(at + 1, ' ');
            res.name[at] = static_cast<char>(chunk[i]);
        }
    }
    for (std::size_t k = 0; k < ins.size(); ++k) {
        if (used[k]) continue;
        ++res.unsolicited;
        if (res.unsolicitedExamples.size() < 8) res.unsolicitedExamples.push_back({ins[k]->frame, hexOf(ins[k]->data, 20), ins[k]->via});
    }
    return res;
}

InputResult analyseInput(const std::vector<const Report*>& reports) {
    InputResult res;
    std::map<std::string, std::size_t> inShapes, outShapes;
    for (const Report* r : reports) {
        const std::string itf = r->interface ? std::to_string(*r->interface) : std::string("None");
        if (!r->out) {
            ++res.in;
            if (!r->data.empty()) ++inShapes["id " + hex2(r->data[0]) + ", " + std::to_string(r->data.size()) + " bytes, interface " + itf];
        } else {
            ++res.out;
            if (!r->data.empty()) {
                ++outShapes["id " + hex2(r->data[0]) + ", " + std::to_string(r->data.size()) + " bytes, interface " + itf + ", " + r->via];
            }
            if (res.outExamples.size() < 10) res.outExamples.push_back({r->frame, hexOf(r->data, 24), r->via});
        }
    }
    const std::function<std::string(const std::string&)> same = [](const std::string& k) { return k; };
    res.inShapes = top(inShapes, 6, same);
    res.outShapes = top(outShapes, 10, same);
    return res;
}

// --- A session --------------------------------------------------------------------------------------------------------

std::filesystem::path descriptorCache(const std::filesystem::path& day, const DeviceDescriptor& d) {
    char b[48];
    std::snprintf(b, sizeof(b), "descriptors-%04X-%04X.txt", d.vendor, d.product);
    return day / b;
}

SessionResult analyseSession(const std::filesystem::path& folder, const std::optional<int> address) {
    SessionResult res;
    res.session = folder.filename().string();
    res.folder = folder;
    std::vector<std::filesystem::path> captures;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](const unsigned char c) { return std::tolower(c); });
        if (entry.is_regular_file() && (ext == ".pcap" || ext == ".pcapng")) captures.push_back(entry.path());
    }
    std::sort(captures.begin(), captures.end());
    for (const auto& c : captures) res.captures.push_back(c.filename().string());
    res.notes = readNotes(folder);
    res.shim = readShimLogs(folder);
    if (captures.empty()) {
        res.error = "no .pcap or .pcapng in this folder";
        return res;
    }

    // Several captures (one per root hub when the hub was not known): the one with the wheel.
    std::vector<Transfer> transfers;
    std::map<DeviceKey, Device> devices;
    bool found = false;
    for (const auto& cap : captures) {
        CaptureFile file = readCapture(cap);
        std::vector<Transfer> tr = transfersOf(urbsOf(file));
        auto devs = findDevices(tr);
        const Pick pick = pickTarget(devs, address);
        if (pick.device != nullptr && (!found || pick.device->descriptor)) {
            res.target = *pick.device;
            res.pickedBy = pick.why;
            res.capture = cap.filename().string();
            transfers = std::move(tr);
            devices = std::move(devs);
            found = true;
            if (res.target.descriptor) break;
        }
    }
    if (!found) {
        std::string names;
        for (const auto& c : res.captures) names += (names.empty() ? "" : ", ") + c;
        res.error = "no G923 found in " + names;
        return res;
    }
    for (const auto& [key, d] : devices) {
        if (d.descriptor && !(d.bus == res.target.bus && d.address == res.target.address)) {
            res.otherDevices.push_back(d.ident());
        }
    }
    if (!transfers.empty()) res.spanS = std::round((transfers.back().ts - transfers.front().ts) * 10.0) / 10.0;

    // Report descriptors come only with a plug-in (S1); the other sessions borrow them from the day's cache.
    if (res.target.descriptor) {
        const std::filesystem::path cache = descriptorCache(folder.parent_path(), *res.target.descriptor);
        if (!res.target.reportDescriptors.empty()) {
            std::ofstream out(cache, std::ios::binary | std::ios::trunc);
            for (const auto& [itf, raw] : res.target.reportDescriptors) out << itf << ' ' << hexPlain(raw) << '\n';
        } else if (std::filesystem::exists(cache)) {
            std::istringstream lines(readText(cache));
            std::string line;
            while (std::getline(lines, line)) {
                const std::size_t sp = line.find(' ');
                if (sp == std::string::npos) continue;
                const std::string hex = trim(line.substr(sp + 1));
                Bytes raw;
                for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
                    raw.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
                }
                res.target.reportDescriptors[std::stoi(line.substr(0, sp))] = std::move(raw);
            }
            res.descriptorsFrom = cache.filename().string();
        }
    }

    const std::vector<Report> reports = reportsOf(res.target, transfers);
    res.kinds = interfaceKinds(res.target, reports);
    std::vector<const Report*> tf, hp, input;
    for (const Report& r : reports) {
        switch (classify(res.target, r, res.kinds)) {
            case Kind::TrueForce: tf.push_back(&r); break;
            case Kind::Hidpp: hp.push_back(&r); break;
            default: input.push_back(&r); break;
        }
    }
    res.trueForce = analyseTrueForce(tf, res.notes.holds, folder / "stream.tsv");
    res.hidpp = analyseHidpp(hp);
    res.input = analyseInput(input);
    return res;
}

}  // namespace wheelbri::g923cap
