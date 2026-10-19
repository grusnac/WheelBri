#include "UhidWheel.hpp"

#include "wheelbri/HidWheelDevice.hpp"

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

UhidWheel::UhidWheel(Write write, HidPid::Sender ffb, Log log, const VirtualWheel& wheel)
    : write_(std::move(write)), log_(log), wheel_(wheel), pid_(std::move(ffb), std::move(log)) {
    if (wheel_.hidpp) {
        HidPp::Identity identity;
        identity.name = std::string(wheel_.product);
        identity.productId = wheel_.productId;
        identity.unitId = 0x57420001;   // "WB" 0001, as the Windows driver's
        identity.deviceType = wheel_.deviceType;
        hidpp_ = std::make_unique<HidPp>(identity, log_);
    }
}

void UhidWheel::hidppRequest(const std::span<const std::uint8_t> report, const std::int64_t nowMs) {
    if (!hidpp_) return;
    const auto answer = hidpp_->handle(report, nowMs);
    if (!answer) return;
    uhid_event event{};
    event.type = UHID_INPUT2;
    const std::size_t n = std::min(answer->size(), sizeof(event.u.input2.data));
    event.u.input2.size = static_cast<std::uint16_t>(n);
    std::memcpy(event.u.input2.data, answer->data(), n);
    write_(event);
}

bool UhidWheel::create() {
    uhid_event event{};
    event.type = UHID_CREATE2;
    auto& req = event.u.create2;
    const std::size_t nameSize = std::min(wheel_.oemName.size(), sizeof(req.name) - 1);
    std::memcpy(req.name, wheel_.oemName.data(), nameSize);
    std::strncpy(reinterpret_cast<char*>(req.phys), "wheelbri", sizeof(req.phys) - 1);
    std::strncpy(reinterpret_cast<char*>(req.uniq), kVirtualWheelUniq, sizeof(req.uniq) - 1);
    // Games read it through Proton's DirectInput: its G920 hack wants the G920's own axis names (see the header).
    const bool g920 = wheel_.profile == "g920";
    const auto descriptor = g920 ? g920ProtonReportDescriptor() : reportDescriptorFor(wheel_, DescriptorFor::Linux);
    if (descriptor.size() > sizeof(req.rd_data)) {
        log_(LogLevel::Error, "uhid: descriptor too large");
        return false;
    }
    std::memcpy(req.rd_data, descriptor.data(), descriptor.size());
    req.rd_size = static_cast<std::uint16_t>(descriptor.size());
    // The G920 on USB, as a real one: the kernel's Logitech driver (hid-logitech-hidpp) matches the ids, finds no
    // HID++ reports in this descriptor and leaves the device to the generic HID driver. The G25 and G27 on the virtual
    // bus: hid-lg would claim them on USB and then refuse them (not a real USB device), leaving them to no driver at
    // all. The G923 there too: Proton's axis hack is for the G920's ids only, so it gets the Windows descriptor, and on
    // the virtual bus no Logitech driver (hid-logitech-hidpp also matches 046D:C26E on USB) looks at it.
    req.bus = g920 ? 0x03 : 0x06;   // BUS_USB, BUS_VIRTUAL
    req.vendor = wheel_.vendorId;
    req.product = wheel_.productId;
    req.version = 0x0100;
    req.country = 0;
    if (!write_(event)) {
        return false;
    }
    submit(neutralReport(), true);
    return true;
}

void UhidWheel::destroy() {
    uhid_event event{};
    event.type = UHID_DESTROY;
    write_(event);
}

bool UhidWheel::submit(const TargetReport& report, const bool force) {
    const InputReport packed = packInputFor(wheel_, report, DescriptorFor::Linux);
    if (!force && haveLast_ && packed == last_) {
        return false;
    }
    last_ = packed;
    haveLast_ = true;
    uhid_event event{};
    event.type = UHID_INPUT2;
    event.u.input2.size = static_cast<std::uint16_t>(packed.size);
    std::memcpy(event.u.input2.data, packed.bytes.data(), packed.size);
    write_(event);
    return true;
}

