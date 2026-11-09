// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The install and the uninstall, step by step, on a SetupSystem; what happens goes to a Reporter (the wizard's
// checklist and log, or the log alone with --quiet). The numbered steps and the item keys are the wizard's rows
// (SetupMain.cpp initRows).
//
// Install, 7 steps: 1 checking this PC (and, before Windows 10, the Universal C Runtime the app needs), 2 closing a
// running WheelBri, 3 copying the program, 4 the drivers and the WheelBri Helper (Windows 11 only; items
// wheel-driver, hide-driver, helper), 5 shortcuts, 6 Settings > Apps (and Firefox, when asked), 7 checking it.
// Uninstall, 7 steps: 1 closing WheelBri (item helper), 2 the shim in games, 3 the drivers (Windows 11 only; items
// hide-driver, wheel-driver, certificate), 4 shortcuts and registry, 5 settings, 6 logs, 7 the program files.
//
// The drivers' own steps stay PowerShell scripts in the install folder (driver\*.ps1; Windows 11, which has
// PowerShell 5.1): the wizard runs them there. Nothing else here needs PowerShell.

#include "SetupLogic.hpp"
#include "SetupSystem.hpp"

#include <string>
#include <vector>

namespace wheelbri::setup {

enum class ItemState { Running, Done, Warn, Failed };

class Reporter {
public:
    virtual ~Reporter() = default;
    virtual void step(int number, const std::wstring& text) = 0;
    virtual void item(const char* key, ItemState state, const std::wstring& note) = 0;
    virtual void note(const std::wstring& text) = 0;   // on the running step's row
    virtual void warn(const std::wstring& text) = 0;   // listed at the end
    virtual void log(const std::wstring& text) = 0;
    virtual void fail(const std::wstring& text) = 0;   // why it failed (the run then returns 1)
    virtual void restartNeeded() = 0;                  // Windows must restart to finish
};

// A file of the package: where it is now, and where it goes in the install folder.
struct PackageFile {
    std::wstring source;
    std::wstring relative;
};
struct Package {
    std::wstring version = L"0.0.0";
    std::wstring setupExe;   // this program (copied as Uninstaller.exe)
    std::vector<PackageFile> files;
    [[nodiscard]] const PackageFile* find(const std::wstring& relative) const;
};

// The package in `folder`: the unpacked zip (its files in their folders), or Setup.exe's flat files next to
// package-files.txt. Empty files when neither is there.
[[nodiscard]] Package readPackage(SetupSystem& system, const std::wstring& folder, const std::wstring& setupExe);

// The drivers (the virtual wheel, the hiding filter) and the WheelBri Helper need Windows 11 (build 22000).
[[nodiscard]] bool withDrivers(std::uint32_t build);
// The name people use for that Windows ("Windows 7", "Windows 8.1", ...).
[[nodiscard]] std::wstring windowsName(std::uint32_t build);

// The folder the logs go to, the user's %LOCALAPPDATA%\wheelbri\logs; %SystemRoot%\Temp when that folder (or the
// one above it) is a link, which an Administrator must not write through. `linked` says which.
[[nodiscard]] std::wstring logFolderFor(SetupSystem& system, const std::wstring& userData, bool& linked);

// Whether `folder` is a WheelBri package rather than an install (an install never keeps wheelbri-setup.exe).
[[nodiscard]] bool isPackageFolder(SetupSystem& system, const std::wstring& folder);

// Removes an install's files from `folder`: those in its installed-files.txt and WheelBri's subfolders, then the
// folders left empty; a folder without that list (from before it) whole. Never through a link. True when gone.
bool removeInstallFolder(SetupSystem& system, const std::wstring& folder, Reporter& reporter);

// options.installDir: a full path; options.userData: the signed-in user's %LOCALAPPDATA%. Returns the exit code
// (0 installed, 1 failed).
int install(SetupSystem& system, const Package& package, const Options& options, Reporter& reporter);

// options.installDir: the recorded install folder. Returns the exit code (0 removed, 1 failed).
int uninstall(SetupSystem& system, const Options& options, Reporter& reporter);

}  // namespace wheelbri::setup
