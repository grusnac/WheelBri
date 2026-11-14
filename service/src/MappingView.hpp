// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/DeviceLabels.hpp"
#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/FfbController.hpp"
#include "wheelbri/LiveLog.hpp"
#include "wheelbri/LogLevelCounter.hpp"
#include "wheelbri/MapperConfig.hpp"

#include "wheelbri/WheelState.hpp"

#include "ShadowView.hpp"
#include "UiWindow.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wheelbri::service {

// Live window for `--ui`: shows every axis and button of the physical wheel next to what the emulated
// wheel reports for it, so the mapping in the config can be checked by moving each control.
// Read-only: the mapping itself is edited in the INI file. Drawn with the system's own window tools
// (UiWindow.hpp: Win32 on Windows, X11 on Linux).
// What the window shows about game force feedback. `controller` is null when it is off or unavailable;
// `status` then says why.
struct FfbDisplay {
    const FfbController* controller = nullptr;
    std::string status;
    bool invert = false;
    std::uint64_t nowNs = 0;
};

// The telemetry files as last seen by the loop (a copy: the recorder belongs to the loop's thread).
struct TelemetryFiles {
    bool on = false;                              // false: `FilesDisplay::telemetryStatus` says why
    std::filesystem::path samplesPath;
    std::filesystem::path ffbPath;
    std::uint64_t rows = 0;
    std::uint64_t ffbRows = 0;
};

// Where the service writes its logs and telemetry, and what happens to old files (bottom of the window).
struct FilesDisplay {
    std::filesystem::path logFolder;
    bool logToFile = true;                        // false: the log file could not be opened
    TelemetryFiles telemetry;
    std::string telemetryStatus;
    std::string oldFiles;                         // Housekeeping::status()
    bool oldFilesFailed = false;
};

class MappingView {
public:
    // The panel's design size in logical pixels: the smallest canvas (UiWindow.hpp fitCanvas). A bigger window gives
    // it more room (the window's width() and height()), which every tab uses: wider bars, cells and columns, more log
    // and event rows, the tab bar at the bottom. It draws from the window's current origin.
    static constexpr int kWidth = 960;
    static constexpr int kHeight = 748;

    // Draws into `window` (not owned; it must outlive the view).
    MappingView(ui::UiWindow& window, std::string sourceName, int sourceAxes, int sourceButtons,
                const IDeviceProfile& profile, const MapperConfig& mapper);

    MappingView(const MappingView&) = delete;
    MappingView& operator=(const MappingView&) = delete;

    // The panel's tabs, switched with the buttons along the bottom (or M / S / G / R / L, and T / F when shown), in
    // this order: Rig, Mapping, Live and Games (the app only: setEditorTabs), Logs, then Telemetry and Force feedback
    // only when the program was started with --telemetry-tab / --ffb-tab (setOptionalTabs); without them those two
    // never show. The Rig tab (WheelSetup) and the Games tab are drawn by the window (WheelSetupView,
    // MappingEditView's Games page); this view then draws only the tab bar. In the app the Mapping tab is the
    // editor too (MappingEditView, the window draws it) and Live is this view's live page (Shadow / List: livePage);
    // without the editor (the command line service's --ui window) there is no Live tab and Mapping is the live page.
    enum class Tab { Mapping, Logs, Telemetry, ForceFeedback, WheelSetup, Games, Live };

    // The tab buttons along the bottom (this view's, and the app's own while stopped): from kTabLeft, each as wide as
    // its label plus kTabPadding on both sides, kTabGap apart, kTabHeight tall, kTabBarHeight from the bottom. Sized
    // to the label so all five tabs leave the bar's right end free for the app's Start and Stop.
    static constexpr float kTabLeft = 4.0F;
    static constexpr float kTabGap = 6.0F;
    static constexpr float kTabPadding = 12.0F;
    static constexpr float kTabHeight = 20.0F;
    static constexpr float kTabBarHeight = 28.0F;   // the bar: a line, 4 above the buttons, 4 below
    [[nodiscard]] static float tabWidth(const std::string_view label) {
        return static_cast<float>(label.size()) * 8.0F + 2.0F * kTabPadding;
    }
    // Where the last of visibleTabs() ends (what is right of it is free).
    [[nodiscard]] static float tabsEnd();

