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

// The app's Mapping tab while stopped: at the top the target (ConfigPicker: G920 virtual device or G920 in-game shim),
// whose axes, buttons and D-pad are fixed, then what drives each of them, from the controls the Rig tab named (the
// wheel's and the accessories'). Without a saved mapping of the user's it is WheelBri's default (the target's built-in
// config), exactly what Start uses. Every source can be picked by hand from the named controls, or Auto-mapped from
// their roles; Save keeps the user's mapping (my-<built-in name>, comments and other settings kept), Revert drops
// unsaved changes, Default drops the saved mapping. A source the rig has not named shows "not in the rig". The live
// view (the session's Mapping tab) shows the result once started.
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
    void setLocked(bool locked);
    // The Rig tab has changes it has not saved: Auto-map (which reads the saved rig) warns.
    void setRigUnsaved(const bool unsaved) { rigUnsaved_ = unsaved; }

    void onClick(float x, float y);
    void onKey(char key);
    // Typing in a number field (a rotation): printable characters, ui::kBackspace, ui::kEnter.
    void onText(const std::string& text);
    // The pointer every frame (panel coordinates): drags the game full lock slider while the left button is held.
    void onPointer(float x, float y, bool leftDown);
    // A number field is being typed in: letters are not the window's shortcuts then, Escape stops typing.
    [[nodiscard]] bool editingText() const { return field_ != Field::None; }
    [[nodiscard]] bool dirty() const { return loaded_ && edit_ != saved_; }
    // A popup (pick a source, the wheel) covers the tab: the window's own controls on it wait.
    [[nodiscard]] bool popupOpen() const { return mode_ != Mode::Idle; }

private:
    enum class Mode { Idle, Pick, PickWheel };
    // What a pick is for: one of the four axes, a D-pad direction, a target button.
    enum class Slot { Axis, Hat, Button };
    // A number field: one of the two rotations, or a game's own rotation (`fieldIndex_` = its gameProfiles() index).
    enum class Field { None, GameRotation, WheelRotation, PerGame };

    struct Hit {
        float x, y, w, h;
        std::function<void()> action;
    };

    void load();
    // A mapped source that the active rig has not named (the default was made for another rig).
    [[nodiscard]] bool missing(const SourceRef& source, bool axis) const;
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
    void drawWheelRow();   // the wheel (it plays force feedback), Change, Auto-map
    void drawPicker();
    void drawWheelPicker();
    // The STEERING ROTATION section from `y`: the game's full lock, the wheel's rotation, the games' advice.
    void drawRotation(float y);
    // A number field showing `value` (or what is typed in it); amber when it differs from the saved mapping.
    void drawField(float x, float y, float w, Field field, const std::string& value, bool changed,
                   std::size_t index = 0);
    [[nodiscard]] std::string fieldText(Field field, std::size_t index) const;
    void startTyping(Field field, std::size_t index = 0);
    // Applies what was typed: false (and the reason in the status) when it is not a usable value.
    bool commitTyping();
    void setGameRotationFromX(float x);
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
    bool rigUnsaved_ = false;
    std::string status_;
    bool statusError_ = false;

    std::string targetName_ = "g920";   // [emulator] target_profile: which games' advice fits
    Field field_ = Field::None;
    std::size_t fieldIndex_ = 0;
    std::string typed_;
    bool dragging_ = false;   // the slider, from a press on it until the left button is let go
    float clickX_ = 0.0F;     // where the click being answered is (the slider's press)
};

}  // namespace wheelbri::service
