#include "MappingView.hpp"

#include "wheelemu/Logging.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

namespace wheelemu::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.ui");
    return logger;
}

// Everything is laid out on a fixed logical canvas; SDL scales it to the window (letterboxed).
// SDL's debug font is 8x8 logical pixels per character.
constexpr int kLogicalWidth = 720;
constexpr int kLogicalHeight = 540;
constexpr int kWindowWidth = 1296;
constexpr int kWindowHeight = 972;

constexpr std::size_t kMaxSourceButtons = 64;

struct Colour {
    Uint8 r, g, b;
};

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

std::unique_ptr<MappingView> MappingView::open(std::string sourceName, const int sourceAxes, const int sourceButtons,
                                               const IDeviceProfile& profile, const MapperConfig& mapper) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        WE_LOG_ERROR(log(), "cannot start SDL video for the mapping window: {}", SDL_GetError());
        return nullptr;
    }
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("wheel-emulator: mapping", kWindowWidth, kWindowHeight, SDL_WINDOW_RESIZABLE,
                                     &window, &renderer)) {
        WE_LOG_ERROR(log(), "cannot create the mapping window: {}", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return nullptr;
    }
    SDL_SetRenderLogicalPresentation(renderer, kLogicalWidth, kLogicalHeight, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderVSync(renderer, 0);  // never let presenting a frame stall the input loop
    const char* const rendererName = SDL_GetRendererName(renderer);
    WE_LOG_INFO(log(), "mapping window open (renderer {})", rendererName != nullptr ? rendererName : "?");
    return std::unique_ptr<MappingView>(
        new MappingView(window, renderer, std::move(sourceName), sourceAxes, sourceButtons, profile, mapper));
}

MappingView::MappingView(SDL_Window* const window, SDL_Renderer* const renderer, std::string sourceName,
                         const int sourceAxes, const int sourceButtons, const IDeviceProfile& profile,
                         const MapperConfig& mapper)
    : window_(window),
      renderer_(renderer),
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

MappingView::~MappingView() {
    SDL_DestroyRenderer(renderer_);
    SDL_DestroyWindow(window_);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    WE_LOG_DEBUG(log(), "mapping window closed");
}

bool MappingView::pumpEvents() {
    SDL_Event event;
    bool open = true;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                open = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_ESCAPE) {
                    open = false;
                }
                break;
            default:
                break;
        }
    }
    return open;
}

// --- drawing helpers ------------------------------------------------------------------------------

namespace {

void setColour(SDL_Renderer* renderer, const Colour colour) {
    SDL_SetRenderDrawColor(renderer, colour.r, colour.g, colour.b, SDL_ALPHA_OPAQUE);
}

void fill(SDL_Renderer* renderer, const float x, const float y, const float w, const float h, const Colour colour) {
    setColour(renderer, colour);
    const SDL_FRect rect{x, y, w, h};
    SDL_RenderFillRect(renderer, &rect);
}

void outline(SDL_Renderer* renderer, const float x, const float y, const float w, const float h, const Colour colour) {
    setColour(renderer, colour);
    const SDL_FRect rect{x, y, w, h};
    SDL_RenderRect(renderer, &rect);
}

void text(SDL_Renderer* renderer, const float x, const float y, const char* value, const Colour colour) {
    setColour(renderer, colour);
    SDL_RenderDebugText(renderer, x, y, value);
}

// -1..1, filled from the centre.
void bipolarBar(SDL_Renderer* renderer, const float x, const float y, const float w, const float h, const float value,
                const Colour colour) {
    fill(renderer, x, y, w, h, kPanel);
    const float half = w * 0.5F;
    const float v = std::clamp(value, -1.0F, 1.0F);
    if (v >= 0.0F) {
        fill(renderer, x + half, y, half * v, h, colour);
    } else {
        fill(renderer, x + half + half * v, y, -half * v, h, colour);
    }
    fill(renderer, x + half - 0.5F, y - 1.0F, 1.0F, h + 2.0F, kCentre);
}

// 0..1, filled from the left.
void unipolarBar(SDL_Renderer* renderer, const float x, const float y, const float w, const float h,
                 const float fraction, const Colour colour) {
    fill(renderer, x, y, w, h, kPanel);
    fill(renderer, x, y, w * std::clamp(fraction, 0.0F, 1.0F), h, colour);
}

}  // namespace

// --- sections -------------------------------------------------------------------------------------