    // Whether the Telemetry and Force feedback tabs exist (--telemetry-tab, --ffb-tab); set once at start.
    static void setOptionalTabs(bool telemetry, bool forceFeedback);
    // How tall the Live tab's force feedback block is (drawFfbGroups).
    static constexpr float kFfbRowH = 12.0F;
    static constexpr float kFfbGroupsHeight = 16 + 6 * kFfbRowH + 2;
    // Whether the editor's tabs exist: the app's window has them (Mapping is its editor, Live this view's live page,
    // and Games), the command line service's --ui window does not.
    static void setEditorTabs(bool editor);
    [[nodiscard]] static bool editorTabs();
    // Whether this view draws its live page (Shadow / List) now: the Live tab, or Mapping without the editor's tabs.
    [[nodiscard]] bool livePage() const {
        return tab_ == Tab::Live || (tab_ == Tab::Mapping && (!editorTabs() || sideBySide_));
    }
    // The app's side by side: the window shows the Mapping tab's editor and this view's live page together (the app
    // draws them: drawLivePage), both tab buttons lit, S switches Shadow / List on either. The header's
    // "Side by side (B)" button asks for it (or back): takeSideBySideRequest.
    void setSideBySide(const bool on) { sideBySide_ = on; }
    [[nodiscard]] bool takeSideBySideRequest() { return std::exchange(sideBySideRequested_, false); }
    [[nodiscard]] static bool pairedTab(const Tab tab) { return tab == Tab::Mapping || tab == Tab::Live; }
    // The tabs shown, left to right.
    [[nodiscard]] static std::vector<Tab> visibleTabs();

    // A key pressed in the window: M / G / R / L (T / F when shown) switch tabs, O opens the logs (or telemetry) folder, D the drivers'
    // log folder; on the Logs tab the arrows, PageUp / PageDown and Home / End scroll the live log, Left / Right
    // sideways, W turns word wrap on or off, and V steps the log level (the Level button, in the app only).
    void onKey(char key);
    // Mouse wheel notches (> 0 = up): on the Logs tab, scrolls the live log.
    void onScroll(int notches);
    // Sideways wheel notches (> 0 = right): on the Logs tab, scrolls the live log's long lines.
    void onHScroll(int notches);
    // Every frame: where the mouse is (panel pixels) and whether the left button is held, to drag the live log's
    // scroll bars (a press on one comes through onClick first).
    void onPointer(float x, float y, bool leftDown);
    // A left click, in the panel's own logical pixels (the window's origin taken off): the tab buttons, and
    // on the Logs tab a component's line opens its folder.
    void onClick(float x, float y);
    [[nodiscard]] Tab tab() const { return tab_; }
    void setTab(const Tab tab) { tab_ = tab; }
    // The tab bar alone (for the Wheel setup tab, whose content the window draws).
    void drawTabBar() { drawTabs(); }
    // Only the Logs tab's page (no tab bar, no session needed): the app shows it while stopped and starting.
    void drawLogsPage() { drawLogs(); }
    // Opens the folder with the logs (or the telemetry, if it has its own) in the file manager.
    void openFilesFolder() const;

    // Shown at the bottom of the panel.
    void setFiles(FilesDisplay files) { files_ = std::move(files); }
    // The target G920 in-game shim: the Logs tab leaves out the virtual device's parts (G920 driver, hide filter),
    // which do not run then.
    // "Target G923  Mode Shim": what the session runs as (fixed from Start to Stop), at the right of the second header
    // line (the Mapping tab).
    void setSessionLine(std::string line) { sessionLine_ = std::move(line); }
    void setShimMode(const bool shim) {
        if (shim != shimMode_) filesCheckedMs_ = 0;   // the components list again at once
        shimMode_ = shim;
    }
    // The virtual devices' state, under the title (red when `warning`); empty: the usual hint.
    void setDevices(std::string devices, const bool warning) {
        devices_ = std::move(devices);
        devicesWarning_ = warning;
    }
    void setTelemetry(TelemetryFiles telemetry) { files_.telemetry = std::move(telemetry); }
    void setOldFiles(std::string status, const bool failed) {
        files_.oldFiles = std::move(status);
        files_.oldFilesFailed = failed;
    }

    // The accessories the mapping reads (device 1, 2, 3), with their names from the Wheel setup tab.
    // How much of the title the Mapping tab shows: the app puts its controls (and the shim's games) to its right.
    void setHeaderChars(const std::size_t chars) { headerChars_ = chars; }
    void setAccessories(const std::array<DeviceLabels, MapperConfig::kMaxAccessories>& accessories) {
        accessories_ = accessories;
        if (shadow_) shadow_->setAccessories(accessories);
    }
    // [buttons] order: the target's buttons show by the numbers the game sees them by (gameButtonNumber): Logitech's
    // driver counts from 1 ("Button 7"), the native order from 0. The INI's target_N stays 0-based, as named.
    void setButtonOrder(const ButtonOrder order) {
        buttonOrder_ = order;
        if (shadow_) shadow_->setButtonOrder(order);
    }