void UhidWheel::reply(const std::uint32_t type, const std::uint32_t id, const std::uint16_t err,
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

void UhidWheel::handle(const uhid_event& event, const std::int64_t nowMs) {
    switch (event.type) {
        case UHID_START:
            started_ = true;
            log_(LogLevel::Info, "uhid: virtual " + label() + " started (the kernel took it)");
            break;
        case UHID_STOP:
            started_ = false;
            log_(LogLevel::Info, "uhid: virtual " + label() + " stopped by the kernel");
            break;
        case UHID_OPEN:
            ++openCount_;
            log_(LogLevel::Info,
                 "uhid: a program opened the virtual " + label() + " (" + std::to_string(openCount_) + " now)");
            break;
        case UHID_CLOSE:
            openCount_ = std::max(0, openCount_ - 1);
            log_(LogLevel::Info,
                 "uhid: a program closed the virtual " + label() + " (" + std::to_string(openCount_) + " left)");
            // The last reader gone (the game exited, or crashed: the kernel closes its handle either way). Its
            // effects must not play on: springs would otherwise get keep-alives for as long as this runs.
            if (openCount_ == 0 && pid_.activeSlots() > 0) {
                pid_.stopAll(nowMs);
                log_(LogLevel::Info, "uhid: nobody reads the virtual wheel any more: the game's forces are stopped");
            }
            break;
        case UHID_OUTPUT: {
            const auto& out = event.u.output;
            const std::size_t size = std::min<std::size_t>(out.size, sizeof(out.data));
            if (size == 0) {
                break;
            }
            const std::uint8_t id = out.data[0];
            const ReportRoute route = routeReport(wheel_, id);
            if (route.to == ReportRoute::To::HidPp) {
                hidppRequest(std::span<const std::uint8_t>(out.data, size), nowMs);
            } else if (route.to == ReportRoute::To::Pid) {
                pid_.output(route.id, withoutId(out.data, size, id), nowMs);
            }
            break;
        }
        case UHID_GET_REPORT: {
            const auto& get = event.u.get_report;
            std::array<std::uint8_t, 64> buffer{};
            std::size_t size = 0;
            const ReportRoute route = routeReport(wheel_, static_cast<std::uint8_t>(get.rnum));
            if (get.rtype == UHID_FEATURE_REPORT && route.to == ReportRoute::To::Pid) {
                size = pid_.getFeature(route.id, buffer);
                if (size > 0) buffer[0] = wirePidId(wheel_, route.id);
            } else if (get.rtype == UHID_INPUT_REPORT && get.rnum == kWheelInputReportId && haveLast_) {
                std::copy(last_.bytes.begin(), last_.bytes.begin() + static_cast<std::ptrdiff_t>(last_.size),
                          buffer.begin());
                size = last_.size;
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
            const ReportRoute route = routeReport(wheel_, set.rnum);
            if (set.rtype == UHID_OUTPUT_REPORT && route.to == ReportRoute::To::HidPp) {
                std::array<std::uint8_t, HidPp::kVeryLongSize> asked{};
                asked[0] = set.rnum;
                const std::size_t n = std::min(data.size(), asked.size() - 1);
                std::copy_n(data.begin(), n, asked.begin() + 1);
                hidppRequest(std::span<const std::uint8_t>(asked.data(), n + 1), nowMs);
            } else if (set.rtype == UHID_FEATURE_REPORT && route.to == ReportRoute::To::Pid) {
                pid_.setFeature(route.id, data, nowMs);
            } else if (set.rtype == UHID_OUTPUT_REPORT && route.to == ReportRoute::To::Pid) {
                pid_.output(route.id, data, nowMs);
            }
            reply(UHID_SET_REPORT_REPLY, set.id, 0, nullptr, 0);
            break;
        }
        default:
            break;
    }
}

}  // namespace wheelbri::uhid
