#include "g923cap/Reports.hpp"

#include "g923cap/Protocol.hpp"
#include "wheelbri/TrueForce.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace wheelbri::g923cap {

namespace {

std::string hex2(const int v) {
    char b[8];
    std::snprintf(b, sizeof(b), "0x%02X", v & 0xFF);
    return b;
}

std::string hex4(const int v) {
    char b[8];
    std::snprintf(b, sizeof(b), "0x%04X", v & 0xFFFF);
    return b;
}

std::string sessionKind(const std::string& name) {
    static const std::regex kKind(R"(^([Ss]\d))");
    std::smatch m;
    if (!std::regex_search(name, m, kKind)) return "";
    std::string k = m[1];
    k[0] = 'S';
    return k;
}

// {'a': 1, 'b': 2} as the summaries show counts.
template <class Map>
std::string countsText(const Map& m, const std::function<std::string(const typename Map::key_type&)>& key) {
    if (m.empty()) return "-";
    std::string s = "{";
    bool first = true;
    for (const auto& [k, v] : m) {
        s += (first ? "" : ", ") + key(k) + ": " + std::to_string(v);
        first = false;
    }
    return s + "}";
}

std::string pairsText(const std::vector<std::pair<std::string, std::size_t>>& v) {
    std::string s = "{";
    for (std::size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + v[i].first + ": " + std::to_string(v[i].second);
    return s + "}";
}

std::string hexKey(const int& k) { return hex2(k); }
std::string intKey(const int& k) { return std::to_string(k); }
std::string strKey(const std::string& k) { return k; }

std::optional<int> indexOfFeature(const std::map<int, std::uint16_t>& map, const std::uint16_t feature) {
    for (const auto& [i, f] : map) {
        if (f == feature) return i;
    }
    return std::nullopt;
}

bool differs(const std::uint32_t a, const std::uint32_t b) {
    if (a == b) return false;
    const double x = static_cast<double>(std::bit_cast<float>(a));
    const double y = static_cast<double>(std::bit_cast<float>(b));
    if (std::isnan(x) || std::isnan(y)) return true;
    return std::abs(x - y) > 1e-4 * std::max(1.0, std::abs(x));
}

std::string valueText(const std::uint32_t bits) { return numberText(static_cast<double>(std::bit_cast<float>(bits))); }

std::vector<Check> rehearsalChecks(const SessionResult& r, const std::string& kind) {
    std::vector<Check> out;
    out.emplace_back(true, "REHEARSAL with " + r.target.ident() + " (TrueForce and HID++ checks left out)");
    out.emplace_back(r.target.descriptor.has_value(), "device descriptor seen");
    out.emplace_back(!r.target.interfaces.empty(),
                     "configuration seen (" + std::to_string(r.target.interfaces.size()) + " interfaces)");
    if (kind == "S1") {
        std::string missing;
        bool hid = false;
        for (const Interface& i : r.target.interfaces) {
            if (i.cls != 0x03) continue;
            hid = true;
            if (r.target.reportDescriptors.count(i.number) == 0) missing += (missing.empty() ? "" : ", ") + std::to_string(i.number);
        }
        out.emplace_back(hid && missing.empty(), "HID report descriptors seen for every HID interface" +
                                                     (missing.empty() ? "" : " (missing: interface " + missing + ")"));
    }
    out.emplace_back(r.input.in > 0, "input reports from the wheel: " + std::to_string(r.input.in));
    out.emplace_back(true, "reports to the wheel (force feedback): " + std::to_string(r.input.out));
    std::size_t holdNotes = 0;
    for (const Note& n : r.notes.lines) holdNotes += (n.kind == "HOLD_START" || n.kind == "HOLD_END") ? 1 : 0;
    if (kind == "S2") out.emplace_back(holdNotes >= 8, "holds noted: " + std::to_string(holdNotes / 2));
    if (kind == "S2" || kind == "S3" || kind == "S4") {
        out.emplace_back(true, "shim logs copied: " + std::to_string(r.shim.files.size()));
    }
    return out;
}

}  // namespace

std::vector<Check> checksOf(const SessionResult& r) {
    if (!r.error.empty()) return {{false, r.error}};
    const std::string kind = sessionKind(r.session);
    if (rehearsalTarget()) return rehearsalChecks(r, kind);
    std::vector<Check> out;
    const TrueForceResult& tf = r.trueForce;
    const HidppResult& hp = r.hidpp;
    const auto answered = [&tf](const int type) -> Check {
        const auto it = tf.pairs.find(type);
        if (it == tf.pairs.end()) return {false, "no " + hex2(type) + " request"};
        return {it->second.answered == it->second.sent, hex2(type) + ": " + std::to_string(it->second.answered) +
                                                            " of " + std::to_string(it->second.sent) + " answered"};
    };
    out.emplace_back(r.target.ident().rfind("046D:C26E", 0) == 0, "the wheel: " + r.target.ident());
    if (kind == "S1") {
        out.emplace_back(r.target.descriptor.has_value(), "device descriptor seen");
        out.emplace_back(!r.target.interfaces.empty(),
                         "configuration seen (" + std::to_string(r.target.interfaces.size()) + " interfaces)");
        std::string missing;
        bool hid = false;
        for (const Interface& i : r.target.interfaces) {
            if (i.cls != 0x03) continue;
            hid = true;
            if (r.target.reportDescriptors.count(i.number) == 0) missing += (missing.empty() ? "" : ", ") + std::to_string(i.number);
        }
        out.emplace_back(hid && missing.empty(), "HID report descriptors seen for every HID interface" +
                                                     (missing.empty() ? "" : " (missing: interface " + missing + ")"));
        out.emplace_back(hp.requests > 0 && hp.answered == hp.requests,
                         "HID++: " + std::to_string(hp.answered) + " of " + std::to_string(hp.requests) +
                             " requests answered");
        const auto ffb = indexOfFeature(hp.featureMap, 0x8123);
        const bool seen = ffb && std::any_of(hp.rows.begin(), hp.rows.end(),
                                             [&ffb](const HidppRow& row) { return row.featureIndex == *ffb; });
        out.emplace_back(seen, seen ? "ForceFeedback (0x8123) requests seen (rotation, gain)"
                                    : "no ForceFeedback (0x8123) request found");
    } else if (kind == "S2" || kind == "S4") {
        const Check params = answered(0x05);
        out.emplace_back(params.first && !tf.parameters.empty(),
                         "parameters: " + std::to_string(tf.parameters.size()) + " distinct; " + params.second);
        out.push_back(answered(0x03));
        // Measured with a real G923 (2026-11-01): the SDK sends no 0x06 or 0x07 to it, and the wheel does not answer
        // 0x0E (operating range). Reported, not judged.
        for (const int type : {0x06, 0x07, 0x0E, 0x0C}) {
            const auto it = tf.pairs.find(type);
            out.emplace_back(true, hex2(type) + ": " +
                                       (it == tf.pairs.end() ? std::string("not sent")
                                                             : "sent " + std::to_string(it->second.sent) +
                                                                   ", answered " + std::to_string(it->second.answered)));
        }
        std::string statuses;
        for (const auto& [type, s] : tf.status) statuses += (statuses.empty() ? "" : ", ") + hex2(type) + " x" + std::to_string(s.count);
        out.emplace_back(!tf.status.empty(), "status reports: " + (statuses.empty() ? std::string("none") : statuses));
        if (kind == "S2") {
            std::vector<const TfHold*> holds;
            for (const TfHold& h : tf.holds) {
                if (h.position) holds.push_back(&h);
            }
            if (holds.size() >= 3) {
                std::sort(holds.begin(), holds.end(), [](const TfHold* a, const TfHold* b) { return a->degrees < b->degrees; });
                bool up = true, down = true;
                for (std::size_t i = 1; i < holds.size(); ++i) {
                    up = up && *holds[i - 1]->position < *holds[i]->position;
                    down = down && *holds[i - 1]->position > *holds[i]->position;
                }
                std::string text = "held angles: positions";
                for (std::size_t i = 0; i < holds.size(); ++i) {
                    text += (i ? ", " : " ") + holds[i]->label + " " + numberText(std::round(*holds[i]->position));
                }
                out.emplace_back(up || down, text + ((up || down) ? " (in order)" : " (NOT in order: check the holds)"));
            } else {
                out.emplace_back(false, "held angles: only " + std::to_string(holds.size()) +
                                            " with status reports (need centre, left, right)");
            }
        }
        out.emplace_back(tf.stream.has_value(), "stream packets: " + std::to_string(tf.stream ? tf.stream->count : 0));
        if (kind == "S4") {
            for (const auto& [type, p] : tf.pairs) {
                if (type == 0x05 || type == 0x07 || type == 0x06 || type == 0x0E || type == 0x03 || type == 0x04) continue;
                out.emplace_back(true, hex2(type) + ": sent " + std::to_string(p.sent) + ", answered " +
                                           std::to_string(p.answered) + " (answer types " +
                                           countsText(p.answerTypes, std::function<std::string(const int&)>(hexKey)) + ")");
            }
        }
    } else if (kind == "S3") {
        const auto it = tf.pairs.find(0x05);
        out.emplace_back(true, "with G HUB closed: " + std::to_string(it == tf.pairs.end() ? 0 : it->second.sent) +
                                   " parameter requests, " + std::to_string(tf.stream ? tf.stream->count : 0) +
                                   " stream packets (information, not pass/fail)");
    } else if (kind == "S5") {
        const auto ffb = indexOfFeature(hp.featureMap, 0x8123);
        int total = 0;
        for (const HidppRow& row : hp.rows) {
            if (!ffb || row.featureIndex == *ffb) total += row.count;
        }
        out.emplace_back(total > 0, "HID++ effects traffic: " + std::to_string(total) + " requests" +
                                        (ffb ? "" : " (feature map unknown here: all HID++ counted)"));
    }
    if (kind == "S2" || kind == "S3" || kind == "S4") {
        std::string calls;
        for (const auto& [name, n] : r.shim.calls) calls += (calls.empty() ? "" : ", ") + name + " x" + std::to_string(n);
        out.emplace_back(!r.shim.calls.empty(),
                         "shim log: " + (calls.empty() ? "no logi* calls (shim missing, or logi_log off?)" : calls));
    }
    return out;
}

std::string summaryMarkdown(const SessionResult& r, const std::map<int, std::uint16_t>& featureNames) {
    std::ostringstream o;
    o << "# " << r.session << "\n\n";
    for (const auto& [ok, text] : checksOf(r)) o << "- " << (ok ? "PASS" : "**FAIL**") << ": " << text << "\n";
    if (!r.error.empty()) return o.str();
    const Device& t = r.target;
    o << "\nCapture `" << r.capture << "`, " << numberText(r.spanS) << " s. Wheel: " << t.ident() << " (picked by "
      << r.pickedBy << ").\n\n";
    if (t.descriptor) {
        char b[64];
        std::snprintf(b, sizeof(b), "(bcdDevice %04X, USB %04X)", t.descriptor->bcdDevice, t.descriptor->bcdUsb);
        o << "Device descriptor (frame " << t.descriptorFrame << "): `" << hexOf(t.descriptor->raw, 64) << "` " << b << "\n\n";
    }
    if (!t.interfaces.empty()) {
        o << "## Interfaces\n\n";
        if (!r.descriptorsFrom.empty()) o << "Report descriptors from `" << r.descriptorsFrom << "` (S1's plug-in).\n\n";
        o << "| # | Class | Endpoints | Report descriptor | Collections | Kind |\n|---|---|---|---|---|---|\n";
        for (const Interface& i : t.interfaces) {
            char cls[16];
            std::snprintf(cls, sizeof(cls), "%02X/%02X/%02X", i.cls, i.subclass, i.protocol);
            std::string eps;
            for (const Endpoint& e : i.endpoints) {
                char b[64];
                std::snprintf(b, sizeof(b), "0x%02X attr %02X max %u every %u", e.address, e.attributes, e.maxPacket, e.interval);
                eps += (eps.empty() ? "" : "<br>") + std::string(b);
            }
            std::string cols;
            for (const Collection& c : t.collections(i.number)) {
                std::string reports;
                for (const auto& [rid, bits] : c.reports) {
                    reports += (reports.empty() ? "" : ", ") + hex2(rid) + " in/out/feat " +
                               std::to_string(c.reportBytes(rid, 0)) + "/" + std::to_string(c.reportBytes(rid, 1)) +
                               "/" + std::to_string(c.reportBytes(rid, 2)) + " B";
                }
                cols += (cols.empty() ? "" : "; ") + pageName(c.usagePage) + " " + hex4(c.usage) + " reports " + reports;
            }
            const std::string len = i.reportDescriptorLength ? std::to_string(*i.reportDescriptorLength) : "None";
            const auto frame = t.reportDescriptorFrames.find(i.number);
            const std::string rd = t.reportDescriptors.count(i.number) != 0
                                       ? len + " B, frame " + (frame == t.reportDescriptorFrames.end() ? std::string("None") : std::to_string(frame->second))
                                       : len + " B, not in this capture";
            o << "| " << int(i.number) << " | " << cls << " | " << eps << " | " << rd << " | " << (cols.empty() ? "-" : cols)
              << " | " << kindName(t.kindOfInterface(i.number)) << " |\n";
        }
        o << "\n";
        for (const Interface& i : t.interfaces) {
            if (!i.hidRaw.empty()) o << "- Interface " << int(i.number) << " HID descriptor: `" << hexOf(i.hidRaw, 64) << "`\n";
        }
        o << "\n";
    }

    const TrueForceResult& tf = r.trueForce;
    const std::function<std::string(const int&)> hk(hexKey);
    if (!tf.requests.empty() || !tf.answers.empty()) {
        o << "## TrueForce\n\nRequests by type: " << countsText(tf.requests, hk) << ". Answers by type: "
          << countsText(tf.answers, hk) << ".\n\n"
          << "| Request | Sent | Answered | Answer types | Delay (median) | First request | First answer |\n"
          << "|---|---|---|---|---|---|---|\n";
        for (const auto& [type, p] : tf.pairs) {
            std::string first = "-";
            if (p.firstAnswer) {
                first = "frame " + std::to_string(p.firstAnswer->frame) + ": `" + p.firstAnswer->hex + "` (seal at " +
                        (p.sealAt ? std::to_string(*p.sealAt) : std::string("None")) + ", " +
                        (p.sealed ? "ok" : "NOT sealed") + ")";
            }
            o << "| " << hex2(type) << " " << typeName(static_cast<std::uint8_t>(type)) << " | " << p.sent << " | "
              << p.answered << " | " << countsText(p.answerTypes, hk) << " | "
              << (p.medianDelayMs ? numberText(*p.medianDelayMs) : std::string("None")) << " ms | frame "
              << p.firstRequest->frame << ": `" << p.firstRequest->hex << "` | " << first << " |\n";
        }
        o << "\n";
        if (!tf.parameters.empty()) {
            o << "### Parameters (0x05)\n\nAsked for, in order:";
            for (std::size_t i = 0; i < std::min<std::size_t>(64, tf.parameterRequests.size()); ++i) {
                char b[4];
                std::snprintf(b, sizeof(b), "%02X", tf.parameterRequests[i]);
                o << " " << b;
            }
            if (tf.parameterRequests.size() > 64) o << " \xE2\x80\xA6";
            o << "\n\n| Index | Real G923 | WheelBri | Frame | Raw (idx 09 idx f32 seal) |\n|---|---|---|---|---|\n";
            std::set<int> indexes;
            for (const auto& [i, p] : tf.parameters) indexes.insert(i);
            for (int i = 0; i < 0x40; ++i) indexes.insert(i);
            for (const int i : indexes) {
                const std::uint32_t ours = TrueForceDevice::parameterBits(static_cast<std::uint8_t>(i));
                const auto it = tf.parameters.find(i);
                if (it == tf.parameters.end()) {
                    o << "| " << hex2(i) << " | not asked | " << valueText(ours) << " | - | `` |\n";
                    continue;
                }
                const TfParameter& p = it->second;
                std::string real = valueText(p.bits);
                if (p.bitsSeen.size() > 1) {
                    real += " (also";
                    for (std::size_t k = 1; k < p.bitsSeen.size(); ++k) real += (k > 1 ? ", " : " ") + valueText(p.bitsSeen[k]);
                    real += ")";
                }
                const std::string mark = differs(p.bits, ours) ? "**" : "";
                o << "| " << hex2(i) << " | " << mark << real << mark << " | " << valueText(ours) << " | " << p.frame
                  << " | `" << p.raw << "`" << (p.wellFormed ? "" : " (odd form)") << " |\n";
            }
            o << "\n";
        }
        if (!tf.slots.empty()) {
            const TfSlots& s = tf.slots.front();
            o << "### Slots (0x07), frame " << s.frame << "\n\nCount " << s.count << ":";
            for (const Bytes& slot : s.slots) o << " [" << hexOf(slot, 3) << "]";
            o << "; sealed: " << (s.sealed ? "True" : "False") << ". Raw `" << s.hex << "`\n\n";
        }
        if (!tf.status.empty()) {
            o << "### Status reports\n\n| Type | Count | Rate/s | Position min..max | [9..12] | [17..20] first | [21] | "
                 "Count range [22..24] | Full scale [25..28] / [29..32] Nm | Sealed | Clock step (us) | First |\n"
                 "|---|---|---|---|---|---|---|---|---|---|---|---|\n";
            for (const auto& [type, s] : tf.status) {
                std::string at17;
                for (const auto& e : s.at17Examples) at17 += (at17.empty() ? "" : ", ") + e;
                o << "| " << hex2(type) << " | " << s.count << " | " << (s.ratePerS ? numberText(*s.ratePerS) : "None")
                  << " | " << s.positionMin << ".." << s.positionMax << " | " << pairsText(s.at9) << " | " << at17
                  << " | " << pairsText(s.at21) << " | " << pairsText(s.countRange) << " | " << pairsText(s.torque1)
                  << " / " << pairsText(s.torque2) << " (`" << s.torqueRaw << "`) | " << s.sealed << " | "
                  << (s.clockStepUs ? numberText(*s.clockStepUs) : "None") << " | frame " << s.first.frame << ": `"
                  << s.first.hex << "` |\n";
            }
            o << "\n";
        }
        if (!tf.holds.empty()) {
            o << "### Held angles\n\nCentre position: " << (tf.centre ? numberText(*tf.centre) : "None")
              << "\n\n| Hold | Degrees (+ right) | Samples | Median position | Counts/degree | Counts/turn |\n"
              << "|---|---|---|---|---|---|\n";
            for (const TfHold& h : tf.holds) {
                o << "| " << h.label << " | " << numberText(h.degrees) << " | " << h.samples << " | "
                  << (h.position ? numberText(*h.position) : "None") << " | "
                  << (h.perDegree ? numberText(*h.perDegree) : "-") << " | " << (h.perTurn ? numberText(*h.perTurn) : "-")
                  << " |\n";
            }
            o << "\nWheelBri's virtual G923 counts 268101.8 a turn (744.7 a degree), a right turn counting up.\n\n";
        }
        if (tf.stream) {
            const TfStream& s = *tf.stream;
            o << "### Stream (0x01)\n\n" << s.count << " packets, " << numberText(s.ratePerS) << "/s, first at frame "
              << s.firstFrame << " (" << countsText(s.via, std::function<std::string(const std::string&)>(strKey))
              << "). cur range "
              << (s.curRange ? "[" + numberText(s.curRange->first) + ", " + numberText(s.curRange->second) + "]" : "None")
              << ", 'none' (0x8000) x" << s.curNone << ". Bytes 10/11: " << pairsText(s.bytes10and11)
              << ". Bytes 8..9: " << pairsText(s.bytes8and9) << ". Texture peak (raw from 0x8000): "
              << (s.texturePeak ? std::to_string(*s.texturePeak) : "None") << ". Every packet: `stream.tsv`.\n\n";
        }
        if (!tf.unanswered.empty()) {
            o << "Unanswered requests (first ones):";
            for (std::size_t i = 0; i < std::min<std::size_t>(10, tf.unanswered.size()); ++i) {
                o << (i ? "; " : " ") << "frame " << tf.unanswered[i].frame << " `" << tf.unanswered[i].hex << "`";
            }
            o << "\n\n";
        }
        if (!tf.examples.empty()) {
            o << "### Request examples\n\n";
            for (const auto& [type, ex] : tf.examples) {
                o << "- " << hex2(type) << ":";
                for (std::size_t i = 0; i < ex.size(); ++i) {
                    o << (i ? "; " : " ") << "frame " << ex[i].frame << " (" << ex[i].via << ") `" << ex[i].hex << "`";
                }
                o << "\n";
            }
            o << "\n";
        }
    }

    const HidppResult& hp = r.hidpp;
    if (hp.requests > 0 || hp.unsolicited > 0) {
        std::map<int, std::uint16_t> fmap = hp.featureMap;
        for (const auto& [i, f] : featureNames) fmap.emplace(i, f);
        o << "## HID++\n\n" << hp.requests << " requests, " << hp.answered << " answered ("
          << countsText(hp.via, std::function<std::string(const std::string&)>(strKey)) << "); " << hp.unsolicited
          << " reports from the wheel nobody asked for.\n\n";
        if (!hp.name.empty()) o << "Name: " << hp.name << "\n";
        for (const Firmware& fw : hp.firmware) {
            o << "Firmware entity " << fw.entity << ": type " << fw.type << " " << fw.prefix << " " << fw.number << "."
              << fw.revision << " build " << fw.build << " (`" << fw.raw << "`)\n";
        }
        o << "\nFeature map (index: id):";
        bool first = true;
        for (const auto& [i, f] : fmap) {
            o << (first ? " " : ", ") << i << ": " << featureName(f);
            first = false;
        }
        o << "\n\n| Index | Feature | Function | Count | Answered | Errors | sw ids | Reports | Examples (params -> answer) |\n"
          << "|---|---|---|---|---|---|---|---|---|\n";
        const std::function<std::string(const int&)> ik(intKey);
        for (const HidppRow& row : hp.rows) {
            const auto f = fmap.find(row.featureIndex);
            std::string ex;
            for (const HidppExample& e : row.examples) {
                ex += (ex.empty() ? "" : "<br>") + std::string("f") + std::to_string(e.frame) + ": `" +
                      (e.params.empty() ? "-" : e.params) + "` -> `" + (e.answer ? *e.answer : "None") + "`";
            }
            o << "| " << int(row.featureIndex) << " | "
              << featureName(f == fmap.end() ? std::nullopt : std::optional<std::uint16_t>(f->second)) << " | "
              << int(row.function) << " | " << row.count << " | " << row.answered << " | "
              << countsText(row.errors, std::function<std::string(const std::string&)>(strKey)) << " | "
              << countsText(row.softwareIds, ik) << " | " << countsText(row.reportIds, hk) << " | " << ex << " |\n";
        }
        if (!hp.unsolicitedExamples.empty()) {
            o << "\nUnsolicited:";
            for (std::size_t i = 0; i < hp.unsolicitedExamples.size(); ++i) {
                o << (i ? "; " : " ") << "frame " << hp.unsolicitedExamples[i].frame << " `" << hp.unsolicitedExamples[i].hex << "`";
            }
            o << "\n";
        }
        o << "\n";
    }
    const InputResult& in = r.input;
    o << "## Other reports\n\nIn: " << pairsText(in.inShapes) << ". Out: "
      << (in.outShapes.empty() ? std::string("none") : pairsText(in.outShapes)) << ".\n\n";
    if (!in.outExamples.empty()) {
        o << "Out examples:";
        for (std::size_t i = 0; i < std::min<std::size_t>(6, in.outExamples.size()); ++i) {
            o << (i ? "; " : " ") << "frame " << in.outExamples[i].frame << " `" << in.outExamples[i].hex << "`";
        }
        o << "\n\n";
    }
    if (!r.shim.files.empty()) {
        o << "## Shim log\n\nFiles:";
        for (std::size_t i = 0; i < r.shim.files.size(); ++i) o << (i ? ", " : " ") << r.shim.files[i];
        o << "\n\n";
        for (const auto& [name, n] : r.shim.calls) {
            const auto f = r.shim.first.find(name);
            o << "- " << name << " x" << n << " (first " << (f == r.shim.first.end() ? "?" : f->second) << ")\n";
        }
        o << "\n";
    }
    if (!r.notes.lines.empty()) {
        o << "## Notes\n\n";
        for (const Note& n : r.notes.lines) o << "- " << n.local << " " << n.kind << " " << n.text << "\n";
        o << "\n";
    }
    return o.str();
}

std::string dayReport(const std::vector<SessionResult>& results, const std::map<int, std::uint16_t>& featureMap) {
    std::ostringstream o;
    o << "# G923 capture report\n\n";
    for (const SessionResult& r : results) {
        const auto cs = checksOf(r);
        const auto bad = std::count_if(cs.begin(), cs.end(), [](const Check& c) { return !c.first; });
        o << "- **" << r.session << "**: "
          << (bad == 0 ? std::string("all checks pass") : std::to_string(bad) + " check(s) failed") << " (see its summary.md)\n";
    }
    o << "\n";
    std::map<int, std::vector<std::pair<std::string, std::uint32_t>>> params;
    for (const SessionResult& r : results) {
        for (const auto& [i, p] : r.trueForce.parameters) params[i].emplace_back(r.session, p.bits);
    }
    if (!params.empty()) {
        o << "## Parameters (all sessions)\n\n| Index | Real G923 | Sessions | WheelBri now |\n|---|---|---|---|\n";
        for (int i = 0; i < 0x40; ++i) params.try_emplace(i);
        for (const auto& [i, seen] : params) {
            const std::uint32_t ours = TrueForceDevice::parameterBits(static_cast<std::uint8_t>(i));
            std::vector<std::uint32_t> values;
            std::string sessions;
            for (const auto& [s, bits] : seen) {
                if (std::find(values.begin(), values.end(), bits) == values.end()) values.push_back(bits);
                sessions += (sessions.empty() ? "" : ", ") + s;
            }
            std::string real = values.empty() ? "not asked" : "";
            bool diff = values.size() > 1;
            for (std::size_t k = 0; k < values.size(); ++k) {
                real += (k ? " / " : "") + valueText(values[k]);
                diff = diff || differs(values[k], ours);
            }
            const std::string mark = diff ? "**" : "";
            o << "| " << hex2(i) << " | " << mark << real << mark << " | " << (sessions.empty() ? "-" : sessions) << " | "
              << valueText(ours) << " |\n";
        }
        o << "\nBold: differs from WheelBri, or differs between sessions.\n\n";
    }
    if (!featureMap.empty()) {
        o << "## HID++ features\n\n";
        bool first = true;
        for (const auto& [i, f] : featureMap) {
            o << (first ? "" : ", ") << i << ": " << featureName(f);
            first = false;
        }
        o << "\n\n";
    }
    for (const SessionResult& r : results) {
        o << "## " << r.session << "\n\n";
        for (const auto& [ok, text] : checksOf(r)) o << "- " << (ok ? "PASS" : "**FAIL**") << ": " << text << "\n";
        o << "\n";
    }
    return o.str();
}

std::vector<SessionResult> decode(const std::filesystem::path& path, const std::optional<int> address) {
    std::vector<std::filesystem::path> sessions;
    bool single = false;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(path, ec)) {
        const std::string ext = e.path().extension().string();
        if (e.is_regular_file() && (ext == ".pcap" || ext == ".pcapng")) single = true;
    }
    if (single) {
        sessions.push_back(path);
    } else {
        static const std::regex kSession(R"(^[Ss][1-5])");
        for (const auto& e : std::filesystem::directory_iterator(path, ec)) {
            if (e.is_directory() && std::regex_search(e.path().filename().string(), kSession)) sessions.push_back(e.path());
        }
        std::sort(sessions.begin(), sessions.end());
    }
    std::vector<SessionResult> results;
    for (const auto& folder : sessions) results.push_back(analyseSession(folder, address));
    // The feature map from any session (S1 has G HUB's discovery) names the HID++ features in the others.
    std::map<int, std::uint16_t> merged;
    for (const SessionResult& r : results) {
        for (const auto& [i, f] : r.hidpp.featureMap) merged.emplace(i, f);
    }
    for (const SessionResult& r : results) {
        std::ofstream(r.folder / "summary.md", std::ios::binary | std::ios::trunc) << summaryMarkdown(r, merged);
    }
    if (!single) std::ofstream(path / "report.md", std::ios::binary | std::ios::trunc) << dayReport(results, merged);
    return results;
}