    // The mapping changed while the session runs (UiFrame::mapping, LiveMapping.hpp): both views show it from now on.
    void setMapping(const MapperConfig& mapper);
    [[nodiscard]] const MapperConfig& mapping() const { return mapper_; }

    // The Mapping tab's two views while running: Shadow (the target drawn, ShadowView.hpp) or List (every rig axis and
    // the mapped buttons, the target's and the rig's, next to what the target reports). Shadow only for a target with a picture (ShadowView::hasPicture), and
    // then the first one shown; the choice holds for the app's run (every session's view starts with it). S or the
    // header's Shadow / List buttons switch.
    [[nodiscard]] bool showingShadow() const { return shadow_ != nullptr && gShowShadow; }
    void showShadow(bool on);
    // The view a target with a picture starts on, for every view from now (tests start each on the List view).
    static void startOnShadow(const bool on) { gShowShadow = on; }

    // Draws the panel (between the window's begin() and present()); `inputs.devices[0]` is the wheel.
    void draw(const RawInputs& inputs, const WheelState& state, const TargetReport& report,
              const FfbDisplay& ffb = {});
    // Only the live page (its header, Shadow or List), without the tab bar: the app's side by side.
    void drawLivePage(const RawInputs& inputs, const WheelState& state, const TargetReport& report,
                      const FfbDisplay& ffb = {});

private:

    // The canvas now (at least kWidth x kHeight), and what it has beyond the design's width, in pixels.
    [[nodiscard]] float canvasW() const { return window_.width(); }
    [[nodiscard]] float canvasH() const { return window_.height(); }
    [[nodiscard]] float extraW() const { return std::max(0.0F, window_.width() - static_cast<float>(kWidth)); }
    [[nodiscard]] std::size_t extraChars() const { return static_cast<std::size_t>(extraW() / 8.0F); }

    // sourceToTarget_, sourceTargetCount_ and sourceHatDirection_ from mapper_.
    void indexMapping();
    void drawHeader();
    void drawLive(const RawInputs& inputs, const WheelState& state, const TargetReport& report, const FfbDisplay& ffb);
    // The Live tab's force feedback, per kind of effect (FfbGroups.hpp): what the game asks (blue) and what reaches the
    // wheel (green), from `x`, `y`, at most `width` wide. Returns where it ends.
    float drawFfbGroups(const FfbDisplay& ffb, float x, float y, float width);
    static constexpr float kSideBySideW = 16 * ui::kCharWidth + 12;   // "Side by side (B)"
    void drawAxes(const RawInputs& inputs, const WheelState& state, const TargetReport& report, float top);
    // "Rig 13 R paddle" on the wheel, "Pedals 2 Gas" on an accessory (cut to `chars`).
    [[nodiscard]] std::string sourceText(int device, int index, bool axis, std::size_t chars) const;
    void drawRawAxes(const RawInputState& raw, float top);
    void drawHat(std::int8_t sourceHat, std::int8_t targetHat, float top);
    void drawTargetButtons(std::uint32_t targetButtons, float top);
    void drawSourceButtons(const SourceButtons& sourceButtons, float top);
    void drawFooter(float top);
    void drawFfb(const FfbDisplay& ffb, float top);
    void drawFfbUsage(const FfbController& controller, float top);
    void drawTabs();
    void drawLogs();
    void refreshLogFiles();
    void drawLiveLog(float top);
    void drawTelemetry();
    void drawForceFeedback(const FfbDisplay& ffb);
    void trackEvents(const RawInputState& raw, const WheelState& state, const TargetReport& report,
                     const FfbDisplay& ffb);
    void addEvent(int kind, std::string text);
    void scrollLiveLog(long long lines);
    void openFolder(const std::filesystem::path& folder) const;
    void trackLastPressed(const SourceButtons& sourceButtons);
    // The name of source button `source` from [button_labels], or an empty string.
    [[nodiscard]] const std::string& sourceLabel(int source) const;

