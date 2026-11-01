#include "wheelbri/LogitechFfbDriver.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

namespace wheelbri::logitech_ffb {
namespace {

template <std::size_t N>
std::string hex(const std::array<std::uint8_t, N>& data) {
    std::string text;
    for (const std::uint8_t b : data) {
        char two[3];
        std::snprintf(two, sizeof(two), "%02X", static_cast<unsigned>(b));
        text += two;
    }
    return text;
}

// As `reg query` showed the entry Logitech's software wrote for the G923 (2026-10-28).
TEST(LogitechFfbDriver, BytesAsLogitechsSoftwareWritesThem) {
    EXPECT_EQ(hex(bytes(std::array<std::uint32_t, 1>{kCreatedBy})), "00080000");
    EXPECT_EQ(hex(bytes(kAttributes)), "0000000040420F0040420F00");
    const char* const expected[] = {
        "26000F0001860000FD030000FD03000020000000", "27000F0002860000FD030000FD03000020000000",
        "30000F0003860000FD030000FD03000020000000", "31000F0003860000FD030000FD03000020000000",
        "32000F0003860000FD030000FD03000020000000", "33000F0003860000FD030000FD03000020000000",
        "34000F0003860000FD030000FD03000020000000", "40000F0004C80000650300006503000020000000",
        "41000F0004C80000650300006503000020000000", "42000F0004C80000650300006503000020000000",
        "43000F0004C80000650300006503000020000000",
    };
    for (std::size_t i = 0; i < kEffects.size(); ++i) {
        EXPECT_EQ(hex(bytes(kEffects[i].attributes)), expected[i]) << i;
    }
}

TEST(LogitechFfbDriver, WritesOnlyWhatIsMissingAndInstalled) {
    EXPECT_EQ(action(kClsid, true), Action::Keep);
    EXPECT_EQ(action(L"{62b43f0e-e7db-4329-8c13-a966d84a289f}", false), Action::Keep);   // any case
    EXPECT_EQ(action(L"", true), Action::Write);
    EXPECT_EQ(action(L"{EEC6993A-B3FD-11D2-A916-00C04FB98638}", true), Action::Write);   // Windows' PID driver
    EXPECT_EQ(action(L"", false), Action::NoDriver);
    EXPECT_EQ(action(L"{EEC6993A-B3FD-11D2-A916-00C04FB98638}", false), Action::NoDriver);
}

}  // namespace
}  // namespace wheelbri::logitech_ffb
