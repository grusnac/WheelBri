#include "MappingView.hpp"

#include "wheelbri/LocalDate.hpp"
#include "wheelbri/Logging.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.ui");
    return logger;
}

// Everything is laid out on a fixed logical canvas; the window scales it (letterboxed).
// Text is 8x8 logical pixels per character (UiWindow.hpp).
constexpr int kLogicalWidth = MappingView::kWidth;
constexpr int kLogicalHeight = MappingView::kHeight;

constexpr std::size_t kMaxSourceButtons = 64;
constexpr float kTabWidth = 156;   // the tab buttons along the bottom

using Colour = ui::Colour;

constexpr Colour kBackground{22, 24, 28};
constexpr Colour kPanel{44, 48, 56};
constexpr Colour kText{222, 226, 232};
constexpr Colour kDim{120, 126, 136};
constexpr Colour kHeading{120, 190, 255};
constexpr Colour kActive{64, 190, 100};
constexpr Colour kWarning{214, 72, 64};
constexpr Colour kUnlabelled{206, 164, 58};
constexpr Colour kDark{16, 18, 20};
constexpr Colour kCentre{160, 164, 172};

// One colour per component, the same in the file list and the live log (index = its line in the list).
constexpr std::array<Colour, 4> kComponentColours = {{
    {120, 190, 255},   // Emulator: blue
    {236, 152, 72},    // Game shim: orange
    {186, 140, 255},   // G920 driver: purple
    {80, 204, 190},    // Hide filter: teal
}};

bool isValidAxis(const int axis) {
    return axis >= 0 && static_cast<std::size_t>(axis) < RawInputState::kMaxAxes;
}

}  // namespace

MappingView::MappingView(ui::UiWindow& window, std::string sourceName, const int sourceAxes,
                         const int sourceButtons, const IDeviceProfile& profile, const MapperConfig& mapper)
    : window_(window),
      sourceName_(std::move(sourceName)),
      sourceAxes_(std::clamp(sourceAxes, 0, static_cast<int>(RawInputState::kMaxAxes))),
      sourceButtons_(std::clamp(sourceButtons, 0, static_cast<int>(kMaxSourceButtons))),
      profile_(profile),
      mapper_(mapper),
      target_(profile.identity().shortName.empty() ? std::string("target") : profile.identity().shortName) {
    // Ask the profile rather than assuming a pedal convention.
    WheelState released;
    WheelState pressed;
    pressed.throttle = 1.0F;
    char note[96];
    std::snprintf(note, sizeof(note), "%s pedals: released = %d, fully pressed = %d (raw %s number shown).",
                  target_.c_str(), profile.encode(released).throttle, profile.encode(pressed).throttle,
                  target_.c_str());
    pedalNote_ = note;

    sourceToTarget_.fill(kUnmapped);
    const HatButtons& hat = mapper_.hatButtons;
    const std::array<std::pair<int, const char*>, 4> hatDirections = {
        {{hat.up, "up"}, {hat.right, "right"}, {hat.down, "down"}, {hat.left, "left"}}};
    for (std::size_t i = 0; i < hatDirections.size(); ++i) {
        const auto& [source, direction] = hatDirections[i];
        if (hat.devices[i] == 0 && source >= 0 && static_cast<std::size_t>(source) < kMaxSourceButtons) {
            sourceHatDirection_[static_cast<std::size_t>(source)] = direction;
        }
    }
    // Only the target's real buttons (the ones its profile names): the rest of the 32 slots are filled 1:1 by
    // default but no game sees them, so they would only show up as duplicates.
    for (std::size_t target = 0; target < mapper_.buttonMap.size(); ++target) {
        const int source = mapper_.buttonMap[target];
        if (profile_.buttonName(target).empty() || mapper_.buttonDevice[target] != 0) {
            continue;   // the rig buttons panel below shows the wheel's own buttons only
        }
        if (source >= 0 && static_cast<std::size_t>(source) < kMaxSourceButtons) {
            const auto index = static_cast<std::size_t>(source);
            if (sourceTargetCount_[index]++ == 0) {
                sourceToTarget_[index] = static_cast<int>(target);
            }
        }
    }
}

namespace {
bool gTelemetryTab = false;       // --telemetry-tab
bool gForceFeedbackTab = false;   // --ffb-tab
}  // namespace

void MappingView::setOptionalTabs(const bool telemetry, const bool forceFeedback) {
    gTelemetryTab = telemetry;
    gForceFeedbackTab = forceFeedback;
}

std::vector<MappingView::Tab> MappingView::visibleTabs() {
    std::vector<Tab> tabs = {Tab::Mapping, Tab::WheelSetup, Tab::Logs};
    if (gTelemetryTab) tabs.push_back(Tab::Telemetry);
    if (gForceFeedbackTab) tabs.push_back(Tab::ForceFeedback);
    return tabs;
}

void MappingView::onKey(const char key) {
    if (key == 'o') {
        openFilesFolder();
    } else if (key == 'f' && gForceFeedbackTab) {
        tab_ = Tab::ForceFeedback;
    } else if (key == 'm') {
        tab_ = Tab::Mapping;
    } else if (key == 'l') {
        tab_ = Tab::Logs;
    } else if (key == 't' && gTelemetryTab) {
        tab_ = Tab::Telemetry;
    } else if (key == 'r') {
        tab_ = Tab::WheelSetup;
    } else if (tab_ == Tab::Logs) {
        const auto page = static_cast<long long>(std::max<std::size_t>(liveRows_ - 1, 1));
        if (key == ui::kPageUp) scrollLiveLog(page);
        if (key == ui::kPageDown) scrollLiveLog(-page);
        if (key == ui::kUp) scrollLiveLog(1);
        if (key == ui::kDown) scrollLiveLog(-1);
        if (key == ui::kHome) scrollLiveLog(static_cast<long long>(LiveLog::kMaxLines));
        if (key == ui::kEnd) liveScroll_ = 0;
        if (key == ui::kLeft) onHScroll(-1);
        if (key == ui::kRight) onHScroll(1);
        if (key == 'w') {
            liveWrap_ = !liveWrap_;
            liveHScroll_ = 0;
        }
    }
}

void MappingView::onScroll(const int notches) {
    if (tab_ == Tab::Logs) {
        scrollLiveLog(static_cast<long long>(notches) * 3);
    }
}

void MappingView::onHScroll(const int notches) {
    if (tab_ != Tab::Logs || liveWrap_) return;
    const auto most = static_cast<long long>(liveLongest_ > kLiveMessageChars ? liveLongest_ - kLiveMessageChars : 0);
    liveHScroll_ = static_cast<std::size_t>(
        std::clamp(static_cast<long long>(liveHScroll_) + static_cast<long long>(notches) * 8, 0LL, most));
}

std::size_t MappingView::liveMostScroll() const {
    const std::size_t total = liveLog_.lines().size();
    return total > liveRows_ ? total - liveRows_ : 0;
}

void MappingView::onPointer(const float x, const float y, const bool leftDown) {
    if (!leftDown || tab_ != Tab::Logs) {
        drag_ = Drag::None;
        return;
    }
    if (drag_ != Drag::None) dragTo(x, y);
}

void MappingView::dragTo(const float x, const float y) {
    // The thumb's start follows the mouse (less where it was taken); its position along the free track is the
    // position in the log.
    if (drag_ == Drag::Vertical && vBar_.shown) {
        const float free = std::max(1.0F, vBar_.h - vBar_.thumbSize);
        const float at = std::clamp((y - dragGrab_ - vBar_.y) / free, 0.0F, 1.0F);
        const auto most = static_cast<float>(liveMostScroll());
        liveScroll_ = static_cast<std::size_t>(std::lround((1.0F - at) * most));   // the top = scrolled furthest up
    } else if (drag_ == Drag::Horizontal && hBar_.shown) {
        const float free = std::max(1.0F, hBar_.w - hBar_.thumbSize);
        const float at = std::clamp((x - dragGrab_ - hBar_.x) / free, 0.0F, 1.0F);
        const float most = static_cast<float>(liveLongest_ > kLiveMessageChars ? liveLongest_ - kLiveMessageChars : 0);
        liveHScroll_ = static_cast<std::size_t>(std::lround(at * most));
    }
}

