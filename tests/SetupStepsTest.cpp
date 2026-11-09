// SPDX-License-Identifier: GPL-3.0-or-later
// The install and the uninstall (installer/setup/SetupSteps.cpp) on a Windows in memory (SetupFakes.hpp): what they
// put where, what they leave alone, and how they stop when something is wrong.

#include "SetupFakes.hpp"

#include <gtest/gtest.h>

namespace wheelbri::setup {
namespace {

using testing::FakeReporter;
using testing::FakeSystem;
using testing::FakeValue;

const std::wstring kPackage = L"C:\\Users\\me\\Downloads\\WheelBri-1.1.0";
const std::wstring kDir = L"C:\\Program Files\\WheelBri";
const std::wstring kLocal = L"C:\\Users\\me\\AppData\\Local";
const std::wstring kData = L"C:\\Users\\me\\AppData\\Local\\wheelbri";
const std::wstring kUninstallKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\wheelbri";
const std::wstring kPrograms = L"C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs";
const std::wstring kDesktop = L"C:\\Users\\Public\\Desktop";

std::string defaultConfig(const std::wstring& name) { return "[target]\nname = " + toUtf8(name) + "\n"; }

std::vector<std::wstring> manifestOf(FakeSystem& s) { return textLines(s.bytes(kDir + L"\\installed-files.txt")); }

bool listed(FakeSystem& s, const std::wstring& entry) {
    const auto lines = manifestOf(s);
    return std::any_of(lines.begin(), lines.end(), [&](const std::wstring& l) { return sameName(l, entry); });
}

class SetupStepsTest : public ::testing::Test {
protected:
    FakeSystem s;
    FakeReporter r;
    Options o;
    Package p;

    void SetUp() override {
        const std::vector<std::pair<std::wstring, std::string>> files = {
            {L"wheelbri.exe", "app 1.1"}, {L"wheelbri-service.exe", "service 1.1"}, {L"wheelbri-helper.exe", "helper"},
            {L"wheelbri-setup.exe", "setup 1.1"}, {L"VERSION.txt", "1.1.0\r\n"}, {L"README.md", "readme"},
            {L"docs\\user-guide.md", "guide"},
            {L"config\\wheelbri-shim.ini", defaultConfig(L"shim")},
            {L"driver\\common.ps1", "#"}, {L"driver\\installer-steps.ps1", "#"}, {L"driver\\install-driver.ps1", "#"},
            {L"driver\\uninstall-driver.ps1", "#"}, {L"driver\\hide\\install-hide.ps1", "#"},
            {L"driver\\hide\\uninstall-hide.ps1", "#"}, {L"driver\\signing-certificate.txt", "AB"},
            {L"shim\\dinput8.dll", "MZ wheelbri shim 1.1"}, {L"shim\\x86\\dinput8.dll", "MZ wheelbri shim x86"},
            {L"ucrt\\Windows6.1-KB2999226-x64.msu", "msu7"}, {L"ucrt\\Windows8-RT-KB2999226-x64.msu", "msu8"},
            {L"ucrt\\Windows8.1-KB2999226-x64.msu", "msu81"},
        };
        for (const auto& [name, bytes] : files) s.addFile(joinPath(kPackage, name), bytes);
        for (const wchar_t* t : {L"g25", L"g27", L"g920", L"g923"}) {
            s.addFile(kPackage + L"\\config\\" + t + L".ini", defaultConfig(t));
        }
        for (const wchar_t* d : {L"driver\\x64\\Release\\wheelbri-wheel\\wheelbri-wheel",
                                 L"driver\\hide\\x64\\Release\\wheelbri-hide\\wheelbri-hide"}) {
            for (const wchar_t* e : {L".inf", L".cat", L".dll"}) s.addFile(kPackage + L"\\" + d + e, "driver");
        }
        s.output[L"pnputil.exe"] = {L"Published Name: oem1.inf", L"Original Name: wheelbri-wheel.inf",
                                    L"Original Name: wheelbri-hide.inf"};
        readThePackage();
        o.installDir = kDir;
        o.userData = kLocal;
    }

