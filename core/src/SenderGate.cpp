// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/SenderGate.hpp"

#include "wheelbri/FileText.hpp"

#include <charconv>
#include <string>
#include <utility>

namespace wheelbri {

namespace {

bool isSeparator(const char c) { return c == '\\' || c == '/'; }

// UTF-8, '/' as '\', ASCII letters in lower case, no trailing separator: enough to compare Windows paths. A network
// share keeps its leading "\\" (MinGW's lexically_normal makes it one separator: a folder on this drive); more than
// two separators at the start are still a share, as Windows reads them, on every system alike.
std::string folded(const std::filesystem::path& path) {
    const std::string original = pathToUtf8(path);
    const bool share = original.size() > 2 && isSeparator(original[0]) && isSeparator(original[1]);
    const std::string raw = pathToUtf8(path.lexically_normal());
    std::string text;
    text.reserve(raw.size());
    for (char c : raw) {
        if (c == '/') c = '\\';
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        // "a\\b" is "a\b" (but a leading "\\server" stays)
        if (c == '\\' && text.size() > 1 && text.back() == '\\') continue;
        text += c;
    }
    while (text.size() > 1 && text.back() == '\\') text.pop_back();
    if (share && text.rfind("\\\\", 0) != 0) text.insert(0, 1, '\\');
    return text;
}

}  // namespace

bool sameFolder(const std::filesystem::path& a, const std::filesystem::path& b) {
    return !a.empty() && !b.empty() && folded(a) == folded(b);
}

bool isDriverHostImage(const std::filesystem::path& image, const std::filesystem::path& systemFolder) {
    return !systemFolder.empty() && folded(image) == folded(systemFolder / "WUDFHost.exe");
}

bool isWheelBriImage(const std::filesystem::path& image) {
    std::string name = folded(image);   // the file name, whichever separator the path uses
    if (const auto slash = name.rfind('\\'); slash != std::string::npos) name.erase(0, slash + 1);
    return name == "wheelbri.exe" || name == "wheelbri-service.exe";
}

SenderGate::SenderGate(Lookup lookup, const Settings settings) : lookup_(std::move(lookup)), settings_(settings) {}

bool SenderGate::allow(const std::uint16_t port, const std::uint64_t nowNs) {
    lastWasNewRefusal_ = false;
    Entry* slot = nullptr;   // where a new verdict goes: this port's old entry, else a free one, else the least used
    for (Entry& entry : entries_) {
        if (entry.used && entry.port == port) {
            if (nowNs < entry.expiresNs) {
                entry.lastUseNs = nowNs;
                if (!entry.accept) ++stats_.refused;
                return entry.accept;
            }
            slot = &entry;
            break;
        }
    }
    if (slot == nullptr) {
        for (Entry& entry : entries_) {
            if (!entry.used) {
                slot = &entry;
                break;
            }
            if (slot == nullptr || entry.lastUseNs < slot->lastUseNs) slot = &entry;
        }
    }
    if (nowNs - windowStartNs_ >= 1'000'000'000 || nowNs < windowStartNs_) {
        windowStartNs_ = nowNs;
        lookupsInWindow_ = 0;
    }
    if (slot == nullptr) return false;   // no entries at all (never: the table has a fixed size)
    if (lookupsInWindow_ >= settings_.maxLookupsPerSecond) {
        ++stats_.overLookupCap;
        ++stats_.refused;
        return false;
    }
    ++lookupsInWindow_;
    ++stats_.lookups;
    SenderVerdict verdict = SenderVerdict::Unknown;
    if (lookup_) verdict = lookup_(port);
    if (verdict == SenderVerdict::Unknown) ++stats_.unknown;
    const bool accept =
        verdict == SenderVerdict::Accept || (verdict == SenderVerdict::Unknown && settings_.acceptUnknown);
    *slot = Entry{port, true, accept, nowNs + (accept ? settings_.acceptTtlNs : settings_.refuseTtlNs), nowNs};
    if (!accept) {
        ++stats_.refused;
        lastWasNewRefusal_ = true;
    }
    return accept;
}

std::optional<std::uint32_t> udpPortUid(const std::string_view procNetUdp, const std::uint16_t port) {
    // A line: "  12: 0100007F:A1B2 00000000:0000 07 00000000:00000000 00:00000000 00000000  1000  0 12345 ..." with
    // the local address in the kernel's byte order (127.0.0.1 reads 0100007F on x86, 7F000001 on big-endian), its
    // port in hex, and the owner's user id as the eighth field.
    const auto fields = [](std::string_view line) {
        std::array<std::string_view, 8> out{};
        std::size_t n = 0;
        while (n < out.size()) {
            const std::size_t start = line.find_first_not_of(" \t");
            if (start == std::string_view::npos) break;
            line.remove_prefix(start);
            const std::size_t end = line.find_first_of(" \t");
            out[n++] = line.substr(0, end);
            if (end == std::string_view::npos) break;
            line.remove_prefix(end);
        }
        return n == out.size() ? std::optional(out) : std::nullopt;
    };
    std::optional<std::uint32_t> owner;
    std::string_view rest = procNetUdp;
    while (!rest.empty()) {
        const std::size_t newline = rest.find('\n');
        const std::string_view line = rest.substr(0, newline);
        rest.remove_prefix(newline == std::string_view::npos ? rest.size() : newline + 1);
        const auto f = fields(line);
        if (!f) continue;
        const std::string_view local = (*f)[1];
        if (local.size() != 13 || local[8] != ':') continue;
        const std::string_view address = local.substr(0, 8);
        if (address != "0100007F" && address != "7F000001" && address != "00000000") continue;
        const auto number = [](const std::string_view text, unsigned int& value, const int base) {
            const char* const end = text.data() + text.size();
            const auto [ptr, ec] = std::from_chars(text.data(), end, value, base);
            return ec == std::errc{} && ptr == end;
        };
        unsigned int listed = 0;
        if (!number(local.substr(9), listed, 16) || listed != port) continue;
        unsigned int uid = 0;
        if (!number((*f)[7], uid, 10)) continue;
        if (owner && *owner != uid) return std::nullopt;
        owner = static_cast<std::uint32_t>(uid);
    }
    return owner;
}

}  // namespace wheelbri
