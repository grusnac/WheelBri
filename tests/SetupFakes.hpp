// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A Windows in memory for the installer's steps (installer/setup/SetupSteps.cpp): files, folders, links, the
// registry, shortcuts, programs run and the WheelBri Helper service. Paths ignore case, as Windows does.

#include "SetupSteps.hpp"

#include <algorithm>
#include <cwctype>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace wheelbri::setup::testing {

inline std::wstring lowered(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](const wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

struct FakeShortcut {
    std::wstring target, arguments, workFolder, description, icon;
};

struct FakeValue {
    enum Type { Text, Multi, Dword } type = Text;
    std::wstring text;
    std::uint32_t dword = 0;
};

class FakeSystem final : public SetupSystem {
public:
    // --- set up
    std::uint32_t build = 22631;
    bool sixtyFourBit = true;
    std::wstring sid = L"S-1-5-21-1-2-3-1001";
    LocalTime time{2026, 11, 9, 14, 5, 6};
    std::map<Folder, std::wstring> folders = {
        {Folder::SystemRoot, L"C:\\Windows"},
        {Folder::System32, L"C:\\Windows\\System32"},
        {Folder::ProgramFiles, L"C:\\Program Files"},
        {Folder::ProgramFilesX86, L"C:\\Program Files (x86)"},
        {Folder::ProgramData, L"C:\\ProgramData"},
        {Folder::CommonPrograms, L"C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs"},
        {Folder::CommonDesktop, L"C:\\Users\\Public\\Desktop"},
        {Folder::Temp, L"C:\\Users\\me\\AppData\\Local\\Temp"},
    };
    // A command line containing the key returns its value (the first key found, in map order); others return 0.
    std::map<std::wstring, int> exitCodes;
    // Called for every command run (after the exit code is picked): lets a test act like the program.
    std::function<void(const std::wstring&)> onRun;
    // Lines a command containing the key prints.
    std::map<std::wstring, std::vector<std::wstring>> output;
    std::set<std::wstring> failCopyTo, failRemove, failWrite;   // lower-case paths
    int runningPrograms = 0;
    std::vector<std::wstring> endedPrograms;
    bool helper = false;
    bool helperIsRunning = false;
    std::wstring helperProblem;
    bool helperDeleteFails = false;

    // --- what happened
    std::vector<std::wstring> commands;
    std::map<std::wstring, FakeShortcut> shortcuts;   // by lower-case path
    std::wstring helperExe;
    int helperStarts = 0;
    int helperStops = 0;

    FakeSystem() {
        for (const auto& [which, path] : folders) makeFolders(path);
        makeFolders(L"C:\\Users\\me\\AppData\\Local");
        addFile(L"C:\\Windows\\System32\\ucrtbase.dll", "ucrt");
    }

    void addFile(const std::wstring& path, const std::string& bytes) {
        makeFolders(parentOf(path));
        files_[lowered(path)] = {path, bytes};
    }
    void addLink(const std::wstring& path, const bool folder) {
        makeFolders(parentOf(path));
        links_.insert(lowered(path));
        if (folder) folders_[lowered(path)] = path;
        else files_[lowered(path)] = {path, ""};
    }
    [[nodiscard]] std::string bytes(const std::wstring& path) const {
        const auto it = files_.find(lowered(path));
        return it == files_.end() ? std::string("<missing>") : it->second.bytes;
    }
    [[nodiscard]] bool has(const std::wstring& path) const {
        return files_.count(lowered(path)) > 0 || folders_.count(lowered(path)) > 0;
    }
    [[nodiscard]] FakeValue* value(const Hive hive, const std::wstring& key, const std::wstring& name) {
        const auto k = keys_.find(keyId(hive, key));
        if (k == keys_.end()) return nullptr;
        const auto v = k->second.find(lowered(name));
        return v == k->second.end() ? nullptr : &v->second.second;
    }
    void setMulti(const Hive hive, const std::wstring& key, const std::wstring& name, const std::wstring& text) {
        regSetMultiText(hive, key, name, text);
    }
    void addKey(const Hive hive, const std::wstring& key) { touchKey(hive, key); }
    [[nodiscard]] bool ran(const std::wstring& part) const {
        return std::any_of(commands.begin(), commands.end(), [&](const std::wstring& c) { return c.find(part) != std::wstring::npos; });
    }
    [[nodiscard]] size_t ranAt(const std::wstring& part) const {
        for (size_t i = 0; i < commands.size(); ++i) {
            if (commands[i].find(part) != std::wstring::npos) return i;
        }
        return commands.size();
    }
    [[nodiscard]] std::vector<std::wstring> filesUnder(const std::wstring& folder) const {
        std::vector<std::wstring> out;
        const std::wstring prefix = lowered(folder) + L"\\";
        for (const auto& [key, file] : files_) {
            if (key.rfind(prefix, 0) == 0) out.push_back(file.path.substr(prefix.size()));
        }
        return out;
    }

    // --- SetupSystem
    bool isFile(const std::wstring& path) override { return files_.count(lowered(path)) > 0; }
    bool isFolder(const std::wstring& path) override { return folders_.count(lowered(path)) > 0; }
    bool isLink(const std::wstring& path) override { return links_.count(lowered(path)) > 0; }
    std::vector<DirEntry> list(const std::wstring& folder) override {
        std::vector<DirEntry> entries;
        const std::wstring f = lowered(withoutTrailingSlash(folder));
        if (!folders_.count(f) || links_.count(f)) return entries;
        for (const auto& [key, path] : folders_) {
            if (lowered(parentOf(key)) == f && key != f) entries.push_back({nameOf(path), true, links_.count(key) > 0});
        }
        for (const auto& [key, file] : files_) {
            if (lowered(parentOf(key)) == f) entries.push_back({nameOf(file.path), false, links_.count(key) > 0});
        }
        return entries;
    }
    bool makeFolders(const std::wstring& path) override {
        const std::wstring p = withoutTrailingSlash(path);
        if (p.size() <= 2) return true;   // "C:"
        if (files_.count(lowered(p))) return false;
        if (!makeFolders(parentOf(p))) return false;
        folders_.emplace(lowered(p), p);
        return true;
    }
    bool copyFile(const std::wstring& from, const std::wstring& to) override {
        const auto source = files_.find(lowered(from));
        if (source == files_.end() || failCopyTo.count(lowered(to)) || !isFolder(parentOf(to)) || isFolder(to)) return false;
        files_[lowered(to)] = {to, source->second.bytes};
        return true;
    }
    bool moveFile(const std::wstring& from, const std::wstring& to) override {
        const auto source = files_.find(lowered(from));
        if (source == files_.end() || has(to) || !isFolder(parentOf(to)) || failRemove.count(lowered(from))) return false;
        const std::string data = source->second.bytes;
        files_.erase(source);
        files_[lowered(to)] = {to, data};
        return true;
    }
    bool removeFile(const std::wstring& path) override {
        if (failRemove.count(lowered(path))) return false;
        links_.erase(lowered(path));
        return files_.erase(lowered(path)) > 0;
    }
    bool removeFolder(const std::wstring& path) override {
        const std::wstring p = lowered(withoutTrailingSlash(path));
        if (failRemove.count(p) || !folders_.count(p)) return false;
        if (!links_.count(p) && !list(path).empty()) return false;
        links_.erase(p);
        folders_.erase(p);
        return true;
    }
    std::optional<std::string> readFile(const std::wstring& path) override {
        const auto it = files_.find(lowered(path));
        if (it == files_.end() || links_.count(lowered(path))) return std::nullopt;
        return it->second.bytes;
    }
    bool writeFile(const std::wstring& path, const std::string& data) override {
        if (failWrite.count(lowered(path)) || !isFolder(parentOf(path)) || isFolder(path)) return false;
        files_[lowered(path)] = {path, data};
        return true;
    }
    std::uint64_t fileSize(const std::wstring& path) override {
        const auto it = files_.find(lowered(path));
        return it == files_.end() ? 0 : it->second.bytes.size();
    }

    std::optional<std::wstring> regText(const Hive hive, const std::wstring& key, const std::wstring& name) override {
        const FakeValue* v = value(hive, key, name);
        if (!v || v->type == FakeValue::Dword) return std::nullopt;
        return v->text;
    }
    bool regKeyExists(const Hive hive, const std::wstring& key) override { return keys_.count(keyId(hive, key)) > 0; }
    bool regValueExists(const Hive hive, const std::wstring& key, const std::wstring& name) override {
        return value(hive, key, name) != nullptr;
    }
    std::vector<std::wstring> regValueNames(const Hive hive, const std::wstring& key) override {
        std::vector<std::wstring> names;
        const auto k = keys_.find(keyId(hive, key));
        if (k != keys_.end()) for (const auto& [lower, v] : k->second) names.push_back(v.first);
        return names;
    }
    std::vector<std::wstring> regSubKeys(const Hive hive, const std::wstring& key) override {
        std::vector<std::wstring> names;
        const std::wstring parent = keyId(hive, key);
        for (const auto& [id, values] : keys_) {
            if (lowered(parentOf(id)) == parent) names.push_back(nameOf(id));
        }
        return names;
    }
    bool regSetText(const Hive hive, const std::wstring& key, const std::wstring& name, const std::wstring& text) override {
        touchKey(hive, key)[lowered(name)] = {name, {FakeValue::Text, text, 0}};
        return true;
    }
    bool regSetMultiText(const Hive hive, const std::wstring& key, const std::wstring& name, const std::wstring& text) override {
        touchKey(hive, key)[lowered(name)] = {name, {FakeValue::Multi, text, 0}};
        return true;
    }
    bool regSetDword(const Hive hive, const std::wstring& key, const std::wstring& name, const std::uint32_t data) override {
        touchKey(hive, key)[lowered(name)] = {name, {FakeValue::Dword, L"", data}};
        return true;
    }
    void regDeleteValue(const Hive hive, const std::wstring& key, const std::wstring& name) override {
        const auto k = keys_.find(keyId(hive, key));
        if (k != keys_.end()) k->second.erase(lowered(name));
    }
    void regDeleteTree(const Hive hive, const std::wstring& key) override {
        const std::wstring id = keyId(hive, key);
        for (auto it = keys_.begin(); it != keys_.end();) {
            if (it->first == id || it->first.rfind(id + L"\\", 0) == 0) it = keys_.erase(it);
            else ++it;
        }
    }

    bool shortcut(const std::wstring& path, const std::wstring& target, const std::wstring& arguments,
                  const std::wstring& workFolder, const std::wstring& description, const std::wstring& icon) override {
        if (!isFolder(parentOf(path))) return false;
        shortcuts[lowered(path)] = {target, arguments, workFolder, description, icon};
        files_[lowered(path)] = {path, "lnk"};
        return true;
    }
    int run(const std::wstring& commandLine, const std::wstring&, const std::function<void(const std::wstring&)>& line) override {
        commands.push_back(commandLine);
        int code = 0;
        for (const auto& [part, exit] : exitCodes) {
            if (commandLine.find(part) != std::wstring::npos) {
                code = exit;
                break;
            }
        }
        for (const auto& [part, lines] : output) {
            if (commandLine.find(part) != std::wstring::npos) for (const std::wstring& l : lines) line(l);
        }
        if (onRun) onRun(commandLine);
        return code;
    }
    int closePrograms(const std::vector<std::wstring>&, int, std::vector<std::wstring>& ended) override {
        ended = endedPrograms;
        return runningPrograms;
    }

    bool helperExists() override { return helper; }
    void stopHelper() override {
        ++helperStops;
        helperIsRunning = false;
    }
    bool deleteHelper() override {
        if (helperDeleteFails) return false;
        helper = false;
        return true;
    }
    std::wstring installHelper(const std::wstring& exe) override {
        helperExe = exe;
        if (!helperProblem.empty()) return helperProblem;
        helper = helperIsRunning = true;
        return {};
    }
    void startHelper() override {
        ++helperStarts;
        helperIsRunning = helper;
    }
    bool helperRunning() override { return helperIsRunning; }

    std::uint32_t windowsBuild() override { return build; }
    bool is64BitWindows() override { return sixtyFourBit; }
    std::wstring folder(const Folder which) override { return folders.at(which); }
    std::wstring desktopUserSid() override { return sid; }
    LocalTime now() override { return time; }

private:
    struct File {
        std::wstring path;
        std::string bytes;
    };
    using Values = std::map<std::wstring, std::pair<std::wstring, FakeValue>>;

    static std::wstring keyId(const Hive hive, const std::wstring& key) {
        const wchar_t* root = hive == Hive::Machine ? L"hklm" : hive == Hive::Users ? L"hku" : L"hkcu";
        return lowered(joinPath(root, key));
    }
    Values& touchKey(const Hive hive, const std::wstring& key) {
        // Its parents exist too, as in the registry.
        for (std::wstring parent = parentOf(key); !parent.empty(); parent = parentOf(parent)) keys_[keyId(hive, parent)];
        return keys_[keyId(hive, key)];
    }

    std::map<std::wstring, File> files_;
    std::map<std::wstring, std::wstring> folders_;
    std::set<std::wstring> links_;
    std::map<std::wstring, Values> keys_;
};

struct FakeReporter final : Reporter {
    struct ItemReport {
        std::string key;
        ItemState state;
        std::wstring note;
    };
    std::vector<int> steps;
    std::vector<ItemReport> items;
    std::vector<std::wstring> notes, warnings, lines, failures;
    bool restart = false;

    void step(const int number, const std::wstring&) override { steps.push_back(number); }
    void item(const char* key, const ItemState state, const std::wstring& note) override { items.push_back({key, state, note}); }
    void note(const std::wstring& text) override { notes.push_back(text); }
    void warn(const std::wstring& text) override { warnings.push_back(text); }
    void log(const std::wstring& text) override { lines.push_back(text); }
    void fail(const std::wstring& text) override { failures.push_back(text); }
    void restartNeeded() override { restart = true; }

    [[nodiscard]] bool warned(const std::wstring& part) const {
        return std::any_of(warnings.begin(), warnings.end(), [&](const std::wstring& w) { return w.find(part) != std::wstring::npos; });
    }
    [[nodiscard]] bool logged(const std::wstring& part) const {
        return std::any_of(lines.begin(), lines.end(), [&](const std::wstring& w) { return w.find(part) != std::wstring::npos; });
    }
    [[nodiscard]] const ItemReport* last(const std::string& key) const {
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            if (it->key == key) return &*it;
        }
        return nullptr;
    }
};

}  // namespace wheelbri::setup::testing
