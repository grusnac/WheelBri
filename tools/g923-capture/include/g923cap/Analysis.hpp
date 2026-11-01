#pragma once

// Decodes one capture session: the wheel's device and interfaces, its reports sorted into TrueForce, HID++ and the
// rest, and what each says. Reports (Report.hpp) turns a SessionResult into summary.md and the day's report.md.

#include "g923cap/Pcap.hpp"
#include "g923cap/UsbDescriptors.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wheelbri::g923cap {

enum class Kind { Unknown, Input, Hidpp, TrueForce, Vendor, Mixed, InputHidpp };
[[nodiscard]] const char* kindName(Kind kind);

struct Device {
    std::uint16_t bus = 0;
    std::uint16_t address = 0;
    std::optional<DeviceDescriptor> descriptor;
    std::size_t descriptorFrame = 0;
    std::vector<Interface> interfaces;
    std::size_t configurationFrame = 0;
    std::map<int, Bytes> reportDescriptors;   // interface -> raw
    std::map<int, std::size_t> reportDescriptorFrames;
    std::size_t transfers = 0;
    std::size_t trueForceLike = 0;

    [[nodiscard]] std::string ident() const;
    [[nodiscard]] std::vector<Collection> collections(int interface) const;
    [[nodiscard]] std::optional<int> interfaceOfEndpoint(std::uint8_t endpoint) const;
    // The interface's kind; Mixed when its collections carry more than one kind (the real G923 has its game
    // controller and HID++ side by side on interface 0): its reports go by report ID then.
    [[nodiscard]] Kind kindOfInterface(std::optional<int> interface) const;
    [[nodiscard]] Kind kindOfReport(std::optional<int> interface, std::uint8_t reportId) const;
};

using DeviceKey = std::pair<std::uint16_t, std::uint16_t>;   // bus, address
[[nodiscard]] std::map<DeviceKey, Device> findDevices(const std::vector<Transfer>& transfers);

struct Pick {
    const Device* device = nullptr;
    std::string why;
};
[[nodiscard]] Pick pickTarget(const std::map<DeviceKey, Device>& devices, std::optional<int> address = std::nullopt);

struct Report {
    double ts = 0;
    bool out = false;   // host -> wheel
    std::optional<int> interface;
    Bytes data;
    std::size_t frame = 0;
    const char* via = "interrupt";   // or SET_REPORT / GET_REPORT
};

[[nodiscard]] std::vector<Report> reportsOf(const Device& device, const std::vector<Transfer>& transfers);
// Each interface's kind: from its report descriptor, or (none in this capture) from what most of its reports look
// like. InputHidpp: an interface the host writes HID++ to, whose HID++-shaped reports are HID++ all the same.
[[nodiscard]] std::map<int, Kind> interfaceKinds(const Device& device, const std::vector<Report>& reports);
// Input, Hidpp or TrueForce.
[[nodiscard]] Kind classify(const Device& device, const Report& report, const std::map<int, Kind>& kinds);

// --- Notes and shim logs ----------------------------------------------------------------------------------------------

struct Note {
    double ts = 0;   // UTC seconds
    std::string local, kind, text;
};
struct Hold {
    std::string label;
    double degrees = 0;   // + = right
    double start = 0;
    double end = 0;
};
struct Notes {
    std::vector<Note> lines;
    std::vector<Hold> holds;
};
// notes.txt: "<unix ms>\t<local time>\t<KIND>\t<text>"; HOLD_START "<label>|<degrees>", HOLD_END "<label>".
[[nodiscard]] Notes readNotes(const std::filesystem::path& folder);

struct ShimLog {
    std::vector<std::string> files;
    std::map<std::string, int> calls;          // logiXxx -> lines
    std::map<std::string, std::string> first;  // logiXxx -> its first time
};
[[nodiscard]] ShimLog readShimLogs(const std::filesystem::path& folder);

// --- Results --------------------------------------------------------------------------------------------------------

struct Example {
    std::size_t frame = 0;
    std::string hex;
    std::string via;
};

struct TfPair {
    int sent = 0;
    int answered = 0;
    std::map<int, int> answerTypes;
    std::optional<Example> firstRequest;
    std::optional<Example> firstAnswer;
    std::optional<std::size_t> sealAt;
    bool sealed = false;
    std::optional<double> medianDelayMs;
};

struct TfParameter {
    std::uint32_t bits = 0;
    std::size_t frame = 0;
    bool wellFormed = false;
    std::string raw;
    std::vector<std::uint32_t> bitsSeen;
    [[nodiscard]] double value() const;
};

