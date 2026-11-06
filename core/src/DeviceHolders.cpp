// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/DeviceHolders.hpp"

#include <algorithm>

namespace wheelbri::holders {

namespace {

char upper(const char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }

bool startsWithIgnoringCase(const std::string_view text, const std::string_view prefix) {
    if (text.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (upper(text[i]) != upper(prefix[i])) return false;
    }
    return true;
}

}  // namespace

bool namesDevice(const std::string_view handleName, const std::vector<std::string>& deviceNames) {
    if (handleName.empty()) return false;
    for (const std::string& device : deviceNames) {
        if (device.empty() || !startsWithIgnoringCase(handleName, device)) continue;
        if (handleName.size() == device.size() || handleName[device.size()] == '\\') return true;
    }
    return false;
}

std::string describe(std::vector<Holder> holders) {
    std::sort(holders.begin(), holders.end(), [](const Holder& a, const Holder& b) { return a.pid < b.pid; });
    std::string text;
    std::uint32_t last = 0;
    bool any = false;
    for (const Holder& holder : holders) {
        if (any && holder.pid == last) continue;
        any = true;
        last = holder.pid;
        if (!text.empty()) text += ", ";
        text += holder.program.empty() ? "pid " + std::to_string(holder.pid)
                                       : holder.program + " (pid " + std::to_string(holder.pid) + ")";
    }
    return text;
}

}  // namespace wheelbri::holders
