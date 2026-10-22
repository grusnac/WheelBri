#include "ShadowView.hpp"

#include "UiStyle.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <tuple>

namespace wheelbri::service {

namespace {

using ui::Colour;

constexpr float kPi = 3.14159265F;

// The G25's and G27's buttons as named (DeviceProfile.hpp, g25Spec, g27Spec): Driver order, 0-based. 0..18 are the
// same on both; the G27 adds its wheel's lower buttons 19..22.
constexpr std::size_t kRed1 = 0;            // red 1-4: 0..3, left to right
constexpr std::size_t kRPaddle = 4;
constexpr std::size_t kLPaddle = 5;
constexpr std::size_t kRWheel = 6;           // the G27's R Wheel 1
constexpr std::size_t kLWheel = 7;           // the G27's L Wheel 1
constexpr std::size_t kGear1 = 8;           // gears 1-6: 8..13, reverse 14
constexpr std::size_t kGearR = 14;
constexpr std::size_t kBlkTop = 15;         // the black diamond: top, left, bottom, right 15..18 (G27: Black 1-4)
constexpr std::size_t kRWheel2 = 19;        // the G27 only
constexpr std::size_t kLWheel2 = 20;
constexpr std::size_t kRWheel3 = 21;
constexpr std::size_t kLWheel3 = 22;
constexpr std::size_t kG25Buttons = 19;
constexpr std::size_t kG27Buttons = 23;

// The picture is laid out on the design's coordinates (the sketches': its heading at y 54); draw() moves it to `top`.
constexpr float kDesignTop = 54.0F;

// Wheel.
constexpr float kWheelX = 260.0F;
constexpr float kWheelY = 222.0F;
constexpr float kWheelR = 122.0F;
// Pedals.
constexpr float kPedalsX = 30.0F;
constexpr float kPedalsY = 412.0F;
// Shifter (its button panel left, the gate right).
constexpr float kShifterX = 470.0F;
constexpr float kShifterY = 404.0F;
constexpr float kShifterW = 480.0F;
constexpr float kShifterH = 262.0F;
// The right-hand list starts here in the design (further right in a wider window).
constexpr float kListX = 600.0F;
constexpr float kListW = 356.0F;

constexpr float kSteeringTrackDeg = 150.0F;   // the arc round the rim: full lock either way

bool bit(const std::uint32_t bits, const std::size_t n) { return n < 32 && ((bits >> n) & 1U) != 0; }

std::string cut(std::string value, const std::size_t chars) {
    if (value.size() > chars) value.resize(chars);
    return value;
}

}  // namespace

bool ShadowView::hasPicture(const IDeviceProfile& profile) {
    const std::string& name = profile.identity().shortName;
    return (name == "G25" && profile.buttonCount() == kG25Buttons) ||
           (name == "G27" && profile.buttonCount() == kG27Buttons);
}

ShadowView::ShadowView(ui::UiWindow& window, const IDeviceProfile& profile, const MapperConfig& mapper)
    : window_(window), profile_(profile), mapper_(mapper), g27_(profile.identity().shortName == "G27") {}

// --- what a control is called ---------------------------------------------------------------------------------------

std::string ShadowView::sourceLabel(const int device, const int index, const bool axis) const {
    if (index < 0) return {};
    if (device == 0) {
        if (axis) return index < static_cast<int>(RawInputState::kMaxAxes) ? mapper_.sourceAxisLabels[static_cast<std::size_t>(index)] : std::string();
        return index < static_cast<int>(MapperConfig::kMaxSourceButtons)
                   ? mapper_.sourceButtonLabels[static_cast<std::size_t>(index)]
                   : std::string();
    }
    if (device < 1 || static_cast<std::size_t>(device) > accessories_.size()) return {};
    const DeviceLabels& labels = accessories_[static_cast<std::size_t>(device - 1)];
    if (axis) return index < static_cast<int>(labels.axes.size()) ? labels.axes[static_cast<std::size_t>(index)] : std::string();
    return index < static_cast<int>(labels.buttons.size()) ? labels.buttons[static_cast<std::size_t>(index)] : std::string();
}

std::string ShadowView::sourceName(const int device, const int index, const bool axis) const {
    if (index < 0) return "--";
    std::string label = sourceLabel(device, index, axis);
    if (!label.empty()) return label;
    std::string owner = "Rig";
    if (device > 0 && static_cast<std::size_t>(device) <= accessories_.size()) {
        const DeviceLabels& labels = accessories_[static_cast<std::size_t>(device - 1)];
        owner = labels.name.empty() ? deviceId(labels.vendorId, labels.productId) : labels.name.substr(0, 10);
    }
    return owner + (axis ? " ax" : " ") + std::to_string(index);
}

bool ShadowView::driven(const std::size_t target) const {
    if (target >= MapperConfig::kMaxTargetButtons) return false;
    return mapper_.buttonMap[target] >= 0 || mapper_.buttonSecondary[target][0].mapped();
}

std::string ShadowView::number(const std::size_t target) const {
    const int n = gameButtonNumber(profile_, order_, target);
    return n < 0 ? std::string() : std::to_string(n);
}

std::string ShadowView::label(const std::size_t target) const {
    if (target < MapperConfig::kMaxTargetButtons) {
        std::string rig = sourceLabel(mapper_.buttonDevice[target], mapper_.buttonMap[target], false);
        if (!rig.empty()) return rig;
    }
    // None: the target's own name; a gear is just its number (or R) on the gate.
    if (target >= kGear1 && target <= kGearR) return target == kGearR ? "R" : std::to_string(target - kGear1 + 1);
    return std::string(profile_.buttonName(target));
}

std::string ShadowView::axisLabel(const PedalConfig& pedal, const char* const fallback) const {
    std::string rig = sourceLabel(pedal.sourceDevice, pedal.sourceAxis, true);
    return rig.empty() ? std::string(fallback) : rig;
}

std::string ShadowView::hatLabel(const std::size_t direction) const {
    static constexpr std::array<const char*, 4> kNames = {"Up", "Right", "Down", "Left"};
    const HatButtons& hat = mapper_.hatButtons;
    const std::array<int, 4> sources = {hat.up, hat.right, hat.down, hat.left};
    std::string rig = sourceLabel(hat.devices[direction], sources[direction], false);
    return rig.empty() ? std::string(kNames[direction]) : rig;
}

float ShadowView::steeringDegrees(const WheelState& state) const {
    return std::clamp(state.steering, -1.0F, 1.0F) * mapper_.steering.targetRotationDeg * 0.5F;
}

Colour ShadowView::controlColour(const std::size_t target, const std::uint32_t pressed, const Colour idle,
                                 const Colour off) const {
    if (bit(pressed, target)) return style::kActive;
    return driven(target) ? idle : off;
}

Colour ShadowView::labelColour(const std::size_t target, const std::uint32_t pressed) const {
    if (bit(pressed, target)) return style::kActive;
    return driven(target) ? style::kText : style::kFaint;
}

// --- drawing helpers ------------------------------------------------------------------------------------------------

void ShadowView::polygon(const std::initializer_list<ShapePoint> points, const Colour colour) {
    points_.assign(points.begin(), points.end());
    window_.fillPolygon(points_, colour);
}

void ShadowView::roundedRect(const float x, const float y, const float w, const float h, const float radius,
                             const Colour colour) {
    const float r = std::min({radius, w / 2, h / 2});
    points_.clear();
    // The four corners' quarter circles, clockwise from the top left.
    const std::array<ShapePoint, 4> centres = {{{x + r, y + r}, {x + w - r, y + r}, {x + w - r, y + h - r}, {x + r, y + h - r}}};
    constexpr int kSteps = 8;
    for (std::size_t c = 0; c < centres.size(); ++c) {
        const float start = kPi + static_cast<float>(c) * kPi / 2;
        for (int i = 0; i <= kSteps; ++i) {
            const float a = start + static_cast<float>(i) * (kPi / 2) / kSteps;
            points_.push_back({centres[c].x + std::cos(a) * r, centres[c].y + std::sin(a) * r});
        }
    }
    window_.fillPolygon(points_, colour);
}

void ShadowView::annularArc(const float cx, const float cy, const float outer, const float inner, const float fromDeg,
                            const float toDeg, const Colour colour) {
    // Angles clockwise from the top, as a wheel turns.
    const float from = std::min(fromDeg, toDeg);
    const float to = std::max(fromDeg, toDeg);
    if (to - from < 0.01F) return;
    const int steps = std::clamp(static_cast<int>((to - from) / 3.0F), 2, 120);
    points_.clear();
    for (int i = 0; i <= steps; ++i) {
        const float a = (from + (to - from) * static_cast<float>(i) / static_cast<float>(steps)) * kPi / 180.0F;
        points_.push_back({cx + std::sin(a) * outer, cy - std::cos(a) * outer});
    }
    for (int i = steps; i >= 0; --i) {
        const float a = (from + (to - from) * static_cast<float>(i) / static_cast<float>(steps)) * kPi / 180.0F;
        points_.push_back({cx + std::sin(a) * inner, cy - std::cos(a) * inner});
    }
    window_.fillPolygon(points_, colour);
}

void ShadowView::numberOn(const float x, const float y, const std::size_t target, const bool pressed) {
    const std::string n = number(target);
    window_.text(x - static_cast<float>(n.size()) * 4.0F, y - 4.0F, n,
                 pressed ? style::kDark : (driven(target) ? style::kText : style::kFaint));
}

void ShadowView::textLeftOf(const float rightX, const float y, const std::string& value, const Colour colour) {
    window_.text(rightX - static_cast<float>(value.size()) * ui::kCharWidth, y, value, colour);
}

void ShadowView::textCentred(const float x, const float y, const std::string& value, const Colour colour) {
    window_.text(x - static_cast<float>(value.size()) * ui::kCharWidth / 2, y, value, colour);
}

// --- the picture ----------------------------------------------------------------------------------------------------

void ShadowView::draw(const WheelState& state, const TargetReport& report, const float top) {
    offY_ = top - kDesignTop;
    const float listX = std::max(kListX, window_.width() - kListW - 4);
    style::headingBand(window_, 0, Y(54), listX - 10);
    window_.text(6, Y(54), "SHADOW", style::kHeading);
    window_.text(62, Y(54), "your labels beside, the game's numbers on the buttons", style::kDim);
    style::headingBand(window_, listX, Y(54), window_.width() - listX);
    window_.text(listX + 6, Y(54), "PRESSED NOW", style::kHeading);
    window_.text(listX + 102, Y(54), "yours -> " + profile_.identity().shortName, style::kDim);

    drawWheel(state, report.buttons);
    drawPedals(state);
    drawShifter(report.buttons, report.hat);
    drawList(state, report, listX);
}

void ShadowView::drawWheel(const WheelState& state, const std::uint32_t pressed) {
    const float cx = kWheelX;
    const float cy = Y(kWheelY);
    const float R = kWheelR;
    const auto at = [&](const float x, const float y) { return ShapePoint{cx + x * R, cy + y * R}; };

    // The paddles: half-moon blades behind the red buttons (the G27's: Wheel 1), nearly upright (the top leaning in a
    // little), the round side out, with rounded tips. Drawn first: the spokes and buttons are in front of them. A blade
    // is its middle line (bowed outwards) with edges offset square to it by a half width that swells from the tips' to
    // the middle's; each tip is a half circle of exactly the tip's half width, turned along the blade, so it runs on
    // from the edges. The G27's are longer and wider, with their number on the top part (above Wheel 2).
    std::array<ShapePoint, 2> paddleNumber{};
    for (const auto& [target, side] : {std::pair{kLPaddle, -1.0F}, std::pair{kRPaddle, 1.0F}}) {
        const ShapePoint centre = at(side * 0.62F, 0.05F);
        const float tipHalf = (g27_ ? 0.09F : 0.05F) * R;
        const float midHalf = (g27_ ? 0.10F : 0.08F) * R;
        const float length = (g27_ ? 1.0F : 0.78F) * R - 2 * tipHalf;   // between the tips' centres
        const float numberAt = g27_ ? 0.06F : 0.22F;                     // along the blade, from its top
        const float tilt = 5.0F * kPi / 180.0F;
        constexpr int kSteps = 32;
        constexpr int kCapSteps = 12;
        // A point of the blade's own frame (x outwards, y down, from `centre`) on the window.
        const auto place = [&](const float lx, const float ly) {
            const float xr = lx * std::cos(tilt) + ly * std::sin(tilt);
            const float yr = -lx * std::sin(tilt) + ly * std::cos(tilt);
            return ShapePoint{centre.x + side * xr, centre.y + yr};
        };
        const float bow = 0.125F * R;
        const auto middle = [&](const float u) {
            return ShapePoint{bow * std::sin(kPi * u) - 0.09F * R, -length / 2 + length * u};
        };
        const auto tangent = [&](const float u) {   // along the blade, downwards
            const float dx = bow * kPi * std::cos(kPi * u);
            const float n = std::hypot(dx, length);
            return ShapePoint{dx / n, length / n};
        };
        const auto half = [&](const float u) { return tipHalf + (midHalf - tipHalf) * std::sin(kPi * u); };
        const auto edge = [&](const float u, const float sign) {   // +1 the outer edge, -1 the inner
            const ShapePoint m = middle(u);
            const ShapePoint t = tangent(u);
            return place(m.x + sign * t.y * half(u), m.y - sign * t.x * half(u));
        };
        // A tip: the half circle round its centre, from one edge's normal round the end to the other. `from` -1 (the
        // top): from the inner edge, up over the end, to the outer; +1 (the bottom): from the outer, under, to the inner.
        const auto cap = [&](const float u, const float from) {
            const ShapePoint m = middle(u);
            const ShapePoint t = tangent(u);
            const ShapePoint n{t.y, -t.x};   // the outward normal
            for (int i = 0; i <= kCapSteps; ++i) {
                const float a = kPi * static_cast<float>(i) / kCapSteps;
                const float dx = from * (n.x * std::cos(a) + t.x * std::sin(a));
                const float dy = from * (n.y * std::cos(a) + t.y * std::sin(a));
                points_.push_back(place(m.x + dx * tipHalf, m.y + dy * tipHalf));
            }
        };
        points_.clear();
        cap(0, -1);   // the top tip: from the inner edge, over the end, to the outer one
        for (int i = 1; i < kSteps; ++i) points_.push_back(edge(static_cast<float>(i) / kSteps, 1));   // outer, down
        cap(1, 1);    // the bottom tip: from the outer edge, under the end, to the inner one
        for (int i = kSteps - 1; i > 0; --i) points_.push_back(edge(static_cast<float>(i) / kSteps, -1));   // inner, up
        window_.fillPolygon(points_, controlColour(target, pressed, style::kShadowEdge));
        const ShapePoint m = middle(numberAt);
        const ShapePoint number = place(m.x, m.y);
        paddleNumber[target == kLPaddle ? 0 : 1] = {number.x + side * 2, number.y};
    }

    // The rim (leather), its stitching, the steering arc round it and the mark where the rim's top is now.
    window_.fillRing(cx, cy, R, R * 0.87F, style::kShadow);
    window_.fillRing(cx, cy, R * 0.935F + 0.5F, R * 0.935F - 0.5F, style::kShadowEdge);
    const float degrees = steeringDegrees(state);
    const float half = std::max(1.0F, mapper_.steering.targetRotationDeg * 0.5F);
    annularArc(cx, cy, R + 14, R + 8, -kSteeringTrackDeg, kSteeringTrackDeg, style::kPanel);
    // The arc and the mark go together: both at the same share of the track as the wheel is of its full lock, so the
    // mark ends where the arc does (full lock left: at the track's left end), never round the far side.
    const float turned = std::clamp(degrees / half, -1.0F, 1.0F) * kSteeringTrackDeg;
    annularArc(cx, cy, R + 14, R + 8, 0, turned, style::kActive);
    annularArc(cx, cy, R, R * 0.87F, turned - 4, turned + 4, style::kCaution);

    if (g27_) {
        // The G27: brushed aluminium spokes, no holes; the side ones wide pods for three buttons, one straight down.
        for (const float side : {-1.0F, 1.0F}) {
            polygon({at(side * 0.30F, -0.20F), at(side * 0.80F, -0.30F), at(side * 0.90F, -0.12F),
                     at(side * 0.90F, 0.22F), at(side * 0.80F, 0.38F), at(side * 0.30F, 0.24F)},
                    style::kSteel);
        }
        polygon({at(-0.12F, 0.30F), at(0.12F, 0.30F), at(0.15F, 0.90F), at(-0.15F, 0.90F)}, style::kSteel);
        // The hub, with the ten rev lights across its top: the game's output, never lit here.
        const float hubY = cy + R * 0.02F;
        window_.fillEllipse(cx, hubY, R * 0.36F + 1, R * 0.36F + 1, style::kShadowEdge);
        window_.fillEllipse(cx, hubY, R * 0.36F, R * 0.36F, style::kShadow);
        for (int i = 0; i < 10; ++i) {
            const float a = (-45.0F + static_cast<float>(i) * 10.0F) * kPi / 180.0F;
            window_.fillEllipse(cx + std::sin(a) * R * 0.27F, hubY - std::cos(a) * R * 0.27F, R * 0.025F, R * 0.025F,
                                style::kShadowDeep);
        }
        window_.fillEllipse(cx, cy + R * 0.06F, R * 0.13F, R * 0.13F, style::kShadowDeep);
    } else {
        // The G25: three drilled steel spokes, left, right and straight down.
        for (const float side : {-1.0F, 1.0F}) {
            polygon({at(side * 0.30F, -0.10F), at(side * 0.90F, -0.02F), at(side * 0.90F, 0.16F),
                     at(side * 0.30F, 0.18F)},
                    style::kSteel);
            for (const float hx : {0.40F, 0.62F, 0.84F}) {
                const ShapePoint hole = at(side * hx, 0.10F);
                window_.fillEllipse(hole.x, hole.y, R * 0.028F, R * 0.028F, style::kShadowDeep);
            }
        }
        polygon({at(-0.11F, 0.30F), at(0.11F, 0.30F), at(0.14F, 0.90F), at(-0.14F, 0.90F)}, style::kSteel);
        for (int k = 0; k < 3; ++k) {
            const ShapePoint hole = at(0, 0.48F + static_cast<float>(k) * 0.13F);
            window_.fillEllipse(hole.x, hole.y, R * 0.028F, R * 0.028F, style::kShadowDeep);
        }
        // The hub: round, six bolts, the centre cap.
        const float hubY = cy + R * 0.04F;
        window_.fillEllipse(cx, hubY, R * 0.34F + 1, R * 0.34F + 1, style::kShadowEdge);
        window_.fillEllipse(cx, hubY, R * 0.34F, R * 0.34F, style::kShadow);
        for (int i = 0; i < 6; ++i) {
            const float a = (30.0F + static_cast<float>(i) * 60.0F) * kPi / 180.0F;
            window_.fillEllipse(cx + std::cos(a) * R * 0.25F, hubY + std::sin(a) * R * 0.25F, R * 0.025F, R * 0.025F,
                                style::kShadowDeep);
        }
        window_.fillEllipse(cx, hubY, R * 0.14F, R * 0.14F, style::kShadowDeep);
    }

    // The red wheel buttons, with their labels outside the rim, level with them. The G25: one a side, low on the side
    // spoke. The G27: three a side in a column on the spoke near the rim, Wheel 2 above Wheel 1 and Wheel 3 below.
    struct WheelButton {
        std::size_t target;
        float side;
        float x;
        float y;
    };
    std::array<WheelButton, 6> buttons{};
    std::size_t count = 0;
    for (const float side : {-1.0F, 1.0F}) {
        const bool left = side < 0;
        if (g27_) {
            buttons[count++] = {left ? kLWheel2 : kRWheel2, side, 0.66F, -0.17F};
            buttons[count++] = {left ? kLWheel : kRWheel, side, 0.66F, 0.05F};
            buttons[count++] = {left ? kLWheel3 : kRWheel3, side, 0.66F, 0.27F};
        } else {
            buttons[count++] = {left ? kLWheel : kRWheel, side, 0.62F, 0.05F};
        }
    }
    const float buttonR = (g27_ ? 0.075F : 0.085F) * R;
    for (std::size_t i = 0; i < count; ++i) {
        const WheelButton& b = buttons[i];
        const ShapePoint p = at(b.side * b.x, b.y);
        window_.fillEllipse(p.x, p.y, buttonR, buttonR,
                            controlColour(b.target, pressed, style::kShadowRed, style::kShadowRedOff));
        numberOn(p.x, p.y, b.target, bit(pressed, b.target));
        const std::string name = cut(label(b.target), 12);
        if (b.side < 0) {
            textLeftOf(cx - R - 24, p.y - 4, name, labelColour(b.target, pressed));
        } else {
            window_.text(cx + R + 30, p.y - 4, name, labelColour(b.target, pressed));
        }
    }
    // The paddles' numbers (in front of the spokes) and their labels, above the buttons'.
    numberOn(paddleNumber[0].x, paddleNumber[0].y, kLPaddle, bit(pressed, kLPaddle));
    numberOn(paddleNumber[1].x, paddleNumber[1].y, kRPaddle, bit(pressed, kRPaddle));
    const float paddleLabelY = cy - R * (g27_ ? 0.42F : 0.30F);
    textLeftOf(cx - R - 24, paddleLabelY, cut(label(kLPaddle), 12), labelColour(kLPaddle, pressed));
    window_.text(cx + R + 30, paddleLabelY, cut(label(kRPaddle), 12), labelColour(kRPaddle, pressed));
    char line[48];
    std::snprintf(line, sizeof(line), "%s %+.0f deg", cut(axisLabel(PedalConfig{mapper_.steering.sourceAxis,
                                                                                mapper_.steering.sourceDevice},
                                                                    "Steering"),
                                                          12)
                                                          .c_str(),
                  static_cast<double>(degrees));
    window_.text(cx + 54, cy - R - 28, line, std::fabs(degrees) >= 0.5F ? style::kActive : style::kDim);
}

void ShadowView::drawPedals(const WheelState& state) {
    const float x = kPedalsX;
    const float y = Y(kPedalsY);
    constexpr float kW = 380;
    constexpr float kH = 230;
    // The base: a sloped plate.
    polygon({{x, y + kH}, {x + kW, y + kH}, {x + kW - 14, y + kH - 46}, {x + 14, y + kH - 46}}, style::kShadow);
    constexpr float kPedalW = 78;
    constexpr float kGap = (kW - 3 * kPedalW) / 4;
    struct Pedal {
        const PedalConfig* config;
        float value;
        const char* name;
    };
    const std::array<Pedal, 3> pedals = {{{&mapper_.clutch, state.clutch, "Clutch"},
                                          {&mapper_.brake, state.brake, "Brake"},
                                          {&mapper_.throttle, state.throttle, "Throttle"}}};
    for (std::size_t i = 0; i < pedals.size(); ++i) {
        const Pedal& pedal = pedals[i];
        const float px = x + kGap + static_cast<float>(i) * (kPedalW + kGap);
        const float top = y + 36;
        constexpr float kFaceH = kH - 92;
        const float v = std::clamp(pedal.value, 0.0F, 1.0F);
        const bool used = pedal.config->sourceAxis >= 0;
        window_.fill(px + kPedalW / 2 - 6, top + kFaceH - 6, 12, 40, style::kShadowEdge);   // the arm
        roundedRect(px, top, kPedalW, kFaceH, 30, used ? style::kSteel : style::kShadowOff);   // the round-topped face
        if (v > 0.0F) {
            const float fh = kFaceH * v;
            roundedRect(px, top + kFaceH - fh, kPedalW, fh, fh > 60 ? 30 : 6, style::kActive);
        }
        // The face's rows of holes (green where it is lit).
        for (int r = 0; r < 5; ++r) {
            for (int c = 0; c < 3; ++c) {
                const float hx = px + 20 + static_cast<float>(c) * 19;
                const float hy = top + 26 + static_cast<float>(r) * 19;
                const bool lit = v > 0.0F && hy > top + kFaceH - kFaceH * v;
                window_.fillEllipse(hx, hy, 3.2F, 3.2F, lit ? style::kActiveDeep : style::kShadowDeep);
            }
        }
        const Colour colour = v > 0.0F ? style::kActive : (used ? style::kText : style::kFaint);
        textCentred(px + kPedalW / 2, top - 18, cut(axisLabel(*pedal.config, pedal.name), 12), colour);
        char percent[8];
        std::snprintf(percent, sizeof(percent), "%.0f%%", static_cast<double>(v * 100.0F));
        textCentred(px + kPedalW / 2, y + kH - 28, percent, v > 0.0F ? style::kActive : style::kDim);
    }
}

void ShadowView::drawShifter(const std::uint32_t pressed, const std::int8_t hat) {
    const float x = kShifterX;
    const float y = Y(kShifterY);
    roundedRect(x - 1, y - 1, kShifterW + 2, kShifterH + 2, 19, style::kShadowEdge);
    roundedRect(x, y, kShifterW, kShifterH, 18, style::kShadow);
    // The button panel on the left: the black diamond on top, the D-pad, the red row along the bottom.
    roundedRect(x + 10, y + 10, 280, kShifterH - 20, 10, style::kShadowPanel);
    const float mid = x + 150;
    constexpr float kStep = 64;   // the red row's spacing

    const float dcx = mid;
    const float dcy = y + 66;
    const std::array<std::pair<float, float>, 4> diamond = {{{0, -24}, {-26, 0}, {0, 24}, {26, 0}}};   // top, left, bottom, right
    for (std::size_t i = 0; i < diamond.size(); ++i) {
        const std::size_t target = kBlkTop + i;
        const auto [ox, oy] = diamond[i];
        window_.fillEllipse(dcx + ox, dcy + oy, 12, 12, style::kShadowEdge);
        window_.fillEllipse(dcx + ox, dcy + oy, 11, 11, controlColour(target, pressed, style::kShadowBlack));
        numberOn(dcx + ox, dcy + oy, target, bit(pressed, target));
        const std::string name = cut(label(target), 12);
        if (oy != 0) {   // top, bottom: to its right
            window_.text(dcx + ox + 16, dcy + oy - 4, name, labelColour(target, pressed));
        } else if (ox > 0) {
            window_.text(dcx + ox + 16, dcy + oy - 4, name, labelColour(target, pressed));
        } else {
            textLeftOf(dcx + ox - 16, dcy + oy - 4, name, labelColour(target, pressed));
        }
    }

    // The D-pad (the G25's hat), each direction's label by its arm.
    const float dx = mid;
    const float dy = y + 158;
    constexpr float kArm = 17;
    constexpr float kHalf = 8;
    polygon({{dx - kHalf, dy - kHalf - kArm}, {dx + kHalf, dy - kHalf - kArm}, {dx + kHalf, dy - kHalf},
             {dx + kHalf + kArm, dy - kHalf}, {dx + kHalf + kArm, dy + kHalf}, {dx + kHalf, dy + kHalf},
             {dx + kHalf, dy + kHalf + kArm}, {dx - kHalf, dy + kHalf + kArm}, {dx - kHalf, dy + kHalf},
             {dx - kHalf - kArm, dy + kHalf}, {dx - kHalf - kArm, dy - kHalf}, {dx - kHalf, dy - kHalf}},
            style::kShadowControl);
    // Hat 0..7 clockwise from up: the arms it presses (a diagonal lights two).
    const bool up = hat == 7 || hat == 0 || hat == 1;
    const bool right = hat >= 1 && hat <= 3;
    const bool down = hat >= 3 && hat <= 5;
    const bool left = hat >= 5 && hat <= 7;
    if (up) window_.fill(dx - kHalf, dy - kHalf - kArm, kHalf * 2, kArm, style::kActive);
    if (right) window_.fill(dx + kHalf, dy - kHalf, kArm, kHalf * 2, style::kActive);
    if (down) window_.fill(dx - kHalf, dy + kHalf, kHalf * 2, kArm, style::kActive);
    if (left) window_.fill(dx - kHalf - kArm, dy - kHalf, kArm, kHalf * 2, style::kActive);
    const auto hatColour = [](const bool on) { return on ? style::kActive : style::kDim; };
    window_.text(dx + 30, dy - kHalf - kArm, cut(hatLabel(0), 12), hatColour(up));
    window_.text(dx + 30, dy - 4, cut(hatLabel(1), 12), hatColour(right));
    window_.text(dx + 30, dy + kArm, cut(hatLabel(2), 12), hatColour(down));
    textLeftOf(dx - 30, dy - 4, cut(hatLabel(3), 12), hatColour(left));

    // The red row: each label right under its button (cut to the spacing).
    const float ry = y + 214;
    for (std::size_t k = 0; k < 4; ++k) {
        const std::size_t target = kRed1 + k;
        const float bx = mid + (static_cast<float>(k) - 1.5F) * kStep;
        window_.fillEllipse(bx, ry, 13, 13, controlColour(target, pressed, style::kShadowRed, style::kShadowRedOff));
        numberOn(bx, ry, target, bit(pressed, target));
        textCentred(bx, ry + 18, cut(label(target), 7), labelColour(target, pressed));
    }

    // The gate: in the round boot (six bolts) on the right, the knob in the gear the game sees engaged.
    const float bcx = x + 382;
    const float bcy = y + 131;
    constexpr float kBoot = 84;
    window_.fillEllipse(bcx, bcy, kBoot + 1, kBoot + 1, style::kShadowEdge);
    window_.fillEllipse(bcx, bcy, kBoot, kBoot, style::kShadowPanel);
    for (int i = 0; i < 6; ++i) {
        const float a = static_cast<float>(i) * kPi / 3;
        window_.fillEllipse(bcx + std::cos(a) * (kBoot - 8), bcy + std::sin(a) * (kBoot - 8), 3, 3, style::kShadowDeep);
    }
    const std::array<float, 4> cols = {bcx - 48, bcx - 16, bcx + 16, bcx + 48};
    const float gateTop = bcy - 44;
    const float gateBottom = bcy + 44;
    window_.fill(cols[0] - 3.5F, bcy - 3.5F, cols[3] - cols[0] + 7, 7, style::kShadowDeep);
    for (std::size_t i = 0; i < cols.size(); ++i) {
        const float from = i == 3 ? bcy : gateTop;   // reverse: down only, right of 6
        window_.fill(cols[i] - 3.5F, from - 3.5F, 7, gateBottom - from + 7, style::kShadowDeep);
    }
    // Gears 1, 3, 5 up; 2, 4, 6 down; R down at the right.
    const std::array<ShapePoint, 7> slots = {{{cols[0], gateTop}, {cols[0], gateBottom}, {cols[1], gateTop},
                                              {cols[1], gateBottom}, {cols[2], gateTop}, {cols[2], gateBottom},
                                              {cols[3], gateBottom}}};
    std::optional<std::size_t> engaged;
    for (std::size_t g = 0; g < slots.size(); ++g) {
        const std::size_t target = kGear1 + g;
        const ShapePoint slot = slots[g];
        if (bit(pressed, target) && !engaged) engaged = g;
        window_.fillEllipse(slot.x, slot.y, 10, 10, controlColour(target, pressed, style::kShadowControl));
        numberOn(slot.x, slot.y, target, bit(pressed, target));
        const std::string name = cut(label(target), 4);
        textCentred(slot.x, slot.y < bcy ? slot.y - 24 : slot.y + 14, name, labelColour(target, pressed));
    }
    const ShapePoint knob = engaged ? slots[*engaged] : ShapePoint{(cols[1] + cols[2]) / 2, bcy};
    window_.fillEllipse(knob.x, knob.y, 16, 16, engaged ? style::kActive : style::kShadowEdge);
    window_.fillEllipse(knob.x, knob.y, 14, 14, style::kShadowKnob);
    if (engaged) {
        const std::string n = number(kGear1 + *engaged);   // the engaged gear's number on the knob
        textCentred(knob.x, knob.y - 4, n, style::kActive);
    } else {
        window_.fillEllipse(knob.x - 4, knob.y - 5, 5, 5, style::kShadowKnobTop);
    }
    // The G25's H / sequential knob: not an input the game sees (dim). The G27 has none.
    if (!g27_) {
        window_.fillEllipse(x + 306, y + 30, 11, 11, style::kShadowEdge);
        window_.fillEllipse(x + 306, y + 30, 10, 10, style::kShadowDeep);
        window_.text(x + 302, y + 46, "H", style::kFaint);
    }
}

// --- the list -------------------------------------------------------------------------------------------------------

void ShadowView::drawList(const WheelState& state, const TargetReport& report, const float listX) {
    const std::string& target = profile_.identity().shortName;
    struct Row {
        std::string yours;
        std::string theirs;
    };
    std::vector<Row> rows;
    for (std::size_t b = 0; b < profile_.buttonCount() && b < 32; ++b) {
        if (!bit(report.buttons, b) || profile_.buttonName(b).empty()) continue;
        // Its main source; pressed by a secondary alone, the first secondary (a button box button, a lever).
        const int source = mapper_.buttonMap[b];
        const ButtonSource& other = mapper_.buttonSecondary[b][0];
        rows.push_back({source >= 0 ? sourceName(mapper_.buttonDevice[b], source, false)
                                    : sourceName(other.device, other.index, other.axis),
                        number(b) + " " + std::string(profile_.buttonName(b))});
    }
    if (report.hat >= 0 && report.hat < 8) {
        static constexpr std::array<const char*, 8> kWays = {"up", "up right", "right", "down right",
                                                             "down", "down left", "left", "up left"};
        const auto way = static_cast<std::size_t>(report.hat);
        rows.push_back({way % 2 == 0 ? hatLabel(way / 2) : std::string("D-pad"), std::string("Hat ") + kWays[way]});
    }
    const std::array<std::tuple<const PedalConfig*, float, const char*, const char*>, 3> pedals = {{
        {&mapper_.throttle, state.throttle, "Gas pedal", "Throttle"},
        {&mapper_.brake, state.brake, "Brake pedal", "Brake"},
        {&mapper_.clutch, state.clutch, "Clutch pedal", "Clutch"}}};
    for (const auto& [config, value, fallback, name] : pedals) {
        if (value < 0.01F) continue;
        char percent[24];
        std::snprintf(percent, sizeof(percent), "%s %.0f%%", name, static_cast<double>(value * 100.0F));
        rows.push_back({axisLabel(*config, fallback), percent});
    }
    const float degrees = steeringDegrees(state);
    if (std::fabs(degrees) >= 0.5F) {
        char turned[32];
        std::snprintf(turned, sizeof(turned), "Steering %+.0f deg", static_cast<double>(degrees));
        rows.push_back({axisLabel(PedalConfig{mapper_.steering.sourceAxis, mapper_.steering.sourceDevice}, "Wheel"),
                        turned});
    }

    const float width = window_.width() - listX - 4;
    const auto chars = static_cast<std::size_t>(std::max(10.0F, (width - 12) / ui::kCharWidth));
    float y = Y(76);
    // Room down to the shifter's top (it reaches under the list in the design's width).
    const float bottom = Y(kShifterY) - 50;
    if (rows.empty()) {
        window_.text(listX + 6, y + 5, "nothing (press a button, a pedal, turn)", style::kDim);
        y += 22;
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (y + 18 > bottom - 22) {
            window_.text(listX + 6, y + 5, "and " + std::to_string(rows.size() - i) + " more", style::kDim);
            y += 22;
            break;
        }
        window_.fill(listX, y, width, 18, style::kPanel);
        char line[96];
        std::snprintf(line, sizeof(line), "%-13s-> %s", cut(rows[i].yours, 12).c_str(), rows[i].theirs.c_str());
        window_.text(listX + 6, y + 5, cut(line, chars), style::kActive);
        y += 22;
    }
    // What nothing drives: faint on the picture, named here.
    std::string idle;
    for (std::size_t b = 0; b < profile_.buttonCount() && b < 32; ++b) {
        if (profile_.buttonName(b).empty() || driven(b)) continue;
        idle += (idle.empty() ? "" : ", ") + std::string(profile_.buttonName(b));
    }
    if (y + 34 <= bottom) {
        window_.text(listX, y + 6, "Nothing drives:", style::kDim);
        const std::string text = idle.empty() ? std::string("(every button is mapped)") : idle;
        // Wrapped at commas into the list's width, four lines at most.
        std::size_t start = 0;
        for (int line = 0; line < 4 && start < text.size(); ++line) {
            std::size_t end = std::min(text.size(), start + chars);
            if (end < text.size()) {
                const std::size_t comma = text.rfind(", ", end);
                if (comma != std::string::npos && comma > start) end = comma + 1;
            }
            window_.text(listX + 6, y + 22 + static_cast<float>(line) * 14, text.substr(start, end - start),
                         style::kFaint);
            start = end;
            while (start < text.size() && text[start] == ' ') ++start;
        }
    }
    const float legendY = window_.height() - 28 - 32;   // above the tab bar
    window_.text(4, legendY,
                 "Beside a control: your rig's label (Rig tab), else the " + target + "'s own name. On it: its number in " +
                     (order_ == ButtonOrder::Native && profile_.hasNativeButtonOrder() ? "the native order, from 0."
                                                                                     : "Logitech's driver, from 1."),
                 style::kDim);
    window_.text(4, legendY + 12, "Faint: nothing drives it. S or the List button: every rig axis and button.",
                 style::kDim);
}

}  // namespace wheelbri::service
