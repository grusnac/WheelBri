#include "MappingView.hpp"

#include "wheelemu/LocalDate.hpp"
#include "wheelemu/Logging.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>

namespace wheelemu::service {

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
    // Ask the profile rather than assuming the G29 convention, so the note is right for every target.
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
    for (const auto& [source, direction] : hatDirections) {
        if (source >= 0 && static_cast<std::size_t>(source) < kMaxSourceButtons) {
            sourceHatDirection_[static_cast<std::size_t>(source)] = direction;
        }
    }
    for (std::size_t target = 0; target < mapper_.buttonMap.size(); ++target) {
        const int source = mapper_.buttonMap[target];
        if (source >= 0 && static_cast<std::size_t>(source) < kMaxSourceButtons) {
            const auto index = static_cast<std::size_t>(source);
            if (sourceTargetCount_[index]++ == 0) {
                sourceToTarget_[index] = static_cast<int>(target);
            }
        }
    }
}

void MappingView::onKey(const char key) {
    if (key == 'f') {
        openFilesFolder();
    } else if (key == 'u') {
        showFfbUsage_ = !showFfbUsage_;
    }
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

void MappingView::draw(const RawInputState& raw, const WheelState& state, const TargetReport& report,
                       const FfbDisplay& ffb) {
    trackLastPressed(raw.buttons);

    drawHeader();
    drawAxes(raw, state, report);
    drawRawAxes(raw);
    drawHat(raw.hat, report.hat);
    drawTargetButtons(report.buttons);
    drawSourceButtons(raw.buttons);
    drawFooter();
    drawFfb(ffb);
    drawFiles();
}

void MappingView::drawHeader() {
    char line[128];
    std::snprintf(line, sizeof(line), "%.40s  ->  %.40s", sourceName_.c_str(),
                  profile_.identity().productName.c_str());
    text(window_, 4, 4, line, kHeading);
    text(window_, 4, 16, "Move every control and check where it lands.", kDim);
}

void MappingView::drawAxes(const RawInputState& raw, const WheelState& state, const TargetReport& report) {
    struct Row {
        const char* name;
        int sourceAxis;
        bool bipolarMapped;
        float mapped;
        std::int32_t target;
    };
    const std::array<Row, 4> rows = {{
        {"Steering", mapper_.steering.sourceAxis, true, state.steering, report.steering},
        {"Throttle", mapper_.throttle.sourceAxis, false, state.throttle, report.throttle},
        {"Brake", mapper_.brake.sourceAxis, false, state.brake, report.brake},
        {"Clutch", mapper_.clutch.sourceAxis, false, state.clutch, report.clutch},
    }};

    constexpr float kTop = 34;
    text(window_, 4, kTop, "AXES", kHeading);
    text(window_, 76, kTop, "R3", kDim);
    text(window_, 112, kTop, "R3 raw", kDim);
    text(window_, 282, kTop, "mapped", kDim);
    const std::string valueHeading = target_ + " value (0..65535)";
    text(window_, 452, kTop, valueHeading.c_str(), kDim);

    char value[32];
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        const float y = kTop + 12 + static_cast<float>(i) * 16;
        const bool valid = isValidAxis(row.sourceAxis);
        text(window_, 4, y + 1, row.name, kText);

        if (valid) {
            std::snprintf(value, sizeof(value), "ax%d", row.sourceAxis);
        } else {
            std::snprintf(value, sizeof(value), "--");
        }
        text(window_, 76, y + 1, value, valid ? kText : kWarning);

        const float rawValue = valid ? raw.axes[static_cast<std::size_t>(row.sourceAxis)] : 0.0F;
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
    text(window_, 4, kTop + 78, pedalNote_.c_str(), kDim);
}

void MappingView::drawRawAxes(const RawInputState& raw) {
    constexpr float kTop = 128;
    text(window_, 4, kTop, "ALL R3 AXES", kHeading);

    auto roleOf = [this](const int axis) -> const char* {
        if (axis == mapper_.steering.sourceAxis) return "-> steering";
        if (axis == mapper_.throttle.sourceAxis) return "-> throttle";
        if (axis == mapper_.brake.sourceAxis) return "-> brake";
        if (axis == mapper_.clutch.sourceAxis) return "-> clutch";
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

void MappingView::drawHat(const std::int8_t sourceHat, const std::int8_t targetHat) {
    constexpr float kLeft = 470;
    constexpr float kTop = 128;
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
    std::snprintf(line, sizeof(line), "R3 hat %2d -> %s hat %2d", sourceHat, target_.c_str(), targetHat);
    text(window_, kLeft, gridTop + 3 * (kSize + 2) + 4, line, kText);
    text(window_, kLeft, gridTop + 3 * (kSize + 2) + 16, "-1 = centred, 0..7 clockwise", kDim);
}

void MappingView::drawTargetButtons(const std::uint32_t targetButtons) {
    constexpr float kTop = 390;
    const std::string heading = target_ + " BUTTONS";
    text(window_, 4, kTop, heading.c_str(), kHeading);
    const float subtitleX = 4 + static_cast<float>(heading.size() + 1) * 8;
    text(window_, subtitleX, kTop, "number (= target_N in the INI), label  <- R3 button that drives it", kDim);

    constexpr int kColumns = 4;
    constexpr float kWidth = 176;
    constexpr float kHeight = 13;
    char cellText[48];
    for (std::size_t target = 0; target < mapper_.buttonMap.size(); ++target) {
        const float x = 4 + static_cast<float>(target % kColumns) * (kWidth + 2);
        const float y = kTop + 12 + static_cast<float>(target / kColumns) * (kHeight + 2);
        const bool pressed = ((targetButtons >> target) & 1U) != 0;
        const int source = mapper_.buttonMap[target];
        const std::string_view name = profile_.buttonName(target);

        char sourceText[16];
        if (source >= 0) {
            std::snprintf(sourceText, sizeof(sourceText), "R3 %d", source);
        } else {
            std::snprintf(sourceText, sizeof(sourceText), "--");
        }
        const std::string label = name.empty() ? std::string("(none)") : std::string(name.substr(0, 9));
        std::snprintf(cellText, sizeof(cellText), "%2zu %-9s<- %s", target, label.c_str(), sourceText);

        fill(window_, x, y, kWidth, kHeight, pressed ? kActive : kPanel);
        const Colour colour = pressed ? kDark : (source < 0 || name.empty() ? kDim : kText);
        text(window_, x + 3, y + 3, cellText, colour);
    }
}

void MappingView::drawSourceButtons(const std::uint64_t sourceButtons) {
    constexpr float kTop = 530;
    text(window_, 4, kTop, "R3 BUTTONS", kHeading);
    const std::string legend =
        "green = " + target_ + " button or D-pad, yellow = unlabelled button, red = not mapped";
    text(window_, 92, kTop, legend.c_str(), kDim);

    constexpr int kColumns = 16;
    constexpr float kWidth = 42;
    constexpr float kHeight = 21;
    char line[16];
    for (int source = 0; source < sourceButtons_; ++source) {
        const auto index = static_cast<std::size_t>(source);
        const float x = 4 + static_cast<float>(source % kColumns) * (kWidth + 2.8F);
        const float y = kTop + 12 + static_cast<float>(source / kColumns) * (kHeight + 2);
        const bool pressed = ((sourceButtons >> source) & 1ULL) != 0;
        const int target = sourceToTarget_[index];
        const char* const hatDirection = sourceHatDirection_[index];

        // A target with no label is outside the buttons this wheel is known to have (e.g. G920 12+).
        const bool labelled = target >= 0 && !profile_.buttonName(static_cast<std::size_t>(target)).empty();
        Colour background = kPanel;
        if (pressed) {
            if (hatDirection != nullptr) {
                background = kActive;
            } else {
                background = target < 0 ? kWarning : (labelled ? kActive : kUnlabelled);
            }
        }
        fill(window_, x, y, kWidth, kHeight, background);
        const Colour foreground = pressed ? kDark : (target >= 0 || hatDirection != nullptr ? kText : kDim);

        std::snprintf(line, sizeof(line), "R3 %d", source);
        text(window_, x + 2, y + 2, line, foreground);
        if (hatDirection != nullptr) {
            // Short arrow so it fits the cell; a second mapping to a button is flagged with '+'.
            std::snprintf(line, sizeof(line), ">D%c%s", hatDirection[0] - 'a' + 'A', target >= 0 ? "+" : "");
        } else if (target >= 0) {
            std::snprintf(line, sizeof(line), sourceTargetCount_[index] > 1 ? ">%d+" : ">%d", target);
        } else {
            std::snprintf(line, sizeof(line), "--");
        }
        text(window_, x + 2, y + 12, line, foreground);
    }
    if (sourceButtons_ == 0) {
        text(window_, 4, kTop + 14, "(the device reports no buttons)", kDim);
    }
}

void MappingView::drawFooter() {
    constexpr float kTop = 644;
    char line[160];
    if (lastPressedSource_ < 0) {
        std::snprintf(line, sizeof(line), "Press any button on the wheel to see where it goes.");
        text(window_, 4, kTop, line, kDim);
    } else {
        const auto index = static_cast<std::size_t>(lastPressedSource_);
        const int target = sourceToTarget_[index];
        const char* const hatDirection = sourceHatDirection_[index];
        if (hatDirection != nullptr) {
            std::snprintf(line, sizeof(line), "Last pressed: R3 button %d -> %s D-pad %s%s", lastPressedSource_,
                          target_.c_str(), hatDirection, target >= 0 ? " (and a button: check [buttons])" : "");
            text(window_, 4, kTop, line, target >= 0 ? kUnlabelled : kText);
        } else if (target >= 0) {
            const std::string_view name = profile_.buttonName(static_cast<std::size_t>(target));
            std::snprintf(line, sizeof(line), "Last pressed: R3 button %d -> %s button %d (%.*s)%s",
                          lastPressedSource_, target_.c_str(), target, static_cast<int>(name.size()),
                          name.empty() ? "" : name.data(),
                          sourceTargetCount_[index] > 1 ? " and others" : "");
            text(window_, 4, kTop, line, kText);
        } else {
            std::snprintf(line, sizeof(line), "Last pressed: R3 button %d -> not mapped to any %s button",
                          lastPressedSource_, target_.c_str());
            text(window_, 4, kTop, line, kWarning);
        }
    }
    text(window_, 4, kTop + 14, "To change: INI [buttons] target_N = R3 button, [hat] up/right/down/left = R3 button.",
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

void MappingView::drawFfb(const FfbDisplay& ffb) {
    constexpr float kTop = 244;
    text(window_, 4, kTop, "FORCE FEEDBACK", kHeading);
    if (ffb.controller == nullptr) {
        text(window_, 132, kTop, ffb.status.empty() ? "off" : ffb.status.c_str(), kDim);
        return;
    }
    const FfbController& controller = *ffb.controller;
    const auto& stats = controller.stats();
    const FfbSafetyConfig& safety = controller.safety();

    char line[160];
    text(window_, 588, kTop, showFfbUsage_ ? "U: live effects" : "U: usage stats", kHeading);
    std::snprintf(line, sizeof(line), "limits: gain %.2f, max %.0f%% of the wheel, slew %.0f/s%s", safety.masterGain,
                  safety.maxMagnitude * 100.0F, safety.maxSlewPerSecond, ffb.invert ? ", direction inverted" : "");
    text(window_, 4, kTop + 12, line, kDim);
    if (showFfbUsage_) {
        drawFfbUsage(controller, kTop);
        return;
    }

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
            WE_LOG_DEBUG(log(), "R3 button {} pressed -> {} target {}", source, target_,
                         sourceToTarget_[static_cast<std::size_t>(source)]);
        }
    }
}

// --- files ----------------------------------------------------------------------------------------

namespace {

// Keeps both ends of a long path: "C:\Users\...\logs\telemetry-20260930-142501.csv".
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
    const std::filesystem::path folder = files_.telemetry.on ? files_.telemetry.samplesPath.parent_path()
                                                              : files_.logFolder;
    if (folder.empty()) {
        WE_LOG_WARN(log(), "no log or telemetry folder to open");
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

void MappingView::drawFiles() {
    constexpr float kTop = 682;
    constexpr std::size_t kPathChars = 76;  // (720 - 4 - 12 * 8 label column) / 8, rounded down
    outline(window_, 0, kTop - 6, kLogicalWidth, kLogicalHeight - kTop + 6, kHeading);
    text(window_, 4, kTop, "LOGS AND TELEMETRY", kHeading);
    text(window_, 164, kTop, "press F to open the folder  (a new file every day)", kText);

    const auto nowMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                      std::chrono::steady_clock::now().time_since_epoch())
                                                      .count());
    if (filesCheckedMs_ == 0 || nowMs - filesCheckedMs_ >= 1000) {
        filesCheckedMs_ = nowMs;
        serviceLogName_ = std::filesystem::path(logging::currentLogFile()).filename().string();
        const std::string today = localDate(WallClock::now());
        const std::string gameLog = "wheelemu-shim-" + today + ".log";
        std::error_code ec;
        gameLogName_ = std::filesystem::exists(files_.logFolder / gameLog, ec) ? gameLog : std::string();
    }

    const float x = 4 + 12 * 8;
    float y = kTop + 14;
    text(window_, 4, y, "Folder", kDim);
    text(window_, x, y, shortenMiddle(displayPath(files_.logFolder), kPathChars).c_str(), kText);

    y += 14;
    text(window_, 4, y, "Logs", kDim);
    if (!files_.logToFile || serviceLogName_.empty()) {
        text(window_, x, y, "console only (the log file could not be opened)", kWarning);
    } else {
        const std::string game = gameLogName_.empty() ? "not started today" : gameLogName_;
        text(window_, x, y, (serviceLogName_ + "   game: " + game).c_str(), kText);
    }

    y += 14;
    text(window_, 4, y, "Telemetry", kDim);
    if (!files_.telemetry.on) {
        text(window_, x, y, files_.telemetryStatus.c_str(), kDim);
    } else {
        const TelemetryFiles& t = files_.telemetry;
        char line[160];
        std::snprintf(line, sizeof(line), "%s (%llu rows)  %s (%llu changes)",
                      t.samplesPath.filename().string().c_str(), static_cast<unsigned long long>(t.rows),
                      t.ffbPath.filename().string().c_str(), static_cast<unsigned long long>(t.ffbRows));
        text(window_, x, y, line, kText);
        if (t.samplesPath.parent_path() != files_.logFolder) {
            // Telemetry in its own folder ([telemetry] directory): say where.
            text(window_, x + static_cast<float>(std::strlen(line) + 1) * 8, y,
                 shortenMiddle("in " + displayPath(t.samplesPath.parent_path()),
                               kPathChars > std::strlen(line) + 1 ? kPathChars - std::strlen(line) - 1 : 0)
                     .c_str(),
                 kDim);
        }
    }

    y += 14;
    text(window_, 4, y, "Old files", kDim);
    if (!files_.oldFiles.empty()) {
        const std::string status = shortenMiddle(files_.oldFiles, kPathChars);
        text(window_, x, y, status.c_str(), files_.oldFilesFailed ? kWarning : kText);
    }
}

}  // namespace wheelemu::service