void MappingView::scrollLiveLog(const long long lines) {
    const auto total = static_cast<long long>(liveLog_.lines().size());
    const long long most = std::max(0LL, total - static_cast<long long>(liveRows_));
    liveScroll_ = static_cast<std::size_t>(std::clamp(static_cast<long long>(liveScroll_) + lines, 0LL, most));
}

// --- drawing helpers ------------------------------------------------------------------------------

namespace {

void fill(ui::UiWindow& window, const float x, const float y, const float w, const float h, const Colour colour) {
    window.fill(x, y, w, h, colour);
}

void outline(ui::UiWindow& window, const float x, const float y, const float w, const float h, const Colour colour) {
    window.outline(x, y, w, h, colour);
}

void text(ui::UiWindow& window, const float x, const float y, const char* value, const Colour colour) {
    window.text(x, y, value, colour);
}

// -1..1, filled from the centre.
void bipolarBar(ui::UiWindow& window, const float x, const float y, const float w, const float h, const float value,
                const Colour colour) {
    fill(window, x, y, w, h, kPanel);
    const float half = w * 0.5F;
    const float v = std::clamp(value, -1.0F, 1.0F);
    if (v >= 0.0F) {
        fill(window, x + half, y, half * v, h, colour);
    } else {
        fill(window, x + half + half * v, y, -half * v, h, colour);
    }
    fill(window, x + half - 0.5F, y - 1.0F, 1.0F, h + 2.0F, kCentre);
}

// 0..1, filled from the left.
void unipolarBar(ui::UiWindow& window, const float x, const float y, const float w, const float h,
                 const float fraction, const Colour colour) {
    fill(window, x, y, w, h, kPanel);
    fill(window, x, y, w * std::clamp(fraction, 0.0F, 1.0F), h, colour);
}

}  // namespace

// --- sections -------------------------------------------------------------------------------------

void MappingView::draw(const RawInputs& inputs, const WheelState& state, const TargetReport& report,
                       const FfbDisplay& ffb) {
    const RawInputState& raw = inputs.devices[0];
    trackLastPressed(raw.buttons);

    trackEvents(raw, state, report, ffb);
    if (tab_ == Tab::WheelSetup) {
        drawTabs();   // the window draws the tab itself (WheelSetupView)
        return;
    }
    if (tab_ != Tab::Logs) drawHeader();   // the Logs page starts at the top (the Mapping tab has the header)
    if (tab_ == Tab::Mapping) {
        // What is mapped to what: the axes and pedals (rig -> G920), every rig axis, the D-pad, the buttons.
        drawAxes(inputs, state, report, 34);
        drawRawAxes(raw, 128);
        drawHat(raw.hat, report.hat, 128);
        drawTargetButtons(report.buttons, 244);
        drawSourceButtons(raw.buttons, 401);
        drawFooter(545);
    } else if (tab_ == Tab::Logs) {
        drawLogs();
    } else if (tab_ == Tab::Telemetry) {
        drawTelemetry();
    } else {
        drawForceFeedback(ffb);
    }
    drawTabs();
}

void MappingView::drawHeader() {
    char line[128];
    std::snprintf(line, sizeof(line), "%.40s  ->  %.40s", sourceName_.c_str(),
                  profile_.identity().productName.c_str());
    if (tab_ == Tab::Mapping) {
        line[std::min(headerChars_, sizeof(line) - 1)] = '\0';   // the app's controls are at the top right of this tab
    }
    text(window_, 4, 4, line, kHeading);
    if (devices_.empty()) {
        text(window_, 4, 16, "Move every control and check where it lands.", kDim);
    } else {
        text(window_, 4, 16, devices_.substr(0, 118).c_str(), devicesWarning_ ? kWarning : kDim);
    }
}

std::string MappingView::sourceText(const int device, const int index, const bool axis, const std::size_t chars) const {
    std::string result;
    if (index < 0) {
        result = "--";
    } else if (device == 0) {
        result = (axis ? "Rig ax" : "Rig ") + std::to_string(index) + " " +
                 (axis ? mapper_.sourceAxisLabels[static_cast<std::size_t>(std::min(index, 7))]
                       : sourceLabel(index));
    } else if (static_cast<std::size_t>(device) <= accessories_.size()) {
        const DeviceLabels& labels = accessories_[static_cast<std::size_t>(device - 1)];
        const std::string name = labels.name.empty() ? deviceId(labels.vendorId, labels.productId) : labels.name;
        const std::string& label = axis ? labels.axes[static_cast<std::size_t>(std::min(index, 7))]
                                        : labels.buttons[static_cast<std::size_t>(std::min(index, 63))];
        result = name.substr(0, 10) + (axis ? " ax" : " ") + std::to_string(index) + " " + label;
    }
    while (!result.empty() && result.back() == ' ') result.pop_back();
    if (result.size() > chars) result.resize(chars);
    return result;
}

void MappingView::drawAxes(const RawInputs& inputs, const WheelState& state, const TargetReport& report,
                           const float top) {
    struct Row {
        const char* name;
        int sourceAxis;
        int sourceDevice;
        bool bipolarMapped;
        float mapped;
        std::int32_t target;
    };
    const std::array<Row, 4> rows = {{
        {"Steering", mapper_.steering.sourceAxis, mapper_.steering.sourceDevice, true, state.steering, report.steering},
        {"Throttle", mapper_.throttle.sourceAxis, mapper_.throttle.sourceDevice, false, state.throttle, report.throttle},
        {"Brake", mapper_.brake.sourceAxis, mapper_.brake.sourceDevice, false, state.brake, report.brake},
        {"Clutch", mapper_.clutch.sourceAxis, mapper_.clutch.sourceDevice, false, state.clutch, report.clutch},
    }};

    const float kTop = top;
    text(window_, 4, kTop, "AXES", kHeading);
    text(window_, 76, kTop, "Rig", kDim);
    text(window_, 112, kTop, "Rig raw", kDim);
    text(window_, 282, kTop, "mapped", kDim);
    const std::string valueHeading = target_ + " value (0..65535)";
    text(window_, 452, kTop, valueHeading.c_str(), kDim);

    char value[32];
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        const float y = kTop + 12 + static_cast<float>(i) * 16;
        const bool valid = isValidAxis(row.sourceAxis) && row.sourceDevice >= 0 &&
                           static_cast<std::size_t>(row.sourceDevice) < kMaxInputDevices;
        text(window_, 4, y + 1, row.name, kText);

        if (!valid) {
            std::snprintf(value, sizeof(value), "--");
        } else if (row.sourceDevice == 0) {
            std::snprintf(value, sizeof(value), "ax%d", row.sourceAxis);
        } else {
            std::snprintf(value, sizeof(value), "d%d.%d", row.sourceDevice, row.sourceAxis);   // an accessory's
        }
        text(window_, 76, y + 1, value, valid ? kText : kWarning);

        const float rawValue =
            valid ? inputs.devices[static_cast<std::size_t>(row.sourceDevice)].axes[static_cast<std::size_t>(row.sourceAxis)]
                  : 0.0F;
        bipolarBar(window_, 112, y, 110, 10, rawValue, valid ? kActive : kDim);
        std::snprintf(value, sizeof(value), "%+.3f", rawValue);
        text(window_, 226, y + 1, value, kText);

        if (row.bipolarMapped) {
            bipolarBar(window_, 282, y, 110, 10, row.mapped, kActive);
            std::snprintf(value, sizeof(value), "%+.3f", row.mapped);
        } else {
            unipolarBar(window_, 282, y, 110, 10, row.mapped, kActive);
            std::snprintf(value, sizeof(value), "%5.1f%%", row.mapped * 100.0F);
        }
        text(window_, 396, y + 1, value, kText);

        unipolarBar(window_, 452, y, 150, 10, static_cast<float>(row.target) / 65535.0F, kHeading);
        std::snprintf(value, sizeof(value), "%5d", row.target);
        text(window_, 608, y + 1, value, kText);
    }
    std::string accessories;
    for (std::size_t i = 0; i < accessories_.size(); ++i) {
        if (accessories_[i].vendorId == 0 && accessories_[i].productId == 0) continue;
        accessories += (accessories.empty() ? "  d" : ", d") + std::to_string(i + 1) + " = " +
                       (accessories_[i].name.empty() ? deviceId(accessories_[i].vendorId, accessories_[i].productId)
                                                     : accessories_[i].name);
    }
    text(window_, 4, kTop + 78, (pedalNote_ + accessories).substr(0, 118).c_str(), kDim);
}

