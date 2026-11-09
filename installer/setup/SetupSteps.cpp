// SPDX-License-Identifier: GPL-3.0-or-later
#include "SetupSteps.hpp"

#include <algorithm>
#include <cstdio>
#include <cwctype>

namespace wheelbri::setup {
namespace {

constexpr int kStepCount = 7;
const std::wstring kUninstallKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\wheelbri";
const std::wstring kWheelbriKey = L"SOFTWARE\\wheelbri";
const std::wstring kManifest = L"installed-files.txt";
const std::wstring kHelperProgram = L"wheelbri-helper.exe";
// A failed step: the run stops (caught in install() and uninstall()), the reporter already told why.
struct Stop {};

std::wstring quoted(const std::wstring& s) { return L"\"" + s + L"\""; }

std::wstring number(const std::uint64_t n) { return std::to_wstring(n); }

std::wstring twoDigits(const int n) {
    wchar_t text[8];
    std::swprintf(text, 8, L"%02d", n);
    return text;
}

std::wstring powershell(SetupSystem& s) { return joinPath(s.folder(Folder::System32), L"WindowsPowerShell\\v1.0\\powershell.exe"); }

// Runs a script in the install folder with Windows PowerShell; its output goes to the log, indented. `restart`:
// set when a line says Windows must restart.
int runScript(SetupSystem& s, Reporter& r, const std::wstring& script, const std::wstring& arguments,
              const std::wstring& workFolder, bool* restart = nullptr) {
    if (!s.isFile(script)) {
        r.log(L"    missing: " + script);
        return 1;
    }
    const std::wstring command = quoted(powershell(s)) + L" -NoProfile -ExecutionPolicy Bypass -File " + quoted(script) +
                                 (arguments.empty() ? L"" : L" " + arguments);
    return s.run(command, workFolder, [&](const std::wstring& line) {
        r.log(L"    " + line);
        if (restart && (line.find(L"after a restart") != std::wstring::npos ||
                        line.find(L"restart Windows") != std::wstring::npos || line.find(L"3010") != std::wstring::npos)) {
            *restart = true;
        }
    });
}

[[noreturn]] void failWith(Reporter& r, const std::wstring& text) {
    r.fail(text);
    throw Stop{};
}

bool exists(SetupSystem& s, const std::wstring& path) { return s.isFile(path) || s.isFolder(path) || s.isLink(path); }

// Deletes a file or a folder with everything in it, never through a link (a link goes as a link). Errors are left for
// the caller to see in what is left.
void removeTree(SetupSystem& s, const std::wstring& path) {
    if (s.isLink(path)) {
        if (s.isFolder(path)) s.removeFolder(path);
        else s.removeFile(path);
        return;
    }
    if (s.isFolder(path)) {
        for (const DirEntry& e : s.list(path)) removeTree(s, joinPath(path, e.name));
        s.removeFolder(path);
    } else if (s.isFile(path)) {
        s.removeFile(path);
    }
}

void removeEmptyFolders(SetupSystem& s, const std::wstring& folder) {
    for (const DirEntry& e : s.list(folder)) {
        if (e.folder && !e.link) removeEmptyFolders(s, joinPath(folder, e.name));
    }
    if (s.list(folder).empty()) s.removeFolder(folder);
}

// Whether <root>\<relative> is reached through no link (the last part included).
bool noLinkBelow(SetupSystem& s, const std::wstring& root, const std::wstring& relative) {
    std::wstring path = root;
    size_t start = 0;
    while (start <= relative.size()) {
        const size_t end = relative.find(L'\\', start);
        path = joinPath(path, relative.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (s.isLink(path)) return false;
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return true;
}

// Every file under `folder` (not through links), as paths relative to it.
void filesBelow(SetupSystem& s, const std::wstring& folder, const std::wstring& relative, std::vector<std::wstring>& out) {
    for (const DirEntry& e : s.list(relative.empty() ? folder : joinPath(folder, relative))) {
        const std::wstring child = relative.empty() ? e.name : joinPath(relative, e.name);
        if (e.link) continue;
        if (e.folder) filesBelow(s, folder, child, out);
        else out.push_back(child);
    }
}

std::uint64_t folderBytes(SetupSystem& s, const std::wstring& folder) {
    std::vector<std::wstring> files;
    filesBelow(s, folder, L"", files);
    std::uint64_t total = 0;
    for (const std::wstring& f : files) total += s.fileSize(joinPath(folder, f));
    return total;
}

bool emptyFolder(SetupSystem& s, const std::wstring& folder) { return s.list(folder).empty(); }

std::wstring stamp(const LocalTime& t, const bool withTime) {
    std::wstring text = std::to_wstring(t.year) + L"-" + twoDigits(t.month) + L"-" + twoDigits(t.day);
    if (withTime) text += L"-" + twoDigits(t.hour) + twoDigits(t.minute) + twoDigits(t.second);
    return text;
}

// The Start menu folder an earlier install recorded, when it is a plain name below Programs.
std::wstring recordedMenuFolder(SetupSystem& s) {
    const std::wstring folder = s.regText(Hive::Machine, kUninstallKey, L"StartMenuFolder").value_or(L"");
    if (folder.empty() || folder.find(L"..") != std::wstring::npos || folder.find(L':') != std::wstring::npos ||
        folder.front() == L'\\') {
        return {};
    }
    return folder;
}

// --- install ----------------------------------------------------------------------------------------------------

struct Installing {
    SetupSystem& s;
    const Package& p;
    const Options& o;
    Reporter& r;
    std::uint32_t build = 0;
    bool drivers = false;
    bool helperWasThere = false;
    int step = 0;

    void next(const std::wstring& text) {
        ++step;
        r.log(L"[" + number(step) + L"/" + number(kStepCount) + L"] " + text);
        r.step(step, text);
    }
    [[nodiscard]] std::wstring dir(const std::wstring& relative) const { return joinPath(o.installDir, relative); }

    void checkPc();
    void universalCRuntime();
    void checkFolder();
    void closeRunning();
    void copyFiles();
    void installDrivers();
    void shortcuts();
    void settingsEntry();
    void firefox();
    void check();
};

void setUpLogFolder(SetupSystem& s, Reporter& r, const std::wstring& userData) {
    bool linked = false;
    const std::wstring logs = logFolderFor(s, userData, linked);
    if (linked) {
        r.warn(joinPath(userData, L"wheelbri\\logs") + L" (or the folder above it) is a link to another folder: the "
               L"drivers keep their logs in C:\\ProgramData\\wheelbri\\logs. Replace the link with a real folder to "
               L"have every log in one place.");
        return;
    }
    s.makeFolders(logs);
    // The drivers run as LocalService and write their logs here too; HKLM\SOFTWARE\wheelbri LogFolder tells them
    // where (the app does the same at every start, ServiceLogging.hpp shareLogFolder).
    s.run(quoted(joinPath(s.folder(Folder::System32), L"icacls.exe")) + L" " + quoted(logs) + L" /grant *S-1-5-19:(OI)(CI)M",
          s.folder(Folder::System32), [](const std::wstring&) {});
    s.regSetText(Hive::Machine, kWheelbriKey, L"LogFolder", logs);
}

void Installing::checkPc() {
    next(L"checking this PC");
    if (!s.is64BitWindows()) failWith(r, L"WheelBri needs 64-bit Windows.");
    if (build < 7601) failWith(r, L"WheelBri needs Windows 7 SP1 (build 7601) or newer; this is build " + number(build) + L".");
    if (drivers) {
        r.log(L"    " + windowsName(build) + L" (build " + number(build) + L"), 64-bit: fine");
    } else {
        r.log(L"    " + windowsName(build) + L" (build " + number(build) + L"), 64-bit: the in-game shim way only (the "
              L"virtual wheel needs Windows 11): no drivers, no WheelBri Helper");
        universalCRuntime();
    }
    std::vector<std::wstring> required = {L"wheelbri.exe", L"wheelbri-service.exe", L"wheelbri-helper.exe",
                                          L"config\\g25.ini", L"config\\g27.ini", L"config\\g920.ini",
                                          L"config\\g923.ini", L"config\\wheelbri-shim.ini", L"driver\\common.ps1",
                                          L"driver\\installer-steps.ps1", L"shim\\dinput8.dll", L"shim\\x86\\dinput8.dll"};
    if (drivers) {
        for (const wchar_t* f : {L"driver\\install-driver.ps1", L"driver\\uninstall-driver.ps1",
                                 L"driver\\hide\\install-hide.ps1", L"driver\\hide\\uninstall-hide.ps1",
                                 L"driver\\signing-certificate.txt"}) {
            required.emplace_back(f);
        }
        for (const wchar_t* d : {L"driver\\x64\\Release\\wheelbri-wheel\\wheelbri-wheel",
                                 L"driver\\hide\\x64\\Release\\wheelbri-hide\\wheelbri-hide"}) {
            for (const wchar_t* extension : {L".inf", L".cat", L".dll"}) required.push_back(std::wstring(d) + extension);
        }
    }
    for (const std::wstring& f : required) {
        if (!p.find(f)) failWith(r, L"the package is incomplete (" + f + L" is missing): unpack the whole package again.");
    }
    if (p.setupExe.empty() || !s.isFile(p.setupExe)) failWith(r, L"the package is incomplete (wheelbri-setup.exe is missing).");
    checkFolder();
    if (drivers) {
        r.log(L"    the hiding filter is only added now: the app puts it on the devices of your rig (the Rig tab) the "
              L"first time you press Start");
        r.log(L"    the drivers are signed with WheelBri's own certificate (not Microsoft's): it is added to the "
              L"certificates this PC trusts (the uninstaller can remove it)");
    }
}

// The app and its shim are built against the Universal C Runtime (ucrtbase.dll): part of Windows 10, an update
// (KB2999226) before it, which the package carries for Windows 7, 8 and 8.1 (ucrt\).
void Installing::universalCRuntime() {
    const std::wstring ucrt = joinPath(s.folder(Folder::System32), L"ucrtbase.dll");
    if (s.isFile(ucrt)) {
        r.log(L"    Universal C Runtime: there");
        return;
    }
    const wchar_t* name = build < 9200 ? L"Windows6.1-KB2999226-x64.msu"
                        : build < 9600 ? L"Windows8-RT-KB2999226-x64.msu"
                        : build < 10240 ? L"Windows8.1-KB2999226-x64.msu" : nullptr;
    const PackageFile* msu = name ? p.find(std::wstring(L"ucrt\\") + name) : nullptr;
    if (!msu) {
        failWith(r, L"WheelBri needs the Universal C Runtime (update KB2999226), which this Windows does not have and "
                    L"this package does not carry for it: install it from Windows Update, then run Setup again.");
    }
    r.log(L"    installing the Universal C Runtime (KB2999226, " + std::wstring(name) + L")...");
    const int code = s.run(quoted(joinPath(s.folder(Folder::System32), L"wusa.exe")) + L" " + quoted(msu->source) +
                               L" /quiet /norestart",
                           s.folder(Folder::System32), [&](const std::wstring& line) { r.log(L"    " + line); });
    // wusa: 0 installed, 3010 installed (restart to finish), 2359302 already there, 0x80240017 not for this Windows.
    if (code == 3010) {
        r.warn(L"the Universal C Runtime is installed, but Windows finishes it at the next restart: restart Windows "
               L"before you open WheelBri");
        r.restartNeeded();
    } else if (code == static_cast<int>(0x80240017)) {
        failWith(r, build >= 9600 && build < 10240
                        ? L"the Universal C Runtime update does not install on this Windows 8.1 yet: it needs the "
                          L"April 2014 update (KB2919355) first. Install it with Windows Update, then run Setup again."
                        : L"the Universal C Runtime update (KB2999226) is not for this Windows: update Windows, then run "
                          L"Setup again.");
    } else if (code != 0 && code != 2359302) {
        failWith(r, L"the Universal C Runtime update (KB2999226) did not install (wusa exit code " + number(static_cast<std::uint32_t>(code)) +
                        L"). Install it with Windows Update, then run Setup again.");
    }
    if (code != 3010 && !s.isFile(ucrt)) {
        failWith(r, L"the Universal C Runtime is still missing after its update: restart Windows and run Setup again.");
    }
    r.log(L"    Universal C Runtime: installed");
}

// The install folder: new, empty, or an earlier install (its installed-files.txt, or the folder Settings > Apps
// records with the program in it). Anything else is refused: the uninstaller removes what the installer put there.
void Installing::checkFolder() {
    const std::wstring& d = o.installDir;
    if (d.size() < 3 || d[1] != L':' || d[2] != L'\\') failWith(r, d + L" is not a full folder path (C:\\...).");
    if (sameName(withoutTrailingSlash(d), withoutTrailingSlash(s.folder(Folder::SystemRoot))) ||
        isInside(d, s.folder(Folder::SystemRoot)) || d.size() <= 3) {
        failWith(r, L"WheelBri cannot be installed into " + d + L": choose a folder of its own.");
    }
    if (s.isLink(d)) failWith(r, d + L" is a link to another folder: choose a real folder.");
    if (!exists(s, d)) return;
    if (!s.isFolder(d)) failWith(r, d + L" is a file, not a folder: choose another folder.");
    if (isPackageFolder(s, d)) failWith(r, d + L" is a WheelBri package, not an install folder: choose another folder.");
    if (emptyFolder(s, d)) return;
    bool earlier = s.isFile(dir(kManifest));
    if (!earlier) {
        const std::wstring recorded = s.regText(Hive::Machine, kUninstallKey, L"InstallLocation").value_or(L"");
        earlier = !recorded.empty() && sameName(withoutTrailingSlash(recorded), withoutTrailingSlash(d)) &&
                  s.isFile(dir(L"wheelbri.exe"));
    }
    if (!earlier) failWith(r, d + L" is not empty and holds no WheelBri installation: choose an empty or new folder.");
}

void Installing::closeRunning() {
    next(L"closing a running WheelBri");
    std::vector<std::wstring> ended;
    const int running = s.closePrograms({L"wheelbri", L"wheelbri-service", L"wheelbri-bridge"}, 15, ended);
    for (const std::wstring& name : ended) r.warn(name + L" did not close within 15 s: ended");
    if (running == 0) {
        r.log(L"    none running");
    } else {
        r.log(L"    closed " + number(static_cast<std::uint64_t>(running)));
        // An ended service could leave the wheel hidden: clear that.
        if (s.isFile(dir(L"wheelbri-service.exe"))) {
            s.run(quoted(dir(L"wheelbri-service.exe")) + L" --unhide", o.installDir, [](const std::wstring&) {});
        }
    }
    // An earlier install's WheelBri Helper: stopped while its program is replaced, started again if this fails.
    if (s.helperExists()) {
        s.stopHelper();
        helperWasThere = true;
        r.log(L"    stopped the WheelBri Helper service");
    }
}

void Installing::copyFiles() {
    next(L"copying the program to " + o.installDir);
    if (!s.makeFolders(o.installDir)) failWith(r, L"could not create " + o.installDir + L".");
    // Outside Program Files a folder gets its drive's permissions, where any program of the user may change files:
    // it gets Program Files' rules instead (only Administrators and SYSTEM change it, users read and run it).
    const bool inProgramFiles = isInside(o.installDir, s.folder(Folder::ProgramFiles)) ||
                                isInside(o.installDir, s.folder(Folder::ProgramFilesX86));
    if (!inProgramFiles) {
        const int code = s.run(quoted(joinPath(s.folder(Folder::System32), L"icacls.exe")) + L" " + quoted(o.installDir) +
                                   L" /inheritance:r /grant:r *S-1-5-32-544:(OI)(CI)F *S-1-5-18:(OI)(CI)F "
                                   L"*S-1-5-32-545:(OI)(CI)RX",
                               s.folder(Folder::System32), [](const std::wstring&) {});
        if (code != 0) {
            failWith(r, L"could not protect " + o.installDir + L" (icacls exit code " + number(static_cast<std::uint32_t>(code)) +
                            L"): install into Program Files instead.");
        }
        r.log(L"    " + o.installDir + L" protected like Program Files (only Administrators may change it)");
    }
    // installed-files.txt: every file the installer put here, so that removing the install deletes those and nothing
    // else. Kept across upgrades: an older version's files stay listed while they are there. An install from before the
    // list: all of it is WheelBri's (checkFolder).
    NameSet listed;
    if (const auto manifest = s.readFile(dir(kManifest))) {
        for (const std::wstring& line : textLines(*manifest)) listed.insert(line);
    } else {
        std::vector<std::wstring> present;
        filesBelow(s, o.installDir, L"", present);
        listed.insert(present.begin(), present.end());
    }
    // An earlier install in another folder: its settings come along, and that folder goes once this one works.
    const std::wstring previous = s.regText(Hive::Machine, kUninstallKey, L"InstallLocation").value_or(L"");
    std::wstring oldFolder;
    if (!previous.empty() && !sameName(withoutTrailingSlash(previous), withoutTrailingSlash(o.installDir)) &&
        s.isFile(joinPath(previous, L"wheelbri.exe")) && !isPackageFolder(s, previous) && !s.isLink(previous)) {
        oldFolder = withoutTrailingSlash(previous);
        s.makeFolders(dir(L"config"));
        for (const DirEntry& e : s.list(joinPath(oldFolder, L"config"))) {
            if (e.folder || e.link || e.name.size() < 4 || !sameName(e.name.substr(e.name.size() - 4), L".ini")) continue;
            const std::wstring target = dir(L"config\\" + e.name);
            if (!exists(s, target) && s.copyFile(joinPath(oldFolder, L"config\\" + e.name), target)) {
                listed.insert(L"config\\" + e.name);
            }
        }
        // Its wheelbri-shim.ini from shim\, where versions before 2026-10-17 put it.
        const std::wstring oldShim = joinPath(oldFolder, L"shim\\wheelbri-shim.ini");
        if (s.isFile(oldShim) && !exists(s, dir(L"config\\wheelbri-shim.ini")) &&
            s.copyFile(oldShim, dir(L"config\\wheelbri-shim.ini"))) {
            listed.insert(L"config\\wheelbri-shim.ini");
        }
        r.log(L"    an earlier install is in " + oldFolder + L": its settings are carried over, the folder is removed below");
    }
    // wheelbri-shim.ini was in shim\ until 2026-10-17, where the app never looked: moved to config\ unless one is
    // there, then kept like any edited config.
    if (s.isFile(dir(L"shim\\wheelbri-shim.ini")) && !exists(s, dir(L"config\\wheelbri-shim.ini"))) {
        s.makeFolders(dir(L"config"));
        if (s.moveFile(dir(L"shim\\wheelbri-shim.ini"), dir(L"config\\wheelbri-shim.ini"))) {
            listed.insert(L"config\\wheelbri-shim.ini");
            r.log(L"    moved shim\\wheelbri-shim.ini to config\\, where the app reads it");
        }
    }

    ConfigHashes defaults;
    int copied = 0;
    for (const PackageFile& f : p.files) {
        const std::wstring folder = parentOf(f.relative);
        // The installer stays in the package (wheelbri-setup.exe goes in below as Uninstaller.exe); so do the
        // updates it installs before (ucrt\) and its own list.
        if (sameName(f.relative, L"wheelbri-setup.exe") || sameName(f.relative, L"package-files.txt") ||
            sameName(f.relative, L"Setup.cmd") || sameName(f.relative, L"install.ps1") || isInside(f.relative, L"ucrt") ||
            isInside(f.relative, L"win7")) {
            continue;
        }
        const std::wstring target = dir(f.relative);
        if (!folder.empty() && !s.makeFolders(dir(folder))) failWith(r, L"could not create " + dir(folder) + L".");
        if (s.isLink(target) || !noLinkBelow(s, o.installDir, f.relative)) {
            failWith(r, target + L" is reached through a link: remove the link and run Setup again.");
        }
        const bool config = sameName(folder, L"config") && f.relative.size() > 4 &&
                            sameName(f.relative.substr(f.relative.size() - 4), L".ini");
        if (config) {
            const auto bytes = s.readFile(f.source);
            if (!bytes) failWith(r, L"could not read " + f.source + L".");
            const std::wstring hash = sha256Hex(*bytes);
            defaults[f.relative] = hash;
            // An edited config stays: the new default goes next to it as .new.
            if (s.isFile(target)) {
                listed.insert(f.relative);
                const auto mine = s.readFile(target);
                if (!mine || sha256Hex(*mine) != hash) {
                    if (!s.writeFile(target + L".new", *bytes)) failWith(r, L"could not write " + target + L".new.");
                    listed.insert(f.relative + L".new");
                    r.log(L"    kept your " + f.relative + L" (the new default is " + f.relative + L".new)");
                } else if (s.isFile(target + L".new")) {
                    s.removeFile(target + L".new");   // this version's default: nothing new to offer any more
                }
                continue;
            }
        }
        if (!s.copyFile(f.source, target)) {
            failWith(r, L"could not copy " + f.relative + L" to " + o.installDir + L" (in use? close WheelBri and any "
                        L"game using its shim, then run Setup again).");
        }
        listed.insert(f.relative);
        ++copied;
    }
    r.log(L"    " + number(static_cast<std::uint64_t>(copied)) + L" files copied");
    // The graphical uninstaller: this program under the name that makes it one (SetupMain.cpp). Earlier installs
    // kept it as wheelbri-setup.exe or Uninstall WheelBri.exe, and ran uninstall.ps1 / Uninstall.cmd: those go.
    for (const wchar_t* old : {L"wheelbri-setup.exe", L"Uninstall WheelBri.exe", L"uninstall.ps1", L"Uninstall.cmd"}) {
        if (s.isFile(dir(old))) s.removeFile(dir(old));
        if (!s.isFile(dir(old))) listed.erase(old);
    }
    if (!s.copyFile(p.setupExe, dir(L"Uninstaller.exe"))) failWith(r, L"could not copy the uninstaller (Uninstaller.exe).");
    listed.insert(L"Uninstaller.exe");
    if (!s.writeFile(dir(L"default-configs.txt"), defaultConfigsFile(defaults))) {
        failWith(r, L"could not write default-configs.txt.");
    }
    listed.insert(L"default-configs.txt");
    // The built-in configs' older names (for the rig since 2026-10-04, one per mode until 2026-10-15).
    for (const std::wstring& old : olderConfigNames()) {
        for (const std::wstring& file : {dir(L"config\\" + old), dir(L"config\\" + old + L".new")}) {
            if (s.isFile(file)) s.removeFile(file);
        }
    }
    // The list: what is there now, sorted; not itself.
    std::vector<std::wstring> lines;
    for (const std::wstring& entry : listed) {
        if (!sameName(entry, kManifest) && isManifestEntry(entry) && s.isFile(dir(entry))) lines.push_back(entry);
    }
    if (!s.writeFile(dir(kManifest), linesFile(lines))) failWith(r, L"could not write installed-files.txt.");
    r.log(L"    " + number(lines.size()) + L" files listed in installed-files.txt (what the uninstaller removes)");

    if (!oldFolder.empty()) {
        if (removeInstallFolder(s, oldFolder, r)) r.log(L"    removed the old folder " + oldFolder);
        else r.warn(L"the old folder " + oldFolder + L" could not be removed completely: delete it by hand");
    }
}

void Installing::installDrivers() {
    next(L"installing the drivers");
    if (!drivers) {
        r.log(L"    none on this Windows (the in-game shim way needs none)");
        return;
    }
    bool hidePending = false;
    int hideCode = -1;
    struct Driver {
        const wchar_t* name;
        const char* key;
        const wchar_t* script;
        bool required;
    };
    for (const Driver& d : {Driver{L"virtual wheel (wheelbri-wheel)", "wheel-driver", L"driver\\install-driver.ps1", true},
                            Driver{L"hiding filter (wheelbri-hide)", "hide-driver", L"driver\\hide\\install-hide.ps1", false}}) {
        r.log(L"  " + std::wstring(d.name));
        r.item(d.key, ItemState::Running, d.name);
        const int code = runScript(s, r, dir(d.script), L"", o.installDir);
        const bool hide = std::string(d.key) == "hide-driver";
        if (hide) hideCode = code;
        if (code == 0) {
            r.item(d.key, ItemState::Done, d.name);
        } else if (hide && code == 2) {
            hidePending = true;
            r.item(d.key, ItemState::Warn, L"starts after a restart");
            r.warn(L"the hiding filter is installed but Windows starts it only after a restart: restart Windows");
        } else if (d.required) {
            r.item(d.key, ItemState::Failed, L"exit code " + number(static_cast<std::uint32_t>(code)));
            failWith(r, L"the " + std::wstring(d.name) + L" driver did not install (exit code " +
                            number(static_cast<std::uint32_t>(code)) + L"); the log says why.");
        } else {
            r.item(d.key, ItemState::Warn, L"exit code " + number(static_cast<std::uint32_t>(code)));
            r.warn(L"the " + std::wstring(d.name) + L" driver did not install (exit code " +
                   number(static_cast<std::uint32_t>(code)) + L"); the app works without it (see the log)");
        }
    }
    // Every release is signed with a certificate of its own: the earlier ones go once no driver uses them.
    if (runScript(s, r, dir(L"driver\\installer-steps.ps1"), L"-RemoveUnusedCertificates", o.installDir) != 0) {
        r.warn(L"an earlier certificate could not be removed (see the log); the uninstaller or clean-pc can remove it later");
    }

    // The WheelBri Helper: lets the app run without Administrator (the virtual wheel, hiding the rig).
    r.log(L"  WheelBri Helper (service)");
    r.item("helper", ItemState::Running, L"WheelBri Helper");
    const std::wstring problem = s.installHelper(dir(kHelperProgram));
    if (problem.empty()) {
        r.item("helper", ItemState::Done, L"running");
        helperWasThere = false;   // running: nothing to start again if a later step fails
        r.log(L"    running (starts with Windows)");
    } else {
        r.item("helper", ItemState::Warn, problem);
        r.warn(L"the WheelBri Helper is not running (" + problem + L"): until it does, the app shows HELPER NOT "
               L"INSTALLED and the virtual wheel works only when WheelBri is started as Administrator");
    }

    // The filter on the saved rig's devices now, after a reinstall that kept the user's settings, so the first Start
    // does not have to (the installed service does it as Start does). No saved rig: the first Start does it.
    if (hideCode == 0 || hideCode == 2) {
        r.log(L"  hiding filter on your saved rig's devices");
        const int code = s.run(quoted(dir(L"wheelbri-service.exe")) + L" --prepare-hide", o.installDir,
                               [&](const std::wstring& line) { r.log(L"    " + line); });
        if (code == 2) hidePending = true;
        else if (code != 0 && code != 3) {
            r.warn(L"the hiding filter was not put on your rig now (exit code " + number(static_cast<std::uint32_t>(code)) +
                   L"): the app sets it up at the first Start instead");
        }
    }
    if (hidePending) r.restartNeeded();
}

void Installing::shortcuts() {
    next(L"shortcuts");
    const std::wstring programs = s.folder(Folder::CommonPrograms);
    const std::wstring desktop = s.folder(Folder::CommonDesktop);
    const std::wstring app = dir(L"wheelbri.exe");
    // Older versions' shortcuts (named wheel-emulator), and the folder an earlier install made (its name may differ).
    removeTree(s, joinPath(programs, L"wheel-emulator.lnk"));
    removeTree(s, joinPath(programs, L"wheel-emulator"));
    removeTree(s, joinPath(desktop, L"wheel-emulator.lnk"));
    const std::wstring earlierMenu = recordedMenuFolder(s);
    if (!earlierMenu.empty()) removeTree(s, joinPath(programs, earlierMenu));
    const std::wstring menu = joinPath(programs, o.startMenuFolder);
    if (!s.makeFolders(menu)) failWith(r, L"could not create the Start menu folder " + menu + L".");
    auto link = [&](const std::wstring& path, const std::wstring& target, const std::wstring& arguments,
                    const std::wstring& folder, const std::wstring& description) {
        if (!s.shortcut(path, target, arguments, folder, description, app)) failWith(r, L"could not create " + path + L".");
        r.log(L"    " + path);
    };
    const std::wstring description = L"WheelBri: your rig as a Logitech wheel";
    link(joinPath(menu, L"WheelBri.lnk"), app, L"", o.installDir, description);
    link(joinPath(menu, L"Uninstaller.lnk"), dir(L"Uninstaller.exe"), L"--uninstall", o.installDir,
         L"Removes WheelBri, its drivers and shortcuts; asks what to keep");
    // Explorer's shell:Local AppData is the user who opens it: one shortcut for all users.
    link(joinPath(menu, L"WheelBri logs.lnk"), joinPath(s.folder(Folder::SystemRoot), L"explorer.exe"),
         L"\"shell:Local AppData\\wheelbri\\logs\"", s.folder(Folder::SystemRoot), L"The folder with every WheelBri log");
    const std::wstring desktopLink = joinPath(desktop, L"WheelBri.lnk");
    if (o.desktopShortcut) link(desktopLink, app, L"", o.installDir, description);
    else if (s.isFile(desktopLink)) s.removeFile(desktopLink);
}

void Installing::settingsEntry() {
    next(L"adding WheelBri to Settings > Apps");
    // The same key every version, so an upgrade replaces the entry instead of adding one.
    s.regDeleteTree(Hive::Machine, kUninstallKey);
    const std::wstring uninstaller = quoted(dir(L"Uninstaller.exe")) + L" --uninstall";
    const LocalTime t = s.now();
    const std::vector<std::pair<std::wstring, std::wstring>> texts = {
        {L"DisplayName", L"WheelBri"},
        {L"DisplayVersion", p.version},
        {L"Publisher", L"WheelBri"},
        {L"InstallLocation", o.installDir},
        {L"DisplayIcon", dir(L"wheelbri.exe")},
        {L"UninstallString", uninstaller},
        {L"QuietUninstallString", uninstaller + L" --quiet"},
        {L"InstallDate", std::to_wstring(t.year) + twoDigits(t.month) + twoDigits(t.day)},
        {L"StartMenuFolder", o.startMenuFolder},   // the uninstaller removes it
    };
    bool ok = true;
    for (const auto& [name, value] : texts) ok = s.regSetText(Hive::Machine, kUninstallKey, name, value) && ok;
    ok = s.regSetDword(Hive::Machine, kUninstallKey, L"NoModify", 1) && ok;
    ok = s.regSetDword(Hive::Machine, kUninstallKey, L"NoRepair", 1) && ok;
    ok = s.regSetDword(Hive::Machine, kUninstallKey, L"EstimatedSize",
                       static_cast<std::uint32_t>(std::min<std::uint64_t>(folderBytes(s, o.installDir) / 1024, 0xFFFFFFFFu))) && ok;
    if (!ok) failWith(r, L"could not write the Settings > Apps entry (HKLM\\" + kUninstallKey + L").");
    r.log(L"    done");
    firefox();
}

// Firefox opens every game controller for its gamepad support, and while a program has the wheel open Windows will
// not restart it with the hiding filter, so Start fails. Only when asked (--stop-browser-gamepads, the wizard's box):
// Firefox's own policy dom.gamepad.enabled = false (locked) for the user signed in here, merged with the preferences
// already set there, and that user recorded (HKLM\SOFTWARE\wheelbri BrowserGamepadPolicy) so the uninstaller takes
// out exactly that preference.
void Installing::firefox() {
    if (!o.stopBrowserGamepads) return;
    const std::wstring user = s.desktopUserSid();
    const std::wstring key = user + L"\\Software\\Policies\\Mozilla\\Firefox";
    const std::wstring manual = L"Firefox: its preferences policy is set in a form WheelBri does not change; to stop it "
                                L"holding the wheel open, set dom.gamepad.enabled to false in about:config";
    if (user.empty() || s.regKeyExists(Hive::Users, key + L"\\Preferences")) {
        r.warn(manual);
        return;
    }
    const FirefoxPolicy policy = turnOffGamepads(toUtf8(s.regText(Hive::Users, key, L"Preferences").value_or(L"")));
    if (policy.result == FirefoxPolicy::Result::NotOurs) {
        r.warn(manual);
    } else if (policy.result == FirefoxPolicy::Result::AlreadyOff) {
        r.log(L"    Firefox: its gamepad support is off already");
    } else if (s.regSetMultiText(Hive::Users, key, L"Preferences", fromUtf8(policy.text)) &&
               s.regSetText(Hive::Machine, kWheelbriKey, L"BrowserGamepadPolicy", user)) {
        r.log(L"    Firefox: gamepad support turned off (applies from its next start)");
    } else {
        r.warn(L"Firefox: its gamepad support could not be turned off (the policy could not be written)");
    }
}

void Installing::check() {
    next(L"checking the installation");
    if (drivers) {
        std::wstring listedDrivers;
        s.run(quoted(joinPath(s.folder(Folder::System32), L"pnputil.exe")) + L" /enum-drivers", s.folder(Folder::System32),
              [&](const std::wstring& line) { listedDrivers += line + L"\n"; });
        std::wstring lower = listedDrivers;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](const wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        for (const wchar_t* inf : {L"wheelbri-wheel.inf", L"wheelbri-hide.inf"}) {
            if (lower.find(inf) != std::wstring::npos) r.log(L"    " + std::wstring(inf) + L" installed");
            else r.warn(std::wstring(inf) + L" is not listed by pnputil");
        }
        if (s.helperRunning()) r.log(L"    WheelBri Helper running");
        else r.warn(L"the WheelBri Helper service is not running (services.msc > WheelBri Helper > Start)");
    } else {
        for (const wchar_t* f : {L"wheelbri.exe", L"shim\\dinput8.dll", L"shim\\x86\\dinput8.dll"}) {
            if (s.isFile(dir(f))) r.log(L"    " + std::wstring(f) + L" there");
            else r.warn(std::wstring(f) + L" is missing");
        }
    }
}

// --- uninstall --------------------------------------------------------------------------------------------------

// Removes a folder of the user's data without following links (a junction in it goes as a link). True when gone.
bool removeDataFolder(SetupSystem& s, const std::wstring& path) {
    removeTree(s, path);
    return !exists(s, path);
}

// Takes the shim out of one game folder, as the app's Remove shim does: our dinput8.dll / dinput8Hooked.dll (and
// their .off copies) and wheelbri-shim.ini go; another mod's dll the app chained (dinput8-chain.dll) gets its name
// back (dinput8Hooked.dll next to the Ultimate ASI Loader, else dinput8.dll). Returns the problems.
std::vector<std::wstring> removeShimFrom(SetupSystem& s, const std::wstring& folder) {
    std::vector<std::wstring> problems;
    for (const wchar_t* name : {L"dinput8.dll", L"dinput8.dll.off", L"dinput8Hooked.dll", L"dinput8Hooked.dll.off"}) {
        const std::wstring path = joinPath(folder, name);
        if (!s.isFile(path) || s.isLink(path)) continue;
        const auto bytes = s.readFile(path);
        if (bytes && isOurShim(*bytes) && !s.removeFile(path)) problems.push_back(std::wstring(name) + L" (in use)");
    }
    const std::wstring ini = joinPath(folder, L"wheelbri-shim.ini");
    if (s.isFile(ini)) s.removeFile(ini);
    const std::wstring chain = joinPath(folder, L"dinput8-chain.dll");
    if (s.isFile(chain)) {
        const std::wstring loader = joinPath(folder, L"dinput8.dll");
        const auto bytes = s.isFile(loader) ? s.readFile(loader) : std::nullopt;
        const std::wstring to = joinPath(folder, bytes && isAsiLoader(*bytes) ? L"dinput8Hooked.dll" : L"dinput8.dll");
        if (exists(s, to)) problems.push_back(L"dinput8-chain.dll not renamed back: " + nameOf(to) + L" is there");
        else if (!s.moveFile(chain, to)) problems.push_back(L"dinput8-chain.dll (in use)");
    }
    return problems;
}

// DirectInput's OEM entries WheelBri wrote under one key: OEMName and OEMData when OEMName is WheelBri's name, the
// Axes and OEMForceFeedback keys when they carry a "WheelBri" value; the key when nothing else is left in it. A real
// Logitech wheel's entries (Windows', Logitech's software's) stay.
void removeOemEntries(SetupSystem& s, Reporter& r, const Hive hive, const std::wstring& key, const std::wstring& ourName) {
    if (!s.regKeyExists(hive, key)) return;
    std::vector<std::wstring> removed;
    if (s.regText(hive, key, L"OEMName").value_or(L"") == ourName) {
        for (const wchar_t* value : {L"OEMName", L"OEMData"}) {
            if (s.regValueExists(hive, key, value)) {
                s.regDeleteValue(hive, key, value);
                removed.emplace_back(value);
            }
        }
    }
    for (const wchar_t* sub : {L"Axes", L"OEMForceFeedback"}) {
        if (s.regValueExists(hive, key + L"\\" + sub, L"WheelBri")) {
            s.regDeleteTree(hive, key + L"\\" + sub);
            removed.emplace_back(sub);
        }
    }
    if (!removed.empty() && s.regValueNames(hive, key).empty() && s.regSubKeys(hive, key).empty()) {
        s.regDeleteTree(hive, key);
        removed.emplace_back(L"(the key, left empty)");
    }
    if (!removed.empty()) {
        std::wstring list;
        for (const std::wstring& x : removed) list += (list.empty() ? L"" : L", ") + x;
        r.log(L"    " + key + L": removed " + list);
    }
}

struct Uninstalling {
    SetupSystem& s;
    const Options& o;
    Reporter& r;
    bool drivers = false;
    bool folderOk = false;
    bool certificateRemoved = false;
    std::wstring data{};   // the user's %LOCALAPPDATA%\wheelbri

    [[nodiscard]] std::wstring dir(const std::wstring& relative) const { return joinPath(o.installDir, relative); }
    void step(const int n, const std::wstring& text) {
        r.log(L"[" + number(static_cast<std::uint32_t>(n)) + L"/" + number(kStepCount) + L"] " + text);
        r.step(n, text);
    }

    void closeRunning();
    void shims();
    void removeDrivers();
    void entries();
    void firefox();
    void settings();
    void logs();
};

void Uninstalling::closeRunning() {
    step(1, L"closing a running WheelBri");
    std::vector<std::wstring> ended;
    const int running = s.closePrograms({L"wheelbri", L"wheelbri-service", L"wheelbri-bridge"}, 15, ended);
    for (const std::wstring& name : ended) r.warn(name + L" did not close within 15 s: ended");
    // The WheelBri Helper: stopped (it removes the virtual wheel it made and shows the rig again) and removed, before
    // its program and the drivers go.
    if (s.helperExists()) {
        r.item("helper", ItemState::Running, L"WheelBri Helper");
        s.stopHelper();
        if (s.deleteHelper()) {
            r.log(L"    removed the WheelBri Helper service");
            r.item("helper", ItemState::Done, L"removed");
        } else {
            r.item("helper", ItemState::Warn, L"could not be removed");
            r.warn(L"the WheelBri Helper service could not be removed: sc.exe delete WheelBriHelper (as Administrator)");
        }
    }
    if (drivers && folderOk) {
        // A driver installer the app started at Start keeps running when the app closes: finished first.
        if (runScript(s, r, dir(L"driver\\installer-steps.ps1"), L"-WaitForDriverInstallers", s.folder(Folder::System32)) != 0) {
            r.warn(L"a driver installer WheelBri started is still running after 2 minutes: removing anyway");
        }
        if (s.isFile(dir(L"wheelbri-service.exe"))) {
            s.run(quoted(dir(L"wheelbri-service.exe")) + L" --unhide", s.folder(Folder::System32), [](const std::wstring&) {});
        }
    }
    const std::wstring said = running > 0 ? L"closed " + number(static_cast<std::uint32_t>(running)) : L"none running";
    r.log(L"    " + said);
    r.note(said);
}

void Uninstalling::shims() {
    step(2, L"taking the shim out of games");
    std::vector<ShimGame> games;
    if (const auto text = s.readFile(joinPath(data, L"shim-games.txt"))) {
        for (ShimGame& g : readShimGames(*text)) {
            if (s.isFolder(g.folder) && !s.isLink(g.folder)) games.push_back(std::move(g));
        }
    }
    if (o.keepShims) {
        r.log(L"    left in " + number(games.size()) + L" game folder(s) (asked)");
        r.note(L"left (asked)");
        return;
    }
    if (games.empty()) {
        r.log(L"    no game has it");
        r.note(L"no game has it");
        return;
    }
    size_t cleaned = 0;
    for (const ShimGame& g : games) {
        const std::vector<std::wstring> problems = removeShimFrom(s, g.folder);
        if (problems.empty()) {
            r.log(L"    " + g.name + L": removed (" + g.folder + L")");
            ++cleaned;
        } else {
            std::wstring list;
            for (const std::wstring& p : problems) list += (list.empty() ? L"" : L"; ") + p;
            r.warn(g.name + L" (" + g.folder + L"): " + list + L" (is the game running?)");
        }
    }
    r.note(number(cleaned) + L" of " + number(games.size()) + L" game(s)");
}

void Uninstalling::removeDrivers() {
    step(3, L"removing the drivers");
    if (!drivers) {
        r.log(L"    none on this Windows (the in-game shim way only)");
        return;
    }
    bool restart = false;
    // The filter first (the rig's devices go back to Windows' own driver), then the virtual wheels'.
    for (const auto& [script, key] : {std::pair{L"driver\\hide\\uninstall-hide.ps1", "hide-driver"},
                                      std::pair{L"driver\\uninstall-driver.ps1", "wheel-driver"}}) {
        r.item(key, ItemState::Running, script);
        const int code = runScript(s, r, dir(script), L"", s.folder(Folder::System32), &restart);
        if (code != 0) {
            r.item(key, ItemState::Warn, L"exit code " + number(static_cast<std::uint32_t>(code)));
            r.warn(std::wstring(script) + L" ended with exit code " + number(static_cast<std::uint32_t>(code)) + L" (see the log)");
        } else {
            r.item(key, ItemState::Done, L"removed");
        }
    }
    if (o.removeCertificate) {
        r.item("certificate", ItemState::Running, L"certificate");
        const int code = runScript(s, r, dir(L"driver\\uninstall-driver.ps1"), L"-RemoveCertificate",
                                   s.folder(Folder::System32), &restart);
        if (code != 0) {
            r.item("certificate", ItemState::Warn, L"exit code " + number(static_cast<std::uint32_t>(code)));
            r.warn(L"the certificate was not removed (exit code " + number(static_cast<std::uint32_t>(code)) + L", see the log)");
        } else {
            certificateRemoved = true;
            r.item("certificate", ItemState::Done, L"removed");
        }
    }
    if (restart) r.restartNeeded();
}

void Uninstalling::entries() {
    step(4, L"removing shortcuts, the Settings > Apps entry and registry entries");
    const std::wstring programs = s.folder(Folder::CommonPrograms);
    const std::wstring desktop = s.folder(Folder::CommonDesktop);
    for (const wchar_t* name : {L"WheelBri", L"wheel-emulator"}) {   // wheel-emulator: the name before the rename
        removeTree(s, joinPath(programs, name));
        removeTree(s, joinPath(programs, std::wstring(name) + L".lnk"));
        removeTree(s, joinPath(desktop, std::wstring(name) + L".lnk"));
    }
    const std::wstring menu = recordedMenuFolder(s);
    if (!menu.empty()) removeTree(s, joinPath(programs, menu));
    s.regDeleteTree(Hive::Machine, kUninstallKey);
    firefox();
    if (certificateRemoved) {
        s.regDeleteTree(Hive::Machine, kWheelbriKey);
    } else {
        // Everything but SigningCertificates: the record of the certificate stays with the certificate, so removing
        // it later (clean-pc.ps1, uninstall-driver.ps1 -RemoveCertificate) finds exactly that one.
        for (const std::wstring& sub : s.regSubKeys(Hive::Machine, kWheelbriKey)) {
            if (!sameName(sub, L"SigningCertificates")) s.regDeleteTree(Hive::Machine, kWheelbriKey + L"\\" + sub);
        }
        for (const std::wstring& value : s.regValueNames(Hive::Machine, kWheelbriKey)) {
            s.regDeleteValue(Hive::Machine, kWheelbriKey, value);
        }
        if (s.regKeyExists(Hive::Machine, kWheelbriKey) && s.regSubKeys(Hive::Machine, kWheelbriKey).empty()) {
            s.regDeleteTree(Hive::Machine, kWheelbriKey);
        }
    }
    // The drivers' UMDF registrations (some removals leave them). g920: the wheel driver's name until 2026-10-19.
    for (const wchar_t* part : {L"hide", L"wheel", L"g920"}) {
        s.regDeleteTree(Hive::Machine, std::wstring(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\WUDF\\Services\\wheelbri_") + part);
    }
    for (const OemEntry& e : oemEntries()) {
        const std::wstring path = std::wstring(L"CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\Joystick\\OEM\\") + e.ids;
        removeOemEntries(s, r, Hive::Machine, L"SYSTEM\\" + path, e.name);
        removeOemEntries(s, r, Hive::CurrentUser, L"System\\" + path, e.name);
    }
    r.log(L"    done");
}

// Firefox's gamepad support, when the installer turned it off: only that preference goes, for the user it was set
// for; any other preference or policy there stays. Before HKLM\SOFTWARE\wheelbri, which records the user.
void Uninstalling::firefox() {
    const std::wstring user = s.regText(Hive::Machine, kWheelbriKey, L"BrowserGamepadPolicy").value_or(L"");
    if (user.empty() || user.find(L'\\') != std::wstring::npos) return;
    if (!s.regKeyExists(Hive::Users, user)) {
        r.warn(L"Firefox's gamepad support stays off: the Windows user it was turned off for is not signed in now "
               L"(remove dom.gamepad.enabled from HKEY_USERS\\" + user + L"\\Software\\Policies\\Mozilla\\Firefox "
               L"Preferences, or reinstall and uninstall while signed in as that user)");
        return;
    }
    const std::wstring key = user + L"\\Software\\Policies\\Mozilla\\Firefox";
    const auto value = s.regText(Hive::Users, key, L"Preferences");
    if (!value) return;
    const FirefoxPolicy policy = undoGamepads(toUtf8(*value));
    if (policy.result != FirefoxPolicy::Result::Changed) return;
    if (policy.text.empty()) s.regDeleteValue(Hive::Users, key, L"Preferences");
    else s.regSetMultiText(Hive::Users, key, L"Preferences", fromUtf8(policy.text));
    // Firefox's policy key, then Mozilla's: removed only when nothing else is left in them.
    for (const std::wstring& k : {key, parentOf(key)}) {
        if (s.regKeyExists(Hive::Users, k) && s.regValueNames(Hive::Users, k).empty() && s.regSubKeys(Hive::Users, k).empty()) {
            s.regDeleteTree(Hive::Users, k);
        }
    }
    r.log(L"    Firefox: gamepad support on again (from its next start)");
}

void Uninstalling::settings() {
    step(5, L"your rig, mappings and settings");
    const std::wstring config = dir(L"config");
    if (folderOk && o.quiet && !o.keepConfig && !o.removeSettings) {
        // No one to ask: the configs changed after install are moved to a backup, never deleted.
        const auto record = s.readFile(dir(L"default-configs.txt"));
        const ConfigHashes defaults = record ? readDefaultConfigs(*record) : ConfigHashes{};
        std::vector<std::wstring> edited;
        for (const DirEntry& e : s.list(config)) {
            if (e.folder || e.link || e.name.size() < 4 || !sameName(e.name.substr(e.name.size() - 4), L".ini")) continue;
            const auto known = defaults.find(L"config\\" + e.name);
            const auto bytes = s.readFile(joinPath(config, e.name));
            if (known == defaults.end() || !bytes || sha256Hex(*bytes) != known->second) edited.push_back(e.name);
        }
        if (edited.empty()) {
            r.log(L"    none changed: removed with the program");
        } else {
            const std::wstring backup = joinPath(data, L"config-backup-" + stamp(s.now(), true));
            if (!s.makeFolders(backup)) failWith(r, L"could not create " + backup + L" for your changed configs.");
            std::wstring names;
            for (const std::wstring& name : edited) {
                if (!s.moveFile(joinPath(config, name), joinPath(backup, name)) &&
                    !(s.copyFile(joinPath(config, name), joinPath(backup, name)))) {
                    failWith(r, L"could not keep your " + name + L" in " + backup + L": nothing more was removed.");
                }
                names += (names.empty() ? L"" : L", ") + name;
            }
            r.warn(L"moved your " + std::wstring(record ? L"changed configs" : L"configs (this install recorded no defaults to tell changed ones by)") +
                   L" to " + backup + L": " + names);
        }
    } else if (folderOk && o.keepConfig) {
        if (s.isFolder(config) && !s.isLink(config)) {
            const std::wstring backup = joinPath(data, L"config-backup");
            if (!s.makeFolders(backup) || s.isLink(backup)) failWith(r, L"could not create " + backup + L" for your configs.");
            for (const DirEntry& e : s.list(config)) {
                if (e.folder || e.link) continue;
                if (!s.copyFile(joinPath(config, e.name), joinPath(backup, e.name))) {
                    failWith(r, L"could not keep " + e.name + L" in " + backup + L": nothing more was removed.");
                }
            }
            r.log(L"    kept in " + backup + L" (copy them into the config folder after a new install)");
        } else {
            r.warn(L"no config folder in " + o.installDir + L": no settings to keep");
        }
    } else {
        r.log(L"    the install folder's configs: removed with the program");
    }
    if (o.removeSettings) {
        // Everything in the data folder but logs\ and dumps\ (the next step's).
        bool gone = true;
        if (s.isFolder(data) && !s.isLink(data)) {
            for (const DirEntry& e : s.list(data)) {
                if (sameName(e.name, L"logs") || sameName(e.name, L"dumps")) continue;
                if (!removeDataFolder(s, joinPath(data, e.name))) gone = false;
            }
        }
        if (gone) {
            r.log(L"    removed from " + data);
            r.note(L"removed");
        } else {
            r.warn(L"some of your settings in " + data + L" could not be removed (in use?): delete them by hand");
        }
    } else {
        r.log(L"    kept in " + data);
        r.note(L"kept");
    }
}

void Uninstalling::logs() {
    step(6, L"logs");
    const std::wstring logs = joinPath(data, L"logs");
    if (!o.removeLogs) {
        r.log(L"    kept in " + logs);
        r.note(L"kept");
        return;
    }
    const bool gone = removeDataFolder(s, logs) && removeDataFolder(s, joinPath(data, L"dumps"));
    // The drivers' own folder, used when they could not write to the user's (driver\Driver.cpp kFallbackLogFolder).
    removeDataFolder(s, joinPath(s.folder(Folder::ProgramData), L"wheelbri"));
    if (gone) {
        r.log(L"    removed");
        r.note(L"removed");
    } else {
        r.warn(L"some logs in " + logs + L" could not be removed (in use?): delete them by hand");
    }
    if (s.isFolder(data) && !s.isLink(data) && emptyFolder(s, data)) s.removeFolder(data);
}

}  // namespace

const PackageFile* Package::find(const std::wstring& relative) const {
    const auto it = std::find_if(files.begin(), files.end(), [&](const PackageFile& f) { return sameName(f.relative, relative); });
    return it == files.end() ? nullptr : &*it;
}

Package readPackage(SetupSystem& system, const std::wstring& folder, const std::wstring& setupExe) {
    Package package;
    package.setupExe = setupExe;
    if (const auto list = system.readFile(joinPath(folder, L"package-files.txt"))) {
        for (const PackageEntry& e : readPackageFiles(*list)) {
            const std::wstring source = joinPath(folder, e.flatName);
            if (system.isFile(source) && !system.isLink(source) && !package.find(e.relative)) {
                package.files.push_back({source, e.relative});
            }
        }
    } else {
        std::vector<std::wstring> files;
        filesBelow(system, folder, L"", files);
        for (const std::wstring& f : files) {
            if (isManifestEntry(f)) package.files.push_back({joinPath(folder, f), f});
        }
    }
    if (const PackageFile* v = package.find(L"VERSION.txt")) {
        if (const auto text = system.readFile(v->source)) {
            const std::vector<std::wstring> lines = textLines(*text);
            if (!lines.empty() && lines[0].size() <= 32) package.version = lines[0];
        }
    }
    return package;
}

bool withDrivers(const std::uint32_t build) { return build >= 22000; }

std::wstring windowsName(const std::uint32_t build) {
    if (build < 9200) return L"Windows 7";
    if (build < 9600) return L"Windows 8";
    if (build < 10240) return L"Windows 8.1";
    if (build < 22000) return L"Windows 10";
    return L"Windows 11";
}

std::wstring logFolderFor(SetupSystem& system, const std::wstring& userData, bool& linked) {
    const std::wstring data = joinPath(userData, L"wheelbri");
    const std::wstring logs = joinPath(data, L"logs");
    linked = userData.empty() || system.isLink(data) || system.isLink(logs);
    return linked ? joinPath(system.folder(Folder::SystemRoot), L"Temp") : logs;
}

bool isPackageFolder(SetupSystem& system, const std::wstring& folder) {
    return system.isFile(joinPath(folder, L"install.ps1")) || system.isFile(joinPath(folder, L"package-files.txt")) ||
           (system.isFile(joinPath(folder, L"wheelbri-setup.exe")) && !system.isFile(joinPath(folder, kManifest)));
}

bool removeInstallFolder(SetupSystem& s, const std::wstring& folderIn, Reporter& r) {
    const std::wstring folder = withoutTrailingSlash(folderIn);
    if (!exists(s, folder)) return true;
    if (s.isLink(folder)) {
        r.log(L"    " + folder + L" is a link (junction or symbolic link): not removed");
        return false;
    }
    const std::wstring manifest = joinPath(folder, kManifest);
    if (const auto text = s.readFile(manifest)) {
        const std::vector<std::wstring> entries = textLines(*text);
        for (const std::wstring& entry : entries) {
            if (!isManifestEntry(entry)) continue;
            if (!noLinkBelow(s, folder, entry)) {
                r.log(L"    skipped " + entry + L": it is reached through a link");
                continue;
            }
            const std::wstring path = joinPath(folder, entry);
            if (s.isFile(path)) removeTree(s, path);
        }
        for (const wchar_t* sub : {L"config", L"driver", L"shim"}) removeTree(s, joinPath(folder, sub));
        // The list goes last, and only with everything it names: a later install or uninstall still knows the rest.
        const bool left = std::any_of(entries.begin(), entries.end(), [&](const std::wstring& e) {
            return isManifestEntry(e) && !sameName(e, kManifest) && exists(s, joinPath(folder, e));
        });
        if (!left) s.removeFile(manifest);
        removeEmptyFolders(s, folder);
    } else {
        removeTree(s, folder);
    }
    return !exists(s, folder);
}

int install(SetupSystem& system, const Package& package, const Options& options, Reporter& reporter) {
    Installing i{system, package, options, reporter};
    i.build = system.windowsBuild();
    i.drivers = withDrivers(i.build);
    reporter.log(L"WheelBri " + package.version + L" setup, " + windowsName(i.build) + L" (build " + number(i.build) + L")");
    reporter.log(L"install into: " + options.installDir);
    try {
        setUpLogFolder(system, reporter, options.userData);
        i.checkPc();
        i.closeRunning();
        i.copyFiles();
        i.installDrivers();
        i.shortcuts();
        i.settingsEntry();
        i.check();
    } catch (const Stop&) {
        // Failed after the earlier install's helper was stopped: started again, so that install keeps working.
        if (i.helperWasThere) system.startHelper();
        return 1;
    }
    reporter.log(L"WheelBri " + package.version + L" is installed.");
    return 0;
}

int uninstall(SetupSystem& system, const Options& options, Reporter& reporter) {
    Uninstalling u{system, options, reporter};
    u.drivers = withDrivers(system.windowsBuild());
    u.data = joinPath(options.userData, L"wheelbri");
    const std::wstring& d = options.installDir;
    u.folderOk = d.size() > 3 && !system.isLink(d) && system.isFile(joinPath(d, L"wheelbri.exe")) &&
                 !isPackageFolder(system, d) &&
                 !sameName(withoutTrailingSlash(d), withoutTrailingSlash(system.folder(Folder::ProgramFiles))) &&
                 !isInside(d, system.folder(Folder::SystemRoot));
    reporter.log(L"WheelBri uninstall, " + windowsName(system.windowsBuild()));
    reporter.log(u.folderOk ? L"installed in: " + d
                            : L"no installation in '" + d + L"': removing what is left of one (shortcuts, the Settings > "
                              L"Apps entry, registry entries)");
    if (options.userData.empty() || system.isLink(u.data)) {
        reporter.fail(L"your WheelBri data folder (" + u.data + L") is a link or unknown: nothing was removed.");
        return 1;
    }
    try {
        u.closeRunning();
        u.shims();
        u.removeDrivers();
        u.entries();
        u.settings();
        u.logs();
        u.step(7, L"removing the program files");
        if (!u.folderOk) {
            reporter.log(L"    no install folder to remove");
        } else if (removeInstallFolder(system, d, reporter)) {
            reporter.log(L"    done");
        } else {
            reporter.warn(d + L" could not be removed completely (a file still in use, or files WheelBri did not put "
                              L"there): delete what is left after a restart");
        }
    } catch (const Stop&) {
        return 1;
    }
    reporter.log(L"WheelBri is removed.");
    return 0;
}

}  // namespace wheelbri::setup
