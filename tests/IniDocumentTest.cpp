#include "wheelbri/IniDocument.hpp"

#include <gtest/gtest.h>

#include <string>

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
    EXPECT_TRUE(result.errors.empty());
    ASSERT_EQ(result.warnings.size(), 1U);   // said: an edit to the first one would do nothing
    EXPECT_EQ(result.warnings[0], "line 3: k again in [a]: this one counts");
}

// A section given twice: its keys go together, and that is said too.
TEST(IniDocument, ASectionGivenTwiceIsMergedAndSaid) {
    const auto result = IniDocument::parse("[a]\nx = 1\n[b]\ny = 2\n[a]\nz = 3\n");
    EXPECT_EQ(result.document.get("a", "x"), "1");
    EXPECT_EQ(result.document.get("a", "z"), "3");
    ASSERT_EQ(result.warnings.size(), 1U);
    EXPECT_EQ(result.warnings[0], "line 5: [a] again: its keys go with the earlier one's");
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

// Headers that name no section ("[]", "[ ]", a lone "["): reported, and the keys below stay in the section before,
// not moved before the first section.
TEST(IniDocument, HeadersWithoutANameAreMalformed) {
    const auto result = IniDocument::parse("[a]\n[]\nx = 1\n[ \t ]\ny = 2\n[\nz = 3\n");
    ASSERT_EQ(result.errors.size(), 3U);
    EXPECT_EQ(result.errors[0], "line 2: malformed section header");
    EXPECT_EQ(result.errors[1], "line 4: malformed section header");
    EXPECT_EQ(result.errors[2], "line 6: malformed section header");
    EXPECT_EQ(result.document.get("a", "x"), "1");
    EXPECT_EQ(result.document.get("a", "y"), "2");
    EXPECT_EQ(result.document.get("a", "z"), "3");
    EXPECT_EQ(result.document.sections().count(""), 0U);
}

// Lines as hand edits leave them: comments everywhere (also inside a key, which then has no '='), '=' in a value,
// spaces in a key, whitespace-only lines, a last line without a line end, comment characters right after '='.
TEST(IniDocument, ReadsHandEditedLines) {
    const auto result = IniDocument::parse("  \t \r\n# top\n[ my section ] ; why\nmy key = a = b\nk#ey = v\n"
                                           "empty =\ncommented = ;gone\n\t\n[next]#c\nlast = 1");
    ASSERT_EQ(result.errors.size(), 1U);
    EXPECT_EQ(result.errors[0], "line 5: expected key = value");   // "k" is all that is left of "k#ey = v"
    EXPECT_EQ(result.document.get("my section", "my key"), "a = b");
    EXPECT_EQ(result.document.get("my section", "empty"), "");
    EXPECT_EQ(result.document.get("my section", "commented"), "");
    EXPECT_EQ(result.document.get("next", "last"), "1");
}

// Non-ASCII names and values are kept byte for byte; a very long value is read whole.
TEST(IniDocument, KeepsNonAsciiAndHugeValues) {
    const std::string huge(1 << 20, 'x');
    const auto result = IniDocument::parse("[p\xC3\xA9" "dales]\nn\xC3\xA4me = Pedal \xE2\x80\x94 \xE6\xB2\xB9\n"
                                           "big = " + huge + "\n");
    ASSERT_TRUE(result.errors.empty());
    EXPECT_EQ(result.document.get("p\xC3\xA9" "dales", "n\xC3\xA4me"), "Pedal \xE2\x80\x94 \xE6\xB2\xB9");
    EXPECT_EQ(result.document.get("p\xC3\xA9" "dales", "big"), huge);
}

// The same section twice (a copied block): its keys merge, the last value of each key counts.
TEST(IniDocument, DuplicateSectionsMerge) {
    const auto result = IniDocument::parse("[a]\nx = 1\ny = 1\n[b]\n[a]\nx = 2\n");
    ASSERT_TRUE(result.errors.empty());
    EXPECT_EQ(result.document.get("a", "x"), "2");
    EXPECT_EQ(result.document.get("a", "y"), "1");
    EXPECT_EQ(result.document.sections().size(), 2U);
    EXPECT_TRUE(result.document.sections().at("b").empty());   // an empty section is still there
}

// A value made safe for the file: tab kept, DEL and other control bytes become spaces, UTF-8 untouched.
TEST(IniDocument, SafeValuesReplaceEveryControlByte) {
    EXPECT_EQ(iniSafeValue("a\tb\x7F" "c\x01" "d\x1F" "e"), "a\tb c d e");
    EXPECT_EQ(iniSafeValue("P\xC3\xA9" "dale"), "P\xC3\xA9" "dale");
    EXPECT_EQ(iniSafeValue(""), "");
    const auto back = IniDocument::parse("[a]\nk = " + iniSafeValue("x #1 ; y") + "\n");
    EXPECT_EQ(back.document.get("a", "k"), "x  1   y");   // read back whole
}

}  // namespace
}  // namespace wheelbri