    void readThePackage() { p = readPackage(s, kPackage, kPackage + L"\\wheelbri-setup.exe"); }
    int runInstall() { return install(s, p, o, r); }
    int runUninstall() {
        Options u = o;
        u.uninstall = true;
        FakeReporter fresh;
        r = fresh;
        return uninstall(s, u, r);
    }
    Options& uninstallOptions() {
        o.uninstall = true;
        return o;
    }
};

// --- the package ----------------------------------------------------------------------------------------------------

TEST_F(SetupStepsTest, PackageFromAFolderHasEveryFileAndItsVersion) {
    EXPECT_EQ(p.version, L"1.1.0");
    EXPECT_NE(p.find(L"CONFIG\\G920.INI"), nullptr);
    EXPECT_NE(p.find(L"driver\\hide\\x64\\Release\\wheelbri-hide\\wheelbri-hide.dll"), nullptr);
    EXPECT_EQ(p.find(L"nothing.txt"), nullptr);
}

TEST_F(SetupStepsTest, PackageFromSetupExesFlatFilesFollowsItsList) {
    const std::wstring flat = L"C:\\Users\\me\\AppData\\Local\\Temp\\IXP000.TMP";
    s.addFile(flat + L"\\f000", "app");
    s.addFile(flat + L"\\f001", "cfg");
    s.addFile(flat + L"\\f002", "1.2.0");
    s.addLink(flat + L"\\f003", false);
    s.addFile(flat + L"\\package-files.txt", "f000\twheelbri.exe\r\nf001\tconfig\\g920.ini\r\nf002\tVERSION.txt\r\n"
                                             "f003\tlinked.dll\r\nf009\tmissing.dll\r\nf000\twheelbri.exe\r\n");
    const Package flatPackage = readPackage(s, flat, flat + L"\\wheelbri-setup.exe");
    ASSERT_EQ(flatPackage.files.size(), 3U);   // no link, no missing file, no second wheelbri.exe
    EXPECT_EQ(flatPackage.find(L"config\\g920.ini")->source, flat + L"\\f001");
    EXPECT_EQ(flatPackage.version, L"1.2.0");
}

TEST_F(SetupStepsTest, PackageVersionThatIsNoVersionIsIgnored) {
    s.addFile(kPackage + L"\\VERSION.txt", std::string(100, '9'));
    readThePackage();
    EXPECT_EQ(p.version, L"0.0.0");
}

// --- install: a new one -----------------------------------------------------------------------------------------------

TEST_F(SetupStepsTest, InstallOnWindows11PutsEverythingInPlace) {
    ASSERT_EQ(runInstall(), 0) << (r.failures.empty() ? "" : toUtf8(r.failures[0]));
    EXPECT_TRUE(r.failures.empty());
    EXPECT_EQ(r.steps, (std::vector<int>{1, 2, 3, 4, 5, 6, 7}));
    EXPECT_EQ(s.bytes(kDir + L"\\wheelbri.exe"), "app 1.1");
    EXPECT_EQ(s.bytes(kDir + L"\\Uninstaller.exe"), "setup 1.1");
    EXPECT_EQ(s.bytes(kDir + L"\\driver\\hide\\x64\\Release\\wheelbri-hide\\wheelbri-hide.inf"), "driver");
    // The installer and the updates it brings stay in the package.
    EXPECT_FALSE(s.has(kDir + L"\\wheelbri-setup.exe"));
    EXPECT_FALSE(s.has(kDir + L"\\ucrt"));
    // The list: every file put there, sorted, not itself.
    const auto lines = manifestOf(s);
    EXPECT_TRUE(std::is_sorted(lines.begin(), lines.end(), NameLess{}));
    EXPECT_TRUE(listed(s, L"Uninstaller.exe"));
    EXPECT_TRUE(listed(s, L"default-configs.txt"));
    EXPECT_TRUE(listed(s, L"config\\g920.ini"));
    EXPECT_FALSE(listed(s, L"installed-files.txt"));
    EXPECT_FALSE(listed(s, L"wheelbri-setup.exe"));
    EXPECT_EQ(lines.size(), s.filesUnder(kDir).size() - 1);
    // The defaults' hashes, for the uninstaller.
    const ConfigHashes defaults = readDefaultConfigs(s.bytes(kDir + L"\\default-configs.txt"));
    EXPECT_EQ(defaults.size(), 5U);
    EXPECT_EQ(defaults.at(L"config\\g920.ini"), sha256Hex(defaultConfig(L"g920")));
    // The drivers, the old certificates, the helper and the filter on the saved rig, in that order.
    const size_t wheel = s.ranAt(L"install-driver.ps1"), hide = s.ranAt(L"install-hide.ps1");
    const size_t certificates = s.ranAt(L"-RemoveUnusedCertificates"), prepare = s.ranAt(L"--prepare-hide");
    EXPECT_LT(wheel, hide);
    EXPECT_LT(hide, certificates);
    EXPECT_LT(certificates, prepare);
    EXPECT_LT(prepare, s.commands.size());
    EXPECT_EQ(s.helperExe, kDir + L"\\wheelbri-helper.exe");
    EXPECT_EQ(r.last("wheel-driver")->state, ItemState::Done);
    EXPECT_EQ(r.last("hide-driver")->state, ItemState::Done);
    EXPECT_EQ(r.last("helper")->state, ItemState::Done);
    // Inside Program Files: its permissions as they are.
    EXPECT_FALSE(s.ran(L"/inheritance:r"));
    EXPECT_TRUE(r.warnings.empty());
}

TEST_F(SetupStepsTest, InstallWritesTheSettingsAppsEntry) {
    ASSERT_EQ(runInstall(), 0);
    EXPECT_EQ(*s.regText(Hive::Machine, kUninstallKey, L"DisplayVersion"), L"1.1.0");
    EXPECT_EQ(*s.regText(Hive::Machine, kUninstallKey, L"InstallLocation"), kDir);
    EXPECT_EQ(*s.regText(Hive::Machine, kUninstallKey, L"UninstallString"), L"\"" + kDir + L"\\Uninstaller.exe\" --uninstall");
    EXPECT_EQ(*s.regText(Hive::Machine, kUninstallKey, L"QuietUninstallString"),
              L"\"" + kDir + L"\\Uninstaller.exe\" --uninstall --quiet");
    EXPECT_EQ(*s.regText(Hive::Machine, kUninstallKey, L"InstallDate"), L"20261109");
    EXPECT_EQ(*s.regText(Hive::Machine, kUninstallKey, L"StartMenuFolder"), L"WheelBri");
    EXPECT_EQ(s.value(Hive::Machine, kUninstallKey, L"NoModify")->dword, 1U);
    EXPECT_LE(s.value(Hive::Machine, kUninstallKey, L"EstimatedSize")->dword, 2U);   // in KB: the fake's few bytes
    EXPECT_EQ(*s.regText(Hive::Machine, L"SOFTWARE\\wheelbri", L"LogFolder"), kData + L"\\logs");
    EXPECT_TRUE(s.ran(L"/grant *S-1-5-19:(OI)(CI)M"));
}

TEST_F(SetupStepsTest, InstallMakesTheShortcuts) {
    ASSERT_EQ(runInstall(), 0);
    const auto& app = s.shortcuts.at(testing::lowered(kPrograms + L"\\WheelBri\\WheelBri.lnk"));
    EXPECT_EQ(app.target, kDir + L"\\wheelbri.exe");
    EXPECT_EQ(app.workFolder, kDir);
    const auto& un = s.shortcuts.at(testing::lowered(kPrograms + L"\\WheelBri\\Uninstaller.lnk"));
    EXPECT_EQ(un.target, kDir + L"\\Uninstaller.exe");
    EXPECT_EQ(un.arguments, L"--uninstall");
    EXPECT_EQ(s.shortcuts.at(testing::lowered(kPrograms + L"\\WheelBri\\WheelBri logs.lnk")).arguments,
              L"\"shell:Local AppData\\wheelbri\\logs\"");
    EXPECT_TRUE(s.shortcuts.count(testing::lowered(kDesktop + L"\\WheelBri.lnk")));
}

TEST_F(SetupStepsTest, InstallWithoutDesktopShortcutRemovesAnEarlierOneAndUsesTheNamedMenuFolder) {
    s.addFile(kDesktop + L"\\WheelBri.lnk", "old");
    s.addFile(kPrograms + L"\\Old Name\\WheelBri.lnk", "old");
    s.regSetText(Hive::Machine, kUninstallKey, L"StartMenuFolder", L"Old Name");
    o.desktopShortcut = false;
    o.startMenuFolder = L"Games\\Racing";
    ASSERT_EQ(runInstall(), 0);
    EXPECT_FALSE(s.has(kDesktop + L"\\WheelBri.lnk"));
    EXPECT_FALSE(s.has(kPrograms + L"\\Old Name"));
    EXPECT_TRUE(s.has(kPrograms + L"\\Games\\Racing\\WheelBri.lnk"));
}

TEST_F(SetupStepsTest, InstallNeverRemovesAMenuFolderOutsideTheStartMenu) {
    s.addFile(L"C:\\Users\\me\\Documents\\keep.txt", "mine");
    for (const wchar_t* recorded : {L"..\\..\\..\\..\\Users\\me\\Documents", L"C:\\Users\\me\\Documents", L"\\Users"}) {
        s.regSetText(Hive::Machine, kUninstallKey, L"StartMenuFolder", recorded);
        ASSERT_EQ(runInstall(), 0);
        EXPECT_TRUE(s.has(L"C:\\Users\\me\\Documents\\keep.txt")) << toUtf8(recorded);
    }
}

TEST_F(SetupStepsTest, InstallOutsideProgramFilesLocksTheFolderDown) {
    o.installDir = L"D:\\WheelBri";
    ASSERT_EQ(runInstall(), 0);
    EXPECT_TRUE(s.ran(L"\"D:\\WheelBri\" /inheritance:r"));
}

TEST_F(SetupStepsTest, InstallStopsWhenTheFolderCannotBeLockedDown) {
    o.installDir = L"D:\\WheelBri";
    s.exitCodes[L"/inheritance:r"] = 5;
    EXPECT_EQ(runInstall(), 1);
    ASSERT_EQ(r.failures.size(), 1U);
    EXPECT_NE(r.failures[0].find(L"could not protect"), std::wstring::npos);
    EXPECT_FALSE(s.has(L"D:\\WheelBri\\wheelbri.exe"));
}

// --- install: Windows 7 to 10 ----------------------------------------------------------------------------------------

TEST_F(SetupStepsTest, InstallOnWindows7InstallsTheUniversalCRuntimeFirstAndNoDrivers) {
    s.build = 7601;
    s.removeFile(L"C:\\Windows\\System32\\ucrtbase.dll");
    s.onRun = [&](const std::wstring& command) {
        if (command.find(L"wusa.exe") != std::wstring::npos) s.addFile(L"C:\\Windows\\System32\\ucrtbase.dll", "ucrt");
    };
    ASSERT_EQ(runInstall(), 0) << (r.failures.empty() ? "" : toUtf8(r.failures[0]));
    EXPECT_TRUE(s.ran(L"\"" + kPackage + L"\\ucrt\\Windows6.1-KB2999226-x64.msu\" /quiet /norestart"));
    EXPECT_LT(s.ranAt(L"wusa.exe"), s.commands.size());
    EXPECT_FALSE(s.ran(L"powershell.exe"));   // nothing needs PowerShell before Windows 11
    EXPECT_TRUE(s.helperExe.empty());
    EXPECT_EQ(r.last("wheel-driver"), nullptr);
    EXPECT_TRUE(s.has(kDir + L"\\shim\\x86\\dinput8.dll"));
    EXPECT_FALSE(r.restart);
}

TEST_F(SetupStepsTest, InstallPicksTheUpdateForEachWindows) {
    const std::vector<std::pair<std::uint32_t, std::wstring>> cases = {
        {7601, L"Windows6.1-"}, {9200, L"Windows8-RT-"}, {9600, L"Windows8.1-"}};
    for (const auto& [build, msu] : cases) {
        FakeSystem fresh;
        s = fresh;
        SetUp();
        s.build = build;
        s.removeFile(L"C:\\Windows\\System32\\ucrtbase.dll");
        s.onRun = [&](const std::wstring& c) {
            if (c.find(L"wusa.exe") != std::wstring::npos) s.addFile(L"C:\\Windows\\System32\\ucrtbase.dll", "u");
        };
        EXPECT_EQ(runInstall(), 0) << build;
        EXPECT_TRUE(s.ran(msu + L"KB2999226-x64.msu")) << build;
    }
}

TEST_F(SetupStepsTest, InstallLeavesTheUniversalCRuntimeAloneWhenItIsThere) {
    s.build = 9600;
    ASSERT_EQ(runInstall(), 0);
    EXPECT_FALSE(s.ran(L"wusa.exe"));
}

TEST_F(SetupStepsTest, InstallOnWindows81WithoutItsAprilUpdateSaysWhatToInstall) {
    s.build = 9600;
    s.removeFile(L"C:\\Windows\\System32\\ucrtbase.dll");
    s.exitCodes[L"wusa.exe"] = static_cast<int>(0x80240017);
    EXPECT_EQ(runInstall(), 1);
    ASSERT_EQ(r.failures.size(), 1U);
    EXPECT_NE(r.failures[0].find(L"KB2919355"), std::wstring::npos);
    EXPECT_FALSE(s.has(kDir));   // nothing put on the PC
}

TEST_F(SetupStepsTest, InstallGoesOnWhenTheUpdateWantsARestartAndSaysSo) {
    s.build = 9200;
    s.removeFile(L"C:\\Windows\\System32\\ucrtbase.dll");
    s.exitCodes[L"wusa.exe"] = 3010;
    ASSERT_EQ(runInstall(), 0);
    EXPECT_TRUE(r.restart);
    EXPECT_TRUE(r.warned(L"restart Windows"));
}

TEST_F(SetupStepsTest, InstallStopsWhenTheUpdateFailsOrLeavesNoRuntime) {
    for (const int code : {1603, 0}) {
        FakeSystem fresh;
        s = fresh;
        r = FakeReporter{};
        SetUp();
        s.build = 7601;
        s.removeFile(L"C:\\Windows\\System32\\ucrtbase.dll");
        s.exitCodes[L"wusa.exe"] = code;   // 0 but no ucrtbase.dll after it
        EXPECT_EQ(runInstall(), 1) << code;
        EXPECT_EQ(r.failures.size(), 1U) << code;
        EXPECT_FALSE(s.has(kDir)) << code;
    }
}

TEST_F(SetupStepsTest, InstallOnWindows10WithoutTheRuntimeHasNoUpdateToOffer) {
    s.build = 19045;
    s.removeFile(L"C:\\Windows\\System32\\ucrtbase.dll");
    EXPECT_EQ(runInstall(), 1);
    EXPECT_FALSE(s.ran(L"wusa.exe"));
    EXPECT_NE(r.failures[0].find(L"Windows Update"), std::wstring::npos);
}

TEST_F(SetupStepsTest, InstallRefusesOldOr32BitWindows) {
    s.build = 7600;
    EXPECT_EQ(runInstall(), 1);
    FakeSystem fresh;
    s = fresh;
    SetUp();
    s.sixtyFourBit = false;
    EXPECT_EQ(runInstall(), 1);
    EXPECT_FALSE(s.has(kDir));
}

// --- install: the folder ---------------------------------------------------------------------------------------------

TEST_F(SetupStepsTest, InstallRefusesAFolderThatIsNotEmptyAndNoInstall) {
    s.addFile(kDir + L"\\my notes.txt", "mine");
    EXPECT_EQ(runInstall(), 1);
    EXPECT_FALSE(s.has(kDir + L"\\wheelbri.exe"));
    EXPECT_EQ(s.bytes(kDir + L"\\my notes.txt"), "mine");
    EXPECT_EQ(s.helperStops, 0);   // stopped before anything changed
}

TEST_F(SetupStepsTest, InstallTakesAnEmptyFolder) {
    s.makeFolders(kDir);
    EXPECT_EQ(runInstall(), 0);
}

TEST_F(SetupStepsTest, InstallRefusesFoldersThatAreNotItsOwn) {
    for (const std::wstring& folder : {std::wstring(L"C:\\Windows\\WheelBri"), std::wstring(L"C:\\Windows"),
                                       std::wstring(L"C:\\"), std::wstring(L"WheelBri"), std::wstring(L"\\\\server\\x"),
                                       kPackage}) {
        r = FakeReporter{};
        o.installDir = folder;
        EXPECT_EQ(runInstall(), 1) << toUtf8(folder);
    }
    s.addFile(L"D:\\file", "x");
    o.installDir = L"D:\\file";
    EXPECT_EQ(runInstall(), 1);
}

TEST_F(SetupStepsTest, InstallRefusesAFolderThatIsALink) {
    s.addLink(kDir, true);
    EXPECT_EQ(runInstall(), 1);
    EXPECT_NE(r.failures[0].find(L"link"), std::wstring::npos);
}

TEST_F(SetupStepsTest, InstallRefusesToWriteThroughALinkInsideTheFolder) {
    s.addFile(kDir + L"\\installed-files.txt", "\xEF\xBB\xBFwheelbri.exe\r\n");
    s.addLink(kDir + L"\\config", true);
    EXPECT_EQ(runInstall(), 1);
    EXPECT_NE(r.failures[0].find(L"link"), std::wstring::npos);
}

TEST_F(SetupStepsTest, InstallStopsWhenThePackageIsIncomplete) {
    for (const wchar_t* missing : {L"config\\g923.ini", L"shim\\x86\\dinput8.dll", L"driver\\installer-steps.ps1",
                                   L"driver\\hide\\x64\\Release\\wheelbri-hide\\wheelbri-hide.cat"}) {
        FakeSystem fresh;
        s = fresh;
        r = FakeReporter{};
        SetUp();
        s.removeFile(kPackage + L"\\" + missing);
        readThePackage();
        EXPECT_EQ(runInstall(), 1) << toUtf8(missing);
        ASSERT_EQ(r.failures.size(), 1U);
        EXPECT_NE(r.failures[0].find(missing), std::wstring::npos);
        EXPECT_FALSE(s.has(kDir));
    }
}

TEST_F(SetupStepsTest, InstallOnWindows10DoesNotNeedTheDriverFiles) {
    s.build = 19045;
    s.removeFile(kPackage + L"\\driver\\hide\\x64\\Release\\wheelbri-hide\\wheelbri-hide.cat");
    readThePackage();
    EXPECT_EQ(runInstall(), 0);
}

TEST_F(SetupStepsTest, InstallStopsWhenAFileIsInUseAndStartsTheEarlierHelperAgain) {
    s.helper = s.helperIsRunning = true;
    s.failCopyTo.insert(testing::lowered(kDir + L"\\wheelbri-helper.exe"));
    EXPECT_EQ(runInstall(), 1);
    EXPECT_NE(r.failures[0].find(L"wheelbri-helper.exe"), std::wstring::npos);
    EXPECT_EQ(s.helperStops, 1);
    EXPECT_EQ(s.helperStarts, 1);
}

TEST_F(SetupStepsTest, InstallStopsWhenTheListCannotBeWritten) {
    s.failWrite.insert(testing::lowered(kDir + L"\\installed-files.txt"));
    EXPECT_EQ(runInstall(), 1);
    EXPECT_NE(r.failures[0].find(L"installed-files.txt"), std::wstring::npos);
}

// --- install: upgrades -----------------------------------------------------------------------------------------------

void addEarlierInstall(FakeSystem& s, const std::wstring& dir) {
    s.addFile(dir + L"\\wheelbri.exe", "app 1.0");
    s.addFile(dir + L"\\uninstall.ps1", "#");
    s.addFile(dir + L"\\Uninstall.cmd", "@");
    s.addFile(dir + L"\\wheelbri-setup.exe", "old setup");
    s.addFile(dir + L"\\old-only.dll", "gone in 1.1");
    s.addFile(dir + L"\\config\\g920.ini", "[mine]\n");                    // edited
    s.addFile(dir + L"\\config\\g25.ini", defaultConfig(L"g25"));          // as shipped
    s.addFile(dir + L"\\config\\g25.ini.new", "stale");
    s.addFile(dir + L"\\config\\rig-as-g920.ini", "older name");
    s.addFile(dir + L"\\config\\rig-as-g920.ini.new", "older name");
    s.addFile(dir + L"\\installed-files.txt",
              linesFile({L"wheelbri.exe", L"uninstall.ps1", L"Uninstall.cmd", L"wheelbri-setup.exe", L"old-only.dll",
                         L"config\\g920.ini", L"config\\g25.ini", L"config\\g25.ini.new", L"config\\rig-as-g920.ini"}));
    s.regSetText(Hive::Machine, kUninstallKey, L"InstallLocation", dir);
    s.regSetText(Hive::Machine, kUninstallKey, L"DisplayVersion", L"1.0.2");
}

TEST_F(SetupStepsTest, UpgradeKeepsEditedConfigsAndDropsWhatIsNoLongerShipped) {
    addEarlierInstall(s, kDir);
    ASSERT_EQ(runInstall(), 0) << (r.failures.empty() ? "" : toUtf8(r.failures[0]));
    EXPECT_EQ(s.bytes(kDir + L"\\wheelbri.exe"), "app 1.1");
    EXPECT_EQ(s.bytes(kDir + L"\\config\\g920.ini"), "[mine]\n");
    EXPECT_EQ(s.bytes(kDir + L"\\config\\g920.ini.new"), defaultConfig(L"g920"));
    EXPECT_TRUE(listed(s, L"config\\g920.ini.new"));
    EXPECT_FALSE(s.has(kDir + L"\\config\\g25.ini.new"));   // the default again: nothing new to offer
    EXPECT_FALSE(listed(s, L"config\\g25.ini.new"));
    for (const wchar_t* gone : {L"uninstall.ps1", L"Uninstall.cmd", L"wheelbri-setup.exe", L"config\\rig-as-g920.ini",
                                L"config\\rig-as-g920.ini.new"}) {
        EXPECT_FALSE(s.has(kDir + L"\\" + gone)) << toUtf8(gone);
        EXPECT_FALSE(listed(s, gone)) << toUtf8(gone);
    }
    // An older version's file stays listed while it is there, so the uninstaller still removes it.
    EXPECT_TRUE(listed(s, L"old-only.dll"));
    EXPECT_EQ(*s.regText(Hive::Machine, kUninstallKey, L"DisplayVersion"), L"1.1.0");
}

TEST_F(SetupStepsTest, UpgradeOfAnInstallFromBeforeTheListCountsAllOfItsFiles) {
    s.addFile(kDir + L"\\wheelbri.exe", "app 0.9");
    s.addFile(kDir + L"\\extra\\thing.dat", "x");
    s.regSetText(Hive::Machine, kUninstallKey, L"InstallLocation", kDir + L"\\");
    ASSERT_EQ(runInstall(), 0);
    EXPECT_TRUE(listed(s, L"extra\\thing.dat"));
}

TEST_F(SetupStepsTest, UpgradeFromAnotherFolderCarriesTheSettingsAndRemovesIt) {
    const std::wstring old = L"D:\\Old WheelBri";
    addEarlierInstall(s, old);
    s.addFile(old + L"\\shim\\wheelbri-shim.ini", "[shim mine]\n");
    s.addFile(old + L"\\config\\custom.ini", "[custom]\n");
    s.addFile(old + L"\\not ours.txt", "mine");   // not in its list: stays
    ASSERT_EQ(runInstall(), 0);
    EXPECT_EQ(s.bytes(kDir + L"\\config\\g920.ini"), "[mine]\n");
    EXPECT_EQ(s.bytes(kDir + L"\\config\\custom.ini"), "[custom]\n");
    EXPECT_TRUE(listed(s, L"config\\custom.ini"));
    EXPECT_FALSE(s.has(old + L"\\wheelbri.exe"));
    EXPECT_FALSE(s.has(old + L"\\config"));
    EXPECT_EQ(s.bytes(old + L"\\not ours.txt"), "mine");
    EXPECT_TRUE(r.warned(old));   // could not be removed completely
}

TEST_F(SetupStepsTest, UpgradeMovesTheShimSettingsToConfig) {
    addEarlierInstall(s, kDir);
    s.addFile(kDir + L"\\shim\\wheelbri-shim.ini", "[shim mine]\n");
    ASSERT_EQ(runInstall(), 0);
    EXPECT_EQ(s.bytes(kDir + L"\\config\\wheelbri-shim.ini"), "[shim mine]\n");
    EXPECT_FALSE(s.has(kDir + L"\\shim\\wheelbri-shim.ini"));
    EXPECT_TRUE(s.has(kDir + L"\\config\\wheelbri-shim.ini.new"));
}

TEST_F(SetupStepsTest, UpgradeClosesTheRunningAppAndClearsWhatAnEndedServiceLeft) {
    addEarlierInstall(s, kDir);
    s.addFile(kDir + L"\\wheelbri-service.exe", "service 1.0");
    s.runningPrograms = 2;
    s.endedPrograms = {L"wheelbri-service"};
    ASSERT_EQ(runInstall(), 0);
    EXPECT_TRUE(s.ran(L"wheelbri-service.exe\" --unhide"));
    EXPECT_LT(s.ranAt(L"--unhide"), s.ranAt(L"install-driver.ps1"));
    EXPECT_TRUE(r.warned(L"wheelbri-service did not close"));
}

// --- install: drivers and helper -------------------------------------------------------------------------------------

TEST_F(SetupStepsTest, InstallStopsWhenTheWheelDriverFailsAndRestartsTheEarlierHelper) {
    s.helper = s.helperIsRunning = true;
    s.exitCodes[L"install-driver.ps1"] = 1;
    EXPECT_EQ(runInstall(), 1);
    EXPECT_EQ(r.last("wheel-driver")->state, ItemState::Failed);
    EXPECT_FALSE(s.ran(L"install-hide.ps1"));
    EXPECT_EQ(s.helperStarts, 1);
}

TEST_F(SetupStepsTest, InstallGoesOnWithoutTheHidingFilter) {
    s.exitCodes[L"install-hide.ps1"] = 1;
    ASSERT_EQ(runInstall(), 0);
    EXPECT_EQ(r.last("hide-driver")->state, ItemState::Warn);
    EXPECT_FALSE(s.ran(L"--prepare-hide"));
    EXPECT_TRUE(r.warned(L"hiding filter"));
}

TEST_F(SetupStepsTest, InstallSaysWhenTheHidingFilterNeedsARestart) {
    s.exitCodes[L"install-hide.ps1"] = 2;
    ASSERT_EQ(runInstall(), 0);
    EXPECT_EQ(r.last("hide-driver")->state, ItemState::Warn);
    EXPECT_TRUE(r.restart);
    EXPECT_TRUE(s.ran(L"--prepare-hide"));
}

TEST_F(SetupStepsTest, InstallReadsTheFilterOnTheRigsResult) {
    for (const auto& [code, warned, restart] : std::vector<std::tuple<int, bool, bool>>{
             {0, false, false}, {2, false, true}, {3, false, false}, {1, true, false}, {-1, true, false}}) {
        FakeSystem fresh;
        s = fresh;
        r = FakeReporter{};
        SetUp();
        s.exitCodes[L"--prepare-hide"] = code;
        ASSERT_EQ(runInstall(), 0) << code;
        EXPECT_EQ(r.warned(L"first Start"), warned) << code;
        EXPECT_EQ(r.restart, restart) << code;
    }
}

TEST_F(SetupStepsTest, InstallWarnsWhenTheHelperDoesNotRunAndDoesNotStartTheOldOne) {
    s.helper = true;
    s.helperProblem = L"the service did not start within 15 s";
    ASSERT_EQ(runInstall(), 0);
    EXPECT_EQ(r.last("helper")->state, ItemState::Warn);
    EXPECT_TRUE(r.warned(L"HELPER NOT INSTALLED"));
    EXPECT_TRUE(r.warned(L"WheelBri Helper service is not running"));
}

TEST_F(SetupStepsTest, InstallWarnsWhenADriverIsNotListedAfterwards) {
    s.output[L"pnputil.exe"] = {L"Original Name: wheelbri-wheel.inf"};
    ASSERT_EQ(runInstall(), 0);
    EXPECT_TRUE(r.warned(L"wheelbri-hide.inf"));
    EXPECT_FALSE(r.warned(L"wheelbri-wheel.inf"));
}

TEST_F(SetupStepsTest, InstallWithLinkedLogsWarnsAndGrantsNothingThroughTheLink) {
    s.addLink(kData, true);
    ASSERT_EQ(runInstall(), 0);
    EXPECT_TRUE(r.warned(L"is a link"));
    EXPECT_FALSE(s.ran(L"S-1-5-19"));
    EXPECT_EQ(s.regText(Hive::Machine, L"SOFTWARE\\wheelbri", L"LogFolder"), std::nullopt);
}

// --- install: Firefox ------------------------------------------------------------------------------------------------

const std::wstring kFirefoxKey = L"S-1-5-21-1-2-3-1001\\Software\\Policies\\Mozilla\\Firefox";

TEST_F(SetupStepsTest, FirefoxIsLeftAloneUnlessAsked) {
    ASSERT_EQ(runInstall(), 0);
    EXPECT_FALSE(s.regKeyExists(Hive::Users, kFirefoxKey));
}

TEST_F(SetupStepsTest, FirefoxGamepadsTurnedOffForTheSignedInUserKeepingTheirPolicy) {
    s.setMulti(Hive::Users, kFirefoxKey, L"Preferences", L"{\"a\":{\"Value\":1}}");
    o.stopBrowserGamepads = true;
    ASSERT_EQ(runInstall(), 0);
    const FakeValue* v = s.value(Hive::Users, kFirefoxKey, L"Preferences");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->type, FakeValue::Multi);
    EXPECT_EQ(v->text, L"{\"a\":{\"Value\":1},\"dom.gamepad.enabled\":{\"Value\":false,\"Status\":\"locked\"}}");
    EXPECT_EQ(*s.regText(Hive::Machine, L"SOFTWARE\\wheelbri", L"BrowserGamepadPolicy"), s.sid);
}

TEST_F(SetupStepsTest, FirefoxPolicyInAnotherFormIsLeftAloneWithAWarning) {
    o.stopBrowserGamepads = true;
    s.addKey(Hive::Users, kFirefoxKey + L"\\Preferences");
    ASSERT_EQ(runInstall(), 0);
    EXPECT_TRUE(r.warned(L"about:config"));
    FakeSystem fresh;
    s = fresh;
    r = FakeReporter{};
    SetUp();
    o.stopBrowserGamepads = true;
    s.setMulti(Hive::Users, kFirefoxKey, L"Preferences", L"not json");
    ASSERT_EQ(runInstall(), 0);
    EXPECT_TRUE(r.warned(L"about:config"));
    EXPECT_EQ(s.value(Hive::Users, kFirefoxKey, L"Preferences")->text, L"not json");
    EXPECT_EQ(s.regText(Hive::Machine, L"SOFTWARE\\wheelbri", L"BrowserGamepadPolicy"), std::nullopt);
}

TEST_F(SetupStepsTest, FirefoxAlreadyOffIsNotRecordedAsWheelBris) {
    o.stopBrowserGamepads = true;
    s.setMulti(Hive::Users, kFirefoxKey, L"Preferences", L"{\"dom.gamepad.enabled\":{\"Value\":false}}");
    ASSERT_EQ(runInstall(), 0);
    EXPECT_EQ(s.regText(Hive::Machine, L"SOFTWARE\\wheelbri", L"BrowserGamepadPolicy"), std::nullopt);
}

// --- removing an install folder --------------------------------------------------------------------------------------

TEST_F(SetupStepsTest, RemovingAFolderDeletesOnlyItsListedFilesAndNeverThroughALink) {
    const std::wstring d = L"D:\\W";
    s.addFile(d + L"\\a.exe", "a");
    s.addFile(d + L"\\sub\\b.dll", "b");
    s.addFile(d + L"\\mine.txt", "mine");
    s.addFile(L"E:\\elsewhere\\c.dll", "c");
    s.addLink(d + L"\\linked", true);
    s.addFile(d + L"\\config\\user.ini", "config");   // WheelBri's subfolder: goes whole
    s.addFile(d + L"\\installed-files.txt",
              linesFile({L"a.exe", L"sub\\b.dll", L"linked\\c.dll", L"..\\outside.txt", L"C:\\Windows\\x.dll", L"gone.dll"}));
    s.addFile(L"D:\\outside.txt", "x");
    EXPECT_FALSE(removeInstallFolder(s, d, r));
    EXPECT_FALSE(s.has(d + L"\\a.exe"));
    EXPECT_FALSE(s.has(d + L"\\sub"));
    EXPECT_FALSE(s.has(d + L"\\config"));
    EXPECT_TRUE(s.has(d + L"\\mine.txt"));
    EXPECT_TRUE(s.has(L"D:\\outside.txt"));
    EXPECT_TRUE(s.has(L"E:\\elsewhere\\c.dll"));
    EXPECT_TRUE(s.isLink(d + L"\\linked"));
    EXPECT_FALSE(s.has(d + L"\\installed-files.txt"));   // all it names is gone
    EXPECT_TRUE(r.logged(L"reached through a link"));
}

TEST_F(SetupStepsTest, RemovingAFolderKeepsTheListWhileAListedFileStays) {
    const std::wstring d = L"D:\\W";
    s.addFile(d + L"\\a.exe", "a");
    s.addFile(d + L"\\busy.dll", "b");
    s.failRemove.insert(testing::lowered(d + L"\\busy.dll"));
    s.addFile(d + L"\\installed-files.txt", linesFile({L"a.exe", L"busy.dll"}));
    EXPECT_FALSE(removeInstallFolder(s, d, r));
    EXPECT_TRUE(s.has(d + L"\\installed-files.txt"));
    EXPECT_FALSE(s.has(d + L"\\a.exe"));
}

TEST_F(SetupStepsTest, RemovingAFolderThatIsALinkLeavesIt) {
    s.addLink(L"D:\\W", true);
    EXPECT_FALSE(removeInstallFolder(s, L"D:\\W", r));
    EXPECT_TRUE(s.isLink(L"D:\\W"));
    EXPECT_TRUE(removeInstallFolder(s, L"D:\\Never", r));
}

// --- uninstall -------------------------------------------------------------------------------------------------------

class UninstallTest : public SetupStepsTest {
protected:
    void SetUp() override {
        SetupStepsTest::SetUp();
        o.stopBrowserGamepads = true;
        s.setMulti(Hive::Users, kFirefoxKey, L"Preferences", L"{\"x\":1}");
        ASSERT_EQ(runInstall(), 0);
        s.commands.clear();
        s.addFile(kData + L"\\rigs\\rig.ini", "rig");
        s.addFile(kData + L"\\app.ini", "app");
        s.addFile(kData + L"\\logs\\wheelbri-service-2026-11-09-01.log", "log");
        s.addFile(kData + L"\\dumps\\x.dmp", "dump");
        s.regSetText(Hive::Machine, L"SOFTWARE\\wheelbri\\SigningCertificates", L"AB", L"CN=WheelBri");
        s.regSetText(Hive::Machine, L"SOFTWARE\\wheelbri\\hide", L"AllowedPid", L"1");
        s.regSetText(Hive::Machine, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\WUDF\\Services\\wheelbri_hide", L"x", L"y");
        r = FakeReporter{};
        uninstallOptions();
        o.keepConfig = true;
    }
    int run() { return uninstall(s, o, r); }
};

TEST_F(UninstallTest, RemovesEverythingItPutThereOnWindows11) {
    ASSERT_EQ(run(), 0) << (r.failures.empty() ? "" : toUtf8(r.failures[0]));
    EXPECT_EQ(r.steps, (std::vector<int>{1, 2, 3, 4, 5, 6, 7}));
    EXPECT_FALSE(s.has(kDir));
    EXPECT_FALSE(s.helper);
    EXPECT_LT(s.ranAt(L"-WaitForDriverInstallers"), s.ranAt(L"uninstall-hide.ps1"));
    EXPECT_LT(s.ranAt(L"uninstall-hide.ps1"), s.ranAt(L"uninstall-driver.ps1"));
    EXPECT_LT(s.ranAt(L"uninstall-driver.ps1"), s.commands.size());
    EXPECT_FALSE(s.ran(L"-RemoveCertificate"));
    EXPECT_FALSE(s.regKeyExists(Hive::Machine, kUninstallKey));
    EXPECT_FALSE(s.has(kPrograms + L"\\WheelBri"));
    EXPECT_FALSE(s.has(kDesktop + L"\\WheelBri.lnk"));
    // The certificate's record stays with the certificate; everything else of WheelBri's goes.
    EXPECT_TRUE(s.regKeyExists(Hive::Machine, L"SOFTWARE\\wheelbri\\SigningCertificates"));
    EXPECT_FALSE(s.regKeyExists(Hive::Machine, L"SOFTWARE\\wheelbri\\hide"));
    EXPECT_EQ(s.regText(Hive::Machine, L"SOFTWARE\\wheelbri", L"LogFolder"), std::nullopt);
    EXPECT_FALSE(s.regKeyExists(Hive::Machine, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\WUDF\\Services\\wheelbri_hide"));
    // Settings and logs kept (the wizard's defaults), the configs copied out.
    EXPECT_TRUE(s.has(kData + L"\\rigs\\rig.ini"));
    EXPECT_TRUE(s.has(kData + L"\\logs\\wheelbri-service-2026-11-09-01.log"));
    EXPECT_EQ(s.bytes(kData + L"\\config-backup\\g920.ini"), defaultConfig(L"g920"));
    // Firefox as it was before the install.
    EXPECT_EQ(s.value(Hive::Users, kFirefoxKey, L"Preferences")->text, L"{\"x\":1}");
    EXPECT_TRUE(r.warnings.empty());
}

TEST_F(UninstallTest, RemovesTheCertificateWhenAskedAndThenAllOfItsRegistry) {
    o.removeCertificate = true;
    ASSERT_EQ(run(), 0);
    EXPECT_LT(s.ranAt(L"\\uninstall-driver.ps1\""), s.ranAt(L"-RemoveCertificate"));
    EXPECT_EQ(r.last("certificate")->state, ItemState::Done);
    EXPECT_FALSE(s.regKeyExists(Hive::Machine, L"SOFTWARE\\wheelbri"));
}

TEST_F(UninstallTest, KeepsTheCertificateRecordWhenItsRemovalFails) {
    o.removeCertificate = true;
    s.exitCodes[L"-RemoveCertificate"] = 1;
    ASSERT_EQ(run(), 0);
    EXPECT_EQ(r.last("certificate")->state, ItemState::Warn);
    EXPECT_TRUE(s.regKeyExists(Hive::Machine, L"SOFTWARE\\wheelbri\\SigningCertificates"));
}

TEST_F(UninstallTest, SaysWhenWindowsMustRestartToFinish) {
    s.output[L"uninstall-hide.ps1"] = {L"the rig's devices go back after a restart"};
    ASSERT_EQ(run(), 0);
    EXPECT_TRUE(r.restart);
}

TEST_F(UninstallTest, DriverScriptsThatFailAreWarningsNotTheEnd) {
    s.exitCodes[L"uninstall-hide.ps1"] = 1;
    s.exitCodes[L"-WaitForDriverInstallers"] = 1;
    ASSERT_EQ(run(), 0);
    EXPECT_EQ(r.last("hide-driver")->state, ItemState::Warn);
    EXPECT_TRUE(r.warned(L"still running after 2 minutes"));
    EXPECT_FALSE(s.has(kDir));
}

TEST_F(UninstallTest, HelperThatCannotBeRemovedIsAWarning) {
    s.helperDeleteFails = true;
    ASSERT_EQ(run(), 0);
    EXPECT_EQ(r.last("helper")->state, ItemState::Warn);
    EXPECT_TRUE(r.warned(L"sc.exe delete"));
}

TEST_F(UninstallTest, OnWindows7RunsNoDriverScripts) {
    s.build = 7601;
    ASSERT_EQ(run(), 0);
    EXPECT_FALSE(s.ran(L"powershell.exe"));
    EXPECT_EQ(r.last("hide-driver"), nullptr);
    EXPECT_FALSE(s.has(kDir));
}

TEST_F(UninstallTest, RemoveSettingsAndLogsLeavesNothingOfTheUsersData) {
    o.keepConfig = false;
    o.removeSettings = true;
    o.removeLogs = true;
    s.addFile(L"C:\\ProgramData\\wheelbri\\logs\\driver.log", "d");
    ASSERT_EQ(run(), 0);
    EXPECT_FALSE(s.has(kData));
    EXPECT_FALSE(s.has(L"C:\\ProgramData\\wheelbri"));
}

TEST_F(UninstallTest, RemoveSettingsKeepsTheLogs) {
    o.keepConfig = false;
    o.removeSettings = true;
    ASSERT_EQ(run(), 0);
    EXPECT_FALSE(s.has(kData + L"\\rigs"));
    EXPECT_FALSE(s.has(kData + L"\\app.ini"));
    EXPECT_TRUE(s.has(kData + L"\\logs\\wheelbri-service-2026-11-09-01.log"));
    EXPECT_TRUE(s.has(kData + L"\\dumps\\x.dmp"));
}

TEST_F(UninstallTest, RemoveSettingsNeverFollowsALinkInTheDataFolder) {
    o.keepConfig = false;
    o.removeSettings = true;
    s.addFile(L"E:\\Important\\file.txt", "precious");
    s.addLink(kData + L"\\linked", true);
    ASSERT_EQ(run(), 0);
    EXPECT_FALSE(s.has(kData + L"\\linked"));
    EXPECT_EQ(s.bytes(L"E:\\Important\\file.txt"), "precious");
}

TEST_F(UninstallTest, StopsBeforeAnythingWhenTheDataFolderIsALink) {
    s.removeFile(kData + L"\\app.ini");
    s.addLink(kData, true);
    EXPECT_EQ(run(), 1);
    EXPECT_TRUE(s.has(kDir + L"\\wheelbri.exe"));
    EXPECT_TRUE(s.commands.empty());
}

TEST_F(UninstallTest, QuietMovesTheChangedConfigsToABackup) {
    o.keepConfig = false;
    o.quiet = true;
    s.addFile(kDir + L"\\config\\g920.ini", "[changed]\n");
    ASSERT_EQ(run(), 0);
    const std::wstring backup = kData + L"\\config-backup-2026-11-09-140506";
    EXPECT_EQ(s.bytes(backup + L"\\g920.ini"), "[changed]\n");
    EXPECT_FALSE(s.has(backup + L"\\g25.ini"));   // as shipped: removed with the program
    EXPECT_TRUE(r.warned(L"changed configs"));
}

TEST_F(UninstallTest, QuietWithoutTheDefaultsRecordKeepsEveryConfig) {
    o.keepConfig = false;
    o.quiet = true;
    s.removeFile(kDir + L"\\default-configs.txt");
    ASSERT_EQ(run(), 0);
    EXPECT_TRUE(s.has(kData + L"\\config-backup-2026-11-09-140506\\g25.ini"));
    EXPECT_TRUE(r.warned(L"recorded no defaults"));
}

TEST_F(UninstallTest, QuietStopsWhenAChangedConfigCannotBeKept) {
    o.keepConfig = false;
    o.quiet = true;
    s.addFile(kDir + L"\\config\\g920.ini", "[changed]\n");
    s.failRemove.insert(testing::lowered(kDir + L"\\config\\g920.ini"));
    s.failCopyTo.insert(testing::lowered(kData + L"\\config-backup-2026-11-09-140506\\g920.ini"));
    EXPECT_EQ(run(), 1);
    EXPECT_TRUE(s.has(kDir + L"\\wheelbri.exe"));   // nothing more removed
}

TEST_F(UninstallTest, LeavesAFolderThatIsNoInstall) {
    s.addFile(kDir + L"\\install.ps1", "#");   // a package, not an install
    ASSERT_EQ(run(), 0);
    EXPECT_TRUE(s.has(kDir + L"\\wheelbri.exe"));
    EXPECT_FALSE(s.regKeyExists(Hive::Machine, kUninstallKey));
}

TEST_F(UninstallTest, WarnsWhenFilesStayInTheFolder) {
    s.addFile(kDir + L"\\my notes.txt", "mine");
    ASSERT_EQ(run(), 0);
    EXPECT_EQ(s.bytes(kDir + L"\\my notes.txt"), "mine");
    EXPECT_TRUE(r.warned(L"could not be removed completely"));
}

TEST_F(UninstallTest, RunsWithoutTheInstallFolder) {
    for (const std::wstring& f : s.filesUnder(kDir)) s.removeFile(kDir + L"\\" + f);
    ASSERT_EQ(run(), 0);
    EXPECT_FALSE(s.regKeyExists(Hive::Machine, kUninstallKey));
    EXPECT_TRUE(r.warned(L"uninstall-hide.ps1"));   // missing: a warning
}

// --- uninstall: the shim in games ------------------------------------------------------------------------------------

TEST_F(UninstallTest, TakesTheShimOutOfGamesAndGivesOtherModsTheirNamesBack) {
    s.addFile(kData + L"\\shim-games.txt", "game\tACC\tD:\\ACC\r\ngame\tNFS\tD:\\NFS\r\ngame\tGRID\tD:\\GRID\r\n"
                                           "game\tGone\tD:\\Gone\r\ngame\tACC twice\td:\\acc\\\r\n");
    s.addFile(L"D:\\ACC\\dinput8.dll", "MZ wheelbri");
    s.addFile(L"D:\\ACC\\wheelbri-shim.ini", "[shim]");
    s.addFile(L"D:\\ACC\\dinput8-chain.dll", "MZ reshade");
    s.addFile(L"D:\\NFS\\dinput8.dll", "MZ IsUltimateASILoader");          // the ASI loader stays
    s.addFile(L"D:\\NFS\\dinput8Hooked.dll", "MZ wheelbri");
    s.addFile(L"D:\\NFS\\dinput8-chain.dll", "MZ other");
    s.addFile(L"D:\\GRID\\dinput8.dll", "MZ someone else's");              // not ours: stays
    ASSERT_EQ(run(), 0);
    EXPECT_EQ(s.bytes(L"D:\\ACC\\dinput8.dll"), "MZ reshade");
    EXPECT_FALSE(s.has(L"D:\\ACC\\wheelbri-shim.ini"));
    EXPECT_EQ(s.bytes(L"D:\\NFS\\dinput8.dll"), "MZ IsUltimateASILoader");
    EXPECT_EQ(s.bytes(L"D:\\NFS\\dinput8Hooked.dll"), "MZ other");
    EXPECT_EQ(s.bytes(L"D:\\GRID\\dinput8.dll"), "MZ someone else's");
    EXPECT_EQ(r.notes[1], L"3 of 3 game(s)");
}

TEST_F(UninstallTest, ShimThatCannotBeRemovedOrRenamedIsAWarning) {
    s.addFile(kData + L"\\shim-games.txt", "game\tACC\tD:\\ACC\r\n");
    s.addFile(L"D:\\ACC\\dinput8.dll", "MZ wheelbri");
    s.failRemove.insert(L"d:\\acc\\dinput8.dll");
    s.addFile(L"D:\\ACC\\dinput8-chain.dll", "MZ reshade");
    ASSERT_EQ(run(), 0);
    EXPECT_TRUE(r.warned(L"is the game running?"));
    EXPECT_TRUE(r.warned(L"dinput8.dll is there"));
    EXPECT_TRUE(s.has(L"D:\\ACC\\dinput8-chain.dll"));
}

TEST_F(UninstallTest, KeepShimsLeavesTheGamesAlone) {
    o.keepShims = true;
    s.addFile(kData + L"\\shim-games.txt", "game\tACC\tD:\\ACC\r\n");
    s.addFile(L"D:\\ACC\\dinput8.dll", "MZ wheelbri");
    ASSERT_EQ(run(), 0);
    EXPECT_TRUE(s.has(L"D:\\ACC\\dinput8.dll"));
}

TEST_F(UninstallTest, ShimInALinkedGameFolderIsNotTouched) {
    s.addFile(kData + L"\\shim-games.txt", "game\tACC\tD:\\ACC\r\n");
    s.addLink(L"D:\\ACC", true);
    ASSERT_EQ(run(), 0);
    EXPECT_EQ(r.notes[1], L"no game has it");
}

// --- uninstall: the registry entries of others -----------------------------------------------------------------------

TEST_F(UninstallTest, RemovesOnlyItsOwnDirectInputEntries) {
    const std::wstring oem = L"CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\Joystick\\OEM\\";
    const std::wstring ours = L"SYSTEM\\" + oem + L"VID_046D&PID_C262";
    s.regSetText(Hive::Machine, ours, L"OEMName", L"Logitech G920 Driving Force Racing Wheel USB");
    s.regSetText(Hive::Machine, ours, L"OEMData", L"x");
    s.regSetText(Hive::Machine, ours + L"\\Axes\\0", L"x", L"y");
    s.regSetText(Hive::Machine, ours + L"\\Axes", L"WheelBri", L"1");
    const std::wstring real = L"SYSTEM\\" + oem + L"VID_046D&PID_C26E";   // a real G923's, renamed by its software
    s.regSetText(Hive::Machine, real, L"OEMName", L"Logitech G923 (G HUB)");
    s.regSetText(Hive::Machine, real, L"OEMData", L"x");
    s.regSetText(Hive::Machine, real + L"\\Axes", L"Name", L"Wheel");
    const std::wstring mixed = L"System\\" + oem + L"VID_046D&PID_C299";
    s.regSetText(Hive::CurrentUser, mixed, L"OEMName", L"Logitech G25 Racing Wheel USB");
    s.regSetText(Hive::CurrentUser, mixed, L"Other", L"theirs");
    ASSERT_EQ(run(), 0);
    EXPECT_FALSE(s.regKeyExists(Hive::Machine, ours));
    EXPECT_EQ(*s.regText(Hive::Machine, real, L"OEMName"), L"Logitech G923 (G HUB)");
    EXPECT_TRUE(s.regKeyExists(Hive::Machine, real + L"\\Axes"));
    EXPECT_EQ(s.regText(Hive::CurrentUser, mixed, L"OEMName"), std::nullopt);
    EXPECT_EQ(*s.regText(Hive::CurrentUser, mixed, L"Other"), L"theirs");
}

TEST_F(UninstallTest, FirefoxKeysGoWhenNothingElseIsInThem) {
    s.regDeleteTree(Hive::Users, L"S-1-5-21-1-2-3-1001\\Software\\Policies\\Mozilla");
    s.setMulti(Hive::Users, kFirefoxKey, L"Preferences", L"{\"dom.gamepad.enabled\":{\"Value\":false}}");
    ASSERT_EQ(run(), 0);
    EXPECT_FALSE(s.regKeyExists(Hive::Users, L"S-1-5-21-1-2-3-1001\\Software\\Policies\\Mozilla"));
    EXPECT_TRUE(s.regKeyExists(Hive::Users, L"S-1-5-21-1-2-3-1001\\Software\\Policies"));
}

TEST_F(UninstallTest, FirefoxOfAUserNotSignedInIsAWarning) {
    s.regDeleteTree(Hive::Users, L"S-1-5-21-1-2-3-1001");
    ASSERT_EQ(run(), 0);
    EXPECT_TRUE(r.warned(L"not signed in"));
}

TEST_F(UninstallTest, FirefoxRecordThatIsNoUserIsIgnored) {
    s.regSetText(Hive::Machine, L"SOFTWARE\\wheelbri", L"BrowserGamepadPolicy", L"..\\other");
    ASSERT_EQ(run(), 0);
    EXPECT_EQ(s.value(Hive::Users, kFirefoxKey, L"Preferences")->text,
              L"{\"x\":1,\"dom.gamepad.enabled\":{\"Value\":false,\"Status\":\"locked\"}}");
}

// --- install, then uninstall: nothing left ---------------------------------------------------------------------------

TEST_F(SetupStepsTest, InstallThenUninstallLeavesThePcAsItWas) {
    o.installDir = L"D:\\Games\\WheelBri";
    ASSERT_EQ(runInstall(), 0);
    Options u = o;
    u.uninstall = true;
    u.removeSettings = true;
    u.removeLogs = true;
    u.removeCertificate = true;
    FakeReporter ur;
    ASSERT_EQ(uninstall(s, u, ur), 0);
    EXPECT_FALSE(s.has(L"D:\\Games\\WheelBri"));
    EXPECT_TRUE(s.has(L"D:\\Games"));
    EXPECT_FALSE(s.has(kData));
    EXPECT_FALSE(s.regKeyExists(Hive::Machine, L"SOFTWARE\\wheelbri"));
    EXPECT_FALSE(s.regKeyExists(Hive::Machine, kUninstallKey));
    EXPECT_TRUE(s.filesUnder(kPrograms).empty());
    EXPECT_TRUE(s.filesUnder(kDesktop).empty());
    EXPECT_TRUE(ur.warnings.empty());
}

}  // namespace
}  // namespace wheelbri::setup
