#pragma once

#include "UiWindow.hpp"

#include "wheelbri/ConfigLibrary.hpp"

#include <filesystem>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wheelbri::service {

// The app's mapping config picker, on its Status page: which config Start uses (< and >), New (a copy of a built-in
// config under a name of the user's, picked at once) and Reset (the picked config gets the built-in it was made
// from again, after a confirmation). Built-in configs are the defaults and are never changed. The pick is
// remembered in the app's state file. The files and the rules are core's ConfigLibrary.
class ConfigPicker {
public:
    static constexpr float kHeight = 124;   // what draw() takes, from its top

    struct Settings {
        std::filesystem::path builtInFolder;   // config/ next to the program
        std::filesystem::path userFolder;      // userConfigFolder()
        std::filesystem::path stateFile;       // appStateFile()
        std::string initial;                   // the config the app started with ("" = the built-in defaults)
    };

    ConfigPicker(ui::UiWindow& window, Settings settings);

    // The config Start uses (a path, or "" for the built-in defaults when there is no config at all).
    [[nodiscard]] const std::string& selected() const { return selected_; }
    // Its entry (null when there is no config), the list it is in, and the built-in it was made from (if any).
    [[nodiscard]] const ConfigEntry* current() const;
    [[nodiscard]] const std::vector<ConfigEntry>& configs() const { return configs_; }
    [[nodiscard]] const std::optional<ConfigEntry>& madeFrom() const { return resetFrom_; }
    // Goes up each time the picked config changes or its file is rewritten here (New, Reset, pick, adopt): the
    // Mapping tab then reads it again, dropping its unsaved edits.
    [[nodiscard]] std::uint64_t changes() const { return changes_; }
    // A config written elsewhere (the Mapping tab's Save as): listed again and picked, with `note` shown.
    void adopt(const std::filesystem::path& path, const std::string& note);
    [[nodiscard]] const std::filesystem::path& userFolder() const { return settings_.userFolder; }

    // Draws at (x, y); `canChange` is false while a session runs (the config is read at Start).
    void draw(float x, float y, bool canChange);
    // A click on the page; true if it was on the picker.
    bool onClick(float x, float y);
    // WindowInput::text while a new config's name is typed (printable characters, kBackspace, kEnter = Create).
    void onText(const std::string& text);
    // Escape closes New or the Reset question.
    void onKey(char key);
    // True while a name is typed: the window must not treat the keys as shortcuts then.
    [[nodiscard]] bool editingText() const { return mode_ == Mode::New; }

private:
    enum class Mode { Idle, New, ConfirmReset };

    struct Hit {
        float x, y, w, h;
        std::function<void()> action;
    };

    void refresh();
    void pick(std::size_t index);
    void create();
    void reset();
    void openFolder();
    void message(std::string text, bool error);

    float button(float x, float y, const std::string& label, std::function<void()> action, bool on = false,
                 bool enabled = true, float width = 0);

    ui::UiWindow& window_;
    Settings settings_;
    std::vector<Hit> hits_;

    std::vector<ConfigEntry> configs_;
    std::size_t index_ = 0;
    std::string selected_;
    std::optional<ConfigEntry> resetFrom_;   // the built-in Reset copies into the picked config, if any

    Mode mode_ = Mode::Idle;
    std::string newName_;
    std::size_t newFrom_ = 0;   // index into configs_ of the built-in New copies
    std::string message_;
    bool messageError_ = false;
    std::uint64_t changes_ = 0;
};

}  // namespace wheelbri::service
