#include "wheelbri/IniEditor.hpp"

#include "wheelbri/IniDocument.hpp"

#include <gtest/gtest.h>

using wheelbri::IniDocument;
using wheelbri::IniEditor;

TEST(IniEditor, KeepsTheTextWhenNothingChanges) {
    const std::string text = "; top\n[a]\nx = 1   ; why\n\n[b]\ny = 2\n";
    IniEditor ini(text);
    ini.set("a", "x", "1");
    EXPECT_EQ(ini.text(), text);
}

TEST(IniEditor, KeepsANumberWrittenDifferently) {
    const std::string text = "[a]\nx = 1.0   ; one\ny = 0.50\n";
    IniEditor ini(text);
    ini.set("a", "x", "1");
    ini.set("a", "y", "0.5");
    EXPECT_EQ(ini.text(), text);
    ini.set("a", "y", "0.6");
    EXPECT_EQ(ini.text(), "[a]\nx = 1.0   ; one\ny = 0.6\n");
}

TEST(IniEditor, ReplacesAValueAndDropsItsOldComment) {
    IniEditor ini("[a]\nx = 1   ; was one\ny = 2\n");
    ini.set("a", "x", "5");
    EXPECT_EQ(ini.text(), "[a]\nx = 5\ny = 2\n");
}

TEST(IniEditor, AddsAKeyAfterTheLastKeyOfItsSection) {
    IniEditor ini("[a]\nx = 1\n; about b\n\n[b]\ny = 2\n");
    ini.set("a", "z", "3");
    EXPECT_EQ(ini.text(), "[a]\nx = 1\nz = 3\n; about b\n\n[b]\ny = 2\n");
}

TEST(IniEditor, AddsAMissingSectionAtTheEnd) {
    IniEditor ini("[a]\nx = 1\n");
    ini.set("c", "k", "v");
    EXPECT_EQ(ini.text(), "[a]\nx = 1\n\n[c]\nk = v\n");
    const auto parsed = IniDocument::parse(ini.text());
    EXPECT_TRUE(parsed.errors.empty());
    EXPECT_EQ(parsed.document.get("c", "k").value_or(""), "v");
}

TEST(IniEditor, RemovesAKeyAndKeepsCrlfAndBom) {
    IniEditor ini("\xEF\xBB\xBF[a]\r\nx = 1\r\ny = 2\r\n");
    ini.remove("a", "x");
    ini.remove("a", "missing");
    EXPECT_EQ(ini.text(), "\xEF\xBB\xBF[a]\r\ny = 2\r\n");
}

TEST(IniEditor, PrependsATitleOnce) {
    IniEditor ini("[a]\nx = 1\n");
    ini.prepend("; title");
    ini.prepend("; title");
    EXPECT_EQ(ini.text(), "; title\n[a]\nx = 1\n");
    EXPECT_TRUE(ini.hasSection("a"));
    EXPECT_FALSE(ini.hasSection("b"));
}

TEST(IniEditor, AKeyWrittenTwiceIsEditedWhereTheReaderLooks) {
    // The reader takes the last copy; the editor used to change the first, so the edit was lost.
    IniEditor ini("[a]\nx = 1\ny = 2\nx = 3\n");
    ini.set("a", "x", "5");
    EXPECT_EQ(ini.text(), "[a]\ny = 2\nx = 5\n");
    EXPECT_EQ(IniDocument::parse(ini.text()).document.get("a", "x"), "5");
    IniEditor twice("[a]\nx = 1\nx = 3\n[b]\nx = 4\n");
    twice.remove("a", "x");
    EXPECT_EQ(twice.text(), "[a]\n[b]\nx = 4\n");   // every copy, and only in its section
}

TEST(IniEditor, ALineBreakInAValueStaysOnItsLine) {
    IniEditor ini("[a]\n");
    ini.set("a", "x", "one\n[b]\ny = 2");
    const auto parsed = IniDocument::parse(ini.text());
    EXPECT_EQ(parsed.document.get("a", "x"), "one [b] y = 2");
    EXPECT_FALSE(parsed.document.get("b", "y").has_value());
}

TEST(IniEditor, SafeValuesKeepEverythingButCommentsAndControls) {
    EXPECT_EQ(wheelbri::iniSafeValue("Button Box #2; v1\r\n"), "Button Box  2  v1  ");
    EXPECT_EQ(wheelbri::iniSafeValue("MOZA R3 (Base)"), "MOZA R3 (Base)");
}

