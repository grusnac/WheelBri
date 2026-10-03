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

// The app's Mapping tab while stopped: maps what the Wheel setup tab named (the wheel's and the accessories' axes
// and buttons) onto the emulated wheel's (G920) axes, buttons and D-pad, in the config picked on the Status page.
// Every source is picked from a list of the named controls. Save writes the config (comments and other settings
// kept; a built-in config is saved as a copy of your own, which is then picked); Revert drops the edits, and so
// does the Status page's Reset. The live view (the session's Mapping tab) shows the result once started.
class MappingEditView {
public:
    static constexpr float kContentHeight = 748 - 30;   // the tab bar is below

    // `rigsFolder` / `stateFile`: the rig the Wheel setup tab made (its active one, [app] rig), whose controls are
    // the sources.
    MappingEditView(ui::UiWindow& window, std::filesystem::path rigsFolder, std::filesystem::path stateFile,
                    ConfigPicker& picker);

    // Every frame while the tab shows: reads the config again when the picker changed it (or picked another),
    // and the devices' names now and then (the Wheel setup tab may just have saved some).
    void update();
    void draw();

    void onClick(float x, float y);
    void onText(const std::string& text);
    void onKey(char key);
    [[nodiscard]] bool editingText() const { return mode_ == Mode::SaveAs; }
    [[nodiscard]] bool dirty() const { return loaded_ && edit_ != saved_; }
    // A popup (pick a source, the wheel, Save as) covers the tab: the window's own controls on it wait.
    [[nodiscard]] bool popupOpen() const { return mode_ != Mode::Idle; }

private:
    enum class Mode { Idle, Pick, PickWheel, SaveAs };
    // What a pick is for: one of the four axes, a D-pad direction, a target button.
    enum class Slot { Axis, Hat, Button };

    struct Hit {
        float x, y, w, h;
        std::function<void()> action;
    };

    void load();
    void save();
    void saveAs();
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
    void drawSaveAs();
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
    std::string saveName_;
    std::string status_;
    bool statusError_ = false;
};

}  // namespace wheelbri::service
