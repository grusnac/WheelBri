#pragma once

#include "UiWindow.hpp"

#include "wheelbri/DeviceProfile.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wheelbri::service {

// The top row of the app's Mapping tab: the target and how it runs, and so the config Start uses. Two rows of buttons
// (segmented: every choice shown, the chosen one filled, one click picks another):
//   - Target: the wheel WheelBri emulates: the Logitech G25, G27, G920 or G923 (more come only when WheelBri supports
//     them, never from the user);
//   - Mode: Virtual (WheelBri's own driver, a device every game sees; Administrator; the rig is hidden) or Shim
//     (dinput8.dll next to the game's .exe presents the rig's wheel as the target to that game only).
// The choice is kept in the app's state file ([app] target, [app] way). Each target has one built-in config for both
// modes (config/g920.ini, g25.ini, g27.ini, g923.ini): the target's own inputs (fixed) and WheelBri's default
// mapping of the rig onto them. The user's own mapping, once saved on the Mapping tab, goes to
// <user configs>/my-<target>.ini (both modes too) and is used instead; "Default" deletes it. Switching Mode never
// changes the mapping.
//   - Order (the G25 and G27 only): the order their buttons reach the game in (ButtonOrder): Logitech driver or
//     Native. Each order has its own whole mapping: my-<target>.ini for Logitech driver, my-<target>-native.ini for
//     Native (WheelBri's default for both is <target>.ini; with Native, Start overrides its [buttons] order). The
//     first time Native is picked while the user has their own Logitech driver mapping, that is copied to the Native
//     file and [app] renumbered_<target> = pending says its buttons' game numbers changed (the Mapping tab marks
//     them until each is changed or "Got it"). Kept in [app] order_<target>; a my-<target>.ini that said
//     order = native before there were two (2026-10-26) becomes the Native mapping once (adoptOlderOrder).
// Where the virtual device cannot exist (a Windows older than its driver, kVirtualDeviceMinBuild), the Mode row has no
// Virtual button: only Shim, chosen and locked, its hover text saying which Windows this is and why; a saved "virtual"
// is read as Shim there and left in app.ini for a later Windows that has it.
class ConfigPicker {
public:
    enum class Way { VirtualDevice, Shim };
    enum class Target { G25, G27, G920, G923 };   // the Target buttons, left to right

    struct Settings {
        std::filesystem::path builtInFolder;   // config/ next to the program
        std::filesystem::path userFolder;      // userConfigFolder()
        std::filesystem::path stateFile;       // appStateFile()
        bool virtualAvailable = true;          // virtualDeviceSupported() on this PC
        std::string windowsName;               // windowsName() of this PC, for the Shim-only hover text
    };

    // The first Windows build the virtual device's driver installs on (its INF: Windows 11).
    static constexpr std::uint32_t kVirtualDeviceMinBuild = 22000;
    // Whether the virtual device can exist on a PC with `windowsBuild` (platform::windowsBuild(); 0 = not Windows or
    // unknown: Linux has its own virtual device, and an unknown Windows is given the benefit of the doubt).
    [[nodiscard]] static bool virtualDeviceSupported(std::uint32_t windowsBuild);
    // The Windows a build number is, as people name it ("Windows 7", "Windows 8.1", "Windows 10"); empty for 0 (not
    // Windows, or unknown).
    [[nodiscard]] static std::string windowsName(std::uint32_t windowsBuild);

    ConfigPicker(ui::UiWindow& window, Settings settings);

    [[nodiscard]] Way way() const { return way_; }
    [[nodiscard]] Target target() const { return target_; }
    [[nodiscard]] bool virtualDevice() const { return way_ == Way::VirtualDevice; }
    // "G920 Mode Virtual", "G25 Mode Shim", "G25 Native Mode Shim": for the log and the status.
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

