// SPDX-License-Identifier: GPL-3.0-or-later
// The installer's rules without a system (installer/setup/SetupLogic.cpp): file lists, names, Firefox's policy text,
// the shim files, the command line, SHA-256.

#include "SetupLogic.hpp"

#include <gtest/gtest.h>

namespace wheelbri::setup {
namespace {

TEST(SetupLogic, ManifestEntriesStayInsideTheFolder) {
    for (const wchar_t* good : {L"wheelbri.exe", L"config\\g920.ini", L"driver\\hide\\x64\\Release\\a.dll", L"a b\\c.d"}) {
        EXPECT_TRUE(isManifestEntry(good)) << std::string(toUtf8(good));
    }
    for (const wchar_t* bad : {L"", L"..", L".", L"..\\x", L"a\\..\\b", L"a\\.\\b", L"C:\\x", L"C:x", L"\\x", L"x\\",
                               L"a\\\\b", L"a/b", L"\\\\server\\share", L"a\tb", L"a?b", L"a*b", L"a\"b", L"a<b", L"a|b"}) {
        EXPECT_FALSE(isManifestEntry(bad)) << std::string(toUtf8(bad));
    }
}

TEST(SetupLogic, TextLinesDropBomCarriageReturnsSpacesAndBlankLines) {
    const std::vector<std::wstring> expected = {L"a.txt", L"config\\b c.ini"};
    EXPECT_EQ(textLines("\xEF\xBB\xBF" "a.txt\r\n\r\n  config\\b c.ini  \r\n"), expected);
    EXPECT_EQ(textLines("a.txt\nconfig\\b c.ini"), expected);
    EXPECT_TRUE(textLines("").empty());
    EXPECT_TRUE(textLines("\xEF\xBB\xBF").empty());
    EXPECT_TRUE(textLines("\r\n \r\n\t").empty());
}

TEST(SetupLogic, LinesFileIsUtf8WithBomAndCrlfAndReadsBack) {
    const std::vector<std::wstring> lines = {L"config\\caf\u00e9.ini", L"z.dll"};
    const std::string file = linesFile(lines);
    EXPECT_EQ(file, "\xEF\xBB\xBF" "config\\caf\xC3\xA9.ini\r\nz.dll\r\n");
    EXPECT_EQ(textLines(file), lines);
    EXPECT_EQ(linesFile({}), "\xEF\xBB\xBF");
}

TEST(SetupLogic, DefaultConfigsRoundTripAndIgnoreBrokenLines) {
    ConfigHashes hashes;
    hashes[L"config\\g920.ini"] = L"AB";
    hashes[L"config\\G25.ini"] = L"CD";
    const ConfigHashes back = readDefaultConfigs(defaultConfigsFile(hashes));
    EXPECT_EQ(back, hashes);
    EXPECT_EQ(back.at(L"CONFIG\\g25.INI"), L"CD");   // names ignore case
    const ConfigHashes broken = readDefaultConfigs("no tab here\r\na\tb\tc\r\nconfig\\x.ini\t EF \r\n");
    ASSERT_EQ(broken.size(), 1U);
    EXPECT_EQ(broken.at(L"config\\x.ini"), L"EF");
}

TEST(SetupLogic, PackageFilesKeepOnlyPlainNamesAndEntriesInsideThePackage) {
    const auto entries = readPackageFiles("f000\twheelbri.exe\r\nf001\tconfig\\g920.ini\r\n"
                                          "sub\\f002\tx.dll\r\nf003\t..\\evil.dll\r\nf004\tC:\\Windows\\x.dll\r\n"
                                          "f005\r\n\tnoname.dll\r\nf006\ta\tb\r\n..\tparent.dll\r\n");
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].flatName, L"f000");
    EXPECT_EQ(entries[0].relative, L"wheelbri.exe");
    EXPECT_EQ(entries[1].relative, L"config\\g920.ini");
}

TEST(SetupLogic, StartMenuNames) {
    for (const wchar_t* good : {L"WheelBri", L"Games\\WheelBri", L"  WheelBri  ", L"Wheel.Bri", L"a b"}) {
        EXPECT_TRUE(startMenuNameProblem(good).empty()) << std::string(toUtf8(good));
    }
    for (const wchar_t* bad : {L"", L"   ", L"..", L".", L"a\\..", L"a\\\\b", L"a:b", L"a/b", L"a*b", L"a?b", L"a\"b",
                               L"a<b", L"a>b", L"a|b", L"a\tb", L"\\"}) {
        EXPECT_FALSE(startMenuNameProblem(bad).empty()) << std::string(toUtf8(bad));
    }
    EXPECT_TRUE(startMenuNameProblem(std::wstring(120, L'a')).empty());
    EXPECT_FALSE(startMenuNameProblem(std::wstring(121, L'a')).empty());
}

