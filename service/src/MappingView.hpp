#pragma once

#include "wheelbri/DeviceLabels.hpp"
#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/FfbController.hpp"
#include "wheelbri/LiveLog.hpp"
#include "wheelbri/MapperConfig.hpp"

#include "wheelbri/WheelState.hpp"

#include "UiWindow.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
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
    // The panel's size in logical pixels; it draws from the window's current origin.
    static constexpr int kWidth = 960;
    static constexpr int kHeight = 748;

    // Draws into `window` (not owned; it must outlive the view).
    MappingView(ui::UiWindow& window, std::string sourceName, int sourceAxes, int sourceButtons,
                const IDeviceProfile& profile, const MapperConfig& mapper);

    MappingView(const MappingView&) = delete;
    MappingView& operator=(const MappingView&) = delete;

    // The panel's tabs, switched with the buttons along the bottom (or M / L / T / F / W). Wheel setup is drawn by
    // the window (WheelSetupView); this view then draws only the tab bar.
    enum class Tab { Mapping, Logs, Telemetry, ForceFeedback, WheelSetup };

    // A key pressed in the window: M / L / T / F switch tabs, O opens the logs (or telemetry) folder, D the drivers'
    // log folder; on the Logs tab the arrows, PageUp / PageDown and Home / End scroll the live log.
    void onKey(char key);
    // Mouse wheel notches (> 0 = up): on the Logs tab, scrolls the live log.
    void onScroll(int notches);
    // A left click, in the panel's own logical pixels (the window's origin taken off): the tab buttons, and
    // on the Logs tab a component's line opens its folder.
    void onClick(float x, float y);
    [[nodiscard]] Tab tab() const { return tab_; }
    void setTab(const Tab tab) { tab_ = tab; }
    // The tab bar alone (for the Wheel setup tab, whose content the window draws).
    void drawTabBar() { drawTabs(); }
    // Opens the folder with the logs (or the telemetry, if it has its own) in the file manager.
    void openFilesFolder() const;

    // Shown at the bottom of the panel.
    void setFiles(FilesDisplay files) { files_ = std::move(files); }
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
    void setAccessories(const std::array<DeviceLabels, MapperConfig::kMaxAccessories>& accessories) {
        accessories_ = accessories;
    }

    // Draws the panel (between the window's begin() and present()); `inputs.devices[0]` is the wheel.
    void draw(const RawInputs& inputs, const WheelState& state, const TargetReport& report,
              const FfbDisplay& ffb = {});

private:

    void drawHeader();
    void drawAxes(const RawInputs& inputs, const WheelState& state, const TargetReport& report, float top);
    // "R3 13 R paddle" on the wheel, "Pedals 2 Gas" on an accessory (cut to `chars`).
    [[nodiscard]] std::string sourceText(int device, int index, bool axis, std::size_t chars) const;
    void drawRawAxes(const RawInputState& raw, float top);
    void drawHat(std::int8_t sourceHat, std::int8_t targetHat, float top);
    void drawTargetButtons(std::uint32_t targetButtons, float top);
    void drawSourceButtons(std::uint64_t sourceButtons, float top);
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
    void trackLastPressed(std::uint64_t sourceButtons);
    // The name of source button `source` from [button_labels], or an empty string.
    [[nodiscard]] const std::string& sourceLabel(int source) const;

    ui::UiWindow& window_;
    std::string sourceName_;
    int sourceAxes_;
    int sourceButtons_;
    const IDeviceProfile& profile_;
    MapperConfig mapper_;
    std::array<DeviceLabels, MapperConfig::kMaxAccessories> accessories_{};
    std::string target_;            // short name of the emulated wheel ("G920")
    std::string pedalNote_;         // how the target encodes released / pressed pedals

    // sourceToTarget_[s] = first target button fed by source button s, or kUnmapped; count for duplicates.
    std::array<int, 64> sourceToTarget_{};
    std::array<int, 64> sourceTargetCount_{};
    // sourceHatDirection_[s] = D-pad direction ("up", "right", ...) source button s drives, or nullptr.
    std::array<const char*, 64> sourceHatDirection_{};

    FilesDisplay files_;
    std::string devices_;
    bool devicesWarning_ = false;
    Tab tab_ = Tab::Mapping;

    // One line of the Logs tab: a part of wheel-emulator and today's log file it writes.
    struct ComponentLog {
        const char* name;               // "Emulator", "G920 driver"
        const char* what;               // what it is
        std::filesystem::path folder;   // where its files are
        std::string file;               // today's file name, empty if it has written none today
        std::string status;             // size and age, or why there is none
    };
    std::vector<ComponentLog> components_;
    std::filesystem::path driverLogFolder_;   // the drivers write to C:\ProgramData\wheelbri\logs
    // Looked up at most once a second (files appear while a session runs and change at midnight).
    std::uint64_t filesCheckedMs_ = 0;

    // The Logs tab's live log: every component's file followed, newest at the bottom.
    LiveLog liveLog_;
    std::uint64_t livePolledMs_ = 0;
    std::size_t liveScroll_ = 0;        // lines scrolled up from the newest; 0 = follow new lines
    std::uint64_t liveSeen_ = 0;        // liveLog_.added() when last drawn (to hold the view while scrolled up)
    std::size_t liveRows_ = 1;          // lines that fit, from the last draw

    // The Telemetry tab's latest events (buttons, D-pad, pedals, game force feedback), newest last.
    struct TelemetryEvent {
        std::string time;               // "22:49:33.488"
        int kind;                       // index into the event colours
        std::string text;
    };
    std::deque<TelemetryEvent> events_;
    bool eventsStarted_ = false;        // the first frame only sets the starting state
    std::uint64_t eventButtons_ = 0;
    std::int8_t eventHat_ = -1;
    std::array<bool, 3> pedalDown_{};
    std::array<float, 3> pedalPeak_{};
    std::array<bool, 8> ffbPlaying_{};


    std::uint64_t previousSourceButtons_ = 0;
    int lastPressedSource_ = kUnmapped;
};

}  // namespace wheelbri::service
