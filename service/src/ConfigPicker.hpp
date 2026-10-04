#pragma once

#include "UiWindow.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace wheelbri::service {

// The top row of the app's Mapping tab: the target, and so the config Start uses.
//
// WheelBri emulates one wheel, the Logitech G920, in one of two ways, picked with < > like the Rig tab's device:
//   - G920 virtual device: WheelBri's own driver, a device every game sees (Administrator; the rig is hidden);
//   - G920 in-game shim: dinput8.dll next to the game's .exe presents the rig's wheel as a G920 to that game only.
// The choice is kept in the app's state file ([app] way). Each target has a built-in config (config/g920-virtual-
// device.ini, config/g920-in-game-shim.ini): the target's own inputs (fixed) and WheelBri's default mapping of the rig
// onto them. The user's own mapping, once saved on the Mapping tab, goes to <user configs>/my-<built-in name> and is
// used instead; "Default" deletes it. Other targets come only when WheelBri supports them, never from the user.
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
    // "G920 virtual device", "G920 in-game shim".
    [[nodiscard]] static const char* name(Way way);
    // The built-in config's file name for a way ("g920-virtual-device.ini").
    [[nodiscard]] static const char* builtInName(Way way);

    // The config Start uses: the user's own mapping if saved, else the built-in default ("" when neither exists).
    [[nodiscard]] const std::string& selected() const { return selected_; }
    // Whether that is the built-in default rather than the user's own mapping.
    [[nodiscard]] bool builtIn() const { return builtIn_; }
    [[nodiscard]] std::filesystem::path builtInPath() const;
    // Where the user's own mapping for the target is saved (it may not exist yet).
    [[nodiscard]] std::filesystem::path ownPath() const;
    // Goes up each time the config Start uses changes (another target, own mapping saved or dropped).
    [[nodiscard]] std::uint64_t changes() const { return changes_; }
    // Looks for the files again (after the Mapping tab saved the user's mapping).
    void refresh();
    // Deletes the user's own mapping for the target: the default again. False (and `error`) if it cannot.
    bool dropOwnEdits(std::string& error);

    // Draws the row at (x, y): "Target", <, the target's name, >. `canChange` is false while a session runs or starts.
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
    std::string selected_;
    bool builtIn_ = true;
    std::uint64_t changes_ = 0;
};

}  // namespace wheelbri::service
