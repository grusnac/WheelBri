// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer's entry (-DWE_BUILD_FUZZ=ON, Clang): build/tests/wheelbri_fuzz -max_total_time=600 <corpus folder>.

#include "FuzzTargets.hpp"

#include <cstdio>
#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    try {
        wheelbri::fuzz::runOne({data, size});
    } catch (const wheelbri::fuzz::Broken& broken) {
        std::fprintf(stderr, "broken: %s\n", broken.what());
        std::abort();
    }
    return 0;
}
