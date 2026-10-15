#pragma once

#include "UiWindow.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wheelbri::service {

// The top row of the app's Mapping tab: the target and how it runs, and so the config Start uses. Two pickers, each
// with < >:
//   - Target: the wheel WheelBri emulates: the Logitech G920, G25 or G27 (more come only when WheelBri supports them,
//     never from the user);
//   - Mode: Virtual (WheelBri's own driver, a device every game sees; Administrator; the rig is hidden) or Shim
//     (dinput8.dll next to the game's .exe presents the rig's wheel as the target to that game only).
// The choice is kept in the app's state file ([app] target, [app] way). Each target has one built-in config for both
// modes (config/g920.ini, config/g25.ini, config/g27.ini): the target's own inputs (fixed) and WheelBri's default
// mapping of the rig onto them. The user's own mapping, once saved on the Mapping tab, goes to
// <user configs>/my-<target>.ini (both modes too) and is used instead; "Default" deletes it. Switching Mode never
// changes the mapping.
// Where the virtual device cannot exist (Windows before 11: its driver needs build 22000), Mode stays Shim and its
// arrows are off; a saved "virtual" is read as Shim there and left in app.ini for a later Windows 11.
class ConfigPicker {
public:
    enum class Way { VirtualDevice, Shim };
    enum class Target { G920, G25, G27 };   // the Target arrows go through them in this order

    struct Settings {
        std::filesystem::path builtInFolder;   // config/ next to the program
        std::filesystem::path userFolder;      // userConfigFolder()
        std::filesystem::path stateFile;       // appStateFile()
        bool virtualAvailable = true;          // virtualDeviceSupported() on this PC
    };

    // The first Windows build the virtual device's driver installs on (its INF: Windows 11).
    static constexpr std::uint32_t kVirtualDeviceMinBuild = 22000;
    // Whether the virtual device can exist on a PC with `windowsBuild` (platform::windowsBuild(); 0 = not Windows or
    // unknown: Linux has its own virtual device, and an unknown Windows is given the benefit of the doubt).
    [[nodiscard]] static bool virtualDeviceSupported(std::uint32_t windowsBuild);

    ConfigPicker(ui::UiWindow& window, Settings settings);

    [[nodiscard]] Way way() const { return way_; }
    [[nodiscard]] Target target() const { return target_; }
    [[nodiscard]] bool virtualDevice() const { return way_ == Way::VirtualDevice; }
    // "G920 virtual device", "G25 in-game shim": for the log and the status.
    [[nodiscard]] std::string name() const;
    // The target's short name ("G920", "G25") and config name ("g920", "g25": target_profile, the shim's settings),
    // and the way's ("Virtual", "Shim"), as the row shows them.
    [[nodiscard]] static const char* targetName(Target target);
    [[nodiscard]] static const char* profileName(Target target);
    [[nodiscard]] const char* profileName() const { return profileName(target_); }
    [[nodiscard]] static const char* wayName(Way way);
    // The target a config name names ("g920", "g25"), the way a saved one does ("virtual", "shim"); nullopt for
    // anything else (an app.ini from a newer WheelBri, or edited by hand).
    [[nodiscard]] static std::optional<Target> parseTarget(std::string_view profile);
    [[nodiscard]] static std::optional<Way> parseWay(std::string_view way);
    // The built-in config's file name for the target ("g920.ini", "g25.ini"): one for both modes.
    [[nodiscard]] static std::string builtInName(Target target);
    [[nodiscard]] std::string builtInName() const { return builtInName(target_); }
    // The user's mapping files from before there was one for both modes (2026-10-15): "my-g920-virtual-device.ini",
    // "my-g920-in-game-shim.ini". Read once, when my-<target>.ini is not there yet (the newer of them is copied to it);
    // never changed or deleted.
    [[nodiscard]] static std::array<std::string, 2> olderOwnNames(Target target);

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

    // Draws the row at (x, y): "Target" < G920 | G25 | G27 >, then "Mode" < Virtual | Shim >. `canChange` is false
    // while a session runs or starts. Returns the x where the row ends.
    float draw(float x, float y, bool canChange);
    // A click on the tab; true if it was on the row.
    bool onClick(float x, float y);

private:
    struct Hit {
        float x, y, w, h;
        std::function<void()> action;
    };

    // my-<target>.ini from the older per-mode files (olderOwnNames), once per target.
    void adoptOlderOwn();
    // The other way or target (the arrows), kept in the state file; it stays chosen when that cannot be written.
    void choose(Way way);
    void choose(Target target);
    void chosen();   // after either: the config again, and the log

    ui::UiWindow& window_;
    Settings settings_;
    std::vector<Hit> hits_;
    Way way_ = Way::VirtualDevice;
    Target target_ = Target::G920;
    std::string selected_;
    bool builtIn_ = true;
    std::uint64_t changes_ = 0;
};

}  // namespace wheelbri::service
