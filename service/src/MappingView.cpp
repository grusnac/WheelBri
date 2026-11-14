// SPDX-License-Identifier: GPL-3.0-or-later
#include "MappingView.hpp"
#include "UiStyle.hpp"

#include "AppLogLevel.hpp"
#include "HelperProbe.hpp"

#include "wheelbri/FfbGroups.hpp"
#include "wheelbri/LocalDate.hpp"
#include "wheelbri/LogFiles.hpp"
#include "wheelbri/Logging.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.ui");
    return logger;
}

// Everything is laid out on the window's logical canvas: at least MappingView::kWidth x kHeight, bigger when the
// window has room (UiWindow.hpp). Text is 8x8 logical pixels per character.

constexpr std::size_t kMaxSourceButtons = MapperConfig::kMaxSourceButtons;

using Colour = ui::Colour;

constexpr Colour kBackground = style::kBackground;
constexpr Colour kPanel = style::kPanel;
constexpr Colour kText = style::kText;
constexpr Colour kDim = style::kDim;
constexpr Colour kHeading = style::kHeading;
constexpr Colour kActive = style::kActive;
constexpr Colour kWarning = style::kWarning;
constexpr Colour kUnlabelled = style::kCaution;
constexpr Colour kDark = style::kDark;
constexpr Colour kCentre{160, 164, 172};

// One colour per component, the same in the file list and the live log (index = its line in the list).
constexpr std::array<Colour, 5> kComponentColours = {{
    {108, 182, 255},   // Emulator: blue (style::kHeading)
    {236, 152, 72},    // Game shim: orange
    {186, 140, 255},   // G920 driver: purple
    {80, 204, 190},    // Hide filter: teal
    {222, 196, 96},    // Helper: yellow
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
    if (ShadowView::hasPicture(profile_)) shadow_ = std::make_unique<ShadowView>(window_, profile_, mapper_);
#if defined(_WIN32)
    // The app's window is open: say Hello to the WheelBri Helper now (on its own thread), so the helper writes what it
    // kept since boot into this user's logs folder and the Logs tab knows how it is.
    (void)HelperProbe::shared();
#endif
    // Ask the profile rather than assuming a pedal convention.
    WheelState released;
    WheelState pressed;
    pressed.throttle = 1.0F;
    char note[96];
    std::snprintf(note, sizeof(note), "%s pedals: released = %d, fully pressed = %d (raw %s number shown).",
                  target_.c_str(), profile.encode(released).throttle, profile.encode(pressed).throttle,
                  target_.c_str());
    pedalNote_ = note;
    indexMapping();
}

void MappingView::setMapping(const MapperConfig& mapper) {
    mapper_ = mapper;
    if (shadow_) shadow_->setMapping(mapper_);
    indexMapping();
}

void MappingView::indexMapping() {
    sourceToTarget_.fill(kUnmapped);
    sourceTargetCount_.fill(0);
    sourceHatDirection_.fill(nullptr);
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
    // A wheel button that is only a secondary source (target_N_or) reaches its target too (after the main sources, so
    // a main source stays the one named).
    for (std::size_t target = 0; target < mapper_.buttonMap.size(); ++target) {
        if (profile_.buttonName(target).empty()) continue;
        for (const ButtonSource& secondary : mapper_.buttonSecondary[target]) {
            if (!secondary.mapped() || secondary.axis || secondary.device != 0 ||
                static_cast<std::size_t>(secondary.index) >= kMaxSourceButtons) {
                continue;
            }
            const auto index = static_cast<std::size_t>(secondary.index);
            if (sourceTargetCount_[index]++ == 0) {
                sourceToTarget_[index] = static_cast<int>(target);
            }
        }
    }
}

namespace {
bool gTelemetryTab = false;       // --telemetry-tab
bool gForceFeedbackTab = false;   // --ffb-tab
bool gEditorTabs = false;         // the app's window: Mapping (the editor), Live, Games
}  // namespace

void MappingView::setOptionalTabs(const bool telemetry, const bool forceFeedback) {
    gTelemetryTab = telemetry;
    gForceFeedbackTab = forceFeedback;
}

void MappingView::setEditorTabs(const bool editor) { gEditorTabs = editor; }
bool MappingView::editorTabs() { return gEditorTabs; }

std::vector<MappingView::Tab> MappingView::visibleTabs() {
    // In the order they are used: the rig first, then its mapping, then the game profiles.
    // With the editor, Live right of Mapping: edit, then see it at work.
    std::vector<Tab> tabs = {Tab::WheelSetup, Tab::Mapping};
    if (gEditorTabs) {
        tabs.push_back(Tab::Live);
        tabs.push_back(Tab::Games);
    }
    tabs.push_back(Tab::Logs);
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
    } else if (key == 'g' && gEditorTabs) {
        tab_ = Tab::Games;
    } else if (key == 'l') {
        tab_ = Tab::Logs;
    } else if (key == 't' && gTelemetryTab) {
        tab_ = Tab::Telemetry;
    } else if (key == 'r') {
        tab_ = Tab::WheelSetup;
    } else if (key == 's' && gEditorTabs && tab_ != Tab::Live && !(sideBySide_ && tab_ == Tab::Mapping)) {
        tab_ = Tab::Live;   // S again there switches Shadow / List
    } else if (key == 's' && livePage() && shadow_) {
        showShadow(!gShowShadow);
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
        if (key == 'v' && AppLogLevel::shared().started()) static_cast<void>(AppLogLevel::shared().next());
    }
}