void MappingView::drawRawAxes(const RawInputState& raw, const float top) {
    const float kTop = top;
    text(window_, 4, kTop, "ALL RIG AXES", kHeading);

    auto roleOf = [this](const int axis) -> const char* {
        if (mapper_.steering.sourceDevice == 0 && axis == mapper_.steering.sourceAxis) return "-> steering";
        if (mapper_.throttle.sourceDevice == 0 && axis == mapper_.throttle.sourceAxis) return "-> throttle";
        if (mapper_.brake.sourceDevice == 0 && axis == mapper_.brake.sourceAxis) return "-> brake";
        if (mapper_.clutch.sourceDevice == 0 && axis == mapper_.clutch.sourceAxis) return "-> clutch";
        return "not used";
    };

    char value[32];
    for (int axis = 0; axis < static_cast<int>(RawInputState::kMaxAxes); ++axis) {
        const float y = kTop + 12 + static_cast<float>(axis) * 12;
        std::snprintf(value, sizeof(value), "axis %d", axis);
        const bool present = axis < sourceAxes_;
        text(window_, 4, y, value, present ? kText : kDim);
        if (!present) {
            text(window_, 64, y, "(not on this device)", kDim);
            continue;
        }
        const float v = raw.axes[static_cast<std::size_t>(axis)];
        bipolarBar(window_, 64, y, 150, 8, v, kActive);
        std::snprintf(value, sizeof(value), "%+.3f", v);
        text(window_, 220, y, value, kText);
        const char* const role = roleOf(axis);
        text(window_, 276, y, role, role[0] == '-' ? kText : kDim);
    }
}

void MappingView::drawHat(const std::int8_t sourceHat, const std::int8_t targetHat, const float top) {
    constexpr float kLeft = 470;
    const float kTop = top;
    text(window_, kLeft, kTop, "D-PAD / HAT", kHeading);

    // Hat index (clockwise from up) -> grid cell.
    static constexpr std::array<std::array<int, 2>, 8> kCell = {{{1, 0}, {2, 0}, {2, 1}, {2, 2},
                                                                {1, 2}, {0, 2}, {0, 1}, {0, 0}}};
    constexpr float kSize = 20;
    const float gridLeft = kLeft + 30;
    const float gridTop = kTop + 14;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            outline(window_, gridLeft + column * (kSize + 2), gridTop + row * (kSize + 2), kSize, kSize, kPanel);
        }
    }
    const bool pressed = targetHat >= 0 && targetHat < 8;
    const auto cell = pressed ? kCell[static_cast<std::size_t>(targetHat)] : std::array<int, 2>{1, 1};
    fill(window_, gridLeft + cell[0] * (kSize + 2) + 2, gridTop + cell[1] * (kSize + 2) + 2, kSize - 4, kSize - 4,
         pressed ? kActive : kPanel);

    char line[48];
    std::snprintf(line, sizeof(line), "Rig hat %2d -> %s hat %2d", sourceHat, target_.c_str(), targetHat);
    text(window_, kLeft, gridTop + 3 * (kSize + 2) + 4, line, kText);
    text(window_, kLeft, gridTop + 3 * (kSize + 2) + 16, "-1 = centred, 0..7 clockwise", kDim);
}

const std::string& MappingView::sourceLabel(const int source) const {
    static const std::string kNone;
    return source >= 0 && static_cast<std::size_t>(source) < mapper_.sourceButtonLabels.size()
               ? mapper_.sourceButtonLabels[static_cast<std::size_t>(source)]
               : kNone;
}

void MappingView::drawTargetButtons(const std::uint32_t targetButtons, const float top) {
    const float kTop = top;
    const std::string heading = target_ + " BUTTONS";
    text(window_, 4, kTop, heading.c_str(), kHeading);
    const float subtitleX = 4 + static_cast<float>(heading.size() + 1) * 8;
    const std::string subtitle = target_ + " number name <- rig number name   (INI: [buttons] target_N = rig number)";
    text(window_, subtitleX, kTop, subtitle.c_str(), kDim);

    // Every cell reads "G920  6 Menu     <- Rig 35 Start": 39 characters fit a cell after its 3-pixel margin.
    constexpr int kColumns = 3;
    constexpr float kWidth = 316;
    constexpr float kHeight = 13;
    char cellText[64];
    // Only the buttons the target names (the G920's 11 and the shifter's 7; its unused 12th is left out); room for 9 rows.
    std::size_t cell = 0;
    for (std::size_t target = 0; target < mapper_.buttonMap.size(); ++target) {
        const std::string_view name = profile_.buttonName(target);
        if (name.empty()) {
            continue;
        }
        const float x = 4 + static_cast<float>(cell % kColumns) * (kWidth + 2);
        const float y = kTop + 12 + static_cast<float>(cell / kColumns) * (kHeight + 2);
        ++cell;
        const bool pressed = ((targetButtons >> target) & 1U) != 0;
        const int source = mapper_.buttonMap[target];

        const std::string label(name.substr(0, 8));
        if (source < 0) {
            std::snprintf(cellText, sizeof(cellText), "%s %2zu %-8s <- --", target_.c_str(), target, label.c_str());
        } else if (mapper_.buttonDevice[target] == 0) {
            std::snprintf(cellText, sizeof(cellText), "%s %2zu %-8s <- Rig %2d %s", target_.c_str(), target,
                          label.c_str(), source, sourceLabel(source).c_str());
        } else {
            std::snprintf(cellText, sizeof(cellText), "%s %2zu %-8s <- %s", target_.c_str(), target, label.c_str(),
                          sourceText(mapper_.buttonDevice[target], source, false, 20).c_str());
        }

        cellText[39] = '\0';   // what fits the cell
        fill(window_, x, y, kWidth, kHeight, pressed ? kActive : kPanel);
        const Colour colour = pressed ? kDark : (source < 0 ? kDim : kText);
        text(window_, x + 3, y + 3, cellText, colour);
    }
}

void MappingView::drawSourceButtons(const std::uint64_t sourceButtons, const float top) {
    const float kTop = top;
    text(window_, 4, kTop, "RIG BUTTONS", kHeading);
    const std::string legend =
        "pressed: green = reaches a " + target_ + " button or the D-pad, red = goes nowhere";
    text(window_, 100, kTop, legend.c_str(), kDim);

    // Every cell reads "Rig 35 Start": the rig button only (where it goes is in the panel above).
    // 14 characters fit a cell after its margin.
    constexpr int kColumns = 8;
    constexpr float kWidth = 116;
    constexpr float kHeight = 13;
    char line[40];
    for (int source = 0; source < sourceButtons_; ++source) {
        const auto index = static_cast<std::size_t>(source);
        const float x = 4 + static_cast<float>(source % kColumns) * (kWidth + 3);
        const float y = kTop + 12 + static_cast<float>(source / kColumns) * (kHeight + 2);
        const bool pressed = ((sourceButtons >> source) & 1ULL) != 0;
        const int target = sourceToTarget_[index];
        const char* const hatDirection = sourceHatDirection_[index];

        Colour background = kPanel;
        if (pressed) {
            background = hatDirection != nullptr || target >= 0 ? kActive : kWarning;
        }
        fill(window_, x, y, kWidth, kHeight, background);
        const Colour foreground = pressed ? kDark : (target >= 0 || hatDirection != nullptr ? kText : kDim);

        std::snprintf(line, sizeof(line), "Rig %d %s", source, sourceLabel(source).c_str());
        text(window_, x + 2, y + 3, std::string(line).substr(0, 14).c_str(), foreground);
    }
    if (sourceButtons_ == 0) {
        text(window_, 4, kTop + 14, "(the device reports no buttons)", kDim);
    }
}

