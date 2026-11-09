// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "SetupSystem.hpp"

namespace wheelbri::setup {

// The real SetupSystem: Win32 only, nothing newer than Windows 7 SP1. The thread using it must have COM initialised
// (the shortcuts).
class SetupSystemWin32 final : public SetupSystem {
public:
    bool isFile(const std::wstring& path) override;
    bool isFolder(const std::wstring& path) override;
    bool isLink(const std::wstring& path) override;
    std::vector<DirEntry> list(const std::wstring& folder) override;
    bool makeFolders(const std::wstring& path) override;
    bool copyFile(const std::wstring& from, const std::wstring& to) override;
    bool moveFile(const std::wstring& from, const std::wstring& to) override;
    bool removeFile(const std::wstring& path) override;
    bool removeFolder(const std::wstring& path) override;
    std::optional<std::string> readFile(const std::wstring& path) override;
    bool writeFile(const std::wstring& path, const std::string& bytes) override;
    std::uint64_t fileSize(const std::wstring& path) override;

    std::optional<std::wstring> regText(Hive hive, const std::wstring& key, const std::wstring& value) override;
    bool regKeyExists(Hive hive, const std::wstring& key) override;
    bool regValueExists(Hive hive, const std::wstring& key, const std::wstring& value) override;
    std::vector<std::wstring> regValueNames(Hive hive, const std::wstring& key) override;
    std::vector<std::wstring> regSubKeys(Hive hive, const std::wstring& key) override;
    bool regSetText(Hive hive, const std::wstring& key, const std::wstring& value, const std::wstring& text) override;
    bool regSetMultiText(Hive hive, const std::wstring& key, const std::wstring& value, const std::wstring& text) override;
    bool regSetDword(Hive hive, const std::wstring& key, const std::wstring& value, std::uint32_t data) override;
    void regDeleteValue(Hive hive, const std::wstring& key, const std::wstring& value) override;
    void regDeleteTree(Hive hive, const std::wstring& key) override;

    bool shortcut(const std::wstring& path, const std::wstring& target, const std::wstring& arguments,
                  const std::wstring& workFolder, const std::wstring& description, const std::wstring& icon) override;
    int run(const std::wstring& commandLine, const std::wstring& workFolder,
            const std::function<void(const std::wstring&)>& line) override;
    int closePrograms(const std::vector<std::wstring>& names, int seconds, std::vector<std::wstring>& ended) override;

    bool helperExists() override;
    void stopHelper() override;
    bool deleteHelper() override;
    std::wstring installHelper(const std::wstring& exe) override;
    void startHelper() override;
    bool helperRunning() override;

    std::uint32_t windowsBuild() override;
    bool is64BitWindows() override;
    std::wstring folder(Folder which) override;
    std::wstring desktopUserSid() override;
    LocalTime now() override;
};

}  // namespace wheelbri::setup
