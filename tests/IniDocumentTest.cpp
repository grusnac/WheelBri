#include "wheelbri/IniDocument.hpp"

#include <gtest/gtest.h>

namespace wheelbri {
namespace {

TEST(IniDocument, ParsesSectionsKeysAndComments) {
    const auto result = IniDocument::parse(
        "; leading comment\n"
        "[steering]\n"
        "axis = 0   # trailing comment\n"
        "  invert=true  \n"
        "\n"
        "[brake]\r\n"
        "gamma = 1.5\r\n");
    ASSERT_TRUE(result.errors.empty());
    EXPECT_EQ(result.document.get("steering", "axis"), "0");
    EXPECT_EQ(result.document.get("steering", "invert"), "true");
    EXPECT_EQ(result.document.get("brake", "gamma"), "1.5");
    EXPECT_FALSE(result.document.get("brake", "axis").has_value());
    EXPECT_FALSE(result.document.get("missing", "axis").has_value());
}

TEST(IniDocument, ReportsMalformedLinesWithLineNumbers) {
    const auto result = IniDocument::parse("[ok]\nno equals sign\n[broken\n= value\n");
    ASSERT_EQ(result.errors.size(), 3U);
    EXPECT_EQ(result.errors[0], "line 2: expected key = value");
    EXPECT_EQ(result.errors[1], "line 3: malformed section header");
    EXPECT_EQ(result.errors[2], "line 4: empty key");
}

TEST(IniDocument, LastValueWins) {
    const auto result = IniDocument::parse("[a]\nk = 1\nk = 2\n");
    EXPECT_EQ(result.document.get("a", "k"), "2");
}

TEST(IniDocument, IgnoresAUtf8ByteOrderMark) {
    const auto result = IniDocument::parse("\xEF\xBB\xBF[emulator]\nstate_rate_hz = 500\n");
    EXPECT_TRUE(result.errors.empty());
    EXPECT_EQ(result.document.get("emulator", "state_rate_hz"), "500");
}

TEST(IniDocument, EmptyInput) {
    const auto result = IniDocument::parse("");
    EXPECT_TRUE(result.errors.empty());
    EXPECT_TRUE(result.document.sections().empty());
}

}  // namespace
}  // namespace wheelbri
