#pragma once

#include "UiWindow.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace wheelbri::service {

// The top row of the app's Mapping tab: how the G920 reaches the game, and which config Start uses.
//
// WheelBri emulates one wheel, the Logitech G920, in one of two ways (stored in the app's state file, [app] way):
//   - the virtual G920: WheelBri's own driver, a device every game sees (Administrator; the rig is hidden);
//   - the in-game shim: dinput8.dll next to the game's .exe presents the rig's wheel as a G920 to that game only.
// The config is the built-in config/rig-as-g920.ini, a template the rig fills (rigMapping), unless the Mapping tab
// saved the user's own edits: <user configs>/my-g920.ini, made from it. "Rig mapping" drops those edits again. Other
// targets come only when WheelBri supports them (a profile, and a driver or the shim), never from the user.
class ConfigPicker {
public:
    enum class Way { VirtualDevice, Shim };

    struct Settings {
        std::filesystem::path builtInFolder;   // config/ next to the program
        std::filesystem::path userFolder;      // userConfigFolder()
        std::filesystem::path stateFile;       // appStateFile()
    };

    ConfigPicker(ui::UiWindow& window, Settings settings);

    [[nodiscard]] Way way() const { return way_; }
    [[nodiscard]] bool virtualDevice() const { return way_ == Way::VirtualDevice; }
    [[nodiscard]] static const char* describe(Way way);

    // The config Start uses: the user's own edits if saved, else the built-in template ("" when neither exists).
    [[nodiscard]] const std::string& selected() const { return selected_; }
    // Whether that is the built-in template (the rig fills it) rather than the user's own edits.
    [[nodiscard]] bool builtIn() const { return builtIn_; }
    [[nodiscard]] const std::filesystem::path& builtInPath() const { return builtInPath_; }
    // Where the user's own edits are saved (it may not exist yet).
    [[nodiscard]] const std::filesystem::path& ownPath() const { return ownPath_; }
    // Goes up each time the config Start uses changes (own edits saved or dropped): the Mapping tab reads it again.
    [[nodiscard]] std::uint64_t changes() const { return changes_; }
    // Looks for the files again (after the Mapping tab saved the user's edits).
    void refresh();
    // Deletes the user's own edits: the rig fills the built-in template again. False (and `error`) if it cannot.
    bool dropOwnEdits(std::string& error);

    // Draws the row at (x, y): "WAY" and the two choices. `canChange` is false while a session runs or starts.
    void draw(float x, float y, bool canChange);
    // A click on the tab; true if it was on the row.
    bool onClick(float x, float y);

private:
    struct Hit {
        float x, y, w, h;
        std::function<void()> action;
    };

    void choose(Way way);

    ui::UiWindow& window_;
    Settings settings_;
    std::vector<Hit> hits_;
    Way way_ = Way::VirtualDevice;
    std::filesystem::path builtInPath_;
    std::filesystem::path ownPath_;
    std::string selected_;
    bool builtIn_ = true;
    std::uint64_t changes_ = 0;
};

}  // namespace wheelbri::service