void MappingView::drawFooter(const float top) {
    const float kTop = top;
    char line[160];
    if (lastPressedSource_ < 0) {
        std::snprintf(line, sizeof(line), "Press any button on the wheel to see where it goes.");
        text(window_, 4, kTop, line, kDim);
    } else {
        const auto index = static_cast<std::size_t>(lastPressedSource_);
        const int target = sourceToTarget_[index];
        const char* const hatDirection = sourceHatDirection_[index];
        // Same pattern as the cells: "G920 6 Menu <- Rig 35 Start".
        const std::string& name = sourceLabel(lastPressedSource_);
        char button[40];
        std::snprintf(button, sizeof(button), name.empty() ? "Rig %d" : "Rig %d %s", lastPressedSource_, name.c_str());
        if (hatDirection != nullptr) {
            std::snprintf(line, sizeof(line), "Last pressed: %s D-pad %s <- %s%s", target_.c_str(), hatDirection,
                          button, target >= 0 ? "  (and a button: check [buttons])" : "");
            text(window_, 4, kTop, line, target >= 0 ? kUnlabelled : kText);
        } else if (target >= 0) {
            const std::string_view targetName = profile_.buttonName(static_cast<std::size_t>(target));
            std::snprintf(line, sizeof(line), "Last pressed: %s %d %.*s <- %s%s", target_.c_str(), target,
                          static_cast<int>(targetName.size()), targetName.empty() ? "" : targetName.data(), button,
                          sourceTargetCount_[index] > 1 ? "  (and others)" : "");
            text(window_, 4, kTop, line, kText);
        } else {
            std::snprintf(line, sizeof(line), "Last pressed: %s  (not mapped to any %s button)", button,
                          target_.c_str());
            text(window_, 4, kTop, line, kWarning);
        }
    }
    text(window_, 4, kTop + 14, "To change: INI [buttons] target_N, [hat] up/right/down/left, [button_labels] button_N.",
         kDim);
}

namespace {

// One line of numbers for an effect, gain folded in the way the limiter does it.
std::string describeFfb(const FfbEffect& e) {
    char text[64];
    if (e.type == FfbEffectType::Constant) {
        std::snprintf(text, sizeof(text), "level %+.2f", e.constantLevel * e.gain);
    } else if (isConditionEffect(e.type)) {
        std::snprintf(text, sizeof(text), "k %+.2f/%+.2f sat %.2f", e.condition.positiveCoefficient * e.gain,
                      e.condition.negativeCoefficient * e.gain, e.condition.positiveSaturation);
    } else {
        std::snprintf(text, sizeof(text), "mag %.2f %3.0fms", e.periodic.magnitude * e.gain, e.periodic.periodMs);
    }
    return text;
}

}  // namespace

void MappingView::drawFfb(const FfbDisplay& ffb, const float top) {
    const float kTop = top;
    text(window_, 4, kTop, "FORCE FEEDBACK", kHeading);
    if (ffb.controller == nullptr) {
        text(window_, 132, kTop, ffb.status.empty() ? "off" : ffb.status.c_str(), kDim);
        return;
    }
    const FfbController& controller = *ffb.controller;
    const auto& stats = controller.stats();
    const FfbSafetyConfig& safety = controller.safety();

    char line[160];
    std::snprintf(line, sizeof(line), "limits: gain %.2f, max %.0f%% of the wheel, slew %.0f/s%s", safety.masterGain,
                  safety.maxMagnitude * 100.0F, safety.maxSlewPerSecond, ffb.invert ? ", direction inverted" : "");
    text(window_, 4, kTop + 12, line, kDim);

    std::string age = "never";
    if (const auto last = controller.lastPacketNs(); last && ffb.nowNs >= *last) {
        age = std::to_string((ffb.nowNs - *last) / 1'000'000) + " ms ago";
    }
    std::snprintf(line, sizeof(line), "last packet from the game %s (%llu in total)", age.c_str(),
                  static_cast<unsigned long long>(stats.packets));
    text(window_, 132, kTop, line, kDim);
    if (controller.watchdogTripped()) {
        text(window_, 4, kTop + 24, "game silent for 500 ms: all forces stopped until it sends again", kWarning);
    } else {
        std::snprintf(line, sizeof(line), "%zu playing, %llu updates to the wheel, %llu failed, %llu watchdog stops",
                      stats.activeSlots, static_cast<unsigned long long>(stats.plays),
                      static_cast<unsigned long long>(stats.playFailures),
                      static_cast<unsigned long long>(stats.watchdogStops));
        text(window_, 4, kTop + 24, line, kDim);
    }

    text(window_, 4, kTop + 38, "slot type      game asked for           -> wheel gets (after limits)", kDim);
    float y = kTop + 50;
    bool anyShown = false;
    for (std::size_t i = 0; i < controller.slotCount(); ++i) {
        const FfbController::SlotView slot = controller.slot(i);
        if (!slot.wanted && !slot.playing) {
            continue;
        }
        anyShown = true;
        const Colour colour = slot.playing ? kText : kDim;
        std::snprintf(line, sizeof(line), "%zu    %-9s %-24s ->", i, ffbTypeName(slot.requested.type),
                      describeFfb(slot.requested).c_str());
        text(window_, 4, y, line, colour);
        if (slot.sent && slot.playing) {
            text(window_, 404, y, describeFfb(*slot.sent).c_str(), kText);
            const float strength = ffbStrength(*slot.sent);
            if (slot.sent->type == FfbEffectType::Constant) {
                bipolarBar(window_, 600, y, 116, 8, strength / std::max(safety.maxMagnitude, 0.01F), kActive);
            } else {
                unipolarBar(window_, 600, y, 116, 8, strength, kActive);
            }
        } else {
            text(window_, 404, y, "stopped", kDim);
        }
        y += 11;
    }
    if (!anyShown) {
        text(window_, 4, y, "no effects from the game yet (start the game, or it has not created any)", kDim);
    }
}