std::vector<Located> locate(const std::vector<std::filesystem::path>& captures) {
    std::vector<Located> found;
    for (const auto& p : captures) {
        const auto devs = findDevices(transfersOf(urbsOf(readCapture(p))));
        for (const auto& [key, d] : devs) {
            if (d.descriptor && isTarget(d.descriptor->vendor, d.descriptor->product)) {
                found.push_back({p, d.bus, d.address, d.descriptor->product});
            }
        }
    }
    return found;
}

std::vector<std::string> keepWheelOnly(const std::filesystem::path& folder) {
    std::vector<std::string> said;
    std::vector<std::filesystem::path> raws;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(folder, ec)) {
        const std::string name = e.path().filename().string();
        if (e.is_regular_file() && name.rfind("raw-", 0) == 0 && name.find(".pcap") != std::string::npos) {
            raws.push_back(e.path());
        }
    }
    std::sort(raws.begin(), raws.end());
    if (raws.empty()) return {"no raw-*.pcap here"};
    struct Wheel {
        std::set<DeviceKey> keys;
        std::string why;
    };
    std::map<std::filesystem::path, Wheel> wheels;
    for (const auto& raw : raws) {
        const CaptureFile file = readCapture(raw);
        if (!file.error.empty()) {
            said.push_back("could not read " + raw.filename().string() + ": " + file.error);
            continue;
        }
        const auto devs = findDevices(transfersOf(urbsOf(file)));
        const Pick pick = pickTarget(devs);
        if (pick.device == nullptr) continue;
        Wheel w{{{pick.device->bus, pick.device->address}}, pick.why};
        for (const auto& [key, d] : devs) {
            if (!d.descriptor) continue;
            if (isTarget(d.descriptor->vendor, d.descriptor->product)) w.keys.insert(key);
            // The G923 in Xbox mode (before G HUB switches it): a Logitech device speaking Xbox's GIP (an interface
            // of class FF, subclass 47, protocol D0), whatever its product ID.
            if (!rehearsalTarget() && d.descriptor->vendor == kLogitech &&
                std::any_of(d.interfaces.begin(), d.interfaces.end(), [](const Interface& i) {
                    return i.cls == 0xFF && i.subclass == 0x47 && i.protocol == 0xD0;
                })) {
                w.keys.insert(key);
            }
        }
        wheels[raw] = std::move(w);
    }
    if (wheels.empty()) {
        said.push_back("no G923 found in any raw capture: all kept as they are (look in Wireshark; they may hold other "
                       "devices' input, so delete them when done)");
        return said;
    }
    int n = 0;
    for (const auto& raw : raws) {
        const auto it = wheels.find(raw);
        if (it == wheels.end()) {
            std::filesystem::remove(raw, ec);
            said.push_back((ec ? "could not delete " : "deleted ") + raw.filename().string() + ": no G923 in it");
            continue;
        }
        const CaptureFile file = readCapture(raw);
        std::vector<std::pair<double, Bytes>> frames;
        for (const FrameRef& f : file.frames) {
            if (f.linkType != kLinkTypeUsbPcap) continue;
            const auto u = parseUrb(f.ts, file.frame(f));
            if (u && it->second.keys.count({u->bus, u->device}) != 0) {
                const auto bytes = file.frame(f);
                frames.emplace_back(f.ts, Bytes(bytes.begin(), bytes.end()));
            }
        }
        const std::string name = n == 0 ? "capture.pcap" : "capture-" + std::to_string(n + 1) + ".pcap";
        ++n;
        if (!writePcap(folder / name, frames)) {
            said.push_back("could not write " + name + ": " + raw.filename().string() + " kept");
            continue;
        }
        std::filesystem::remove(raw, ec);
        std::string where;
        for (const auto& [bus, address] : it->second.keys) {
            where += (where.empty() ? "" : ", ") + std::string("bus ") + std::to_string(bus) + " address " + std::to_string(address);
        }
        said.push_back("kept " + std::to_string(frames.size()) + " frames of the wheel (" + where + "; found by " +
                       it->second.why + ") from " + raw.filename().string() + " in " + name +
                       (ec ? " (the raw file could not be deleted)" : ""));
    }
    return said;
}

}  // namespace wheelbri::g923cap
