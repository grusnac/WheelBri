#include "UhidG920.hpp"

#include "wheelbri/HidG920.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <span>

namespace wheelbri::uhid {

namespace {

const char* reportType(const std::uint8_t rtype) {
    switch (rtype) {
        case UHID_FEATURE_REPORT: return "feature";
        case UHID_OUTPUT_REPORT: return "output";
        case UHID_INPUT_REPORT: return "input";
        default: return "?";
    }
}

// Reports from hidraw come with their report ID first (the descriptor numbers every report).
std::span<const std::uint8_t> withoutId(const std::uint8_t* data, std::size_t size, const std::uint8_t id) {
    if (size > 0 && data[0] == id) {
        return {data + 1, size - 1};
    }
    return {data, size};
}

}  // namespace

UhidG920::UhidG920(Write write, HidPid::Sender ffb, Log log)
    : write_(std::move(write)), log_(log), pid_(std::move(ffb), std::move(log)) {}

bool UhidG920::create() {
    uhid_event event{};
    event.type = UHID_CREATE2;
    auto& req = event.u.create2;
    std::strncpy(reinterpret_cast<char*>(req.name), kName, sizeof(req.name) - 1);
    std::strncpy(reinterpret_cast<char*>(req.phys), "wheelbri", sizeof(req.phys) - 1);
    std::strncpy(reinterpret_cast<char*>(req.uniq), "wheelbri-g920", sizeof(req.uniq) - 1);
    const auto descriptor = g920ReportDescriptor();
    if (descriptor.size() > sizeof(req.rd_data)) {
        log_(LogLevel::Error, "uhid: descriptor too large");
        return false;
    }
    std::memcpy(req.rd_data, descriptor.data(), descriptor.size());
    req.rd_size = static_cast<std::uint16_t>(descriptor.size());
    // USB, as a real G920: the kernel's Logitech driver (hid-logitech-hidpp) matches the ids, finds no HID++
    // reports in this descriptor and leaves the device to the generic HID driver.
    req.bus = 0x03;   // BUS_USB
    req.vendor = kG920VendorId;
    req.product = kG920ProductId;
    req.version = 0x0100;
    req.country = 0;
    if (!write_(event)) {
        return false;
    }
    submit(g920Neutral(), true);
    return true;
}

void UhidG920::destroy() {
    uhid_event event{};
    event.type = UHID_DESTROY;
    write_(event);
}

void UhidG920::submit(const TargetReport& report, const bool force) {
    const auto packed = packG920Input(report);
    static_assert(packed.size() == std::tuple_size_v<decltype(last_)>);
    if (!force && haveLast_ && packed == last_) {
        return;
    }
    last_ = packed;
    haveLast_ = true;
    uhid_event event{};
    event.type = UHID_INPUT2;
    event.u.input2.size = static_cast<std::uint16_t>(packed.size());
    std::memcpy(event.u.input2.data, packed.data(), packed.size());
    write_(event);
}

void UhidG920::reply(const std::uint32_t type, const std::uint32_t id, const std::uint16_t err,
                     const std::uint8_t* data, const std::size_t size) {
    uhid_event event{};
    event.type = type;
    if (type == UHID_GET_REPORT_REPLY) {
        event.u.get_report_reply.id = id;
        event.u.get_report_reply.err = err;
        const std::size_t n = std::min<std::size_t>(size, sizeof(event.u.get_report_reply.data));
        event.u.get_report_reply.size = static_cast<std::uint16_t>(n);
        if (n > 0) {
            std::memcpy(event.u.get_report_reply.data, data, n);
        }
    } else {
        event.u.set_report_reply.id = id;
        event.u.set_report_reply.err = err;
    }
    write_(event);
}

void UhidG920::handle(const uhid_event& event, const std::int64_t nowMs) {
    switch (event.type) {
        case UHID_START:
            started_ = true;
            log_(LogLevel::Info, "uhid: virtual G920 started (the kernel took it)");
            break;
        case UHID_STOP:
            started_ = false;
            log_(LogLevel::Info, "uhid: virtual G920 stopped by the kernel");
            break;
        case UHID_OPEN:
            ++openCount_;
            log_(LogLevel::Info, "uhid: a program opened the virtual G920 (" + std::to_string(openCount_) + " now)");
            break;
        case UHID_CLOSE:
            openCount_ = std::max(0, openCount_ - 1);
            log_(LogLevel::Info, "uhid: a program closed the virtual G920 (" + std::to_string(openCount_) + " left)");
            // The last reader gone (the game exited, or crashed: the kernel closes its handle either way). Its
            // effects must not play on: springs would otherwise get keep-alives for as long as this runs.
            if (openCount_ == 0 && pid_.activeSlots() > 0) {
                pid_.stopAll(nowMs);
                log_(LogLevel::Info, "uhid: nobody reads the virtual G920 any more: the game's forces are stopped");
            }
            break;
        case UHID_OUTPUT: {
            const auto& out = event.u.output;
            const std::size_t size = std::min<std::size_t>(out.size, sizeof(out.data));
            if (size == 0) {
                break;
            }
            const std::uint8_t id = out.data[0];
            pid_.output(id, withoutId(out.data, size, id), nowMs);
            break;
        }
        case UHID_GET_REPORT: {
            const auto& get = event.u.get_report;
            std::array<std::uint8_t, 64> buffer{};
            std::size_t size = 0;
            if (get.rtype == UHID_FEATURE_REPORT) {
                size = pid_.getFeature(get.rnum, buffer);
            } else if (get.rtype == UHID_INPUT_REPORT && get.rnum == kG920InputReportId && haveLast_) {
                std::copy(last_.begin(), last_.end(), buffer.begin());
                size = last_.size();
            }
            if (size == 0 && unsupported_.emplace(get.rtype, get.rnum).second) {   // once each: some poll it
                log_(LogLevel::Warn, "uhid: get " + std::string(reportType(get.rtype)) + " report " +
                                         std::to_string(get.rnum) + ": not supported (said once)");
            }
            reply(UHID_GET_REPORT_REPLY, get.id, size > 0 ? 0 : EIO, buffer.data(), size);
            break;
        }
        case UHID_SET_REPORT: {
            const auto& set = event.u.set_report;
            const std::size_t size = std::min<std::size_t>(set.size, sizeof(set.data));
            const auto data = withoutId(set.data, size, set.rnum);
            if (set.rtype == UHID_FEATURE_REPORT) {
                pid_.setFeature(set.rnum, data, nowMs);
            } else if (set.rtype == UHID_OUTPUT_REPORT) {
                pid_.output(set.rnum, data, nowMs);
            }
            reply(UHID_SET_REPORT_REPLY, set.id, 0, nullptr, 0);
            break;
        }
        default:
            break;
    }
}

}  // namespace wheelbri::uhid
