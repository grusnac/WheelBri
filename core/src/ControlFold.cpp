// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/ControlFold.hpp"

#include <cstddef>

namespace wheelbri {

ControlFold foldUnnamed(const std::vector<bool>& named, const bool fold) {
    ControlFold result;
    for (std::size_t i = 0; i < named.size(); ++i) {
        (named[i] || !fold ? result.shown : result.hidden).push_back(static_cast<int>(i));
    }
    return result;
}

std::string indexRanges(const std::vector<int>& indexes) {
    std::string text;
    const auto add = [&text](const std::string& part) { text += (text.empty() ? "" : ", ") + part; };
    for (std::size_t i = 0; i < indexes.size();) {
        std::size_t end = i;   // the run's last element
        while (end + 1 < indexes.size() && indexes[end + 1] == indexes[end] + 1) ++end;
        if (end - i >= 2) {
            add(std::to_string(indexes[i]) + "-" + std::to_string(indexes[end]));
        } else {
            for (std::size_t k = i; k <= end; ++k) add(std::to_string(indexes[k]));
        }
        i = end + 1;
    }
    return text;
}

}  // namespace wheelbri