// Usage since the service started: a row per effect type the game used, and for the constant force
// (road and tyre feel) how much of its time was spent at each strength the game asked for.
void MappingView::drawFfbUsage(const FfbController& controller, const float top) {
    const FfbUsage& usage = controller.usage();
    char line[160];
    const auto hms = [](const std::uint64_t ns) {
        const std::uint64_t s = ns / 1'000'000'000ULL;
        char t[24];
        std::snprintf(t, sizeof(t), "%llu:%02llu:%02llu", static_cast<unsigned long long>(s / 3600),
                      static_cast<unsigned long long>(s / 60 % 60), static_cast<unsigned long long>(s % 60));
        return std::string(t);
    };
    std::snprintf(line, sizeof(line), "since start: game active %s, forces on %.0f%% of it",
                  hms(usage.gameNs()).c_str(),
                  usage.gameNs() == 0 ? 0.0
                                      : 100.0 * static_cast<double>(std::min(usage.anyForceNs(), usage.gameNs())) /
                                            static_cast<double>(usage.gameNs()));
    text(window_, 132, top, line, kDim);
    text(window_, 4, top + 24, "share: of the game's time; capped / slew: of that effect's time cut by max / slew",
         kDim);

    if (usage.empty()) {
        text(window_, 4, top + 50, "no force feedback played yet (start the game and drive)", kDim);
        return;
    }
    text(window_, 4, top + 38, FfbUsage::tableHeader().c_str(), kDim);
    float y = top + 50;
    const auto used = usage.usedTypes();
    for (std::size_t i = 0; i < used.size() && i < 8; ++i) {
        const auto& t = usage.type(used[i]);
        const bool limited = t.playNs > 0 && (t.cappedNs * 10 > t.playNs || t.slewNs * 10 > t.playNs);
        text(window_, 4, y, usage.tableRow(used[i]).c_str(), limited ? kUnlabelled : kText);
        y += 11;
    }

    // Histogram of the constant force: bars scaled to the tallest, which is labelled.
    constexpr float kLeft = 560;
    constexpr float kBarW = 14;
    constexpr float kGap = 2;
    const float barsTop = top + 50;
    const float barsH = 72;
    text(window_, kLeft, top + 38, "constant asked", kDim);
    const auto& bins = usage.constantHistogramNs();
    std::uint64_t total = 0;
    std::uint64_t tallest = 0;
    for (const auto ns : bins) {
        total += ns;
        tallest = std::max(tallest, ns);
    }
    for (std::size_t i = 0; i < bins.size(); ++i) {
        const float x = kLeft + static_cast<float>(i) * (kBarW + kGap);
        fill(window_, x, barsTop, kBarW, barsH, kPanel);
        if (tallest > 0 && bins[i] > 0) {
            const float h = std::max(1.0F, barsH * static_cast<float>(bins[i]) / static_cast<float>(tallest));
            fill(window_, x, barsTop + barsH - h, kBarW, h, i >= 8 ? kUnlabelled : kActive);
        }
    }
    text(window_, kLeft, barsTop + barsH + 4, "0%", kDim);
    text(window_, kLeft + 10 * (kBarW + kGap) - kGap - 32, barsTop + barsH + 4, "100%", kDim);
    if (tallest > 0) {
        std::snprintf(line, sizeof(line), "top %.0f%%", 100.0 * static_cast<double>(tallest) / static_cast<double>(total));
        text(window_, kLeft + 40, barsTop + barsH + 4, line, kDim);
    }
}

void MappingView::trackLastPressed(const std::uint64_t sourceButtons) {
    const std::uint64_t newlyPressed = sourceButtons & ~previousSourceButtons_;
    previousSourceButtons_ = sourceButtons;
    for (int source = 0; source < static_cast<int>(kMaxSourceButtons); ++source) {
        if (((newlyPressed >> source) & 1ULL) != 0) {
            lastPressedSource_ = source;
            WE_LOG_DEBUG(log(), "rig button {} pressed -> {} target {}", source, target_,
                         sourceToTarget_[static_cast<std::size_t>(source)]);
        }
    }
}

// --- files ----------------------------------------------------------------------------------------

namespace {

// Keeps both ends of a long path: "C:\Users\...\logs\telemetry-2026-09-30.log".
std::string shortenMiddle(const std::string& value, const std::size_t maxChars) {
    if (value.size() <= maxChars || maxChars < 8) {
        return value;
    }
    const std::size_t tail = (maxChars - 3) * 2 / 3;
    const std::size_t head = maxChars - 3 - tail;
    return value.substr(0, head) + "..." + value.substr(value.size() - tail);
}

std::string displayPath(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    return (ec ? path : absolute).lexically_normal().string();
}

}  // namespace

void MappingView::openFilesFolder() const {
    openFolder(files_.telemetry.on ? files_.telemetry.samplesPath.parent_path() : files_.logFolder);
}

void MappingView::openFolder(const std::filesystem::path& folder) const {
    if (folder.empty()) {
        WE_LOG_WARN(log(), "no folder to open");
        return;
    }
    const std::string path = displayPath(folder);
    std::string error;
    if (window_.openFolder(folder, error)) {
        WE_LOG_INFO(log(), "opened folder {}", path);
    } else {
        WE_LOG_WARN(log(), "cannot open folder {}: {}", path, error);
    }
}

namespace {

// "12 KB, written 3 min ago".
std::string describeFile(const std::filesystem::path& path) {
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(path, ec);
    const auto written = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return "cannot read it: " + ec.message();
    }
    const auto ageS = std::chrono::duration_cast<std::chrono::seconds>(
                          std::filesystem::file_time_type::clock::now() - written)
                          .count();
    char age[32];
    if (ageS < 60) {
        std::snprintf(age, sizeof(age), "%llds ago", static_cast<long long>(std::max<long long>(ageS, 0)));
    } else if (ageS < 3600) {
        std::snprintf(age, sizeof(age), "%lld min ago", static_cast<long long>(ageS / 60));
    } else {
        std::snprintf(age, sizeof(age), "%lld h ago", static_cast<long long>(ageS / 3600));
    }
    char line[64];
    std::snprintf(line, sizeof(line), "%llu KB, written %s", static_cast<unsigned long long>((bytes + 1023) / 1024),
                  age);
    return line;
}

// Logs tab layout (logical pixels): the folders, then one line per component.
constexpr float kLogsTop = 4;   // no header above the Logs page
constexpr float kLogRowHeight = 16;
constexpr float kLogRowsTop = kLogsTop + 14 + 2 * kLogRowHeight + 22;   // after the heading and the two folders
// Telemetry tab: the heading and folder, then the two file lines.
constexpr float kTelemetryTop = 34;
constexpr float kTelemetryRowsTop = kTelemetryTop + 30;

}  // namespace

void MappingView::refreshLogFiles() {
    const auto nowMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                      std::chrono::steady_clock::now().time_since_epoch())
                                                      .count());
    if (filesCheckedMs_ != 0 && nowMs - filesCheckedMs_ < 1000) {
        return;
    }
    filesCheckedMs_ = nowMs;
    const std::string today = localDate(WallClock::now());

    // Every part of wheel-emulator that writes a log, in the order they sit between the wheel and the game.
    struct Source {
        const char* name;
        const char* what;
        bool driver;            // in the drivers' folder, not the user's
        const char* prefix;     // today's file is <prefix><date>.log
    };
    static constexpr Source kSources[] = {
        {"Emulator", "service and app: reads the rig, maps, plays FFB", false,
         "wheelbri-service-"},
        {"Game shim", "dinput8.dll next to the game (Phases 2-3)", false, "wheelbri-shim-"},
        {"G920 driver", "the virtual G920 (wheelbri-g920)", true, "wheelbri-driver-"},
        {"Hide filter", "hides the rig from games (wheelbri-hide)", true, "wheelbri-hide-"},
    };
    components_.clear();
    for (const Source& source : kSources) {
        if (shimMode_ && source.driver) continue;   // the shim's session runs no driver
        // The drivers write to the same folder (the app shares it with them: ServiceLogging.hpp shareLogFolder).
#if defined(_WIN32)
        const std::filesystem::path folder = files_.logFolder;
#else
        const std::filesystem::path folder = source.driver ? std::filesystem::path() : files_.logFolder;
#endif
        ComponentLog component{source.name, source.what, folder, {}, {}};
        std::filesystem::path path = component.folder / (std::string(source.prefix) + today + ".log");
        if (std::string_view(source.prefix) == "wheelbri-service-") {
            // The service knows its own file (it may be elsewhere through [logging] file).
            if (!files_.logToFile || logging::currentLogFile().empty()) {
                component.status = "console only (no log file)";
                components_.push_back(std::move(component));
                continue;
            }
            path = std::filesystem::path(logging::currentLogFile());
            component.folder = path.parent_path();
        }
        // The live log follows today's file (it appears as soon as the component writes).
        liveLog_.setFile(static_cast<int>(components_.size()), component.folder.empty() ? std::filesystem::path() : path);
        std::error_code ec;
        if (component.folder.empty()) {
            component.status = "Windows only";
        } else if (std::filesystem::exists(path, ec)) {
            component.file = path.filename().string();
            component.status = describeFile(path);
        } else {
            component.status = "no log today";
        }
        components_.push_back(std::move(component));
    }
}

