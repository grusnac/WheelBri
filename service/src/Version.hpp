#pragma once

#include "BuildInfo.hpp"   // generated on every build (cmake/BuildInfo.cmake): WHEELBRI_BUILD

#include <string>

namespace wheelbri::service {

// The version in the app's top-right corner: "0.9.3 ee5b047", the project version and the git commit built ("+":
// with uncommitted changes); only the version when built without git.
[[nodiscard]] inline std::string buildVersion() {
    std::string version = WHEELBRI_VERSION;
    if (WHEELBRI_BUILD[0] != '\0') {
        version += ' ';
        version += WHEELBRI_BUILD;
    }
    return version;
}

// What the version takes of the top row, from the right edge (logical pixels: 14 characters and a gap). A view with
// controls in its top row keeps them left of it (the Rig tab's Save and Revert).
inline constexpr float kVersionReserve = 14 * 8 + 8;

}  // namespace wheelbri::service