    ui::UiWindow& window_;
    std::unique_ptr<ShadowView> shadow_;   // null: no picture for this target
    static inline bool gShowShadow = true;   // the view chosen last (held for the app's run)
    std::string sourceName_;
    std::size_t headerChars_ = 50;
    int sourceAxes_;
    int sourceButtons_;
    const IDeviceProfile& profile_;
    MapperConfig mapper_;
    bool sideBySide_ = false;
    bool sideBySideRequested_ = false;
    float sideBySideX_ = -1000.0F;   // where the header drew Side by side (none yet)
    std::array<DeviceLabels, MapperConfig::kMaxAccessories> accessories_{};
    std::string target_;            // short name of the emulated wheel ("G920")
    ButtonOrder buttonOrder_ = ButtonOrder::Driver;
    std::string pedalNote_;         // how the target encodes released / pressed pedals

    // sourceToTarget_[s] = first target button fed by source button s, or kUnmapped; count for duplicates.
    std::array<int, MapperConfig::kMaxSourceButtons> sourceToTarget_{};
    std::array<int, MapperConfig::kMaxSourceButtons> sourceTargetCount_{};
    // sourceHatDirection_[s] = D-pad direction ("up", "right", ...) source button s drives, or nullptr.
    std::array<const char*, MapperConfig::kMaxSourceButtons> sourceHatDirection_{};

    FilesDisplay files_;
    std::string devices_;
    bool devicesWarning_ = false;
    Tab tab_ = Tab::Mapping;

    // One line of the Logs tab: a part of wheel-emulator and today's log file it writes.
    struct ComponentLog {
        const char* name;               // "WheelBri", "G920 driver"
        const char* what;               // what it is
        std::filesystem::path folder;   // where its files are
        std::string file;               // today's file name, empty if it has written none today
        std::string status;             // size and age, or why there is none
        std::string state;              // the WheelBri Helper's: what the app last heard from it (else empty)
        bool warns = false;             // its file has warnings or errors today: the row in the warning colour
    };
    std::vector<ComponentLog> components_;
    // Looked up at most once a second (files appear while a session runs and change at midnight).
    std::uint64_t filesCheckedMs_ = 0;
    bool shimMode_ = false;
    std::string sessionLine_;
    // The warnings and errors in today's helper log (the Helper row turns to the warning colour).
    LogLevelCounter helperLevels_;

    // The Logs tab's live log: every component's file followed, newest at the bottom.
    LiveLog liveLog_;
    std::uint64_t livePolledMs_ = 0;
    std::size_t liveScroll_ = 0;        // lines scrolled up from the newest; 0 = follow new lines
    std::uint64_t liveSeen_ = 0;        // liveLog_.added() when last drawn (to hold the view while scrolled up)
    std::size_t liveRows_ = 1;          // lines that fit, from the last draw
    std::size_t liveMessageChars_ = 80;   // a message row, to the scroll bar (from the canvas width, each draw)
    std::size_t liveHScroll_ = 0;       // characters scrolled right in the messages (without word wrap)
    std::size_t liveLongest_ = 0;       // the longest message, in characters (from the last draw)
    bool liveWrap_ = false;             // word wrap: long messages go on over several rows instead of off the side
    // The scroll bars as last drawn, for clicks and drags.
    struct Bar {
        float x = 0, y = 0, w = 0, h = 0;              // the track
        float thumbStart = 0, thumbSize = 0;           // along it
        bool shown = false;
    };
    Bar vBar_;
    Bar hBar_;
    float wrapX_ = 0, wrapY_ = 0, wrapW_ = 0;           // the Wrap button
    float levelX_ = 0, levelY_ = 0, levelW_ = 0;        // the Level button (0 wide: not shown)
    enum class Drag { None, Vertical, Horizontal };
    Drag drag_ = Drag::None;
    float dragGrab_ = 0;                // where on the thumb it was taken
    void dragTo(float x, float y);
    [[nodiscard]] std::size_t liveMostScroll() const;

    // The Telemetry tab's latest events (buttons, D-pad, pedals, game force feedback), newest last.
    struct TelemetryEvent {
        std::string time;               // "22:49:33.488"
        int kind;                       // index into the event colours
        std::string text;
    };
    std::deque<TelemetryEvent> events_;
    bool eventsStarted_ = false;        // the first frame only sets the starting state
    SourceButtons eventButtons_;
    std::int8_t eventHat_ = -1;
    std::array<bool, 3> pedalDown_{};
    std::array<float, 3> pedalPeak_{};
    std::array<bool, 8> ffbPlaying_{};


    SourceButtons previousSourceButtons_;
    int lastPressedSource_ = kUnmapped;
};

}  // namespace wheelbri::service