void MappingView::drawLogs() {
    constexpr std::size_t kPathChars = 90;
    constexpr std::size_t kWhatChars = 44;   // to the right edge
    refreshLogFiles();
    text(window_, 4, kLogsTop, "LOGS", kHeading);
    text(window_, 52, kLogsTop, "one line per component, a new file every day; click a line to open its folder",
         kDim);

    const float x = 4 + 14 * 8;
    float y = kLogsTop + 14;
    text(window_, 4, y, "Logs folder", kDim);
    text(window_, x, y, shortenMiddle(displayPath(files_.logFolder), kPathChars).c_str(), kText);
    text(window_, x + (kPathChars + 2) * 8, y, "O opens", kDim);
    y += kLogRowHeight;
    text(window_, 4, y, "Everything", kDim);
    text(window_, x, y, "app, game shim, drivers, installers and telemetry all write to this one folder", kDim);

    y = kLogRowsTop - 14;
    text(window_, 4, y, "COMPONENT", kHeading);
    text(window_, x, y, "TODAY'S LOG", kHeading);
    text(window_, x + 33 * 8, y, "SIZE, LAST WRITTEN", kHeading);
    y = kLogRowsTop;
    for (const ComponentLog& component : components_) {
        fill(window_, 2, y - 2, kLogicalWidth - 4, kLogRowHeight - 2, kPanel);
        text(window_, 4, y + 2, component.name, kComponentColours[static_cast<std::size_t>(&component - components_.data()) % kComponentColours.size()]);
        text(window_, x, y + 2, component.file.empty() ? "-" : shortenMiddle(component.file, 32).c_str(),
             component.file.empty() ? kDim : kText);
        text(window_, x + 33 * 8, y + 2, shortenMiddle(component.status, 27).c_str(), component.file.empty() ? kDim : kText);
        text(window_, x + 61 * 8, y + 2, shortenMiddle(component.what, kWhatChars).c_str(), kDim);
        y += kLogRowHeight;
    }

    // What the emulator records besides its log: the Telemetry tab.
    y += 10;
    text(window_, 4, y, "Telemetry", kText);
    text(window_, x, y,
         gTelemetryTab ? "the recorded telemetry files and the latest events are on the Telemetry tab (T)"
                       : "recorded to telemetry-<date>.log and ffb-<date>.log in the logs folder (O opens it)",
         kDim);
    y += kLogRowHeight;
    text(window_, 4, y, "Old files", kText);
    if (!files_.oldFiles.empty()) {
        text(window_, x, y, shortenMiddle(files_.oldFiles, kPathChars).c_str(),
             files_.oldFilesFailed ? kWarning : kText);
    }
    drawLiveLog(y + 24);
}

void MappingView::drawLiveLog(const float top) {
    constexpr float kLineHeight = 11;
    constexpr float kMessageX = 4 + 35 * 8;
    constexpr float kBarThickness = 8;
    const auto nowMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                      std::chrono::steady_clock::now().time_since_epoch())
                                                      .count());
    if (nowMs - livePolledMs_ >= 250) {
        livePolledMs_ = nowMs;
        liveLog_.poll();
    }
    const auto& lines = liveLog_.lines();
    // Scrolled up: keep the same lines in view while new ones arrive below.
    if (liveScroll_ > 0) {
        liveScroll_ += static_cast<std::size_t>(liveLog_.added() - liveSeen_);
    }
    liveSeen_ = liveLog_.added();
    liveLongest_ = 0;
    for (const LiveLogLine& line : lines) liveLongest_ = std::max(liveLongest_, line.message.size());
    const bool sideways = !liveWrap_ && liveLongest_ > kLiveMessageChars;
    if (!sideways) liveHScroll_ = 0;
    liveHScroll_ = std::min(liveHScroll_, liveLongest_ > kLiveMessageChars ? liveLongest_ - kLiveMessageChars : 0);

    const float listTop = top + 14;
    const float listBottom = kLogicalHeight - 34 - (sideways ? kBarThickness + 2 : 0);
    liveRows_ = static_cast<std::size_t>(std::max(1.0F, (listBottom - listTop) / kLineHeight));
    liveScroll_ = std::min(liveScroll_, liveMostScroll());

    fill(window_, 0, top - 8, kLogicalWidth, 1, kPanel);
    text(window_, 4, top, "LIVE LOG", kHeading);
    char status[112];
    if (liveScroll_ == 0) {
        std::snprintf(status, sizeof(status), "newest at the bottom; drag the bar, wheel or PageUp to go back (%zu lines)",
                      lines.size());
    } else {
        std::snprintf(status, sizeof(status), "%zu lines up: End (or scroll down) to follow the newest again",
                      liveScroll_);
    }
    text(window_, 76, top, status, liveScroll_ == 0 ? kDim : kUnlabelled);
    // Word wrap (W): long messages on several rows instead of off the side.
    wrapW_ = 96;
    wrapX_ = kLogicalWidth - 4 - wrapW_;
    wrapY_ = top - 3;
    fill(window_, wrapX_, wrapY_, wrapW_, 14, liveWrap_ ? kHeading : kPanel);
    const char* const wrapLabel = liveWrap_ ? "Wrap: on (W)" : "Wrap: off (W)";
    text(window_, wrapX_ + (wrapW_ - static_cast<float>(std::strlen(wrapLabel)) * 8) / 2, top, wrapLabel,
         liveWrap_ ? kDark : kText);
    vBar_.shown = false;
    hBar_.shown = false;
    if (lines.empty()) {
        text(window_, 4, listTop, "no log lines today yet: start the service, the game or a driver", kDim);
        return;
    }

    // A message's rows: one, cut to the view and scrolled sideways; or, wrapped, as many as it needs (at spaces
    // where it can).
    const auto rowsOf = [this](const std::string& message) {
        std::vector<std::string> rows;
        if (!liveWrap_) {
            std::string row = liveHScroll_ < message.size() ? message.substr(liveHScroll_) : std::string();
            if (row.size() > kLiveMessageChars) row = row.substr(0, kLiveMessageChars - 3) + "...";
            rows.push_back(std::move(row));
            return rows;
        }
        std::size_t start = 0;
        do {
            std::size_t end = std::min(message.size(), start + kLiveMessageChars);
            if (end < message.size()) {
                const std::size_t space = message.rfind(' ', end);
                if (space != std::string::npos && space > start + kLiveMessageChars / 2) end = space + 1;
            }
            rows.push_back(message.substr(start, end - start));
            start = end;
        } while (start < message.size());
        return rows;
    };

    // The lines that fit, from the newest one shown upwards (with wrap a line may take several rows).
    const std::size_t last = lines.size() - liveScroll_;   // one past the newest line shown
    std::size_t first = last;
    std::size_t rowsUsed = 0;
    while (first > 0) {
        const std::size_t need = liveWrap_ ? rowsOf(lines[first - 1].message).size() : 1;
        if (rowsUsed + need > liveRows_ && rowsUsed > 0) break;
        rowsUsed += need;
        --first;
    }
    float y = listTop;
    char head[16];
    for (std::size_t i = first; i < last; ++i) {
        const LiveLogLine& line = lines[i];
        const auto source = static_cast<std::size_t>(std::max(line.source, 0));
        std::snprintf(head, sizeof(head), "%-8.8s", line.time.c_str());
        text(window_, 4, y, head, kDim);
        const char* name = source < components_.size() ? components_[source].name : "?";
        text(window_, 4 + 9 * 8, y, name, kComponentColours[source % kComponentColours.size()]);
        text(window_, 4 + 21 * 8, y, shortenMiddle(line.logger, 13).c_str(), kDim);
        const Colour colour = line.level == LiveLogLevel::Error   ? kWarning
                              : line.level == LiveLogLevel::Warn  ? kUnlabelled
                              : line.level == LiveLogLevel::Debug || line.level == LiveLogLevel::Trace ? kDim
                                                                                                      : kText;
        for (const std::string& row : rowsOf(line.message)) {
            if (y + kLineHeight > listBottom + 1) break;
            text(window_, kMessageX, y, row.c_str(), colour);
            y += kLineHeight;
        }
    }

    // Vertical scroll bar: where the lines shown sit in all of them. Drag it, or click the track to jump.
    const float total = static_cast<float>(lines.size());
    vBar_ = {kLogicalWidth - 2 - kBarThickness, listTop, kBarThickness, listBottom - listTop, 0, 0, true};
    vBar_.thumbSize = std::max(12.0F, vBar_.h * static_cast<float>(last - first) / total);
    const float vMost = static_cast<float>(liveMostScroll());
    const float vAt = vMost > 0 ? 1.0F - static_cast<float>(liveScroll_) / vMost : 1.0F;
    vBar_.thumbStart = (vBar_.h - vBar_.thumbSize) * vAt;
    fill(window_, vBar_.x, vBar_.y, vBar_.w, vBar_.h, kPanel);
    fill(window_, vBar_.x, vBar_.y + vBar_.thumbStart, vBar_.w, vBar_.thumbSize,
         drag_ == Drag::Vertical ? kText : kHeading);

    // Horizontal scroll bar, under the messages, when one is wider than the view (not with word wrap).
    if (sideways) {
        const float width = static_cast<float>(kLiveMessageChars) * 8;
        hBar_ = {kMessageX, listBottom + 2, width, kBarThickness, 0, 0, true};
        hBar_.thumbSize = std::max(12.0F, width * static_cast<float>(kLiveMessageChars) / static_cast<float>(liveLongest_));
        const float hMost = static_cast<float>(liveLongest_ - kLiveMessageChars);
        hBar_.thumbStart = (width - hBar_.thumbSize) * static_cast<float>(liveHScroll_) / hMost;
        fill(window_, hBar_.x, hBar_.y, hBar_.w, hBar_.h, kPanel);
        fill(window_, hBar_.x + hBar_.thumbStart, hBar_.y, hBar_.thumbSize, hBar_.h,
             drag_ == Drag::Horizontal ? kText : kHeading);
    }
}