TEST(SetupLogic, PathHelpers) {
    EXPECT_EQ(joinPath(L"C:\\a", L"b"), L"C:\\a\\b");
    EXPECT_EQ(joinPath(L"C:\\a\\", L"\\b"), L"C:\\a\\b");
    EXPECT_EQ(joinPath(L"", L"b"), L"b");
    EXPECT_EQ(parentOf(L"C:\\a\\b.txt"), L"C:\\a");
    EXPECT_EQ(parentOf(L"b.txt"), L"");
    EXPECT_EQ(nameOf(L"C:\\a\\b.txt"), L"b.txt");
    EXPECT_EQ(withoutTrailingSlash(L"C:\\a\\\\"), L"C:\\a");
    EXPECT_EQ(withoutTrailingSlash(L"C:\\"), L"C:\\");
    EXPECT_TRUE(isInside(L"C:\\Program Files\\WheelBri", L"c:\\program files"));
    EXPECT_TRUE(isInside(L"C:\\Program Files", L"C:\\Program Files\\"));
    EXPECT_FALSE(isInside(L"C:\\Program Files (x86)\\x", L"C:\\Program Files"));
    EXPECT_FALSE(isInside(L"C:\\Prog", L"C:\\Program Files"));
    EXPECT_TRUE(isInside(L"config\\x.ini", L"config"));
    EXPECT_FALSE(isInside(L"configs\\x.ini", L"config"));
}

TEST(SetupLogic, Utf8RoundTripsAndReplacesBrokenBytes) {
    const std::wstring text = L"Gr\u00e9goire \u65e5\u672c \U0001F3CE";
    EXPECT_EQ(fromUtf8(toUtf8(text)), text);
    EXPECT_EQ(fromUtf8("a\xFF" "b"), L"a\uFFFDb");
    EXPECT_EQ(fromUtf8("\xE6\x97"), L"\uFFFD\uFFFD");   // cut short
    EXPECT_EQ(fromUtf8("\xC3(" ), L"\uFFFD(");
}

TEST(SetupLogic, FirefoxTurnOffAddsToAnEmptyOrMissingPolicy) {
    for (const char* empty : {"", "  ", "{}", " { } "}) {
        const FirefoxPolicy p = turnOffGamepads(empty);
        EXPECT_EQ(p.result, FirefoxPolicy::Result::Changed) << empty;
        EXPECT_EQ(p.text, R"({"dom.gamepad.enabled":{"Value":false,"Status":"locked"}})") << empty;
    }
}

TEST(SetupLogic, FirefoxTurnOffKeepsOtherPreferencesAsWritten) {
    const FirefoxPolicy p = turnOffGamepads(R"({ "browser.x": {"Value": "a,}\"b", "Status": "default"}, "n": [1, {"k": null}], "e": -1.5e3 })");
    ASSERT_EQ(p.result, FirefoxPolicy::Result::Changed);
    EXPECT_EQ(p.text, R"({"browser.x":{"Value": "a,}\"b", "Status": "default"},"n":[1, {"k": null}],"e":-1.5e3,)"
                      R"("dom.gamepad.enabled":{"Value":false,"Status":"locked"}})");
}

TEST(SetupLogic, FirefoxTurnOffReplacesAnOnSettingAndLeavesAnOffOne) {
    const FirefoxPolicy on = turnOffGamepads(R"({"dom.gamepad.enabled":{"Value":true},"a":1})");
    ASSERT_EQ(on.result, FirefoxPolicy::Result::Changed);
    EXPECT_EQ(on.text, R"({"a":1,"dom.gamepad.enabled":{"Value":false,"Status":"locked"}})");
    EXPECT_EQ(turnOffGamepads(R"({"dom.gamepad.enabled":{"Status":"locked","Value":false}})").result,
              FirefoxPolicy::Result::AlreadyOff);
    // Escaped key: the same name.
    EXPECT_EQ(turnOffGamepads(R"({"dom.gamepad\u002eenabled":{"Value":false}})").result, FirefoxPolicy::Result::AlreadyOff);
}

