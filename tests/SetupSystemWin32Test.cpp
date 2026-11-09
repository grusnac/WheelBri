// SPDX-License-Identifier: GPL-3.0-or-later
// The installer's real system layer (installer/setup/SetupSystemWin32.cpp) on a temporary folder and a registry key
// of the tests' own under HKEY_CURRENT_USER: files, junctions (never followed), the registry, programs run. Needs no
// Administrator.

#include "SetupSteps.hpp"
#include "SetupSystemWin32.hpp"

#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>
#include <string>

namespace wheelbri::setup {
namespace {

struct NullReporter final : Reporter {
    void step(int, const std::wstring&) override {}
    void item(const char*, ItemState, const std::wstring&) override {}
    void note(const std::wstring&) override {}
    void warn(const std::wstring&) override {}
    void log(const std::wstring&) override {}
    void fail(const std::wstring&) override {}
    void restartNeeded() override {}
};

class SetupSystemWin32Test : public ::testing::Test {
protected:
    SetupSystemWin32 s;
    std::wstring root;
    std::wstring key;

    void SetUp() override {
        static std::atomic<int> counter{0};
        const std::wstring unique = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++counter);
        root = joinPath(s.folder(Folder::Temp), L"wheelbri-setup-test-" + unique);
        key = L"Software\\wheelbri-setup-test-" + unique;
        ASSERT_TRUE(s.makeFolders(root));
    }
    void TearDown() override {
        s.regDeleteTree(Hive::CurrentUser, key);
        removeAll(root);
    }
    void removeAll(const std::wstring& path) {
        if (s.isLink(path)) {
            s.removeFolder(path);
            return;
        }
        for (const DirEntry& e : s.list(path)) {
            const std::wstring child = joinPath(path, e.name);
            if (e.folder) removeAll(child);
            else s.removeFile(child);
        }
        s.removeFolder(path);
    }
    // A directory junction at `link` to `target` (no Administrator needed, unlike a symbolic link).
    bool junction(const std::wstring& link, const std::wstring& target) {
        return s.run(L"cmd.exe /c mklink /J \"" + link + L"\" \"" + target + L"\"", root, [](const std::wstring&) {}) == 0;
    }
};

TEST_F(SetupSystemWin32Test, FilesAreWrittenReadCopiedMovedAndRemoved) {
    const std::wstring a = joinPath(root, L"deep\\er\\a.txt");
    ASSERT_TRUE(s.makeFolders(parentOf(a)));
    ASSERT_TRUE(s.writeFile(a, std::string("one\0two", 7)));
    EXPECT_TRUE(s.isFile(a));
    EXPECT_FALSE(s.isFolder(a));
    EXPECT_EQ(s.readFile(a), std::string("one\0two", 7));
    EXPECT_EQ(s.fileSize(a), 7U);
    EXPECT_FALSE(s.isFile(a + L".tmp"));   // written through a temporary file, renamed
    ASSERT_TRUE(s.writeFile(a, ""));        // replaced, even empty
    EXPECT_EQ(s.readFile(a), std::string());
    const std::wstring b = joinPath(root, L"b.txt");
    ASSERT_TRUE(s.writeFile(b, "bee"));
    SetFileAttributesW(b.c_str(), FILE_ATTRIBUTE_READONLY);
    EXPECT_TRUE(s.copyFile(a, b));          // a read-only target is replaced too
    EXPECT_EQ(s.readFile(b), std::string());
    EXPECT_FALSE(s.moveFile(a, b));         // never over a file
    EXPECT_TRUE(s.moveFile(a, joinPath(root, L"c.txt")));
    EXPECT_FALSE(s.isFile(a));
    SetFileAttributesW(b.c_str(), FILE_ATTRIBUTE_READONLY);
    EXPECT_TRUE(s.removeFile(b));
    EXPECT_FALSE(s.isFile(b));
    EXPECT_TRUE(s.removeFile(b));           // already gone
    EXPECT_EQ(s.readFile(b), std::nullopt);
    EXPECT_FALSE(s.removeFolder(root));     // not empty
    EXPECT_FALSE(s.makeFolders(joinPath(root, L"c.txt\\sub")));   // a file is in the way
}

TEST_F(SetupSystemWin32Test, ListShowsFoldersFilesAndJunctions) {
    ASSERT_TRUE(s.makeFolders(joinPath(root, L"dir")));
    ASSERT_TRUE(s.writeFile(joinPath(root, L"file.txt"), "x"));
    ASSERT_TRUE(junction(joinPath(root, L"link"), joinPath(root, L"dir")));
    int folders = 0, files = 0, links = 0;
    for (const DirEntry& e : s.list(root)) {
        folders += e.folder;
        files += !e.folder;
        links += e.link;
    }
    EXPECT_EQ(folders, 2);
    EXPECT_EQ(files, 1);
    EXPECT_EQ(links, 1);
    EXPECT_TRUE(s.isLink(joinPath(root, L"link")));
    EXPECT_FALSE(s.isLink(joinPath(root, L"dir")));
    EXPECT_TRUE(s.list(joinPath(root, L"missing")).empty());
}

TEST_F(SetupSystemWin32Test, RemovingAnInstallNeverDeletesThroughAJunction) {
    const std::wstring outside = joinPath(root, L"outside");
    const std::wstring install = joinPath(root, L"install");
    ASSERT_TRUE(s.makeFolders(outside));
    ASSERT_TRUE(s.writeFile(joinPath(outside, L"precious.txt"), "keep"));
    ASSERT_TRUE(s.makeFolders(install));
    ASSERT_TRUE(s.writeFile(joinPath(install, L"wheelbri.exe"), "app"));
    ASSERT_TRUE(junction(joinPath(install, L"config"), outside));   // WheelBri's own subfolder name, a junction
    ASSERT_TRUE(junction(joinPath(install, L"linked"), outside));
    ASSERT_TRUE(s.writeFile(joinPath(install, L"installed-files.txt"),
                            linesFile({L"wheelbri.exe", L"linked\\precious.txt", L"config\\precious.txt"})));
    NullReporter r;
    EXPECT_FALSE(removeInstallFolder(s, install, r));   // the other junction stays (not listed)
    EXPECT_EQ(s.readFile(joinPath(outside, L"precious.txt")), std::string("keep"));
    EXPECT_FALSE(s.isFile(joinPath(install, L"wheelbri.exe")));
    EXPECT_FALSE(s.isLink(joinPath(install, L"config")));   // the junction itself went
}

TEST_F(SetupSystemWin32Test, RegistryValuesOfEveryKind) {
    const std::wstring sub = key + L"\\a\\b";
    ASSERT_TRUE(s.regSetText(Hive::CurrentUser, sub, L"Text", L"café"));
    ASSERT_TRUE(s.regSetMultiText(Hive::CurrentUser, sub, L"Multi", L"{\"a\":1}\nsecond line"));
    ASSERT_TRUE(s.regSetDword(Hive::CurrentUser, sub, L"Number", 7));
    EXPECT_EQ(s.regText(Hive::CurrentUser, sub, L"Text"), std::wstring(L"café"));
    EXPECT_EQ(s.regText(Hive::CurrentUser, sub, L"Multi"), std::wstring(L"{\"a\":1}\nsecond line"));
    EXPECT_EQ(s.regText(Hive::CurrentUser, sub, L"Number"), std::nullopt);   // not text
    EXPECT_TRUE(s.regValueExists(Hive::CurrentUser, sub, L"Number"));
    EXPECT_EQ(s.regValueNames(Hive::CurrentUser, sub).size(), 3U);
    EXPECT_EQ(s.regSubKeys(Hive::CurrentUser, key + L"\\a"), std::vector<std::wstring>{L"b"});
    s.regDeleteValue(Hive::CurrentUser, sub, L"Text");
    EXPECT_FALSE(s.regValueExists(Hive::CurrentUser, sub, L"Text"));
    ASSERT_TRUE(s.regSetText(Hive::CurrentUser, sub, L"Empty", L""));
    EXPECT_EQ(s.regText(Hive::CurrentUser, sub, L"Empty"), std::wstring());
    ASSERT_TRUE(s.regSetMultiText(Hive::CurrentUser, sub, L"EmptyMulti", L""));
    EXPECT_EQ(s.regText(Hive::CurrentUser, sub, L"EmptyMulti"), std::wstring());
    s.regDeleteTree(Hive::CurrentUser, key + L"\\a");
    EXPECT_FALSE(s.regKeyExists(Hive::CurrentUser, key + L"\\a"));
    EXPECT_TRUE(s.regKeyExists(Hive::CurrentUser, key));
    EXPECT_EQ(s.regText(Hive::CurrentUser, key + L"\\none", L"x"), std::nullopt);
}

TEST_F(SetupSystemWin32Test, ProgramsRunHiddenWithTheirOutputAndExitCode) {
    std::vector<std::wstring> lines;
    const int code = s.run(L"cmd.exe /c \"echo first& echo second& exit 3\"", root,
                           [&](const std::wstring& line) { lines.push_back(line); });
    EXPECT_EQ(code, 3);
    ASSERT_EQ(lines.size(), 2U);
    EXPECT_EQ(lines[0], L"first");
    EXPECT_EQ(lines[1], L"second");
    EXPECT_EQ(s.run(L"\"" + joinPath(root, L"no such program.exe") + L"\"", root, [](const std::wstring&) {}), -1);
}

TEST_F(SetupSystemWin32Test, ThisPcAnswers) {
    EXPECT_GE(s.windowsBuild(), 7601U);
    EXPECT_TRUE(s.is64BitWindows());
    for (const Folder f : {Folder::SystemRoot, Folder::System32, Folder::ProgramFiles, Folder::ProgramData,
                           Folder::CommonPrograms, Folder::CommonDesktop, Folder::Temp}) {
        const std::wstring path = s.folder(f);
        EXPECT_TRUE(s.isFolder(path)) << toUtf8(path);
        EXPECT_NE(path.back(), L'\\') << toUtf8(path);
    }
    EXPECT_EQ(s.desktopUserSid().rfind(L"S-1-5-", 0), 0U);
    const LocalTime t = s.now();
    EXPECT_GE(t.year, 2026);
}

TEST_F(SetupSystemWin32Test, PackageFromSetupExesFlatFiles) {
    ASSERT_TRUE(s.writeFile(joinPath(root, L"f000"), "app"));
    ASSERT_TRUE(s.writeFile(joinPath(root, L"f001"), "1.2.3\r\n"));
    ASSERT_TRUE(s.writeFile(joinPath(root, L"package-files.txt"), "f000\twheelbri.exe\r\nf001\tVERSION.txt\r\n"));
    const Package p = readPackage(s, root, joinPath(root, L"wheelbri-setup.exe"));
    ASSERT_EQ(p.files.size(), 2U);
    EXPECT_EQ(p.version, L"1.2.3");
    EXPECT_EQ(p.find(L"wheelbri.exe")->source, joinPath(root, L"f000"));
}

}  // namespace
}  // namespace wheelbri::setup