// "[ ]" is no header (as the reader says): a key set in [a] after it stays in [a], and reads back there.
TEST(IniEditor, AHeaderWithoutANameIsNoSection) {
    IniEditor ini("[a]\nx = 1\n[ ]\ny = 2\n");
    EXPECT_FALSE(ini.hasSection(""));
    ini.set("a", "y", "3");
    EXPECT_EQ(ini.text(), "[a]\nx = 1\n[ ]\ny = 3\n");
    EXPECT_EQ(IniDocument::parse(ini.text()).document.get("a", "y"), "3");
}

// An empty value and a number are different values; so are "1" and "1abc" (not a number at all).
TEST(IniEditor, EmptyAndNotQuiteNumbersAreChanges) {
    IniEditor ini("[a]\nx = 0\ny =\nz = 1abc\n");
    ini.set("a", "x", "");
    ini.set("a", "y", "0");
    ini.set("a", "z", "1");
    EXPECT_EQ(ini.text(), "[a]\nx = \ny = 0\nz = 1\n");
}

// A file with no line end after its last line, or no text at all: edited and given line ends.
TEST(IniEditor, NoFinalLineEndAndEmptyText) {
    IniEditor last("[a]\nx = 1");
    last.set("a", "y", "2");
    EXPECT_EQ(last.text(), "[a]\nx = 1\ny = 2\n");
    IniEditor empty("");
    EXPECT_EQ(empty.text(), "");
    empty.set("c", "k", "v");
    EXPECT_EQ(empty.text(), "[c]\nk = v\n");   // no blank line before the first section
    IniEditor title("");
    title.prepend("; title");
    EXPECT_EQ(title.text(), "; title\n");
}

// A new section after a file that already ends with a blank line: no second blank line.
TEST(IniEditor, NewSectionAfterABlankLineAddsNoOther) {
    IniEditor ini("[a]\r\nx = 1\r\n\r\n");
    ini.set("c", "k", "v");
    EXPECT_EQ(ini.text(), "[a]\r\nx = 1\r\n\r\n[c]\r\nk = v\r\n");
}

// A carriage return alone in a value would end the line for some editors: it becomes a space too.
TEST(IniEditor, ACarriageReturnInAValueStaysOnItsLine) {
    IniEditor ini("[a]\n");
    ini.set("a", "x", "one\rtwo");
    EXPECT_EQ(ini.text(), "[a]\nx = one two\n");
}

// Write then read gives the same: every value set is what the reader gets, CRLF and BOM files included, a key in a
// section that appears twice included.
TEST(IniEditor, RoundTripsThroughTheReader) {
    IniEditor ini("\xEF\xBB\xBF; mine\r\n[a]\r\nx = 1 ; one\r\n[b]\r\n[a]\r\ny = 2\r\n");
    ini.set("a", "x", "10");
    ini.set("a", "y", "P\xC3\xA9" "dale gauche");
    ini.set("a", "z", "");
    ini.set("b", "w", "4");
    ini.set("new", "k", std::string(5000, 'v'));
    const auto parsed = IniDocument::parse(ini.text());
    ASSERT_TRUE(parsed.errors.empty());
    EXPECT_EQ(parsed.document.get("a", "x"), "10");
    EXPECT_EQ(parsed.document.get("a", "y"), "P\xC3\xA9" "dale gauche");
    EXPECT_EQ(parsed.document.get("a", "z"), "");
    EXPECT_EQ(parsed.document.get("b", "w"), "4");
    EXPECT_EQ(parsed.document.get("new", "k"), std::string(5000, 'v'));
    EXPECT_EQ(ini.text().rfind("\xEF\xBB\xBF", 0), 0U);
    EXPECT_EQ(ini.text().find("\n\n"), std::string::npos);   // every line end is CRLF
    EXPECT_EQ(IniEditor(ini.text()).text(), ini.text());     // and reading it again changes nothing
}

// A line that opens a header but never closes it ("[ffb" by hand) is no header: a key set later lands in the section
// before it, and the line stays as written.
TEST(IniEditor, AnUnclosedHeaderIsNoHeader) {
    IniEditor ini("[a]\nx = 1\n[ffb\n");
    ini.set("a", "y", "2");
    EXPECT_EQ(ini.text(), "[a]\nx = 1\ny = 2\n[ffb\n");
}
