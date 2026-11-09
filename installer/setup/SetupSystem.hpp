// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Everything the install and the uninstall ask of Windows (SetupSteps.cpp), so the tests run them on a fake.
// SetupSystemWin32.cpp is the real one. Paths are full Windows paths; nothing here follows a link unless it says so.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wheelbri::setup {

enum class Hive { Machine, Users, CurrentUser };

enum class Folder {
    SystemRoot,      // C:\Windows
    System32,        // C:\Windows\System32 (the 64-bit one)
    ProgramFiles,    // the 64-bit Program Files
    ProgramFilesX86,
    ProgramData,
    CommonPrograms,  // the all-users Start menu's Programs
    CommonDesktop,   // the all-users desktop
    Temp,            // this process's %TEMP%
};

struct DirEntry {
    std::wstring name;
    bool folder = false;
    bool link = false;   // a junction or symbolic link (a reparse point)
};

struct LocalTime {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
};

class SetupSystem {
public:
    virtual ~SetupSystem() = default;

    // --- files
    [[nodiscard]] virtual bool isFile(const std::wstring& path) = 0;
    [[nodiscard]] virtual bool isFolder(const std::wstring& path) = 0;
    [[nodiscard]] virtual bool isLink(const std::wstring& path) = 0;
    // A folder's entries; empty when it cannot be read.
    [[nodiscard]] virtual std::vector<DirEntry> list(const std::wstring& folder) = 0;
    virtual bool makeFolders(const std::wstring& path) = 0;
    virtual bool copyFile(const std::wstring& from, const std::wstring& to) = 0;   // replaces `to`
    virtual bool moveFile(const std::wstring& from, const std::wstring& to) = 0;   // never replaces
    virtual bool removeFile(const std::wstring& path) = 0;      // read-only too; a link: only the link
    virtual bool removeFolder(const std::wstring& path) = 0;    // an empty folder, or a folder link (only the link)
    [[nodiscard]] virtual std::optional<std::string> readFile(const std::wstring& path) = 0;
    virtual bool writeFile(const std::wstring& path, const std::string& bytes) = 0;
    [[nodiscard]] virtual std::uint64_t fileSize(const std::wstring& path) = 0;

    // --- registry (64-bit view). Multi-strings as their lines joined by \n.
    [[nodiscard]] virtual std::optional<std::wstring> regText(Hive hive, const std::wstring& key,
                                                              const std::wstring& value) = 0;
    [[nodiscard]] virtual bool regKeyExists(Hive hive, const std::wstring& key) = 0;
    [[nodiscard]] virtual bool regValueExists(Hive hive, const std::wstring& key, const std::wstring& value) = 0;
    [[nodiscard]] virtual std::vector<std::wstring> regValueNames(Hive hive, const std::wstring& key) = 0;
    [[nodiscard]] virtual std::vector<std::wstring> regSubKeys(Hive hive, const std::wstring& key) = 0;
    virtual bool regSetText(Hive hive, const std::wstring& key, const std::wstring& value, const std::wstring& text) = 0;
    virtual bool regSetMultiText(Hive hive, const std::wstring& key, const std::wstring& value,
                                 const std::wstring& text) = 0;
    virtual bool regSetDword(Hive hive, const std::wstring& key, const std::wstring& value, std::uint32_t data) = 0;
    virtual void regDeleteValue(Hive hive, const std::wstring& key, const std::wstring& value) = 0;
    virtual void regDeleteTree(Hive hive, const std::wstring& key) = 0;   // the key with everything in it

    // --- the rest
    virtual bool shortcut(const std::wstring& path, const std::wstring& target, const std::wstring& arguments,
                          const std::wstring& workFolder, const std::wstring& description,
                          const std::wstring& icon) = 0;
    // Runs a command line, hidden, waits; each line it prints to `line`. -1 when it could not start.
    virtual int run(const std::wstring& commandLine, const std::wstring& workFolder,
                    const std::function<void(const std::wstring&)>& line) = 0;
    // Closes the running processes of these names (exe names without .exe): their windows first, after `seconds`
    // ends the rest. Returns how many ran; `ended` gets the names of those that had to be ended.
    virtual int closePrograms(const std::vector<std::wstring>& names, int seconds, std::vector<std::wstring>& ended) = 0;

    // The WheelBri Helper service (WheelBriHelper).
    [[nodiscard]] virtual bool helperExists() = 0;
    virtual void stopHelper() = 0;            // and waits for its process to end
    virtual bool deleteHelper() = 0;          // true when gone, or marked for deletion
    virtual std::wstring installHelper(const std::wstring& exe) = 0;   // registered and running: "", else why not
    virtual void startHelper() = 0;
    [[nodiscard]] virtual bool helperRunning() = 0;

    // --- this PC
    [[nodiscard]] virtual std::uint32_t windowsBuild() = 0;
    [[nodiscard]] virtual bool is64BitWindows() = 0;
    [[nodiscard]] virtual std::wstring folder(Folder which) = 0;
    // The SID of the user signed in to this session (the owner of its Explorer), else this process's user.
    [[nodiscard]] virtual std::wstring desktopUserSid() = 0;
    [[nodiscard]] virtual LocalTime now() = 0;
};

}  // namespace wheelbri::setup