void MappingView::render(const RawInputState& raw, const WheelState& state, const TargetReport& report) {
    trackLastPressed(raw.buttons);

    setColour(renderer_, kBackground);
    SDL_RenderClear(renderer_);

    drawHeader();
    drawAxes(raw, state, report);
    drawRawAxes(raw);
    drawHat(raw.hat, report.hat);
    drawTargetButtons(report.buttons);
    drawSourceButtons(raw.buttons);
    drawFooter();

    SDL_RenderPresent(renderer_);
}

void MappingView::drawHeader() {
    char line[128];
    std::snprintf(line, sizeof(line), "%.40s  ->  %.40s", sourceName_.c_str(),
                  profile_.identity().productName.c_str());
    text(renderer_, 4, 4, line, kHeading);
    text(renderer_, 4, 16, "Move every control and check where it lands. Esc or close to quit.", kDim);
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
    text(renderer_, 4, kTop, "AXES", kHeading);
    text(renderer_, 76, kTop, "R3", kDim);
    text(renderer_, 112, kTop, "R3 raw", kDim);
    text(renderer_, 282, kTop, "mapped", kDim);
    const std::string valueHeading = target_ + " value (0..65535)";
    text(renderer_, 452, kTop, valueHeading.c_str(), kDim);

    char value[32];
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        const float y = kTop + 12 + static_cast<float>(i) * 16;
        const bool valid = isValidAxis(row.sourceAxis);
        text(renderer_, 4, y + 1, row.name, kText);

        if (valid) {
            std::snprintf(value, sizeof(value), "ax%d", row.sourceAxis);
        } else {
            std::snprintf(value, sizeof(value), "--");
        }
        text(renderer_, 76, y + 1, value, valid ? kText : kWarning);

        const float rawValue = valid ? raw.axes[static_cast<std::size_t>(row.sourceAxis)] : 0.0F;
        bipolarBar(renderer_, 112, y, 110, 10, rawValue, valid ? kActive : kDim);
        std::snprintf(value, sizeof(value), "%+.3f", rawValue);
        text(renderer_, 226, y + 1, value, kText);

        if (row.bipolarMapped) {
            bipolarBar(renderer_, 282, y, 110, 10, row.mapped, kActive);
            std::snprintf(value, sizeof(value), "%+.3f", row.mapped);
        } else {
            unipolarBar(renderer_, 282, y, 110, 10, row.mapped, kActive);
            std::snprintf(value, sizeof(value), "%5.1f%%", row.mapped * 100.0F);
        }
        text(renderer_, 396, y + 1, value, kText);

        unipolarBar(renderer_, 452, y, 150, 10, static_cast<float>(row.target) / 65535.0F, kHeading);
        std::snprintf(value, sizeof(value), "%5d", row.target);
        text(renderer_, 608, y + 1, value, kText);
    }
    text(renderer_, 4, kTop + 78, pedalNote_.c_str(), kDim);
}

void MappingView::drawRawAxes(const RawInputState& raw) {
    constexpr float kTop = 128;
    text(renderer_, 4, kTop, "ALL R3 AXES", kHeading);

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
        text(renderer_, 4, y, value, present ? kText : kDim);
        if (!present) {
            text(renderer_, 64, y, "(not on this device)", kDim);
            continue;
        }
        const float v = raw.axes[static_cast<std::size_t>(axis)];
        bipolarBar(renderer_, 64, y, 150, 8, v, kActive);
        std::snprintf(value, sizeof(value), "%+.3f", v);
        text(renderer_, 220, y, value, kText);
        const char* const role = roleOf(axis);
        text(renderer_, 276, y, role, role[0] == '-' ? kText : kDim);
    }
}

void MappingView::drawHat(const std::int8_t sourceHat, const std::int8_t targetHat) {
    constexpr float kLeft = 470;
    constexpr float kTop = 128;
    text(renderer_, kLeft, kTop, "D-PAD / HAT", kHeading);

    // Hat index (clockwise from up) -> grid cell.
    static constexpr std::array<std::array<int, 2>, 8> kCell = {{{1, 0}, {2, 0}, {2, 1}, {2, 2},
                                                                {1, 2}, {0, 2}, {0, 1}, {0, 0}}};
    constexpr float kSize = 20;
    const float gridLeft = kLeft + 30;
    const float gridTop = kTop + 14;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            outline(renderer_, gridLeft + column * (kSize + 2), gridTop + row * (kSize + 2), kSize, kSize, kPanel);
        }
    }
    const bool pressed = targetHat >= 0 && targetHat < 8;
    const auto cell = pressed ? kCell[static_cast<std::size_t>(targetHat)] : std::array<int, 2>{1, 1};
    fill(renderer_, gridLeft + cell[0] * (kSize + 2) + 2, gridTop + cell[1] * (kSize + 2) + 2, kSize - 4, kSize - 4,
         pressed ? kActive : kPanel);

    char line[48];
    std::snprintf(line, sizeof(line), "R3 hat %2d -> %s hat %2d", sourceHat, target_.c_str(), targetHat);
    text(renderer_, kLeft, gridTop + 3 * (kSize + 2) + 4, line, kText);
    text(renderer_, kLeft, gridTop + 3 * (kSize + 2) + 16, "-1 = centred, 0..7 clockwise", kDim);
}

