#pragma once

#include "UiWindow.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace wheelbri::service {

// The top row of the app's Mapping tab: the target and how it runs, and so the config Start uses. Two pickers, each
// with < >:
//   - Target: the wheel WheelBri emulates. One for now, the Logitech G920 (more come only when WheelBri supports them,
//     never from the user; docs/g25-target-plan.md);
//   - Run as: Virtual (WheelBri's own driver, a device every game sees; Administrator; the rig is hidden) or Shim
//     (dinput8.dll next to the game's .exe presents the rig's wheel as the target to that game only).
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
    // "G920 virtual device", "G920 in-game shim": for the log and the status.
    [[nodiscard]] static const char* name(Way way);
    // The target's short name ("G920"), and the way's ("Virtual", "Shim"), as the row shows them.
    [[nodiscard]] static const char* targetName();
    [[nodiscard]] static const char* wayName(Way way);
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

    // Draws the row at (x, y): "Target" < G920 >, then "Run as" < Virtual | Shim >. `canChange` is false while a
    // session runs or starts. Returns the x where the row ends.
    float draw(float x, float y, bool canChange);
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