void MappingView::onScroll(const int notches) {
    if (tab_ == Tab::Logs) {
        scrollLiveLog(static_cast<long long>(notches) * 3);
    }
}

void MappingView::onHScroll(const int notches) {
    if (tab_ != Tab::Logs || liveWrap_) return;
    const auto most = static_cast<long long>(liveLongest_ > liveMessageChars_ ? liveLongest_ - liveMessageChars_ : 0);
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
        const float most = static_cast<float>(liveLongest_ > liveMessageChars_ ? liveLongest_ - liveMessageChars_ : 0);
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

void MappingView::drawLivePage(const RawInputs& inputs, const WheelState& state, const TargetReport& report,
                               const FfbDisplay& ffb) {
    const RawInputState& raw = inputs.devices[0];
    trackLastPressed(raw.buttons);
    trackEvents(raw, state, report, ffb);
    drawHeader();
    drawLive(inputs, state, report, ffb);
}

void MappingView::drawLive(const RawInputs& inputs, const WheelState& state, const TargetReport& report,
                           const FfbDisplay& ffb) {
    const RawInputState& raw = inputs.devices[0];
    if (showingShadow()) {
        // The target drawn, what the game sees lit; its force feedback at the bottom of the right column.
        shadow_->setColumnReserve(kFfbGroupsHeight + 4);
        shadow_->draw(state, report, 38);
        drawFfbGroups(ffb, shadow_->columnX(), shadow_->reserveTop() + 4, shadow_->columnWidth());
        return;
    }
    // What is mapped to what: the axes and pedals (rig -> G920), every rig axis, the D-pad, the buttons.
    drawAxes(inputs, state, report, 34);
    drawRawAxes(raw, 128);
    drawHat(raw.hat, report.hat, 128);
    drawTargetButtons(report.buttons, 244);
    drawSourceButtons(raw.buttons, 401);
    drawFooter(545);
    drawFfbGroups(ffb, 0, 584, std::min(canvasW(), 480.0F));
}

float MappingView::drawFfbGroups(const FfbDisplay& ffb, const float x, float y, const float width) {
    style::headingBand(window_, x, y, width);
    style::heading(window_, x + 6, y, "FORCE FEEDBACK");
    const bool live = ffb.controller != nullptr;
    text(window_, x + 128, y, live ? "blue: the game asks, green: the wheel gets" : "", kDim);
    y += 16;
    if (!live) {
        // No game force feedback in this session: why (game_effects off, the port taken, the wheel has none).
        text(window_, x + 6, y, ffb.status.empty() ? "off" : ffb.status.c_str(), kDim);
        return y + 14;
    }
    const FfbGroupLevels levels = ffbGroupLevels(*ffb.controller);
    constexpr float kNameW = 16 * ui::kCharWidth;
    constexpr float kTextW = 12 * ui::kCharWidth;   // "100% -> 100%"
    const float barW = std::clamp((width - 6 - kNameW - kTextW - 18) / 2, 32.0F, 96.0F);
    const auto percent = [](const float v) { return std::to_string(std::lround(v * 100.0F)) + "%"; };
    for (std::size_t g = 0; g < kFfbGroupCount; ++g) {
        const float row = y + static_cast<float>(g) * kFfbRowH;
        const float asked = levels.asked[g];
        const float sent = levels.sent[g];
        const bool used = asked > 0.0F || sent > 0.0F;
        text(window_, x + 6, row, std::string(kFfbGroupNames[g]).c_str(), used ? kText : kDim);
        const float barX = x + 6 + kNameW;
        fill(window_, barX, row, barW, 8, kPanel);
        fill(window_, barX, row, barW * asked, 8, kHeading);
        fill(window_, barX + barW + 6, row, barW, 8, kPanel);
        fill(window_, barX + barW + 6, row, barW * sent, 8, kActive);
        text(window_, barX + 2 * barW + 18, row, used ? (percent(asked) + " -> " + percent(sent)).c_str() : "-",
             kDim);
    }
    return y + static_cast<float>(kFfbGroupCount) * kFfbRowH;
}

void MappingView::draw(const RawInputs& inputs, const WheelState& state, const TargetReport& report,
                       const FfbDisplay& ffb) {
    const RawInputState& raw = inputs.devices[0];
    trackLastPressed(raw.buttons);

    trackEvents(raw, state, report, ffb);
    if (tab_ == Tab::WheelSetup || tab_ == Tab::Games || (tab_ == Tab::Mapping && gEditorTabs)) {
        drawTabs();   // the window draws the tab itself (WheelSetupView, MappingEditView's pages)
        return;
    }
    if (tab_ != Tab::Logs) drawHeader();   // the Logs page starts at the top (the Mapping tab has the header)
    if (livePage()) {
        drawLive(inputs, state, report, ffb);
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
    if (livePage()) {
        // The app's controls are at the top right of this tab: what is left of them (more in a wider window).
        // (With the shim its games row is right of the title: that stays as it is.)
        line[std::min(headerChars_ + (shimMode_ ? 0 : extraChars()), sizeof(line) - 1)] = '\0';
    }
    text(window_, 4, 4, line, kHeading);
    // The second line: the devices (or the hint), and on the Mapping tab what the session runs as at its right end.
    std::size_t room = 118 + extraChars();
    if (livePage() && !sessionLine_.empty()) {
        const float x = window_.width() - 4 - static_cast<float>(sessionLine_.size()) * ui::kCharWidth;
        text(window_, x, 16, sessionLine_.c_str(), kActive);
        room = std::min(room, static_cast<std::size_t>(std::max(0.0F, x - 16) / ui::kCharWidth));
    }
    // LIVE: the running session's view (the Mapping tab's editor is the EDIT page), at the second line's start.
    float x = 4;
    if (livePage()) {
        window_.fill(2, 14, 40, 12, kActive);
        text(window_, 6, 16, "LIVE", kDark);
        x = 48;
        room = room > 6 ? room - 6 : 0;
        if (shadow_) {
            // The view switch right of LIVE: Shadow (the picture) or List (every rig axis and button).
            style::button(window_, ShadowView::kSwitchX, ShadowView::kSwitchY, ShadowView::kSwitchShadowW,
                          ShadowView::kSwitchH, "Shadow", gShowShadow, true);
            style::button(window_, ShadowView::kSwitchX + ShadowView::kSwitchShadowW + ShadowView::kSwitchGap,
                          ShadowView::kSwitchY, ShadowView::kSwitchListW, ShadowView::kSwitchH, "List", !gShowShadow, true);
            x = ShadowView::kSwitchEnd + 8;
            const auto used = static_cast<std::size_t>((x - 48) / ui::kCharWidth);
            room = room > used ? room - used : 0;
        }
        // The app's Side by side (the editor left, this page right), on when it is.
        if (gEditorTabs) {
            sideBySideX_ = x;
            style::button(window_, x, ShadowView::kSwitchY, kSideBySideW, ShadowView::kSwitchH, "Side by side (B)",
                          sideBySide_, true);
            x += kSideBySideW + 8;
            const auto used = static_cast<std::size_t>((kSideBySideW + 8) / ui::kCharWidth);
            room = room > used ? room - used : 0;
        }
    }
    if (devices_.empty()) {
        text(window_, x, 16, std::string("Move every control and check where it lands.").substr(0, room).c_str(), kDim);
    } else {
        text(window_, x, 16, devices_.substr(0, room).c_str(), devicesWarning_ ? kWarning : kDim);
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
                                        : labels.buttons[static_cast<std::size_t>(std::min(index, static_cast<int>(kMaxSourceButtons) - 1))];
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
    // The three bars share what a wider window adds (a quarter, a quarter, and the target's a bit more).
    const float rawW = 110 + extraW() * 0.25F;
    const float mappedW = 110 + extraW() * 0.25F;
    const float targetW = 150 + extraW() * 0.3F;
    const float rawX = 112;
    const float mappedX = rawX + rawW + 60;
    const float targetX = mappedX + mappedW + 60;
    text(window_, 4, kTop, "AXES", kHeading);
    text(window_, 76, kTop, "Rig", kDim);
    text(window_, rawX, kTop, "Rig raw", kDim);
    text(window_, mappedX, kTop, "mapped", kDim);
    const std::string valueHeading = target_ + " value (0..65535)";
    text(window_, targetX, kTop, valueHeading.c_str(), kDim);

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
        bipolarBar(window_, rawX, y, rawW, 10, rawValue, valid ? kActive : kDim);
        std::snprintf(value, sizeof(value), "%+.3f", rawValue);
        text(window_, rawX + rawW + 4, y + 1, value, kText);

        if (row.bipolarMapped) {
            bipolarBar(window_, mappedX, y, mappedW, 10, row.mapped, kActive);
            std::snprintf(value, sizeof(value), "%+.3f", row.mapped);
        } else {
            unipolarBar(window_, mappedX, y, mappedW, 10, row.mapped, kActive);
            std::snprintf(value, sizeof(value), "%5.1f%%", row.mapped * 100.0F);
        }
        text(window_, mappedX + mappedW + 4, y + 1, value, kText);

        unipolarBar(window_, targetX, y, targetW, 10, static_cast<float>(row.target) / 65535.0F, kHeading);
        std::snprintf(value, sizeof(value), "%5d", row.target);
        text(window_, targetX + targetW + 6, y + 1, value, kText);
    }
    std::string accessories;
    for (std::size_t i = 0; i < accessories_.size(); ++i) {
        if (accessories_[i].vendorId == 0 && accessories_[i].productId == 0) continue;
        accessories += (accessories.empty() ? "  d" : ", d") + std::to_string(i + 1) + " = " +
                       (accessories_[i].name.empty() ? deviceId(accessories_[i].vendorId, accessories_[i].productId)
                                                     : accessories_[i].name);
    }
    text(window_, 4, kTop + 78, (pedalNote_ + accessories).substr(0, 118 + extraChars()).c_str(), kDim);
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
        const float barW = 150 + extraW() * 0.25F;   // the D-pad panel moves right by as much
        bipolarBar(window_, 64, y, barW, 8, v, kActive);
        std::snprintf(value, sizeof(value), "%+.3f", v);
        text(window_, 64 + barW + 6, y, value, kText);
        const char* const role = roleOf(axis);
        text(window_, 64 + barW + 62, y, role, role[0] == '-' ? kText : kDim);
    }
}

void MappingView::drawHat(const std::int8_t sourceHat, const std::int8_t targetHat, const float top) {
    const float kLeft = 470 + extraW() * 0.25F;   // right of the rig axes, whose bars grow as much
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
    // The target's numbers are the game's (gameButtonNumber); the INI's target_N stays as named, from 0.
    const std::string subtitle =
        buttonOrder_ == ButtonOrder::Native && profile_.hasNativeButtonOrder()
            ? target_ + " button (native, from 0) name <- rig number name   (INI: target_N, Logitech's order)"
            : target_ + " button (from 1, as games show it) name <- rig number name   (INI: target_N, N = button - 1)";
    text(window_, subtitleX, kTop, subtitle.c_str(), kDim);

    // Every cell reads "G920  7 Menu     <- Rig 35 Start": three columns across the canvas (316 wide in the design:
    // 39 characters after the 3-pixel margin; more in a wider window).
    constexpr int kColumns = 3;
    const float kWidth = (canvasW() - 8 - 2 * (kColumns - 1)) / kColumns;
    constexpr float kHeight = 13;
    const std::size_t chars = std::clamp<std::size_t>(static_cast<std::size_t>((kWidth - 3) / 8), 39, 120);
    char cellText[128];
    // Only the buttons the target names (the G920's 11 and the shifter's 7; its unused 12th is left out) and that
    // something feeds (a main or a secondary source), packed; room for 9 rows.
    std::size_t cell = 0;
    for (std::size_t target = 0; target < mapper_.buttonMap.size(); ++target) {
        const std::string_view name = profile_.buttonName(target);
        if (name.empty() || (mapper_.buttonMap[target] < 0 && !mapper_.buttonSecondary[target][0].mapped())) {
            continue;
        }
        const float x = 4 + static_cast<float>(cell % kColumns) * (kWidth + 2);
        const float y = kTop + 12 + static_cast<float>(cell / kColumns) * (kHeight + 2);
        ++cell;
        const bool pressed = ((targetButtons >> target) & 1U) != 0;
        const int source = mapper_.buttonMap[target];

        const std::string label(name.substr(0, 8));
        const int number = gameButtonNumber(profile_, buttonOrder_, target);   // as the game numbers it
        if (source < 0) {
            std::snprintf(cellText, sizeof(cellText), "%s %2d %-8s <- --", target_.c_str(), number, label.c_str());
        } else if (mapper_.buttonDevice[target] == 0) {
            std::snprintf(cellText, sizeof(cellText), "%s %2d %-8s <- Rig %2d %s", target_.c_str(), number,
                          label.c_str(), source, sourceLabel(source).c_str());
        } else {
            std::snprintf(cellText, sizeof(cellText), "%s %2d %-8s <- %s", target_.c_str(), number, label.c_str(),
                          sourceText(mapper_.buttonDevice[target], source, false, 20 + (chars - 39)).c_str());
        }

        cellText[chars] = '\0';   // what fits the cell
        if (mapper_.buttonSecondary[target][0].mapped()) {
            // Secondary sources too (a button, or an axis past a threshold): a "+" at the cell's end.
            const std::size_t length = std::strlen(cellText);
            if (length < chars - 1) {
                std::memset(cellText + length, ' ', chars - 1 - length);
            }
            cellText[chars - 1] = '+';
            cellText[chars] = '\0';
        }
        fill(window_, x, y, kWidth, kHeight, pressed ? kActive : kPanel);
        const Colour colour = pressed ? kDark : (source < 0 ? kDim : kText);
        text(window_, x + 3, y + 3, cellText, colour);
    }
    if (cell == 0) {
        const std::string none = "(no " + target_ + " button is mapped)";
        text(window_, 4, kTop + 14, none.c_str(), kDim);
    }
}

void MappingView::drawSourceButtons(const SourceButtons& sourceButtons, const float top) {
    const float kTop = top;
    text(window_, 4, kTop, "RIG BUTTONS", kHeading);
    const std::string legend = "only the ones that reach a " + target_ + " button or the D-pad; pressed = green";
    text(window_, 100, kTop, legend.c_str(), kDim);

    // Every cell reads "Rig 35 Start": the rig button only (where it goes is in the panel above), only the mapped ones,
    // packed. Eight columns across the canvas: 116 wide in the design, 14 characters after the margin; more in a
    // wider window.
    constexpr int kColumns = 8;
    const float kWidth = (canvasW() - 8 - 3 * (kColumns - 1)) / kColumns;
    constexpr float kHeight = 13;
    const std::size_t chars = std::max<std::size_t>(14, static_cast<std::size_t>((kWidth - 4) / 8));
    char line[80];
    int cell = 0;
    for (int source = 0; source < sourceButtons_; ++source) {
        const auto index = static_cast<std::size_t>(source);
        const int target = sourceToTarget_[index];
        const char* const hatDirection = sourceHatDirection_[index];
        if (target < 0 && hatDirection == nullptr) {
            continue;   // goes nowhere: left out
        }
        const float x = 4 + static_cast<float>(cell % kColumns) * (kWidth + 3);
        const float y = kTop + 12 + static_cast<float>(cell / kColumns) * (kHeight + 2);
        ++cell;
        const bool pressed = sourceButtons.test(source);

        fill(window_, x, y, kWidth, kHeight, pressed ? kActive : kPanel);
        const Colour foreground = pressed ? kDark : kText;

        std::snprintf(line, sizeof(line), "Rig %d %s", source, sourceLabel(source).c_str());
        text(window_, x + 2, y + 3, std::string(line).substr(0, chars).c_str(), foreground);
    }
    if (sourceButtons_ == 0) {
        text(window_, 4, kTop + 14, "(the device reports no buttons)", kDim);
    } else if (cell == 0) {
        text(window_, 4, kTop + 14, "(no rig button is mapped)", kDim);
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
        // Same pattern as the cells: "G920 7 Menu <- Rig 35 Start".
        const std::string& name = sourceLabel(lastPressedSource_);
        char button[40];
        std::snprintf(button, sizeof(button), name.empty() ? "Rig %d" : "Rig %d %s", lastPressedSource_, name.c_str());
        if (hatDirection != nullptr) {
            std::snprintf(line, sizeof(line), "Last pressed: %s D-pad %s <- %s%s", target_.c_str(), hatDirection,
                          button, target >= 0 ? "  (and a button: check [buttons])" : "");
            text(window_, 4, kTop, line, target >= 0 ? kUnlabelled : kText);
        } else if (target >= 0) {
            const std::string_view targetName = profile_.buttonName(static_cast<std::size_t>(target));
            std::snprintf(line, sizeof(line), "Last pressed: %s %d %.*s <- %s%s", target_.c_str(),
                          gameButtonNumber(profile_, buttonOrder_, static_cast<std::size_t>(target)),
                          static_cast<int>(targetName.size()), targetName.empty() ? "" : targetName.data(), button,
                          sourceTargetCount_[index] > 1 ? "  (and others)" : "");
            text(window_, 4, kTop, line, kText);
        } else {
            std::snprintf(line, sizeof(line), "Last pressed: %s  (not mapped to any %s button)", button,
                          target_.c_str());
            text(window_, 4, kTop, line, kWarning);
        }
    }
    text(window_, 4, kTop + 14,
         gEditorTabs ? "To change: the Mapping tab (M); each change is in use at once."
                     : "To change: INI [buttons] target_N, [hat] up/right/down/left, [button_labels] button_N.",
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

void MappingView::trackLastPressed(const SourceButtons& sourceButtons) {
    const SourceButtons newlyPressed = sourceButtons & ~previousSourceButtons_;
    previousSourceButtons_ = sourceButtons;
    for (int source = 0; source < static_cast<int>(kMaxSourceButtons); ++source) {
        if (newlyPressed.test(source)) {
            lastPressedSource_ = source;
            // target: the INI's target_N (0-based, as named); the game shows it as the second number.
            const int target = sourceToTarget_[static_cast<std::size_t>(source)];
            if (target >= 0) {
                WE_LOG_DEBUG(log(), "rig button {} pressed -> {} target {} (button {} in the game)", source, target_,
                             target, gameButtonNumber(profile_, buttonOrder_, static_cast<std::size_t>(target)));
            } else {
                WE_LOG_DEBUG(log(), "rig button {} pressed -> {} target {}", source, target_, target);
            }
        }
    }
}

// --- files ----------------------------------------------------------------------------------------

namespace {

// Keeps both ends of a long path: "C:\Users\...\logs\telemetry-2026-09-30-01.log".
// A logger's name as the live log shows it: its last part, at most 13 characters ("service.devices": "devices").
std::string shortLogger(const std::string& logger) {
    const std::size_t dot = logger.rfind('.');
    std::string name = dot == std::string::npos ? logger : logger.substr(dot + 1);
    if (name.size() > 13) name = name.substr(0, 12) + ".";
    return name;
}

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
    // Each call's error is checked on its own: a folder named like a log has a write time but no size, and the
    // second call would otherwise clear the first one's error ("0 KB").
    const auto bytes = std::filesystem::file_size(path, ec);
    if (ec) return "cannot read it: " + ec.message();
    const auto written = std::filesystem::last_write_time(path, ec);
    if (ec) return "cannot read it: " + ec.message();
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

    // Every part of wheel-emulator that writes a log, in the order they sit between the wheel and the game.
    struct Source {
        const char* name;
        const char* what;
        bool driver;            // in the drivers' folder, not the user's
        const char* stem;       // its files are <stem>-<date>-<NN>.log, one per run (LogFiles.hpp)
    };
    static constexpr Source kSources[] = {
        {"WheelBri", "the app: rig, mapping, force feedback", false, "wheelbri-service"},
        {"Game shim", "dinput8.dll next to the game (Mode Shim)", false, "wheelbri-shim"},
        {"Wheel driver", "the virtual wheel, every target (wheelbri-wheel)", true, "wheelbri-driver"},
        {"Hide filter", "hides the rig from games (wheelbri-hide)", true, "wheelbri-hide"},
        // The WheelBri Helper (a service) writes into the connected user's folder (HelperLog), the same one.
        {"Helper", "the Administrator steps for the app (WheelBri Helper)", true, "wheelbri-helper"},
    };
    components_.clear();
    std::map<std::u8string, std::vector<std::string>> names;   // each folder listed once
    for (const Source& source : kSources) {
        if (shimMode_ && source.driver) continue;   // the shim's session runs no driver
        // The drivers write to the same folder (the app shares it with them: ServiceLogging.hpp shareLogFolder).
#if defined(_WIN32)
        const std::filesystem::path folder = files_.logFolder;
#else
        const std::filesystem::path folder = source.driver ? std::filesystem::path() : files_.logFolder;
#endif
        ComponentLog component{source.name, source.what, folder, {}, {}, {}, false};
        const bool isHelper = std::string_view(source.stem) == "wheelbri-helper";
        // The latest run's file (another component may have started yesterday and still be writing).
        const std::string stem = source.stem;
        if (!names.count(component.folder.u8string())) {
            names[component.folder.u8string()] = logfiles::namesIn(component.folder);
        }
        const std::optional<std::string> newest = logfiles::newestOf(names[component.folder.u8string()], stem);
        std::filesystem::path path = newest ? component.folder / *newest : std::filesystem::path();
        if (stem == "wheelbri-service") {
            // The service knows its own file (it may be elsewhere through [logging] file).
            if (!files_.logToFile || logging::currentLogFile().empty()) {
                component.status = "console only (no log file)";
                components_.push_back(std::move(component));
                continue;
            }
            path = std::filesystem::path(logging::currentLogFile());
            component.folder = path.parent_path();
        }
        // The live log follows the latest run's file (a new run's appears as soon as it writes).
        liveLog_.setFile(static_cast<int>(components_.size()), component.folder.empty() ? std::filesystem::path() : path);
        std::error_code ec;
        if (component.folder.empty()) {
            component.status = "Windows only";
        } else if (!path.empty() && std::filesystem::exists(path, ec)) {
            component.file = path.filename().string();
            component.status = describeFile(path);
        } else {
            component.status = "no log yet";
        }
        if (isHelper && !component.folder.empty()) {
            // What the app last heard from the helper (it says Hello at start, and again from here at most every
            // 5 minutes), and whether its latest log has warnings or errors.
            HelperProbe::shared().request();
            component.state = helper::describeHelper(HelperProbe::shared().latest());
            helperLevels_.setFile(path);
            helperLevels_.poll();
            const LogLevelCount& count = helperLevels_.count();
            if (count.any()) {
                component.warns = true;
                // "2 err, 3 warn, last 14:03" (the column is 27 characters)
                std::string said;
                if (count.errors > 0) said = std::to_string(count.errors) + " err";
                if (count.warnings > 0) said += (said.empty() ? "" : ", ") + std::to_string(count.warnings) + " warn";
                if (count.lastTime.size() >= 5) said += ", last " + count.lastTime.substr(0, 5);
                component.status = said;
            }
        }
        components_.push_back(std::move(component));
    }
}

void MappingView::drawLogs() {
    const std::size_t kPathChars = 90 + extraChars();
    const std::size_t kWhatChars = 41 + extraChars();   // to the right edge
    refreshLogFiles();
    text(window_, 4, kLogsTop, "LOGS", kHeading);
    text(window_, 52, kLogsTop, "one line per component, a new file every run and every day; click a line to open its folder",
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
    style::headingBand(window_, 0, y, canvasW());
    style::heading(window_, 8, y, "COMPONENT");
    style::heading(window_, x, y, "LATEST LOG (ONE PER RUN)");
    text(window_, x + 36 * 8, y, "SIZE, LAST WRITTEN", kHeading);
    y = kLogRowsTop;
    for (const ComponentLog& component : components_) {
        fill(window_, 2, y - 2, canvasW() - 4, kLogRowHeight - 2, kPanel);
        const Colour own = kComponentColours[static_cast<std::size_t>(&component - components_.data()) %
                                             kComponentColours.size()];
        text(window_, 4, y + 2, component.name, component.warns ? kWarning : own);
        // "wheelbri-service-2026-10-08-03.log" is 34 characters.
        text(window_, x, y + 2, component.file.empty() ? "-" : shortenMiddle(component.file, 35).c_str(),
             component.file.empty() ? kDim : kText);
        text(window_, x + 36 * 8, y + 2, shortenMiddle(component.status, 27).c_str(),
             component.warns ? kWarning : component.file.empty() ? kDim : kText);
        // The helper's line says what the app last heard from it instead of what it is.
        text(window_, x + 64 * 8, y + 2,
             shortenMiddle(component.state.empty() ? component.what : component.state, kWhatChars).c_str(), kDim);
        y += kLogRowHeight;
    }

    // What the emulator records besides its log (the Telemetry tab when it shows), and what happened to old files: one
    // line.
    y += 10;
    text(window_, 4, y, "Recordings", kText);
    const std::string recorded = gTelemetryTab ? "telemetry and force feedback: Telemetry tab (T)"
                                               : "telemetry and force feedback, a file each per run";
    text(window_, x, y, recorded.c_str(), kDim);
    if (!files_.oldFiles.empty()) {   // after it: what was done with old files ("3 files older than 7 days deleted")
        const float oldX = x + static_cast<float>(recorded.size() + 3) * 8;
        const std::size_t room = kPathChars > recorded.size() + 3 ? kPathChars - recorded.size() - 3 : 0;
        text(window_, oldX, y, shortenMiddle("old files: " + files_.oldFiles, room).c_str(),
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
    // The messages run to the scroll bar: 80 characters in the design, more in a wider window.
    liveMessageChars_ = std::max<std::size_t>(20, static_cast<std::size_t>((canvasW() - kMessageX - 30) / 8));
    const bool sideways = !liveWrap_ && liveLongest_ > liveMessageChars_;
    if (!sideways) liveHScroll_ = 0;
    liveHScroll_ = std::min(liveHScroll_, liveLongest_ > liveMessageChars_ ? liveLongest_ - liveMessageChars_ : 0);

    const float listTop = top + 14;
    // More rows in a taller window.
    const float listBottom = canvasH() - 34 - (sideways ? kBarThickness + 2 : 0);
    liveRows_ = static_cast<std::size_t>(std::max(1.0F, (listBottom - listTop) / kLineHeight));
    liveScroll_ = std::min(liveScroll_, liveMostScroll());

    fill(window_, 0, top - 8, canvasW(), 1, kPanel);
    style::headingBand(window_, 0, top, canvasW());
    style::heading(window_, 8, top, "LIVE LOG");
    // Word wrap (W): long messages on several rows instead of off the side.
    wrapW_ = 96;
    wrapX_ = canvasW() - 4 - wrapW_;
    wrapY_ = top - 3;
    style::checkbox(window_, wrapX_, wrapY_ - 1, wrapW_, "Wrap (W)", liveWrap_, true);
    // The log level (V, the app only): warn, info, debug, trace and round again, for every component. Warnings and
    // errors are always written. Lit while it writes more than warnings, so a forgotten trace shows.
    levelW_ = 0;
    if (AppLogLevel::shared().started()) {
        const LogLevel level = AppLogLevel::shared().current();
        levelW_ = 136;   // "Level: debug (V)" and a margin
        levelX_ = wrapX_ - 8 - levelW_;
        levelY_ = top - 3;
        const bool loud = level != LogLevel::Warn;
        fill(window_, levelX_, levelY_, levelW_, 14, loud ? kHeading : kPanel);
        char levelLabel[24];
        std::snprintf(levelLabel, sizeof(levelLabel), "Level: %s (V)", logLevelName(level));
        text(window_, levelX_ + (levelW_ - static_cast<float>(std::strlen(levelLabel)) * 8) / 2, top, levelLabel,
             loud ? kDark : kText);
    }
    // What is left between the title and the buttons.
    char status[112];
    if (liveScroll_ == 0) {
        std::snprintf(status, sizeof(status), "newest last; wheel or PageUp goes back (%zu)", lines.size());
    } else {
        std::snprintf(status, sizeof(status), "%zu lines up: End follows the newest", liveScroll_);
    }
    const float statusRoom = (levelW_ > 0 ? levelX_ : wrapX_) - 8 - 76;
    std::string statusText = status;
    if (statusText.size() * 8 > static_cast<std::size_t>(std::max(0.0F, statusRoom))) {
        statusText.resize(static_cast<std::size_t>(std::max(0.0F, statusRoom)) / 8);
    }
    text(window_, 76, top, statusText.c_str(), liveScroll_ == 0 ? kDim : kUnlabelled);
    vBar_.shown = false;
    hBar_.shown = false;
    if (lines.empty()) {
        text(window_, 4, listTop, "no log lines yet: start the service, the game or a driver", kDim);
        return;
    }

    // A message's rows: one, cut to the view and scrolled sideways; or, wrapped, as many as it needs (at spaces
    // where it can).
    const auto rowsOf = [this](const std::string& message) {
        std::vector<std::string> rows;
        if (!liveWrap_) {
            std::string row = liveHScroll_ < message.size() ? message.substr(liveHScroll_) : std::string();
            if (row.size() > liveMessageChars_) row = row.substr(0, liveMessageChars_ - 3) + "...";
            rows.push_back(std::move(row));
            return rows;
        }
        std::size_t start = 0;
        do {
            std::size_t end = std::min(message.size(), start + liveMessageChars_);
            if (end < message.size()) {
                const std::size_t space = message.rfind(' ', end);
                if (space != std::string::npos && space > start + liveMessageChars_ / 2) end = space + 1;
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
        text(window_, 4 + 21 * 8, y, shortLogger(line.logger).c_str(), kDim);
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
    vBar_ = {canvasW() - 2 - kBarThickness, listTop, kBarThickness, listBottom - listTop, 0, 0, true};
    vBar_.thumbSize = std::max(12.0F, vBar_.h * static_cast<float>(last - first) / total);
    const float vMost = static_cast<float>(liveMostScroll());
    const float vAt = vMost > 0 ? 1.0F - static_cast<float>(liveScroll_) / vMost : 1.0F;
    vBar_.thumbStart = (vBar_.h - vBar_.thumbSize) * vAt;
    fill(window_, vBar_.x, vBar_.y, vBar_.w, vBar_.h, kPanel);
    fill(window_, vBar_.x, vBar_.y + vBar_.thumbStart, vBar_.w, vBar_.thumbSize,
         drag_ == Drag::Vertical ? kText : kHeading);

    // Horizontal scroll bar, under the messages, when one is wider than the view (not with word wrap).
    if (sideways) {
        const float width = static_cast<float>(liveMessageChars_) * 8;
        hBar_ = {kMessageX, listBottom + 2, width, kBarThickness, 0, 0, true};
        hBar_.thumbSize = std::max(12.0F, width * static_cast<float>(liveMessageChars_) / static_cast<float>(liveLongest_));
        const float hMost = static_cast<float>(liveLongest_ - liveMessageChars_);
        hBar_.thumbStart = (width - hBar_.thumbSize) * static_cast<float>(liveHScroll_) / hMost;
        fill(window_, hBar_.x, hBar_.y, hBar_.w, hBar_.h, kPanel);
        fill(window_, hBar_.x + hBar_.thumbStart, hBar_.y, hBar_.thumbSize, hBar_.h,
             drag_ == Drag::Horizontal ? kText : kHeading);
    }
}

namespace {

// The tab buttons' labels.
const char* tabLabel(const MappingView::Tab tab) {
    using Tab = MappingView::Tab;
    switch (tab) {
        case Tab::Mapping: return "Mapping (M)";
        case Tab::Live: return "Live (S)";
        case Tab::Games: return "Games (G)";
        case Tab::WheelSetup: return "Rig (R)";
        case Tab::Logs: return "Logs (L)";
        case Tab::Telemetry: return "Telemetry (T)";
        case Tab::ForceFeedback: return "Force feedback (F)";
    }
    return "?";
}

}  // namespace

float MappingView::tabsEnd() {
    float x = kTabLeft;
    for (const Tab tab : visibleTabs()) x += tabWidth(tabLabel(tab)) + kTabGap;
    return x - kTabGap;
}

void MappingView::drawTabs() {
    const float kBarTop = canvasH() - kTabBarHeight;   // always at the bottom
    fill(window_, 0, kBarTop, canvasW(), 1, kPanel);
    float x = kTabLeft;
    for (const Tab tab : visibleTabs()) {
        const char* label = tabLabel(tab);
        // Side by side, Mapping and Live show together: both lit.
        const bool active = tab == tab_ || (sideBySide_ && pairedTab(tab) && pairedTab(tab_));
        const float width = tabWidth(label);
        fill(window_, x, kBarTop + 4, width, kTabHeight, active ? kHeading : kPanel);
        text(window_, x + kTabPadding, kBarTop + 10, label, active ? kDark : kText);
        x += width + kTabGap;
    }
}

void MappingView::showShadow(const bool on) {
    gShowShadow = on;
    WE_LOG_INFO(log(), "Mapping tab: {} view", on ? "Shadow" : "List");
}

void MappingView::onClick(const float x, const float y) {
    if (livePage() && gEditorTabs && y >= ShadowView::kSwitchY && y < ShadowView::kSwitchY + ShadowView::kSwitchH &&
        x >= sideBySideX_ && x < sideBySideX_ + kSideBySideW) {
        sideBySideRequested_ = true;
        return;
    }
    if (livePage() && shadow_ && y >= ShadowView::kSwitchY && y < ShadowView::kSwitchY + ShadowView::kSwitchH) {
        if (x >= ShadowView::kSwitchX && x < ShadowView::kSwitchX + ShadowView::kSwitchShadowW) {
            showShadow(true);
            return;
        }
        const float listX = ShadowView::kSwitchX + ShadowView::kSwitchShadowW + ShadowView::kSwitchGap;
        if (x >= listX && x < listX + ShadowView::kSwitchListW) {
            showShadow(false);
            return;
        }
    }
    const float kBarTop = canvasH() - kTabBarHeight;
    if (y >= kBarTop + 4 && y < kBarTop + 24) {
        float left = kTabLeft;
        for (const Tab tab : visibleTabs()) {
            const float width = tabWidth(tabLabel(tab));
            if (x >= left && x < left + width) {
                tab_ = tab;
                break;
            }
            left += width + kTabGap;
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
        if (levelW_ > 0 && inside(levelX_, levelY_, levelW_, 14)) {
            static_cast<void>(AppLogLevel::shared().next());   // warn -> info -> debug -> trace -> warn
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
    const std::tm local = localTime(seconds);
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
    const SourceButtons changed = raw.buttons ^ eventButtons_;
    for (int source = changed.next(0); source >= 0; source = changed.next(source + 1)) {
        const bool down = raw.buttons.test(source);
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
            text += " pressed -> " + target_ + " " +
                    std::to_string(gameButtonNumber(profile_, buttonOrder_, static_cast<std::size_t>(target))) + " " +
                    std::string(targetName);
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
    const std::size_t kPathChars = 80 + extraChars();
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
        fill(window_, 2, y - 2, canvasW() - 4, kLogRowHeight - 2, kPanel);
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
        if (eventButtons_.test(source)) {
            const std::string& name = sourceLabel(source);
            held += (held.empty() ? "" : ",  ") + ("Rig " + std::to_string(source)) + (name.empty() ? "" : " " + name);
        }
    }
    text(window_, x, y, held.empty() ? "no buttons" : shortenMiddle(held, 104 + extraChars()).c_str(),
         held.empty() ? kDim : kText);

    // The latest events: as many as fit, newest at the bottom (no scrolling: the telemetry files have everything).
    y += 24;
    fill(window_, 0, y - 8, canvasW(), 1, kPanel);
    text(window_, 4, y, "LATEST EVENTS", kHeading);
    text(window_, 116, y, "buttons, D-pad, pedals and game force feedback since this window opened, newest at the bottom",
         kDim);
    constexpr float kLineHeight = 11;
    const float listTop = y + 14;
    const float listBottom = canvasH() - 34;   // more rows in a taller window
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
        text(window_, 4 + 23 * 8, y, shortenMiddle(event.text, 94 + extraChars()).c_str(), kind == kEventReleased ? kDim : kText);
        y += kLineHeight;
    }
}

void MappingView::drawForceFeedback(const FfbDisplay& ffb) {
    // What the game asks for now and what reaches the wheel, then how it has been used since the start.
    drawFfb(ffb, 34);
    float y = 196;
    if (ffb.controller != nullptr) {
        fill(window_, 0, y - 8, canvasW(), 1, kPanel);
        text(window_, 4, y, "USAGE", kHeading);
        drawFfbUsage(*ffb.controller, y);
        y = 356;
    }

    // The latest starts and stops (the same events as the Telemetry tab, force feedback only).
    fill(window_, 0, y - 8, canvasW(), 1, kPanel);
    text(window_, 4, y, "LATEST EFFECTS", kHeading);
    text(window_, 124, y, "effects starting and stopping since this window opened, newest at the bottom", kDim);
    constexpr float kLineHeight = 11;
    const float listTop = y + 14;
    const auto fit =
        static_cast<std::size_t>(std::max(1.0F, (canvasH() - 34 - listTop) / kLineHeight));
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
