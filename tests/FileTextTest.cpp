#include "wheelbri/FileText.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <csignal>
#include <cstdlib>
#include <sys/resource.h>
#include <unistd.h>
#endif

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

// Every way a byte sequence is not UTF-8 is refused: overlong forms, code points past U+10FFFF, a lead byte that
// cannot start one, a continuation byte where a character starts, a character cut by the next one.
TEST(FileText, EveryKindOfBadUtf8IsRefused) {
    for (const char* bad : {"\xE0\x80\xAF", "\xF0\x80\x80\xAF", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80",
                            "\xC1\xBF", "\x80", "\xC3\x28", "\xE2\x28\xA1", "\xF0\x9F\x8F"}) {
        EXPECT_TRUE(pathFromUtf8(std::string("x") + bad + "y").empty()) << pathToUtf8(bad);
    }
    // The edges that are valid: the last code point, the first 3- and 4-byte ones, just around the surrogates.
    for (const char* good : {"\xF4\x8F\xBF\xBF", "\xE0\xA0\x80", "\xF0\x90\x80\x80", "\xED\x9F\xBF", "\xEE\x80\x80"}) {
        EXPECT_FALSE(pathFromUtf8(std::string("x") + good).empty()) << good;
    }
}

TEST(FileText, TruncatingToNothingOrAtAnAsciiByte) {
    EXPECT_EQ(truncateUtf8("abc", 0), "");
    EXPECT_EQ(truncateUtf8("\xC3\xA9", 0), "");
    EXPECT_EQ(truncateUtf8("ab\xC3\xA9", 2), "ab");
    EXPECT_EQ(truncateUtf8("", 0), "");
}

// Something that is not a regular file (a device) is never read as a file.
TEST(FileText, ADeviceIsNotAFile) {
    EXPECT_FALSE(readFileText("/dev/null").has_value());
    EXPECT_FALSE(readFileText("").has_value());
}

// A link where the file goes is replaced by the file, never followed: the file it pointed to is untouched.
TEST_F(FileTextFiles, ALinkIsReplacedNotFollowed) {
    std::ofstream(dir_ / "elsewhere.ini") << "the user's";
    std::error_code ec;
    std::filesystem::create_symlink(dir_ / "elsewhere.ini", dir_ / "app.ini", ec);
    // Windows makes links only for Administrators or in Developer Mode.
    if (ec) GTEST_SKIP() << "links cannot be made here: " << ec.message();
    ASSERT_TRUE(writeFileReplacing(dir_ / "app.ini", "new"));
    EXPECT_FALSE(std::filesystem::is_symlink(dir_ / "app.ini"));
    EXPECT_EQ(readFileText(dir_ / "app.ini").value_or(""), "new");
    EXPECT_EQ(readFileText(dir_ / "elsewhere.ini").value_or(""), "the user's");
}

// A leftover <file>.tmp (an earlier attempt that died) is replaced; one that is a link is removed, not followed.
TEST_F(FileTextFiles, ALeftoverTemporaryFileIsReplaced) {
    std::ofstream(dir_ / "a.ini.tmp") << "half";
    ASSERT_TRUE(writeFileReplacing(dir_ / "a.ini", "whole"));
    EXPECT_EQ(readFileText(dir_ / "a.ini").value_or(""), "whole");
    EXPECT_FALSE(std::filesystem::exists(dir_ / "a.ini.tmp"));
    std::ofstream(dir_ / "target") << "keep me";
    std::error_code ec;
    std::filesystem::create_symlink(dir_ / "target", dir_ / "a.ini.tmp", ec);
    if (ec) GTEST_SKIP() << "links cannot be made here: " << ec.message();
    ASSERT_TRUE(writeFileReplacing(dir_ / "a.ini", "again"));
    EXPECT_EQ(readFileText(dir_ / "target").value_or(""), "keep me");
    EXPECT_EQ(readFileText(dir_ / "a.ini").value_or(""), "again");
}

// A folder whose name is not ASCII is made and written into.
TEST_F(FileTextFiles, NonAsciiFoldersAreMade) {
    const auto file = dir_ / pathFromUtf8("Jeux \xC3\xA9t\xC3\xA9") / pathFromUtf8("\xE6\xB8\xB8\xE6\x88\x8F.ini");
    ASSERT_TRUE(writeFileReplacing(file, "x"));
    EXPECT_EQ(readFileText(file).value_or(""), "x");
}

