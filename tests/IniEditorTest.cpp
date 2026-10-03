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
