// SPDX-License-Identifier: GPL-3.0-or-later
#include "DeviceAudit.hpp"

#include "DiFormat.hpp"
#include "ModuleWatch.hpp"
#include "ShimLogging.hpp"

#include <chrono>
#include <cstdio>
#include <span>

namespace wheelbri::shim {

namespace {

const logging::Logger& log() {
    static const logging::Logger instance = logger("shim.audit");
    return instance;
}

constexpr std::uint64_t kRiskyLogged = 20;   // could-remap calls of one kind logged in full
constexpr std::int64_t kFirstSummaryMs = 60'000;
constexpr std::int64_t kSummaryEveryMs = 300'000;

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::uint32_t propertyId(REFGUID property) {
    // DIPROP_* are MAKEDIPROP(n): the integer n cast to a GUID reference. A real GUID: a custom property, unknown.
    const auto id = reinterpret_cast<ULONG_PTR>(&property);
    return id > 0xFFFF ? 0xFFFFFFFFU : static_cast<std::uint32_t>(id);
}

std::span<const std::uint8_t> bytesOf(const void* data, const DWORD size) {
    if (data == nullptr || size == 0) return {};
    return {static_cast<const std::uint8_t*>(data), size};
}

}  // namespace

void DeviceAudit::record(const std::string_view kind, const std::string_view details, const AxisAudit::Risk risk) {
    const std::uint64_t number = audit_.record(kind, risk);
    if (risk == AxisAudit::Risk::CouldRemap) {
        if (number <= kRiskyLogged) {
            WE_LOG_WARN(log(), "{}: {} #{} could remap axes{}{}", label_, kind, number, details.empty() ? "" : ": ",
                        details);
        }
    } else if (number == 1) {
        WE_LOG_INFO(log(), "{}: first {}{}{}", label_, kind, details.empty() ? "" : ": ", details);
    }
}

void DeviceAudit::call(const std::string_view kind, const std::string_view details) {
    record(kind, details, AxisAudit::Risk::None);
}

void DeviceAudit::risky(const std::string_view kind, const std::string_view details) {
    record(kind, details, AxisAudit::Risk::CouldRemap);
}

void DeviceAudit::getProperty(REFGUID property, LPCDIPROPHEADER header, const HRESULT result) {
    std::string details = describeHresult(result);
    if (SUCCEEDED(result)) details += " " + describePropertyValue(property, header);
    call("GetProperty " + describeProperty(property), details);
}

void DeviceAudit::setProperty(REFGUID property, LPCDIPROPHEADER header, const HRESULT result) {
    const std::string details = describePropertyValue(property, header) + " -> " + describeHresult(result);
    const std::string kind = "SetProperty " + describeProperty(property);
    record(kind, details, setPropertyCouldRemap(propertyId(property)) ? AxisAudit::Risk::CouldRemap
                                                                       : AxisAudit::Risk::None);
}

void DeviceAudit::escape(LPDIEFFESCAPE escape, const HRESULT result) {
    if (escape == nullptr) {
        risky("Escape (null)");
        return;
    }
    const auto in = bytesOf(escape->lpvInBuffer, escape->cbInBuffer);
    const auto out = bytesOf(escape->lpvOutBuffer, escape->cbOutBuffer);
    if (const auto lights = decodeRevLights(escape->dwCommand, in)) {
        char text[96];
        std::snprintf(text, sizeof(text), "rpm %.3f, first light %.3f, redline %.3f", static_cast<double>(lights->rpm),
                      static_cast<double>(lights->firstLight), static_cast<double>(lights->redline));
        call("Escape rev lights (Logitech, command 0x0, 20 bytes)",
             std::string(text) + "; in: " + hexBytes(in) + " -> " + describeHresult(result));
        return;
    }
    char kind[96];
    std::snprintf(kind, sizeof(kind), "Escape command 0x%lx (%lu bytes in, %lu out)",
                  static_cast<unsigned long>(escape->dwCommand), static_cast<unsigned long>(escape->cbInBuffer),
                  static_cast<unsigned long>(escape->cbOutBuffer));
    risky(kind, "in: " + hexBytes(in) + " -> " + describeHresult(result) +
                    (out.empty() ? std::string() : "; out: " + hexBytes(out)));
}

void DeviceAudit::tick() {
    const std::int64_t now = nowMs();
    std::int64_t next = nextSummaryMs_.load(std::memory_order_relaxed);
    if (next < 0) {
        nextSummaryMs_.compare_exchange_strong(next, now + kFirstSummaryMs, std::memory_order_relaxed);
        return;
    }
    if (now < next || !nextSummaryMs_.compare_exchange_strong(next, now + kSummaryEveryMs, std::memory_order_relaxed)) {
        return;
    }
    // Only the first periodic summary looks at the modules: a Toolhelp snapshot on the game's input thread every few
    // minutes could cost a frame.
    if (periodicSummaries_.fetch_add(1) == 0) {
        summary("the first minute of reading");
    } else {
        WE_LOG_INFO(log(), "{}: axis audit after {} more minutes: {}", label_, kSummaryEveryMs / 60'000,
                    audit_.summary());
    }
}

void DeviceAudit::summary(const std::string_view when) {
    if (audit_.couldRemapCalls() != 0) {
        WE_LOG_WARN(log(), "{}: axis audit at {}: {}", label_, when, audit_.summary());
    } else {
        WE_LOG_INFO(log(), "{}: axis audit at {}: {}", label_, when, audit_.summary());
    }
    logNewModules(when);
}

}  // namespace wheelbri::shim
