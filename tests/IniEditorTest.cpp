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