TEST(SetupLogic, FirefoxPolicyThatIsNotOneObjectIsLeftAlone) {
    for (const char* bad : {"[]", "\"text\"", "1", "{", "{\"a\":}", "{\"a\" 1}", "{\"a\":1,}", "{\"a\":1} x",
                            "{\"a\":tru}", "{\"a\":\"unterminated}", "{a:1}", "{\"a\":\"\x01\"}", "{\"a\":\"\\q\"}",
                            "{\"a\":\"\\u12\"}", "{\"a\":[1,2}", "null"}) {
        EXPECT_EQ(turnOffGamepads(bad).result, FirefoxPolicy::Result::NotOurs) << bad;
        EXPECT_EQ(undoGamepads(bad).result, FirefoxPolicy::Result::NotOurs) << bad;
    }
    std::string deep;
    for (int i = 0; i < 200; ++i) deep += "[";
    for (int i = 0; i < 200; ++i) deep += "]";
    EXPECT_EQ(turnOffGamepads("{\"a\":" + deep + "}").result, FirefoxPolicy::Result::NotOurs);   // too deep: refused
}

TEST(SetupLogic, FirefoxUndoTakesOutOnlyItsOwnSetting) {
    const FirefoxPolicy p = undoGamepads(R"({"a":1,"dom.gamepad.enabled":{"Value":false,"Status":"locked"},"b":2})");
    ASSERT_EQ(p.result, FirefoxPolicy::Result::Changed);
    EXPECT_EQ(p.text, R"({"a":1,"b":2})");
    const FirefoxPolicy last = undoGamepads(R"({"dom.gamepad.enabled":{"Value":false,"Status":"locked"}})");
    ASSERT_EQ(last.result, FirefoxPolicy::Result::Changed);
    EXPECT_TRUE(last.text.empty());   // nothing left: the value goes
    // Turned on again by the user since: not WheelBri's to change.
    EXPECT_EQ(undoGamepads(R"({"dom.gamepad.enabled":{"Value":true}})").result, FirefoxPolicy::Result::AlreadyOff);
    EXPECT_EQ(undoGamepads(R"({"a":1})").result, FirefoxPolicy::Result::AlreadyOff);
    EXPECT_EQ(undoGamepads("").result, FirefoxPolicy::Result::NotOurs);
}

TEST(SetupLogic, FirefoxTurnOffThenUndoGivesBackThePolicy) {
    const std::string before = R"({"x":{"Value":"y"}})";
    const FirefoxPolicy off = turnOffGamepads(before);
    ASSERT_EQ(off.result, FirefoxPolicy::Result::Changed);
    const FirefoxPolicy back = undoGamepads(off.text);
    ASSERT_EQ(back.result, FirefoxPolicy::Result::Changed);
    EXPECT_EQ(back.text, before);
}

TEST(SetupLogic, ShimGamesOnePerFolderInFileOrder) {
    const auto games = readShimGames("\xEF\xBB\xBF" "game\tACC\tD:\\Games\\ACC\\\r\n"
                                     "game\tNFS\tD:\\Games\\NFS\r\n"
                                     "game\tACC again\td:\\games\\acc\r\n"
                                     "other\tx\tD:\\x\r\n"
                                     "game\tno folder\t\r\n"
                                     "game\ttoo\tmany\tparts\r\n"
                                     "game\tDiRT \xC3\xA9\tE:\\DiRT");
    ASSERT_EQ(games.size(), 3U);
    EXPECT_EQ(games[0].name, L"ACC");
    EXPECT_EQ(games[0].folder, L"D:\\Games\\ACC");
    EXPECT_EQ(games[1].name, L"NFS");
    EXPECT_EQ(games[2].name, L"DiRT \u00e9");
    EXPECT_TRUE(readShimGames("").empty());
}

TEST(SetupLogic, ShimFilesAreToldApart) {
    EXPECT_TRUE(isOurShim(std::string("MZ\0\0wheelbri-shim", 17)));
    EXPECT_FALSE(isOurShim("MZ other mod"));
    EXPECT_TRUE(isAsiLoader("MZ IsUltimateASILoader"));
    EXPECT_FALSE(isAsiLoader("MZ IsUltimateASILoader IsWheelBriShim"));
    EXPECT_FALSE(isAsiLoader("MZ"));
}

TEST(SetupLogic, OptionsOfTheInstall) {
    std::wstring problem;
    const Options o = parseOptions({L"--quiet", L"--install-dir", L"D:\\Wheel Bri", L"--start-menu-folder",
                                    L"Games\\WheelBri", L"--no-desktop-shortcut", L"--stop-browser-gamepads",
                                    L"--restart", L"now", L"--user-data", L"C:\\Users\\me\\AppData\\Local"},
                                   problem);
    EXPECT_TRUE(problem.empty());
    EXPECT_TRUE(o.quiet);
    EXPECT_FALSE(o.uninstall);
    EXPECT_EQ(o.installDir, L"D:\\Wheel Bri");
    EXPECT_EQ(o.startMenuFolder, L"Games\\WheelBri");
    EXPECT_FALSE(o.desktopShortcut);
    EXPECT_TRUE(o.stopBrowserGamepads);
    EXPECT_TRUE(o.restartNow);
    EXPECT_EQ(o.userData, L"C:\\Users\\me\\AppData\\Local");
    const Options defaults = parseOptions({}, problem);
    EXPECT_TRUE(problem.empty());
    EXPECT_EQ(defaults.startMenuFolder, L"WheelBri");
    EXPECT_TRUE(defaults.desktopShortcut);
    EXPECT_FALSE(defaults.restartNow);
}

