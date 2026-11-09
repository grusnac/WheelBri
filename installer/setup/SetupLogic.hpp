// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The installer's rules that need no system: file lists, the config record, names, Firefox's policy text, the shim
// files and the command line. Portable (no windows.h), so the tests run everywhere; paths are Windows paths
// (backslashes) whatever the system.

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace wheelbri::setup {

[[nodiscard]] std::wstring fromUtf8(std::string_view text);
[[nodiscard]] std::string toUtf8(std::wstring_view text);

// Windows' file name comparison for the names WheelBri uses: ignores case.
[[nodiscard]] bool sameName(std::wstring_view a, std::wstring_view b);
struct NameLess {
    [[nodiscard]] bool operator()(const std::wstring& a, const std::wstring& b) const;
};
using NameSet = std::set<std::wstring, NameLess>;

// "<a>\<b>", one backslash between them.
[[nodiscard]] std::wstring joinPath(std::wstring_view a, std::wstring_view b);
// The folder part of a path ("" for a bare name).
[[nodiscard]] std::wstring parentOf(std::wstring_view path);
// The last part of a path.
[[nodiscard]] std::wstring nameOf(std::wstring_view path);
// A full path without its trailing backslash (but "C:\" keeps it).
[[nodiscard]] std::wstring withoutTrailingSlash(std::wstring_view path);
// Whether `path` is `folder` or inside it.
[[nodiscard]] bool isInside(std::wstring_view path, std::wstring_view folder);

// A line of installed-files.txt that stays inside the install folder: relative, no drive, no "/", no empty, "." or
// ".." part.
[[nodiscard]] bool isManifestEntry(std::wstring_view entry);

// The lines of a UTF-8 text file (a BOM and \r dropped, each trimmed, empty ones left out).
[[nodiscard]] std::vector<std::wstring> textLines(std::string_view utf8);
// Lines as such a file: UTF-8 with a BOM, CRLF after each (as Windows PowerShell's Set-Content wrote them).
[[nodiscard]] std::string linesFile(const std::vector<std::wstring>& lines);

// default-configs.txt: "config\<name>.ini<TAB><SHA-256, hex>" per default config of the installed version.
using ConfigHashes = std::map<std::wstring, std::wstring, NameLess>;
[[nodiscard]] ConfigHashes readDefaultConfigs(std::string_view utf8);
[[nodiscard]] std::string defaultConfigsFile(const ConfigHashes& hashes);

// package-files.txt, Setup.exe's list of its files (IExpress keeps no folders): "<flat name><TAB><relative path>".
// Lines whose flat name is not a plain file name or whose path is not a manifest entry are left out.
struct PackageEntry {
    std::wstring flatName;
    std::wstring relative;
};
[[nodiscard]] std::vector<PackageEntry> readPackageFiles(std::string_view utf8);

// The built-in configs' older names (removed with their .new copies at an upgrade).
[[nodiscard]] std::vector<std::wstring> olderConfigNames();

// Why `name` cannot be the Start menu folder (folders separated by \, each a valid file name); empty when it can.
[[nodiscard]] std::wstring startMenuNameProblem(std::wstring_view name);

// Firefox's Preferences policy (JSON, one object): HKEY_USERS\<user>\Software\Policies\Mozilla\Firefox.
struct FirefoxPolicy {
    enum class Result { Changed, AlreadyOff, NotOurs };   // NotOurs: not a JSON object, left as it is
    Result result = Result::NotOurs;
    std::string text;   // the new value (Changed); empty: remove the value (nothing left in it)
};
// dom.gamepad.enabled set to false (locked), the rest kept. `current` empty: no value yet.
[[nodiscard]] FirefoxPolicy turnOffGamepads(std::string_view current);
// The uninstall: dom.gamepad.enabled taken out when it is false, the rest kept. AlreadyOff here means: not ours to
// change (absent, or not false).
[[nodiscard]] FirefoxPolicy undoGamepads(std::string_view current);

// DirectInput's OEM entries WheelBri writes for its virtual wheels (service/src/VirtualWheelDevice.cpp
// writeDirectInputEntries), by device ids: the name it writes as OEMName (core/src/HidWheelDevice.cpp kVirtualWheels).
struct OemEntry {
    const wchar_t* ids;
    const wchar_t* name;
};
[[nodiscard]] const std::vector<OemEntry>& oemEntries();

// The games the app put the shim into: "game<TAB>name<TAB>folder" lines of shim-games.txt (the app's
// ShimGames.hpp), one per folder (the first), in file order. Whether the folder is still there is the caller's.
struct ShimGame {
    std::wstring name;
    std::wstring folder;
};
[[nodiscard]] std::vector<ShimGame> readShimGames(std::string_view utf8);
// A shim of ours (the bytes of a dinput8.dll): "wheelbri" is in it.
[[nodiscard]] bool isOurShim(std::string_view bytes);
// The Ultimate ASI Loader (and not our shim): another mod's dinput8.dll the app chained as dinput8Hooked.dll.
[[nodiscard]] bool isAsiLoader(std::string_view bytes);

// The command line. Install: wheelbri-setup.exe [--quiet] [--install-dir <folder>] [--start-menu-folder <name>]
// [--no-desktop-shortcut] [--stop-browser-gamepads] [--restart now|later]. Uninstall (--uninstall, or the exe named
// Uninstall*): [--quiet] [--keep-config | --remove-settings] [--remove-logs] [--keep-shims] [--remove-certificate].
// Both: --user-data <folder> (whose %LOCALAPPDATA%), --temp-copy (the uninstaller's own copy, internal).
struct Options {
    bool uninstall = false;
    bool quiet = false;
    bool tempCopy = false;
    std::wstring userData;
    std::wstring installDir;
    std::wstring startMenuFolder = L"WheelBri";
    bool desktopShortcut = true;
    bool stopBrowserGamepads = false;
    bool restartNow = false;
    bool keepConfig = false;
    bool removeSettings = false;
    bool removeLogs = false;
    bool keepShims = false;
    bool removeCertificate = false;
};
// Empty `problem` when the arguments (without the program's own name) are understood.
[[nodiscard]] Options parseOptions(const std::vector<std::wstring>& arguments, std::wstring& problem);
// The options again as arguments for the next copy (elevated, or the uninstaller's temp copy), quoted.
[[nodiscard]] std::wstring optionsCommandLine(const Options& options);

// The SHA-256 of `bytes` as upper-case hex (as PowerShell's Get-FileHash wrote it).
[[nodiscard]] std::wstring sha256Hex(std::string_view bytes);

}  // namespace wheelbri::setup