struct TfStatus {
    std::size_t count = 0;
    std::optional<double> ratePerS;
    std::uint32_t positionMin = 0, positionMax = 0;
    std::vector<std::pair<std::string, std::size_t>> at9, at21, countRange, torque1, torque2;   // value, how often
    std::vector<std::string> at17Examples;
    std::string torqueRaw;
    std::size_t sealed = 0;
    std::optional<double> clockStepUs;
    std::optional<double> hostGapUs;
    std::int64_t speedMin = 0, speedMax = 0;
    Example first;
};

struct TfHold {
    std::string label;
    double degrees = 0;
    std::size_t samples = 0;
    std::optional<double> position;
    std::optional<double> perDegree;
    std::optional<double> perTurn;
};

struct TfStream {
    std::size_t count = 0;
    double ratePerS = 0;
    std::size_t firstFrame = 0;
    std::optional<std::pair<double, double>> curRange;
    std::size_t curNone = 0;
    std::vector<std::pair<std::string, std::size_t>> bytes10and11, bytes8and9;
    std::optional<int> texturePeak;
    std::map<std::string, std::size_t> via;
};

struct TfSlots {
    std::size_t frame = 0;
    int count = 0;
    std::vector<Bytes> slots;
    std::string hex;
    bool sealed = false;
};

struct TrueForceResult {
    std::map<int, std::size_t> requests, answers;   // by type
    std::map<int, TfPair> pairs;                    // by request type (not the stream)
    std::map<int, TfParameter> parameters;          // by index
    std::vector<std::uint8_t> parameterRequests;
    std::vector<TfSlots> slots;
    std::map<int, TfStatus> status;
    std::vector<TfHold> holds;
    std::optional<double> centre;
    std::optional<TfStream> stream;
    std::vector<Example> unanswered;
    std::map<int, std::vector<Example>> examples;   // request type -> first few
};

struct HidppExample {
    std::size_t frame = 0;
    std::string params;
    std::optional<std::string> answer;
    std::optional<std::size_t> answerFrame;
};
struct HidppRow {
    std::uint8_t featureIndex = 0;
    std::uint8_t function = 0;
    int count = 0;
    int answered = 0;
    std::map<std::string, int> errors;
    std::map<int, int> softwareIds;
    std::map<int, int> reportIds;
    std::vector<HidppExample> examples;
};
struct Firmware {
    int entity = 0;
    int type = 0;
    std::string prefix, number, revision, build, raw;
};
struct HidppResult {
    int requests = 0;
    int answered = 0;
    std::vector<Example> unanswered;
    std::map<int, std::uint16_t> featureMap;   // index -> feature id
    std::vector<HidppRow> rows;
    int unsolicited = 0;
    std::vector<Example> unsolicitedExamples;
    std::vector<Firmware> firmware;
    std::string name;
    std::map<std::string, int> via;
};

struct InputResult {
    std::size_t in = 0;
    std::size_t out = 0;
    std::vector<std::pair<std::string, std::size_t>> inShapes, outShapes;
    std::vector<Example> outExamples;
};

struct SessionResult {
    std::string session;
    std::filesystem::path folder;
    std::vector<std::string> captures;
    Notes notes;
    ShimLog shim;
    std::string error;   // set: nothing else below
    std::string capture;
    Device target;
    std::string pickedBy;
    std::vector<std::string> otherDevices;
    double spanS = 0;
    std::map<int, Kind> kinds;
    std::string descriptorsFrom;   // the cache file the report descriptors came from, when not in this capture
    TrueForceResult trueForce;
    HidppResult hidpp;
    InputResult input;
};

// One session folder. `streamTsv`: where to write every stream packet (none: not written).
[[nodiscard]] SessionResult analyseSession(const std::filesystem::path& folder,
                                           std::optional<int> address = std::nullopt);

[[nodiscard]] TrueForceResult analyseTrueForce(const std::vector<const Report*>& reports,
                                               const std::vector<Hold>& holds,
                                               const std::optional<std::filesystem::path>& streamTsv);
[[nodiscard]] HidppResult analyseHidpp(const std::vector<const Report*>& reports);
[[nodiscard]] InputResult analyseInput(const std::vector<const Report*>& reports);

// The day's report-descriptor cache, next to the session folders: "descriptors-VVVV-PPPP.txt", one line per
// interface: "<interface> <hex>".
[[nodiscard]] std::filesystem::path descriptorCache(const std::filesystem::path& day, const DeviceDescriptor& d);

}  // namespace wheelbri::g923cap
