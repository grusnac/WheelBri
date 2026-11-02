// The real-G923 capture decoder (tools/g923-capture) on the captures of a fake wheel that answers as a real G923
// did (2026-11-01): pcap reading, descriptors, TrueForce and HID++ decoding, the checks, keep, locate and the probe.

#include "g923cap/Analysis.hpp"
#include "g923cap/Pcap.hpp"
#include "g923cap/Probe.hpp"
#include "g923cap/Protocol.hpp"
#include "g923cap/Reports.hpp"
#include "g923cap/UsbDescriptors.hpp"
#include "wheelbri/TrueForce.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace wheelbri::g923cap {
namespace {

namespace fs = std::filesystem;

// --- A fake G923 -------------------------------------------------------------------------------------------------------

constexpr std::uint32_t kCountRange = 673643;
constexpr double kCountsPerTurn = 268101.8125;
constexpr std::uint32_t kFullScale = 0x828A0000U;

const Bytes kDevice = {18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x6D, 0x04, 0x6E, 0xC2, 0x02, 0x39, 1, 2, 3, 1};
// Game input (Generic Desktop joystick, report 1), HID++ (FF43: 0x11 long, 0x12 very long), TrueForce (FFFD:FD01,
// report 1, 63 bytes each way).
const Bytes kRdGame = {0x05, 0x01, 0x09, 0x04, 0xA1, 0x01, 0x85, 0x01, 0x75, 0x08, 0x95, 0x0B, 0x81, 0x02, 0xC0};
const Bytes kRdHidpp = {0x06, 0x43, 0xFF, 0x0A, 0x02, 0x06, 0xA1, 0x01, 0x85, 0x11, 0x75, 0x08, 0x95, 0x13, 0x15, 0x00,
                        0x26, 0xFF, 0x00, 0x09, 0x02, 0x81, 0x00, 0x09, 0x02, 0x91, 0x00, 0xC0, 0x06, 0x43, 0xFF, 0x0A,
                        0x04, 0x06, 0xA1, 0x01, 0x85, 0x12, 0x75, 0x08, 0x95, 0x3F, 0x09, 0x04, 0x81, 0x00, 0x09, 0x04,
                        0x91, 0x00, 0xC0};
const Bytes kRdTrueForce = {0x06, 0xFD, 0xFF, 0x0A, 0x01, 0xFD, 0xA1, 0x01, 0x85, 0x01, 0x75, 0x08,
                            0x95, 0x3F, 0x09, 0x01, 0x81, 0x02, 0x09, 0x01, 0x91, 0x02, 0xC0};

Bytes interfaceBlock(const std::uint8_t number, const Bytes& rd, const std::uint8_t in, const std::uint8_t out) {
    Bytes d = {9, 4, number, 0, 2, 3, 0, 0, 0, 9, 0x21, 0x11, 0x01, 0, 1, 0x22, static_cast<std::uint8_t>(rd.size()), 0};
    for (const std::uint8_t ep : {in, out}) {
        const Bytes e = {7, 5, ep, 3, 64, 0, 1};
        d.insert(d.end(), e.begin(), e.end());
    }
    return d;
}

Bytes configuration() {
    Bytes body = interfaceBlock(0, kRdGame, 0x81, 0x01);
    for (const Bytes& b : {interfaceBlock(1, kRdHidpp, 0x82, 0x02), interfaceBlock(2, kRdTrueForce, 0x83, 0x03)}) {
        body.insert(body.end(), b.begin(), b.end());
    }
    const auto total = static_cast<std::uint16_t>(9 + body.size());
    Bytes d = {9, 2, static_cast<std::uint8_t>(total & 0xFF), static_cast<std::uint8_t>(total >> 8), 3, 1, 0, 0x80, 250};
    d.insert(d.end(), body.begin(), body.end());
    return d;
}

void seal(Bytes& r, const std::size_t at) {
    unsigned sum = 0;
    for (std::size_t i = 4; i < at; ++i) sum += r[i];
    r[at] = static_cast<std::uint8_t>((0x100U - (sum & 0xFFU)) & 0xFFU);
}

void put(Bytes& r, const std::size_t at, const std::uint32_t v, const int bytes) {
    for (int i = 0; i < bytes; ++i) r[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
}

Bytes blank(const std::uint8_t type, const std::uint8_t seq) {
    Bytes r(64, 0);
    r[0] = 1;
    r[4] = type;
    r[5] = seq;
    return r;
}

Bytes status(const std::uint8_t type, const std::uint8_t seq, const std::uint32_t position, const std::uint32_t clock,
             const std::uint32_t at17 = 0x80000000U) {
    Bytes r = blank(type, seq);
    put(r, 6, position, 3);
    put(r, 9, 0x80008000U, 4);
    put(r, 13, clock, 4);
    put(r, 17, at17, 4);
    put(r, 22, kCountRange, 3);
    put(r, 25, kFullScale, 4);
    put(r, 29, kFullScale, 4);
    seal(r, 33);
    return r;
}

std::uint32_t fakeParameter(const std::uint8_t index) {
    return index == 0x06 ? std::bit_cast<std::uint32_t>(123.0F) : TrueForceDevice::parameterBits(index);
}

std::vector<Bytes> answer(const Bytes& q, const std::uint32_t position, const std::uint32_t clock) {
    const std::uint8_t type = q[4], seq = q[5];
    if (type == 0x05) {
        Bytes r = blank(0x05, seq);
        r[6] = q[6];
        r[7] = 0x09;
        r[8] = q[6];
        put(r, 9, fakeParameter(q[6]), 4);
        seal(r, 13);
        return {r};
    }
    // The operating range too, as a real G923 (2026-11-01, S2 frame 251 -> 261).
    if (type == 0x03 || type == 0x04 || type == 0x01 || type == 0x0E) return {status(0x02, seq, position, clock)};
    return {};   // the rest: nothing, as a real G923
}

Bytes tfRequest(const std::uint8_t type, const std::uint8_t seq, const Bytes& payload = {}) {
    Bytes r = blank(type, seq);
    std::copy(payload.begin(), payload.end(), r.begin() + 6);
    return r;
}

Bytes hidppLong(const std::uint8_t index, const std::uint8_t function, const std::uint8_t sw, const Bytes& params) {
    Bytes d(20, 0);
    d[0] = 0x11;
    d[1] = 0xFF;
    d[2] = index;
    d[3] = static_cast<std::uint8_t>(function << 4 | sw);
    std::copy(params.begin(), params.end(), d.begin() + 4);
    return d;
}

const std::vector<std::uint16_t> kFeatures = {0x0000, 0x0001, 0x0003, 0x0005, 0x8123};
const std::string kName = "G923 Racing Wheel for Xbox One and PC";

Bytes hidppAnswer(const Bytes& q) {
    const std::uint8_t fi = q[2], fn = q[3] >> 4;
    Bytes a(20, 0);
    std::copy(q.begin(), q.begin() + 4, a.begin());
    if (fi == 0 && fn == 0) {
        const std::uint16_t fid = static_cast<std::uint16_t>(q[4] << 8 | q[5]);
        const auto it = std::find(kFeatures.begin(), kFeatures.end(), fid);
        a[4] = it == kFeatures.end() ? 0 : static_cast<std::uint8_t>(it - kFeatures.begin());
    } else if (fi == 0 && fn == 1) {
        a[4] = 4;
        a[5] = 2;
        a[6] = q[6];
    } else if (fi == 1 && fn == 0) {
        a[4] = static_cast<std::uint8_t>(kFeatures.size() - 1);
    } else if (fi == 1 && fn == 1) {
        a[4] = static_cast<std::uint8_t>(kFeatures[q[4]] >> 8);
        a[5] = static_cast<std::uint8_t>(kFeatures[q[4]] & 0xFF);
    } else if (fi == 2 && fn == 0) {
        a[4] = 1;
    } else if (fi == 2 && fn == 1) {
        const Bytes fw = {0, 'U', '1', ' ', 0x39, 0x02, 0x00, 0x50};
        std::copy(fw.begin(), fw.end(), a.begin() + 4);
    } else if (fi == 3 && fn == 0) {
        a[4] = static_cast<std::uint8_t>(kName.size());
    } else if (fi == 3 && fn == 1) {
        for (std::size_t i = 0; i < 16 && q[4] + i < kName.size(); ++i) a[4 + i] = static_cast<std::uint8_t>(kName[q[4] + i]);
    } else if (fi == 3 && fn == 2) {
        a[4] = 0x0A;
    } else if (fi != 4) {
        a = Bytes{0x11, 0xFF, 0xFF, fi, q[3], 6};
        a.resize(20, 0);
    }
    return a;
}

// The frames of a capture, as USBPcap writes them.
struct Capture {
    double t = 1800000000.0;
    std::uint16_t bus = 1;
    std::uint16_t address = 7;
    std::uint64_t irp = 0x1000;
    std::vector<std::pair<double, Bytes>> frames;

    void controlIn(const Bytes& setup, const Bytes& data) {
        irp += 8;
        frames.emplace_back(t, urbBytes(irp, false, bus, address, 0x80, kTransferControl, setup, kStageSetup));
        t += 0.001;
        frames.emplace_back(t, urbBytes(irp, true, bus, address, 0x80, kTransferControl, data, kStageComplete));
    }
    void descriptors(const bool reports, const Bytes& device = kDevice) {
        controlIn({0x80, 6, 0, 1, 0, 0, 18, 0}, device);
        const Bytes cfg = configuration();
        controlIn({0x80, 6, 0, 2, 0, 0, static_cast<std::uint8_t>(cfg.size()), 0}, cfg);
        if (!reports) return;
        const Bytes* rds[] = {&kRdGame, &kRdHidpp, &kRdTrueForce};
        for (std::uint8_t i = 0; i < 3; ++i) {
            controlIn({0x81, 6, 0, 0x22, i, 0, static_cast<std::uint8_t>(rds[i]->size()), 0}, *rds[i]);
        }
    }
    void out(const std::uint8_t ep, const Bytes& data, const double dt = 0.0005) {
        irp += 8;
        frames.emplace_back(t, urbBytes(irp, false, bus, address, ep, kTransferInterrupt, data));
        t += dt;
        frames.emplace_back(t, urbBytes(irp, true, bus, address, ep, kTransferInterrupt, {}));
    }
    void in(const std::uint8_t ep, const Bytes& data, const double dt = 0.0005) {
        irp += 8;
        frames.emplace_back(t, urbBytes(irp, false, bus, address, ep, kTransferInterrupt, {}));
        t += dt;
        frames.emplace_back(t, urbBytes(irp, true, bus, address, ep, kTransferInterrupt, data));
    }
};

std::vector<std::pair<double, Bytes>> merged(std::vector<std::pair<double, Bytes>> a,
                                             const std::vector<std::pair<double, Bytes>>& b) {
    a.insert(a.end(), b.begin(), b.end());
    std::stable_sort(a.begin(), a.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    return a;
}

std::uint32_t positionAt(const double degrees) {
    return static_cast<std::uint32_t>(kCountRange / 2 + std::llround(degrees / 360.0 * kCountsPerTurn));
}

// A session like S2: the SDK's set-up, holds at known angles, a stream.
void s2Like(const fs::path& folder) {
    Capture c;
    c.descriptors(false);   // USBPcap injects device and configuration only
    std::uint32_t position = positionAt(0), clock = 0;
    std::uint8_t seq = 0;
    const auto ask = [&](const std::uint8_t type, const Bytes& payload = {}) {
        const Bytes q = tfRequest(type, ++seq, payload);
        c.out(0x03, q);
        for (const Bytes& a : answer(q, position, clock)) c.in(0x83, a);
        clock += 1000;
    };
    for (std::uint8_t i = 0; i <= 0x3C; ++i) {
        if (i <= 0x1D || i >= 0x2B) ask(0x05, {i});   // the order the SDK asks in, give or take
    }
    ask(0x0E, {0x00, 0xC0, 0x5B, 0x44});
    ask(0x03);
    ask(0x09);
    std::ofstream notes(folder / "notes.txt", std::ios::binary);
    notes << "\xEF\xBB\xBF";
    const auto hold = [&](const std::string& label, const double degrees) {
        position = positionAt(degrees);
        notes << std::llround(c.t * 1000) << "\t2027-01-15T10:00:00\tHOLD_START\t" << label << "|" << degrees << "\n";
        for (int k = 0; k < 300; ++k) {   // 3 s of status reports at 100/s
            c.in(0x83, status(0x02, seq, position, clock));
            clock += 10000;
            c.t += 0.0095;
        }
        notes << std::llround(c.t * 1000) << "\t2027-01-15T10:00:00\tHOLD_END\t" << label << "\n";
    };
    hold("centre", 0);
    hold("left90", -90);
    hold("right90", 90);
    hold("left180", -180);
    for (int k = 0; k < 500; ++k) {   // the stream: cur moves, texture window valid
        Bytes r = tfRequest(0x01, ++seq);
        put(r, 6, static_cast<std::uint32_t>(0x8000 + 8000 * ((k % 50) - 25) / 25), 2);
        r[10] = 4;
        r[11] = 0x0D;
        for (std::size_t slot = 0; slot < 13; ++slot) {
            put(r, 12 + slot * 4, static_cast<std::uint32_t>(0x8000 + slot * 10), 2);
            put(r, 14 + slot * 4, static_cast<std::uint32_t>(0x8000 - slot * 10), 2);
        }
        c.out(0x03, r, 0.001);
    }
    ask(0x04);
    ASSERT_TRUE(writePcap(folder / "capture.pcap", c.frames));
    std::ofstream(folder / "wheelbri-shim-2027-01-15-01.log", std::ios::binary)
        << "[2027-01-15 10:00:01.000] [info] [shim.logi] logiTrueForceSetTorqueKF #1: rcx=0 | rax=1\n"
           "[2027-01-15 10:00:01.001] [info] [shim.logi] logiTrueForceSetTorqueKF #2: rcx=0 | rax=1\n";
}

void s1Like(const fs::path& folder) {
    Capture c;
    c.descriptors(true);
    const auto ask = [&c](const Bytes& q) {
        c.out(0x02, q);
        c.in(0x82, hidppAnswer(q));
    };
    for (const std::uint16_t fid : {0x0001, 0x0003, 0x0005, 0x8123}) {
        ask(hidppLong(0, 0, 1, {static_cast<std::uint8_t>(fid >> 8), static_cast<std::uint8_t>(fid & 0xFF)}));
    }
    ask(hidppLong(1, 0, 1, {}));
    for (std::uint8_t i = 1; i < kFeatures.size(); ++i) ask(hidppLong(1, 1, 1, {i}));
    ask(hidppLong(2, 1, 1, {0}));
    for (const std::uint8_t off : {0, 16, 32}) ask(hidppLong(3, 1, 1, {off}));
    for (const int rot : {900, 540, 900}) ask(hidppLong(4, 6, 1, {static_cast<std::uint8_t>(rot >> 8), static_cast<std::uint8_t>(rot & 0xFF)}));
    ASSERT_TRUE(writePcap(folder / "capture.pcap", c.frames));
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

void setTarget(const char* value) {
#ifdef _WIN32
    _putenv_s("G923_TARGET", value == nullptr ? "" : value);
#else
    if (value == nullptr) unsetenv("G923_TARGET");
    else setenv("G923_TARGET", value, 1);
#endif
}

class G923Cap : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic<int> n{0};
        dir_ = fs::temp_directory_path() / ("wheelbri-g923cap-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                                            "-" + std::to_string(n++));
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        setTarget(nullptr);
    }
    void TearDown() override {
        setTarget(nullptr);
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    fs::path folder(const std::string& name) {
        const fs::path p = dir_ / name;
        fs::create_directories(p);
        return p;
    }
    fs::path dir_;
};

// --- Reading --------------------------------------------------------------------------------------------------------

TEST_F(G923Cap, ReadsItsOwnPcapAndACutShortOne) {
    Capture c;
    c.descriptors(true);
    const Bytes q = tfRequest(0x05, 1, {2});
    c.out(0x03, q);
    c.in(0x83, answer(q, 0, 0)[0]);
    const fs::path p = dir_ / "a.pcap";
    ASSERT_TRUE(writePcap(p, c.frames));
    const auto tr = transfersOf(urbsOf(readCapture(p)));
    EXPECT_EQ(std::count_if(tr.begin(), tr.end(), [](const Transfer& t) { return t.kind == TransferKind::Control; }), 5);
    EXPECT_EQ(std::count_if(tr.begin(), tr.end(), [](const Transfer& t) { return t.kind == TransferKind::Out; }), 1);
    EXPECT_EQ(std::count_if(tr.begin(), tr.end(), [](const Transfer& t) { return t.kind == TransferKind::In; }), 1);
    std::string bytes = readFile(p);
    bytes.resize(bytes.size() - 10);   // cut in the last packet
    const auto cut = transfersOf(urbsOf(parseCapture(Bytes(bytes.begin(), bytes.end()))));
    EXPECT_EQ(cut.size(), tr.size() - 1);
    EXPECT_FALSE(parseCapture(Bytes{1, 2, 3, 4, 5}).error.empty());
}

TEST_F(G923Cap, ReadsPcapng) {
    Capture c;
    c.descriptors(true);
    Bytes file;
    const auto u32 = [&file](const std::uint32_t v) {
        for (int i = 0; i < 4; ++i) file.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    const auto u16 = [&file](const std::uint16_t v) {
        file.push_back(static_cast<std::uint8_t>(v & 0xFF));
        file.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    u32(0x0A0D0D0A); u32(28); u32(0x1A2B3C4D); u16(1); u16(0); u32(0xFFFFFFFF); u32(0xFFFFFFFF); u32(28);
    u32(1); u32(20); u16(kLinkTypeUsbPcap); u16(0); u32(65535); u32(20);
    for (const auto& [ts, frame] : c.frames) {
        const std::size_t pad = (4 - frame.size() % 4) % 4;
        const auto total = static_cast<std::uint32_t>(32 + frame.size() + pad);
        const auto us = static_cast<std::uint64_t>(std::llround(ts * 1e6));
        u32(6); u32(total); u32(0); u32(static_cast<std::uint32_t>(us >> 32)); u32(static_cast<std::uint32_t>(us));
        u32(static_cast<std::uint32_t>(frame.size())); u32(static_cast<std::uint32_t>(frame.size()));
        file.insert(file.end(), frame.begin(), frame.end());
        file.insert(file.end(), pad, 0);
        u32(total);
    }
    const auto tr = transfersOf(urbsOf(parseCapture(file)));
    ASSERT_EQ(tr.size(), 5U);
    EXPECT_NEAR(tr[0].ts, c.frames[1].first, 1e-5);
}

TEST_F(G923Cap, ParsesDescriptors) {
    const auto tf = parseReportDescriptor(kRdTrueForce);
    ASSERT_EQ(tf.size(), 1U);
    EXPECT_EQ(tf[0].usagePage, 0xFFFD);
    EXPECT_EQ(tf[0].usage, 0xFD01);
    EXPECT_EQ(tf[0].reportBytes(1, 0), 64U);
    EXPECT_EQ(tf[0].reportBytes(1, 1), 64U);
    const auto hp = parseReportDescriptor(kRdHidpp);
    ASSERT_EQ(hp.size(), 2U);
    EXPECT_EQ(hp[0].reportBytes(0x11, 0), 20U);
    EXPECT_EQ(hp[1].reportBytes(0x12, 1), 64U);
    const auto ifs = parseConfiguration(configuration());
    ASSERT_EQ(ifs.size(), 3U);
    EXPECT_EQ(ifs[2].endpoints[0].address, 0x83);
    EXPECT_EQ(ifs[2].reportDescriptorLength, kRdTrueForce.size());
}

// --- Decoding -------------------------------------------------------------------------------------------------------

TEST_F(G923Cap, DecodesAnAccSession) {
    const fs::path s2 = folder("S2-acc");
    s2Like(s2);
    const auto results = decode(s2);
    ASSERT_EQ(results.size(), 1U);
    const SessionResult& r = results[0];
    for (const auto& [ok, text] : checksOf(r)) EXPECT_TRUE(ok) << text;
    const TrueForceResult& tf = r.trueForce;
    EXPECT_EQ(tf.parameters.size(), 0x1EU + 0x12U);
    EXPECT_NEAR(tf.parameters.at(0x08).value(), 0.65032, 1e-5);
    EXPECT_NEAR(tf.parameters.at(0x06).value(), 123.0, 1e-9);
    EXPECT_TRUE(std::isnan(tf.parameters.at(0x30).value()));
    for (const auto& [i, p] : tf.parameters) EXPECT_TRUE(p.wellFormed) << i;
    const TfStatus& st = tf.status.at(0x02);
    EXPECT_EQ(st.torque1.at(0).first, "0.65");
    EXPECT_EQ(st.countRange.at(0).first, "673643");
    for (const TfHold& h : tf.holds) {
        if (h.degrees != 0) {
            EXPECT_NEAR(*h.perTurn, kCountsPerTurn, 2.0) << h.label;
        }
    }
    EXPECT_EQ(tf.pairs.at(0x0E).answered, 1);   // the operating range: a plain status, as a real G923
    EXPECT_EQ(tf.pairs.at(0x0E).answerTypes.at(0x02), 1);
    EXPECT_EQ(tf.pairs.at(0x09).answered, 0);
    ASSERT_TRUE(tf.stream);
    EXPECT_EQ(tf.stream->count, 500U);
    EXPECT_TRUE(fs::exists(s2 / "stream.tsv"));
    const std::string summary = readFile(s2 / "summary.md");
    EXPECT_NE(summary.find("| 0x06 | **123** | 0.6 |"), std::string::npos);
    EXPECT_NE(summary.find("| 0x08 | 0.65032 | 0.65032 |"), std::string::npos);
    EXPECT_NE(summary.find("logiTrueForceSetTorqueKF x2"), std::string::npos);
}

TEST_F(G923Cap, HoldsOutOfOrderFail) {
    const fs::path s2 = folder("S2-acc");
    s2Like(s2);
    std::string notes = readFile(s2 / "notes.txt");
    notes.replace(notes.find("right90|90"), 10, "right90|-45");
    std::ofstream(s2 / "notes.txt", std::ios::binary | std::ios::trunc) << notes;
    const auto cs = checksOf(decode(s2)[0]);
    EXPECT_TRUE(std::any_of(cs.begin(), cs.end(), [](const Check& c) { return !c.first && c.second.find("held angles") != std::string::npos; }));
}

TEST_F(G923Cap, DecodesAPlugInWithGHub) {
    const fs::path s1 = folder("S1-plugin");
    s1Like(s1);
    const SessionResult r = decode(s1)[0];
    for (const auto& [ok, text] : checksOf(r)) EXPECT_TRUE(ok) << text;
    EXPECT_EQ(r.hidpp.featureMap.at(4), 0x8123);
    EXPECT_EQ(r.hidpp.name, kName);
    ASSERT_FALSE(r.hidpp.firmware.empty());
    EXPECT_EQ(r.hidpp.firmware[0].prefix, "U1 ");
    EXPECT_EQ(r.target.kindOfInterface(0), Kind::Input);
    EXPECT_EQ(r.target.kindOfInterface(1), Kind::Hidpp);
    EXPECT_EQ(r.target.kindOfInterface(2), Kind::TrueForce);
}

TEST_F(G923Cap, ADayReportCoversEverySessionAndBorrowsTheDescriptors) {
    s2Like(folder("S2-acc"));
    s1Like(folder("S1-plugin"));
    const auto results = decode(dir_);
    ASSERT_EQ(results.size(), 2U);
    const std::string report = readFile(dir_ / "report.md");
    EXPECT_NE(report.find("S1-plugin"), std::string::npos);
    EXPECT_NE(report.find("4: 0x8123 ForceFeedback"), std::string::npos);
    EXPECT_NE(report.find("| 0x06 | **123** |"), std::string::npos);
    EXPECT_TRUE(fs::exists(dir_ / "descriptors-046D-C26E.txt"));
    EXPECT_EQ(results[1].descriptorsFrom, "descriptors-046D-C26E.txt");   // S2, decoded after S1
    EXPECT_EQ(results[1].target.kindOfInterface(2), Kind::TrueForce);
}

TEST_F(G923Cap, NoWheelIsAFailedCheck) {
    const fs::path f = folder("S2-x");
    Capture c;
    c.out(0x01, {0x05, 0x00});
    ASSERT_TRUE(writePcap(f / "c.pcap", c.frames));
    EXPECT_FALSE(checksOf(decode(f)[0]).at(0).first);
}

TEST_F(G923Cap, KindsByMajorityWithoutDescriptors) {
    Capture c;
    c.descriptors(false);
    for (int k = 0; k < 30; ++k) {   // game reports on interface 0, one shaped like HID++ long
        Bytes d(k == 3 ? 20 : 13, 0);
        d[0] = k == 3 ? 0x11 : 0x01;
        c.in(0x81, d);
    }
    const fs::path f = folder("S5-x");
    ASSERT_TRUE(writePcap(f / "capture.pcap", c.frames));
    const SessionResult r = analyseSession(f);
    EXPECT_EQ(r.kinds.at(0), Kind::Input);
    EXPECT_EQ(r.hidpp.requests, 0);
    EXPECT_EQ(r.hidpp.unsolicited, 0);
}

TEST_F(G923Cap, HidppSharingTheGameInterface) {
    Capture c;
    c.descriptors(false);
    for (int k = 0; k < 20; ++k) c.in(0x81, Bytes(13, 0));
    const Bytes q = hidppLong(0, 0, 1, {0x81, 0x23});
    c.out(0x01, q);
    c.in(0x81, hidppAnswer(q));
    const fs::path f = folder("S5-y");
    ASSERT_TRUE(writePcap(f / "capture.pcap", c.frames));
    const SessionResult r = analyseSession(f);
    EXPECT_EQ(r.kinds.at(0), Kind::InputHidpp);
    EXPECT_EQ(r.hidpp.requests, 1);
    EXPECT_EQ(r.hidpp.answered, 1);
}

TEST_F(G923Cap, AMixedInterfaceGoesByReportId) {
    Bytes tf = kRdTrueForce;
    tf[9] = 0x05;   // report 5
    Bytes rd = kRdGame;
    rd.insert(rd.end(), kRdHidpp.begin(), kRdHidpp.end());
    rd.insert(rd.end(), tf.begin(), tf.end());
    Device d;
    d.reportDescriptors[0] = rd;
    EXPECT_EQ(d.kindOfInterface(0), Kind::Mixed);
    EXPECT_EQ(d.kindOfReport(0, 0x01), Kind::Input);
    EXPECT_EQ(d.kindOfReport(0, 0x11), Kind::Hidpp);
    EXPECT_EQ(d.kindOfReport(0, 0x05), Kind::TrueForce);
    Bytes hp(20, 0);
    hp[0] = 0x11;
    hp[1] = 0xFF;
    EXPECT_EQ(classify(d, {0, false, 0, hp, 1, "interrupt"}, {}), Kind::Hidpp);
    EXPECT_EQ(classify(d, {0, false, 0, Bytes(13, 1), 2, "interrupt"}, {}), Kind::Input);
}

// --- Locate, keep, rehearsal ------------------------------------------------------------------------------------------

TEST_F(G923Cap, LocatesTheWheel) {
    const fs::path a = dir_ / "hub1.pcap", b = dir_ / "hub2.pcap";
    ASSERT_TRUE(writePcap(a, {}));
    Capture c;
    c.bus = 2;
    c.address = 9;
    c.descriptors(false);
    ASSERT_TRUE(writePcap(b, c.frames));
    const auto found = locate({a, b});
    ASSERT_EQ(found.size(), 1U);
    EXPECT_EQ(found[0].file, b);
    EXPECT_EQ(found[0].bus, 2);
    EXPECT_EQ(found[0].address, 9);
    EXPECT_EQ(found[0].product, kG923);
}

TEST_F(G923Cap, KeepsOnlyTheWheelsFramesAndEveryAddressItHad) {
    const fs::path f = folder("S3-x");
    Capture first, second, keyboard;
    first.descriptors(false);
    first.in(0x81, Bytes(13, 0));
    second.t = first.t + 1;
    second.address = 9;   // the wheel again, after a reset
    second.descriptors(false);
    keyboard.address = 3;
    keyboard.in(0x81, Bytes(8, 0));
    ASSERT_TRUE(writePcap(f / "raw-USBPcap1.pcap", merged(merged(first.frames, second.frames), keyboard.frames)));
    ASSERT_TRUE(writePcap(f / "raw-USBPcap2.pcap", keyboard.frames));
    const auto said = keepWheelOnly(f);
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(f)) names.push_back(e.path().filename().string());
    EXPECT_EQ(names, std::vector<std::string>{"capture.pcap"});
    std::set<int> devices;
    for (const Transfer& t : transfersOf(urbsOf(readCapture(f / "capture.pcap")))) devices.insert(t.device);
    EXPECT_EQ(devices, (std::set<int>{7, 9}));
    EXPECT_TRUE(std::any_of(said.begin(), said.end(), [](const std::string& s) { return s.find("deleted raw-USBPcap2.pcap") == 0; }));
}

TEST_F(G923Cap, KeepsEverythingWhenNoWheelIsFound) {
    const fs::path f = folder("S1-y");
    Capture keyboard;
    keyboard.address = 3;
    keyboard.in(0x81, Bytes(8, 0));
    ASSERT_TRUE(writePcap(f / "raw-USBPcap1.pcap", keyboard.frames));
    const auto said = keepWheelOnly(f);
    EXPECT_TRUE(fs::exists(f / "raw-USBPcap1.pcap"));
    EXPECT_NE(said.at(0).find("all kept"), std::string::npos);
}

TEST_F(G923Cap, ARehearsalWithAnotherWheel) {
    const fs::path f = folder("S1-plugin-ghub");
    Bytes moza = kDevice;
    moza[8] = 0x6E;
    moza[9] = 0x34;
    moza[10] = 0x05;
    moza[11] = 0x00;
    Capture c;
    c.bus = 2;
    c.address = 4;
    c.descriptors(true, moza);
    c.in(0x81, Bytes(11, 1));
    ASSERT_TRUE(writePcap(f / "raw-USBPcap2.pcap", c.frames));
    EXPECT_NE(keepWheelOnly(f).at(0).find("all kept"), std::string::npos);   // not a G923: nothing deleted
    setTarget("346E");
    EXPECT_EQ(keepWheelOnly(f).at(0).rfind("kept", 0), 0U);
    const auto cs = checksOf(decode(f)[0]);
    EXPECT_NE(cs.at(0).second.find("REHEARSAL"), std::string::npos);
    for (const auto& [ok, text] : cs) EXPECT_TRUE(ok) << text;
}

// --- Probe ----------------------------------------------------------------------------------------------------------

class FakeTransport : public Transport {
public:
    explicit FakeTransport(const bool hidpp, const bool needsSeal = false) : hidpp_(hidpp), needsSeal_(needsSeal) {}
    void write(const std::span<const std::uint8_t> report) override {
        const Bytes q(report.begin(), report.end());
        written.push_back(q);
        if (hidpp_) {
            queue_.push_back(hidppAnswer(q));
            return;
        }
        for (int i = 0; i < 2; ++i) queue_.push_back(status(0x02, 0x33, positionAt(0), 0));   // status noise
        if (needsSeal_ && !sealedAt(q, 7)) return;
        for (const Bytes& a : answer(q, positionAt(0), 0)) queue_.push_back(a);
    }
    std::optional<Bytes> read(double) override {
        if (queue_.empty()) return std::nullopt;
        Bytes d = queue_.front();
        queue_.pop_front();
        return d;
    }
    [[nodiscard]] std::size_t outputLength() const override { return hidpp_ ? 20 : 64; }
    std::vector<Bytes> written;

private:
    bool hidpp_;
    bool needsSeal_;
    std::deque<Bytes> queue_;
};

TEST_F(G923Cap, ProbesEveryParameterReadOnly) {
    FakeTransport t(false);
    const TrueForceProbe p = probeTrueForce(t);
    EXPECT_EQ(p.parameters.size(), 0x40U);
    EXPECT_EQ(p.parameters.at(0x0C).bits, TrueForceDevice::parameterBits(0x0C));
    EXPECT_EQ(p.sealedRequests, false);
    EXPECT_EQ(p.otherReports.at(0x02), 2 * 0x40);
    for (const Bytes& w : t.written) EXPECT_EQ(w[4], 0x05);
}

TEST_F(G923Cap, ProbeTriesAChecksumWhenThereIsNoAnswer) {
    FakeTransport t(false, true);
    const TrueForceProbe p = probeTrueForce(t, 0.3, 0, 3);
    EXPECT_EQ(p.sealedRequests, true);
    EXPECT_EQ(p.parameters.size(), 4U);
}

TEST_F(G923Cap, ProbesHidppReadOnly) {
    FakeTransport t(true);
    const HidppProbe p = probeHidpp(t);
    EXPECT_EQ(p.protocol, "4.2");
    EXPECT_EQ(p.features.at(0x8123).index, 4);
    EXPECT_EQ(p.name, kName);
    ASSERT_EQ(p.firmware.size(), 1U);
    for (const Bytes& w : t.written) {
        EXPECT_TRUE(safeHidpp(kFeatures[w[2]], static_cast<std::uint8_t>(w[3] >> 4)));
        EXPECT_EQ(w[3] & 0x0F, kSoftwareId);
    }
}

TEST_F(G923Cap, OnlyReadsCanBeSent) {
    EXPECT_EQ(kSafeTrueForceTypes, (std::array<std::uint8_t, 1>{0x05}));
    for (const std::uint8_t type : {0x01, 0x03, 0x04, 0x06, 0x07, 0x0C, 0x0E}) {
        EXPECT_THROW((void)trueForceRequest(type, 1, {}, 64, false), Unsafe);
    }
    EXPECT_THROW((void)hidppRequest(4, 0x8123, 1, {}, 20), Unsafe);
    for (std::uint32_t fid = 0; fid <= 0xFFFF; ++fid) {
        for (std::uint8_t fn = 0; fn < 16; ++fn) {
            if (safeHidpp(static_cast<std::uint16_t>(fid), fn)) {
                EXPECT_TRUE(fid == 0x0000 || fid == 0x0001 || fid == 0x0003 || fid == 0x0005) << fid;
            }
        }
    }
}

TEST_F(G923Cap, ProbeMarkdownShowsDifferencesInBold) {
    ProbeRun run;
    TrueForceProbe p;
    p.parameters[0x01] = {std::bit_cast<std::uint32_t>(9.0F), true, ""};
    p.parameters[0x00] = {TrueForceDevice::parameterBits(0), true, ""};
    run.trueForce = p;
    const std::string md = probeMarkdown(run);
    EXPECT_NE(md.find("| 0x01 | **9** | 2 |"), std::string::npos);
    EXPECT_NE(md.find("| 0x00 | 2 | 2 |"), std::string::npos);
}

}  // namespace
}  // namespace wheelbri::g923cap