// A visitor that throws for one entry (a name it cannot convert) skips that entry; the others are still listed.
TEST_F(FileTextFiles, AnEntryThatThrowsIsSkipped) {
    for (const char* name : {"a", "b", "c"}) std::ofstream(dir_ / name) << name;
    std::set<std::string> seen;
    forEachEntry(dir_, [&seen](const std::filesystem::directory_entry& entry) {
        const std::string name = entry.path().filename().string();
        if (name == "b") throw std::runtime_error("cannot convert");
        seen.insert(name);
    });
    EXPECT_EQ(seen, (std::set<std::string>{"a", "c"}));
    int calls = 0;
    forEachEntry({}, [&calls](const std::filesystem::directory_entry&) { ++calls; });
    forEachEntry(dir_ / "a", [&calls](const std::filesystem::directory_entry&) { ++calls; });   // a file
    EXPECT_EQ(calls, 0);
}

// A file that cannot be read (no permission) is nothing, not an empty text.
TEST_F(FileTextFiles, AFileThatCannotBeOpenedIsNothing) {
    const auto file = dir_ / "a.ini";
    ASSERT_TRUE(writeFileReplacing(file, "secret"));
    std::filesystem::permissions(file, std::filesystem::perms::none);
    if (std::ifstream(file).good()) {
        std::filesystem::permissions(file, std::filesystem::perms::owner_all);
        GTEST_SKIP() << "the file stays readable here";
    }
    EXPECT_FALSE(readFileText(file).has_value());
    std::filesystem::permissions(file, std::filesystem::perms::owner_all);
}

// A folder that cannot be written to: the write says it failed and leaves no file behind.
TEST_F(FileTextFiles, AFolderThatCannotBeWrittenToFails) {
    const auto file = dir_ / "a.ini";
    ASSERT_TRUE(writeFileReplacing(file, "original"));
    std::filesystem::permissions(dir_, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    if (std::ofstream(dir_ / "probe").good()) {
        std::filesystem::permissions(dir_, std::filesystem::perms::owner_all);
        GTEST_SKIP() << "the folder stays writable here";
    }
    EXPECT_FALSE(writeFileReplacing(file, "new"));
    std::filesystem::permissions(dir_, std::filesystem::perms::owner_all);
    EXPECT_EQ(readFileText(file).value_or(""), "original");
    EXPECT_FALSE(std::filesystem::exists(dir_ / "a.ini.tmp"));
}

#ifndef _WIN32
// A bare file name (no folder): written in the current folder, no folder made for it. In a child process, whose
// current folder can be changed without touching the tests'.
TEST_F(FileTextFiles, ABareFileNameIsWrittenWhereWeAre) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const std::string dir = dir_.string();
    EXPECT_EXIT(
        {
            const bool ok = ::chdir(dir.c_str()) == 0 && writeFileReplacing("bare.ini", "text") &&
                            readFileText("bare.ini") == std::optional<std::string>("text");
            std::exit(ok ? 0 : 1);
        },
        ::testing::ExitedWithCode(0), "");
    EXPECT_EQ(readFileText(dir_ / "bare.ini").value_or(""), "text");
}

// The disk fills up while writing (here: the process may not write files past a size): the write fails, the
// original stays as it was and the half-written temporary file is removed.
TEST_F(FileTextFiles, AFullDiskKeepsTheOriginal) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const auto file = dir_ / "a.ini";
    ASSERT_TRUE(writeFileReplacing(file, "original"));
    EXPECT_EXIT(
        {
            std::signal(SIGXFSZ, SIG_IGN);
            rlimit before{};
            ::getrlimit(RLIMIT_FSIZE, &before);
            rlimit small = before;
            small.rlim_cur = 1000;
            ::setrlimit(RLIMIT_FSIZE, &small);
            const bool refused = !writeFileReplacing(file, std::string(100000, 'x'));
            ::setrlimit(RLIMIT_FSIZE, &before);
            std::exit(refused ? 0 : 1);
        },
        ::testing::ExitedWithCode(0), "");
    EXPECT_EQ(readFileText(file).value_or(""), "original");
    EXPECT_FALSE(std::filesystem::exists(dir_ / "a.ini.tmp"));
}
#endif

}  // namespace
}  // namespace wheelbri