void MappingView::drawTabs() {
    constexpr float kBarTop = kLogicalHeight - 28;
    fill(window_, 0, kBarTop, kLogicalWidth, 1, kPanel);
    const auto labelOf = [](const Tab tab) {
        switch (tab) {
            case Tab::Mapping: return "Mapping (M)";
            case Tab::WheelSetup: return "Rig (R)";
            case Tab::Logs: return "Logs (L)";
            case Tab::Telemetry: return "Telemetry (T)";
            case Tab::ForceFeedback: return "Force feedback (F)";
        }
        return "?";
    };
    float x = 4;
    for (const Tab tab : visibleTabs()) {
        const char* label = labelOf(tab);
        const bool active = tab == tab_;
        fill(window_, x, kBarTop + 4, kTabWidth, 20, active ? kHeading : kPanel);
        const float labelWidth = static_cast<float>(std::strlen(label)) * 8;
        text(window_, x + (kTabWidth - labelWidth) / 2, kBarTop + 10, label, active ? kDark : kText);
        x += kTabWidth + 6;
    }
}

void MappingView::onClick(const float x, const float y) {
    constexpr float kBarTop = kLogicalHeight - 28;
    if (y >= kBarTop + 4 && y < kBarTop + 24) {
        const std::vector<Tab> kTabs = visibleTabs();
        const float offset = x - 4;
        const auto index = static_cast<std::size_t>(std::max(0.0F, offset) / (kTabWidth + 6));
        if (offset >= 0 && index < kTabs.size() && offset - static_cast<float>(index) * (kTabWidth + 6) < kTabWidth) {
            tab_ = kTabs[index];
        }
        return;
    }
    if (tab_ == Tab::Telemetry && y >= kTelemetryRowsTop - 2 && y < kTelemetryRowsTop - 2 + 2 * kLogRowHeight) {
        openFilesFolder();   // the telemetry files' folder
        return;
    }
    if (tab_ == Tab::Logs) {
        const auto inside = [x, y](const float bx, const float by, const float bw, const float bh) {
            return x >= bx && x < bx + bw && y >= by && y < by + bh;
        };
        if (wrapW_ > 0 && inside(wrapX_, wrapY_, wrapW_, 14)) {
            liveWrap_ = !liveWrap_;
            liveHScroll_ = 0;
            return;
        }
        // A press on a scroll bar: on the thumb it is taken where pressed; elsewhere on the track the thumb jumps
        // there (centred on the mouse). Either way the drag goes on while the button is held.
        if (vBar_.shown && inside(vBar_.x - 3, vBar_.y, vBar_.w + 6, vBar_.h)) {
            const float offset = y - (vBar_.y + vBar_.thumbStart);
            const bool onThumb = offset >= 0 && offset < vBar_.thumbSize;
            dragGrab_ = onThumb ? offset : vBar_.thumbSize / 2;
            drag_ = Drag::Vertical;
            dragTo(x, y);
            return;
        }
        if (hBar_.shown && inside(hBar_.x, hBar_.y - 3, hBar_.w, hBar_.h + 6)) {
            const float offset = x - (hBar_.x + hBar_.thumbStart);
            const bool onThumb = offset >= 0 && offset < hBar_.thumbSize;
            dragGrab_ = onThumb ? offset : hBar_.thumbSize / 2;
            drag_ = Drag::Horizontal;
            dragTo(x, y);
            return;
        }
    }
    if (tab_ == Tab::Logs && y >= kLogRowsTop - 2) {
        const auto row = static_cast<std::size_t>((y - (kLogRowsTop - 2)) / kLogRowHeight);
        if (row < components_.size()) {
            openFolder(components_[row].folder);
        }
    }
}

// --- the Telemetry tab --------------------------------------------------------------------------

namespace {

enum EventKind { kEventPressed, kEventReleased, kEventDpad, kEventPedal, kEventFfb, kEventKinds };
constexpr std::array<Colour, kEventKinds> kEventColours = {{
    {132, 214, 96},    // a button pressed: green
    {120, 126, 136},   // released: dim
    {80, 204, 190},    // D-pad: teal
    {236, 152, 72},    // pedals: orange
    {186, 140, 255},   // game force feedback: purple
}};
constexpr std::array<const char*, kEventKinds> kEventNames = {"button", "release", "D-pad", "pedal", "FFB"};
constexpr std::size_t kMaxEvents = 200;

// "22:49:33.488", local time.
std::string clockNow() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char text[16];
    std::snprintf(text, sizeof(text), "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec,
                  static_cast<int>(ms));
    return text;
}

const char* hatName(const std::int8_t hat) {
    static constexpr std::array<const char*, 8> kNames = {"up",   "up-right",  "right", "down-right",
                                                          "down", "down-left", "left",  "up-left"};
    return hat >= 0 && hat < 8 ? kNames[static_cast<std::size_t>(hat)] : "centred";
}

}  // namespace

void MappingView::addEvent(const int kind, std::string text) {
    events_.push_back({clockNow(), kind, std::move(text)});
    while (events_.size() > kMaxEvents) events_.pop_front();
}

