#include "wheelbri/FileText.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace wheelbri {
namespace {

class FileTextFiles : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("wheelbri-filetext-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    std::filesystem::path dir_;
};

TEST_F(FileTextFiles, WriteReplacesAndReadsBack) {
    const auto file = dir_ / "a.ini";
    ASSERT_TRUE(writeFileReplacing(file, "first\r\n"));
    ASSERT_TRUE(writeFileReplacing(file, "second\r\n"));
    EXPECT_EQ(readFileText(file).value_or(""), "second\r\n");
    EXPECT_FALSE(std::filesystem::exists(dir_ / "a.ini.tmp"));   // nothing left behind
    EXPECT_FALSE(readFileText(dir_ / "missing.ini").has_value());
}

TEST_F(FileTextFiles, FailedWriteKeepsTheOriginal) {
    const auto file = dir_ / "a.ini";
    ASSERT_TRUE(writeFileReplacing(file, "original"));
    std::filesystem::create_directories(dir_ / "a.ini.tmp" / "inside");   // the temporary file cannot be made
    EXPECT_FALSE(writeFileReplacing(file, "new"));
    EXPECT_EQ(readFileText(file).value_or(""), "original");
}

TEST_F(FileTextFiles, AFolderIsNeitherReadNorReplaced) {
    std::filesystem::create_directories(dir_ / "folder.ini");
    EXPECT_FALSE(readFileText(dir_ / "folder.ini").has_value());
    EXPECT_FALSE(writeFileReplacing(dir_ / "folder.ini", "text"));
    EXPECT_TRUE(std::filesystem::is_directory(dir_ / "folder.ini"));
}

TEST_F(FileTextFiles, ForEachEntryListsAndToleratesAMissingFolder) {
    std::ofstream(dir_ / "x.txt") << "x";
    std::ofstream(dir_ / "y.txt") << "y";
    std::set<std::string> names;
    forEachEntry(dir_, [&](const std::filesystem::directory_entry& e) { names.insert(e.path().filename().string()); });
    EXPECT_EQ(names, (std::set<std::string>{"x.txt", "y.txt"}));
    int calls = 0;
    forEachEntry(dir_ / "missing", [&](const std::filesystem::directory_entry&) { ++calls; });
    EXPECT_EQ(calls, 0);
}

TEST(FileText, TruncateNeverSplitsACharacter) {
    EXPECT_EQ(truncateUtf8("h\xC3\xA9llo", 2), "h");          // é is two bytes: not cut in half
    EXPECT_EQ(truncateUtf8("h\xC3\xA9llo", 3), "h\xC3\xA9");
    EXPECT_EQ(truncateUtf8("abc", 10), "abc");
    EXPECT_EQ(truncateUtf8("\xE2\x82\xAC", 2), "");           // € is three bytes
}

TEST(FileText, PathsRoundTripAndBadUtf8IsEmpty) {
    const std::string name = "Spiele/Need for Speed\xE2\x84\xA2";
    EXPECT_EQ(pathToUtf8(pathFromUtf8(name)), name);
    EXPECT_TRUE(pathFromUtf8("bad \xFF byte").empty());
    EXPECT_TRUE(pathFromUtf8("overlong \xC0\xAF").empty());
    EXPECT_TRUE(pathFromUtf8("cut \xE2\x82").empty());
    EXPECT_TRUE(pathFromUtf8("surrogate \xED\xA0\x80").empty());
    EXPECT_FALSE(pathFromUtf8("emoji \xF0\x9F\x8F\x81").empty());
}

}  // namespace
}  // namespace wheelbri
