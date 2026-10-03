#pragma once

#include "ConfigPicker.hpp"
#include "UiWindow.hpp"

#include "wheelbri/DeviceLabels.hpp"
#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/MappingEdit.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wheelbri::service {

// The app's Mapping tab while stopped: at the top how the G920 reaches the game (ConfigPicker: the virtual device or
// the in-game shim), then what drives each of the G920's axes, buttons and D-pad directions, from the controls the
// Rig tab named (the wheel's and the accessories'). With no edits of the user's the mapping is the rig's
// (rigMapping on the built-in config/rig-as-g920.ini: roles, then same-named controls), exactly what Start uses.
// Every source can be picked by hand from the named controls; Save keeps those edits (my-g920.ini, comments and
// other settings kept), Revert drops unsaved ones, Rig mapping drops the saved ones. The live view (the session's
// Mapping tab) shows the result once started.
class MappingEditView {
public:
    static constexpr float kContentHeight = 748 - 30;   // the tab bar is below

    // `rigsFolder` / `stateFile`: the rig the Wheel setup tab made (its active one, [app] rig), whose controls are
    // the sources.
    MappingEditView(ui::UiWindow& window, std::filesystem::path rigsFolder, std::filesystem::path stateFile,
                    ConfigPicker& picker);

    // Every frame while the tab shows: reads the config again when it changed, and the rig now and then (the Rig tab
    // may just have saved it; the rig's mapping follows it).
    void update();
    void draw();
    // While a session runs or starts the way and the config cannot change.
    void setLocked(const bool locked) { locked_ = locked; }

    void onClick(float x, float y);
    void onKey(char key);
    [[nodiscard]] bool dirty() const { return loaded_ && edit_ != saved_; }
    // A popup (pick a source, the wheel) covers the tab: the window's own controls on it wait.
    [[nodiscard]] bool popupOpen() const { return mode_ != Mode::Idle; }

private:
    enum class Mode { Idle, Pick, PickWheel };
    // What a pick is for: one of the four axes, a D-pad direction, a target button.
    enum class Slot { Axis, Hat, Button };

    struct Hit {
        float x, y, w, h;
        std::function<void()> action;
    };

    void load();
    void save();
    void dropOwnEdits();
    void refreshDevices();
    void changeWheel(const DeviceLabels& wheel);
    void autoMapFromRig();
    [[nodiscard]] const RigControl* rigControl(const SourceRef& source, bool axis) const;

    [[nodiscard]] const DeviceLabels* wheelLabels() const;
    [[nodiscard]] const DeviceLabels* labelsOf(const SourceRef& source) const;
    [[nodiscard]] std::string describe(const SourceRef& source, bool axis) const;
    [[nodiscard]] SourceRef& slot(Slot kind, std::size_t index);

    void drawSource(float x, float y, float w, Slot kind, std::size_t index);
    void drawPicker();
    void drawWheelPicker();
    float button(float x, float y, const std::string& label, std::function<void()> action, bool on = false,
                 bool enabled = true, float width = 0);
    void hit(float x, float y, float w, float h, std::function<void()> action);

    ui::UiWindow& window_;
    std::filesystem::path rigsFolder_;
    std::filesystem::path stateFile_;
    std::string rigName_;
    Rig rig_;
    ConfigPicker& picker_;
    std::vector<Hit> hits_;

    std::uint64_t seenChanges_ = ~0ULL;
    std::string path_;
    bool builtIn_ = false;
    std::string fileText_;
    bool loaded_ = false;
    std::unique_ptr<IDeviceProfile> profile_;
    MappingModel saved_;
    MappingModel edit_;
    std::vector<DeviceLabels> devices_;
    std::uint64_t devicesReadMs_ = 0;

    Mode mode_ = Mode::Idle;
    Slot pickKind_ = Slot::Axis;
    std::size_t pickIndex_ = 0;
    bool locked_ = false;
    std::string status_;
    bool statusError_ = false;
};

}  // namespace wheelbri::service