void MappingView::trackEvents(const RawInputState& raw, const WheelState& state, const TargetReport& report,
                              const FfbDisplay& ffb) {
    const std::array<float, 3> pedals = {state.throttle, state.brake, state.clutch};
    if (!eventsStarted_) {
        // What is already held when the window opens is not an event.
        eventsStarted_ = true;
        eventButtons_ = raw.buttons;
        eventHat_ = report.hat;
        for (std::size_t i = 0; i < pedals.size(); ++i) pedalDown_[i] = pedals[i] > 0.02F;
        return;
    }
    const std::uint64_t changed = raw.buttons ^ eventButtons_;
    for (int source = 0; changed != 0 && source < static_cast<int>(kMaxSourceButtons); ++source) {
        if (((changed >> source) & 1ULL) == 0) continue;
        const bool down = ((raw.buttons >> source) & 1ULL) != 0;
        const std::string& name = sourceLabel(source);
        std::string text = "Rig " + std::to_string(source) + (name.empty() ? "" : " " + name);
        if (!down) {
            addEvent(kEventReleased, text + " released");
            continue;
        }
        const auto index = static_cast<std::size_t>(source);
        const int target = sourceToTarget_[index];
        if (sourceHatDirection_[index] != nullptr) {
            text += " pressed -> " + target_ + " D-pad " + sourceHatDirection_[index];
        } else if (target >= 0) {
            const std::string_view targetName = profile_.buttonName(static_cast<std::size_t>(target));
            text += " pressed -> " + target_ + " " + std::to_string(target) + " " + std::string(targetName);
        } else {
            text += " pressed -> nothing (not mapped)";
        }
        addEvent(kEventPressed, text);
    }
    eventButtons_ = raw.buttons;

    if (report.hat != eventHat_) {
        addEvent(kEventDpad, target_ + " D-pad " + hatName(report.hat));
        eventHat_ = report.hat;
    }

    static constexpr std::array<const char*, 3> kPedals = {"Throttle", "Brake", "Clutch"};
    for (std::size_t i = 0; i < pedals.size(); ++i) {
        // Pressed past 2 %, released below 1 % (the gap stops a pedal resting at the edge from flickering).
        if (!pedalDown_[i] && pedals[i] > 0.02F) {
            pedalDown_[i] = true;
            pedalPeak_[i] = pedals[i];
            addEvent(kEventPedal, std::string(kPedals[i]) + " pressed");
        } else if (pedalDown_[i]) {
            pedalPeak_[i] = std::max(pedalPeak_[i], pedals[i]);
            if (pedals[i] < 0.01F) {
                pedalDown_[i] = false;
                char text[64];
                std::snprintf(text, sizeof(text), "%s released (deepest %.0f%%)", kPedals[i], pedalPeak_[i] * 100.0F);
                addEvent(kEventPedal, text);
            }
        }
    }

    if (ffb.controller != nullptr) {
        const std::size_t slots = std::min(ffb.controller->slotCount(), ffbPlaying_.size());
        for (std::size_t i = 0; i < slots; ++i) {
            const FfbController::SlotView slot = ffb.controller->slot(i);
            if (slot.playing != ffbPlaying_[i]) {
                ffbPlaying_[i] = slot.playing;
                char text[96];
                std::snprintf(text, sizeof(text), "slot %zu %s %s", i, ffbTypeName(slot.requested.type),
                              slot.playing ? "starts" : "stops");
                addEvent(kEventFfb, text);
            }
        }
    }
}

void MappingView::drawTelemetry() {
    constexpr std::size_t kPathChars = 80;
    const float x = 4 + 14 * 8;
    const TelemetryFiles& t = files_.telemetry;

    // The files: where the service records all of this (click a line to open the folder).
    text(window_, 4, kTelemetryTop, "TELEMETRY", kHeading);
    text(window_, 92, kTelemetryTop, "what the service records (logs, one file a day) and the latest events", kDim);
    float y = kTelemetryTop + 14;
    text(window_, 4, y, "Folder", kDim);
    const std::filesystem::path folder = t.on ? t.samplesPath.parent_path() : files_.logFolder;
    text(window_, x, y, shortenMiddle(displayPath(folder), kPathChars).c_str(), kText);
    text(window_, x + (kPathChars + 2) * 8, y, t.on ? "recording   O opens" : "not recording   O opens",
         t.on ? kDim : kUnlabelled);
    y = kTelemetryRowsTop;
    struct Row {
        const char* name;
        std::filesystem::path path;
        std::uint64_t rows;
        const char* unit;
        const char* what;
    };
    const std::array<Row, 2> rows = {{{"Samples", t.samplesPath, t.rows, "rows", "the wheel, the mapping, what the game sees"},
                                      {"Force fb", t.ffbPath, t.ffbRows, "changes", "every force feedback change the game sent"}}};
    for (const Row& row : rows) {
        fill(window_, 2, y - 2, kLogicalWidth - 4, kLogRowHeight - 2, kPanel);
        text(window_, 4, y + 2, row.name, kText);
        if (!t.on || row.path.empty()) {
            text(window_, x, y + 2, t.on ? "-" : files_.telemetryStatus.c_str(), kDim);
        } else {
            text(window_, x, y + 2, shortenMiddle(row.path.filename().string(), 32).c_str(), kText);
            std::error_code ec;
            const std::string size = std::filesystem::exists(row.path, ec) ? ", " + describeFile(row.path) : "";
            const std::string status = std::to_string(row.rows) + " " + row.unit + size;
            text(window_, x + 33 * 8, y + 2, shortenMiddle(status, 27).c_str(), kText);
        }
        text(window_, x + 61 * 8, y + 2, row.what, kDim);
        y += kLogRowHeight;
    }

    y += 10;

    // Held right now.
    text(window_, 4, y, "Held now", kText);
    std::string held;
    for (int source = 0; source < static_cast<int>(kMaxSourceButtons); ++source) {
        if (((eventButtons_ >> source) & 1ULL) != 0) {
            const std::string& name = sourceLabel(source);
            held += (held.empty() ? "" : ",  ") + ("Rig " + std::to_string(source)) + (name.empty() ? "" : " " + name);
        }
    }
    text(window_, x, y, held.empty() ? "no buttons" : shortenMiddle(held, 104).c_str(), held.empty() ? kDim : kText);

    // The latest events: as many as fit, newest at the bottom (no scrolling: the telemetry files have everything).
    y += 24;
    fill(window_, 0, y - 8, kLogicalWidth, 1, kPanel);
    text(window_, 4, y, "LATEST EVENTS", kHeading);
    text(window_, 116, y, "buttons, D-pad, pedals and game force feedback since this window opened, newest at the bottom",
         kDim);
    constexpr float kLineHeight = 11;
    const float listTop = y + 14;
    const float listBottom = kLogicalHeight - 34;
    const auto fit = static_cast<std::size_t>(std::max(1.0F, (listBottom - listTop) / kLineHeight));
    if (events_.empty()) {
        text(window_, 4, listTop, "nothing yet: press a button or a pedal", kDim);
        return;
    }
    const std::size_t first = events_.size() > fit ? events_.size() - fit : 0;
    y = listTop;
    for (std::size_t i = first; i < events_.size(); ++i) {
        const TelemetryEvent& event = events_[i];
        const auto kind = static_cast<std::size_t>(event.kind);
        text(window_, 4, y, event.time.substr(0, 12).c_str(), kDim);
        text(window_, 4 + 14 * 8, y, kEventNames[kind], kEventColours[kind]);
        text(window_, 4 + 23 * 8, y, shortenMiddle(event.text, 94).c_str(), kind == kEventReleased ? kDim : kText);
        y += kLineHeight;
    }
}

void MappingView::drawForceFeedback(const FfbDisplay& ffb) {
    // What the game asks for now and what reaches the wheel, then how it has been used since the start.
    drawFfb(ffb, 34);
    float y = 196;
    if (ffb.controller != nullptr) {
        fill(window_, 0, y - 8, kLogicalWidth, 1, kPanel);
        text(window_, 4, y, "USAGE", kHeading);
        drawFfbUsage(*ffb.controller, y);
        y = 356;
    }

    // The latest starts and stops (the same events as the Telemetry tab, force feedback only).
    fill(window_, 0, y - 8, kLogicalWidth, 1, kPanel);
    text(window_, 4, y, "LATEST EFFECTS", kHeading);
    text(window_, 124, y, "effects starting and stopping since this window opened, newest at the bottom", kDim);
    constexpr float kLineHeight = 11;
    const float listTop = y + 14;
    const auto fit = static_cast<std::size_t>(std::max(1.0F, (kLogicalHeight - 34 - listTop) / kLineHeight));
    std::vector<const TelemetryEvent*> shown;
    for (auto it = events_.rbegin(); it != events_.rend() && shown.size() < fit; ++it) {
        if (it->kind == kEventFfb) shown.push_back(&*it);
    }
    if (shown.empty()) {
        text(window_, 4, listTop, "none yet: start the game and drive", kDim);
        return;
    }
    y = listTop;
    for (auto it = shown.rbegin(); it != shown.rend(); ++it) {
        text(window_, 4, y, (*it)->time.substr(0, 12).c_str(), kDim);
        text(window_, 4 + 14 * 8, y, (*it)->text.c_str(), kText);
        y += kLineHeight;
    }
}

}  // namespace wheelbri::service