void MappingView::drawTargetButtons(const std::uint32_t targetButtons) {
    constexpr float kTop = 244;
    const std::string heading = target_ + " BUTTONS";
    text(renderer_, 4, kTop, heading.c_str(), kHeading);
    const float subtitleX = 4 + static_cast<float>(heading.size() + 1) * 8;
    text(renderer_, subtitleX, kTop, "number (= target_N in the INI), label  <- R3 button that drives it", kDim);

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

        fill(renderer_, x, y, kWidth, kHeight, pressed ? kActive : kPanel);
        const Colour colour = pressed ? kDark : (source < 0 || name.empty() ? kDim : kText);
        text(renderer_, x + 3, y + 3, cellText, colour);
    }
}

void MappingView::drawSourceButtons(const std::uint64_t sourceButtons) {
    constexpr float kTop = 384;
    text(renderer_, 4, kTop, "R3 BUTTONS", kHeading);
    const std::string legend =
        "green = " + target_ + " button or D-pad, yellow = unlabelled button, red = not mapped";
    text(renderer_, 92, kTop, legend.c_str(), kDim);

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
        fill(renderer_, x, y, kWidth, kHeight, background);
        const Colour foreground = pressed ? kDark : (target >= 0 || hatDirection != nullptr ? kText : kDim);

        std::snprintf(line, sizeof(line), "R3 %d", source);
        text(renderer_, x + 2, y + 2, line, foreground);
        if (hatDirection != nullptr) {
            // Short arrow so it fits the cell; a second mapping to a button is flagged with '+'.
            std::snprintf(line, sizeof(line), ">D%c%s", hatDirection[0] - 'a' + 'A', target >= 0 ? "+" : "");
        } else if (target >= 0) {
            std::snprintf(line, sizeof(line), sourceTargetCount_[index] > 1 ? ">%d+" : ">%d", target);
        } else {
            std::snprintf(line, sizeof(line), "--");
        }
        text(renderer_, x + 2, y + 12, line, foreground);
    }
    if (sourceButtons_ == 0) {
        text(renderer_, 4, kTop + 14, "(the device reports no buttons)", kDim);
    }
}

void MappingView::drawFooter() {
    constexpr float kTop = 504;
    char line[160];
    if (lastPressedSource_ < 0) {
        std::snprintf(line, sizeof(line), "Press any button on the wheel to see where it goes.");
        text(renderer_, 4, kTop, line, kDim);
    } else {
        const auto index = static_cast<std::size_t>(lastPressedSource_);
        const int target = sourceToTarget_[index];
        const char* const hatDirection = sourceHatDirection_[index];
        if (hatDirection != nullptr) {
            std::snprintf(line, sizeof(line), "Last pressed: R3 button %d -> %s D-pad %s%s", lastPressedSource_,
                          target_.c_str(), hatDirection, target >= 0 ? " (and a button: check [buttons])" : "");
            text(renderer_, 4, kTop, line, target >= 0 ? kUnlabelled : kText);
        } else if (target >= 0) {
            const std::string_view name = profile_.buttonName(static_cast<std::size_t>(target));
            std::snprintf(line, sizeof(line), "Last pressed: R3 button %d -> %s button %d (%.*s)%s",
                          lastPressedSource_, target_.c_str(), target, static_cast<int>(name.size()),
                          name.empty() ? "" : name.data(),
                          sourceTargetCount_[index] > 1 ? " and others" : "");
            text(renderer_, 4, kTop, line, kText);
        } else {
            std::snprintf(line, sizeof(line), "Last pressed: R3 button %d -> not mapped to any %s button",
                          lastPressedSource_, target_.c_str());
            text(renderer_, 4, kTop, line, kWarning);
        }
    }
    text(renderer_, 4, kTop + 14, "To change: INI [buttons] target_N = R3 button, [hat] up/right/down/left = R3 button.",
         kDim);
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

}  // namespace wheelemu::service
