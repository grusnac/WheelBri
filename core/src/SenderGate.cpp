#include "wheelbri/SenderGate.hpp"

#include "wheelbri/FileText.hpp"

#include <string>
#include <utility>

namespace wheelbri {

namespace {

bool isSeparator(const char c) { return c == '\\' || c == '/'; }

// UTF-8, '/' as '\', ASCII letters in lower case, no trailing separator: enough to compare Windows paths. A network
// share keeps its leading "\\" (MinGW's lexically_normal makes it one separator: a folder on this drive).
std::string folded(const std::filesystem::path& path) {
    const std::string original = pathToUtf8(path);
    const bool share = original.size() > 2 && isSeparator(original[0]) && isSeparator(original[1]) &&
                       !isSeparator(original[2]);
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

}  // namespace wheelbri