    // Whether the target has two button orders, each with its own mapping (the G25 and G27).
    [[nodiscard]] static bool hasButtonOrders(Target target);
    // The order picked for the current target (Driver for one without two orders): Start runs with it.
    [[nodiscard]] ButtonOrder buttonOrder() const { return order_; }
    // The user's own mapping file's name for a target and order: "my-g25.ini", "my-g25-native.ini".
    [[nodiscard]] static std::string ownName(Target target, ButtonOrder order);
    // [app] renumbered_<target>: "pending" right after the copy, then the Mapping tab's list of marked buttons.
    [[nodiscard]] static std::string renumberedKey(Target target);

    // The config Start uses: the user's own mapping if saved, else the built-in default ("" when neither exists).
    [[nodiscard]] const std::string& selected() const { return selected_; }
    // Whether that is the built-in default rather than the user's own mapping.
    [[nodiscard]] bool builtIn() const { return builtIn_; }
    [[nodiscard]] std::filesystem::path builtInPath() const;
    // Where the user's own mapping for the target is saved (it may not exist yet).
    [[nodiscard]] std::filesystem::path ownPath() const;
    // Goes up each time the config Start uses changes (another target or order, own mapping saved or dropped).
    [[nodiscard]] std::uint64_t changes() const { return changes_; }
    // Looks for the files again (after the Mapping tab saved the user's mapping).
    void refresh();
    // Deletes the user's own mapping for the target: the default again. False (and `error`) if it cannot.
    bool dropOwnEdits(std::string& error);

    // From Start until Stop the Target and the Mode cannot change: the app sets it every frame. Locked, the buttons are
    // drawn greyed (the chosen ones still marked) and nothing changes them: no click (not even on buttons drawn before
    // the lock), no choose.
    void setLocked(const bool locked) { locked_ = locked; }
    [[nodiscard]] bool locked() const { return locked_; }
    // A target chosen from elsewhere on the Mapping tab (its "Back to the G920"); nothing while locked.
    void useTarget(const Target target) { choose(target); }
    // A button order chosen from elsewhere (tests, as its button); nothing while locked or for a target without two.
    void useButtonOrder(const ButtonOrder order) { choose(order); }

    // Where a button's hover text goes (x, y, w, h, text): the Mapping tab's own tooltips.
    using TipSink = std::function<void(float, float, float, float, std::string)>;
    // Draws the row at (x, y): "Target" [G25|G27|G920|G923], "Mode" [Virtual|Shim], and for the G25 and G27 a second
    // row under it: "Order" [Logitech driver|Native]; each button says what it is in its hover text (to `tip`, when
    // given), a greyed one why. `canChange` is false while a session runs or starts. Returns the x where the first
    // row ends.
    float draw(float x, float y, bool canChange, const TipSink& tip = {});
    // How tall what draw draws is: one row, or two with Order.
    [[nodiscard]] float rowsHeight() const;
    // A click on the tab; true if it was on the row.
    bool onClick(float x, float y);

private:
    struct Hit {
        float x, y, w, h;
        std::function<void()> action;
    };

    // my-<target>.ini from the older per-mode files (olderOwnNames), once per target.
    void adoptOlderOwn();
    // [app] order_<target> once: native when my-<target>.ini says order = native (copied to my-<target>-native.ini
    // if that is not there, its own line set back to driver), else driver.
    void adoptOlderOrder();
    // The order kept for the current target (Driver for one without two).
    void readOrder();
    // Another way or target (its button), kept in the state file; it stays chosen when that cannot be written.
    void choose(Way way);
    void choose(Target target);
    void choose(ButtonOrder order);
    void chosen();   // after either: the config again, and the log

    ui::UiWindow& window_;
    Settings settings_;
    std::vector<Hit> hits_;
    bool locked_ = false;
    Way way_ = Way::VirtualDevice;
    Target target_ = Target::G920;
    ButtonOrder order_ = ButtonOrder::Driver;
    std::string selected_;
    bool builtIn_ = true;
    std::uint64_t changes_ = 0;
};

}  // namespace wheelbri::service