TEST(SetupLogic, OptionsThatAreWrongAreRefused) {
    for (const std::vector<std::wstring>& bad : std::vector<std::vector<std::wstring>>{
             {L"--bogus"}, {L"-Quiet"}, {L"--install-dir"}, {L"--install-dir", L""}, {L"--restart", L"soon"},
             {L"--restart"}, {L"--start-menu-folder", L"a:b"}, {L"--start-menu-folder", L".."},
             {L"--uninstall", L"--keep-config", L"--remove-settings"}, {L"quiet"}}) {
        std::wstring problem;
        (void)parseOptions(bad, problem);
        EXPECT_FALSE(problem.empty()) << std::string(toUtf8(bad[0]));
    }
}

TEST(SetupLogic, OptionsOfTheUninstall) {
    std::wstring problem;
    const Options o = parseOptions({L"--uninstall", L"--quiet", L"--remove-settings", L"--remove-logs", L"--keep-shims",
                                    L"--remove-certificate", L"--temp-copy"},
                                   problem);
    EXPECT_TRUE(problem.empty());
    EXPECT_TRUE(o.uninstall && o.quiet && o.removeSettings && o.removeLogs && o.keepShims && o.removeCertificate &&
                o.tempCopy);
    EXPECT_FALSE(o.keepConfig);
}

TEST(SetupLogic, OptionsPassedOnReadBackTheSame) {
    std::wstring problem;
    Options o;
    o.quiet = true;
    o.installDir = L"D:\\";   // a trailing backslash must not escape the quote
    o.userData = L"C:\\Users\\Gr\u00e9 g\\AppData\\Local";
    o.startMenuFolder = L"Games\\Wheel Bri";
    o.desktopShortcut = false;
    o.stopBrowserGamepads = true;
    o.restartNow = true;
    const std::wstring line = optionsCommandLine(o);
    // Split as CommandLineToArgvW would: quotes group, a doubled backslash before a quote is one.
    std::vector<std::wstring> arguments;
    std::wstring current;
    bool inQuotes = false, any = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const wchar_t c = line[i];
        if (c == L'\\' && i + 2 < line.size() && line[i + 1] == L'\\' && line[i + 2] == L'"') {
            current += L'\\';
            ++i;
        } else if (c == L'"') {
            inQuotes = !inQuotes;
            any = true;
        } else if (c == L' ' && !inQuotes) {
            if (any || !current.empty()) arguments.push_back(current);
            current.clear();
            any = false;
        } else {
            current += c;
        }
    }
    if (any || !current.empty()) arguments.push_back(current);
    const Options back = parseOptions(arguments, problem);
    EXPECT_TRUE(problem.empty()) << std::string(toUtf8(problem));
    EXPECT_EQ(back.installDir, o.installDir);
    EXPECT_EQ(back.userData, o.userData);
    EXPECT_EQ(back.startMenuFolder, o.startMenuFolder);
    EXPECT_EQ(back.quiet, o.quiet);
    EXPECT_EQ(back.desktopShortcut, o.desktopShortcut);
    EXPECT_EQ(back.stopBrowserGamepads, o.stopBrowserGamepads);
    EXPECT_EQ(back.restartNow, o.restartNow);
}

TEST(SetupLogic, Sha256MatchesTheStandardsVectors) {
    EXPECT_EQ(sha256Hex(""), L"E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855");
    EXPECT_EQ(sha256Hex("abc"), L"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
    // 56 bytes: the length no longer fits the first block.
    EXPECT_EQ(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              L"248D6A61D20638B8E5C026930C3E6039A33CE45964FF2167F6ECEDD419DB06C1");
    EXPECT_EQ(sha256Hex(std::string(1000000, 'a')), L"CDC76E5C9914FB9281A1C7E284D73E67F1809A48A497200E046D39CCC7112CD0");
    EXPECT_EQ(sha256Hex(std::string(64, 'a')), L"FFE054FE7AE0CB6DC65C3AF9B61D5209F439851DB43D0BA5997337DF154668EB");
}

}  // namespace
}  // namespace wheelbri::setup
