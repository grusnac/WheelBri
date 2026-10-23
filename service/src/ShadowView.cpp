#include "ShadowView.hpp"

#include "UiStyle.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
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

// The G920's (g920Spec): A B X Y 0..3, the paddles as above, Menu, View, RSB, LSB, Xbox 6..10, gears 1-6 and R 12..18.
constexpr std::size_t kMenu = 6;
constexpr std::size_t kView = 7;
constexpr std::size_t kRsb = 8;
constexpr std::size_t kLsb = 9;
constexpr std::size_t kXbox = 10;
constexpr std::size_t kG920Gear1 = 12;
constexpr std::size_t kG920Buttons = 19;
// The G923 (Xbox and PC): the G920's 0..18, then its Plus / Minus rocker and the dial (g923Spec).
constexpr std::size_t kPlus = 19;
constexpr std::size_t kMinus = 20;
constexpr std::size_t kDialRight = 21;
constexpr std::size_t kDialLeft = 22;
constexpr std::size_t kDialEnter = 23;
constexpr std::size_t kG923Buttons = 24;

// The picture is laid out on the design's coordinates (the sketches': its heading at y 54); draw() moves it to `top`.
constexpr float kDesignTop = 54.0F;

// Wheel.
constexpr float kWheelX = 260.0F;
constexpr float kWheelY = 222.0F;
constexpr float kWheelR = 122.0F;
// The G920's Driving Force Shifter: a box with the gate in its boot, no buttons.
constexpr float kDfsX = 560.0F;
constexpr float kDfsY = 400.0F;
constexpr float kDfsW = 260.0F;
constexpr float kDfsH = 250.0F;
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

// --- the G920's face, in the wheel's units (R = 1, x right, y down from its centre) --------------------------------

// The face's parts and their buttons' places, sized down from the photo's (Logitech's G920 front view) so the paddles
// show beside them, as on the real wheel; the buttons themselves keep their size, for their print.
constexpr float kFace = 0.88F;

// A right half (top to bottom) and its mirror image: the whole outline, clockwise.
std::vector<ShapePoint> mirrored(const std::vector<ShapePoint>& half) {
    std::vector<ShapePoint> all = half;
    for (auto it = half.rbegin(); it != half.rend(); ++it) all.push_back({-it->x, it->y});
    return all;
}

// Corners rounded off: each round cuts every corner of the closed outline at a quarter and three quarters of its
// sides (Chaikin); three rounds leave smooth curves.
std::vector<ShapePoint> rounded(std::vector<ShapePoint> points, const int rounds) {
    for (int round = 0; round < rounds; ++round) {
        std::vector<ShapePoint> next;
        next.reserve(points.size() * 2);
        for (std::size_t i = 0; i < points.size(); ++i) {
            const ShapePoint a = points[i];
            const ShapePoint b = points[(i + 1) % points.size()];
            next.push_back({0.75F * a.x + 0.25F * b.x, 0.75F * a.y + 0.25F * b.y});
            next.push_back({0.25F * a.x + 0.75F * b.x, 0.25F * a.y + 0.75F * b.y});
        }
        points = std::move(next);
    }
    return points;
}

// Sized by `scale` (the G920's kFace), and kept inside `reach` (the rim's outer edge: what runs under the rim stops
// there).
std::vector<ShapePoint> placed(std::vector<ShapePoint> points, const float scale = kFace, const float reach = 0.98F) {
    for (ShapePoint& p : points) {
        p = {p.x * scale, p.y * scale};
        const float d = std::hypot(p.x, p.y);
        if (d > reach) p = {p.x * reach / d, p.y * reach / d};
    }
    return points;
}

// The brushed plate across the middle (View, Menu, LSB and RSB sit on it) out to the rim, running down into the
// bottom spoke; open below it either side of the spoke.
const std::vector<ShapePoint>& g920Plate() {
    static const std::vector<ShapePoint> outline = placed(rounded(
        mirrored({{0.0F, -0.14F}, {1.10F, -0.14F}, {1.10F, 0.30F}, {0.62F, 0.31F}, {0.36F, 0.34F}, {0.22F, 0.42F},
                  {0.15F, 0.56F}, {0.20F, 0.78F}, {0.33F, 1.10F}, {0.0F, 1.10F}}),
        2));
    return outline;
}

// The black console over the plate's top: two wings (the D-pad's, A B X Y's), dipping between them over the hub.
// The G25's spokes (Logitech's front view): one brushed plate, no holes: an oval round the hub, slim side spokes
// straight out to the rim, curving down underneath into the bottom spoke, which narrows to a waist and widens a little
// into the rim.
const std::vector<ShapePoint>& g25Spokes() {
    static const std::vector<ShapePoint> outline = placed(
        rounded(mirrored({{0.0F, -0.34F}, {0.15F, -0.33F}, {0.28F, -0.25F}, {0.40F, -0.13F}, {0.70F, -0.12F},
                          {1.0F, -0.15F}, {1.0F, 0.12F}, {0.70F, 0.10F}, {0.42F, 0.11F}, {0.32F, 0.20F},
                          {0.24F, 0.32F}, {0.15F, 0.48F}, {0.13F, 0.60F}, {0.15F, 0.80F}, {0.18F, 1.0F},
                          {0.0F, 1.0F}}),
                2),
        1.0F);
    return outline;
}

// The G27's (Logitech's front view): brushed side spokes straight out, sweeping down into the rim underneath; a
// slim bottom spoke.
const std::vector<ShapePoint>& g27Spokes() {
    static const std::vector<ShapePoint> outline = placed(
        rounded(mirrored({{0.0F, -0.19F}, {0.40F, -0.19F}, {0.60F, -0.18F}, {1.0F, -0.20F}, {1.0F, 0.42F},
                          {0.62F, 0.38F}, {0.50F, 0.26F}, {0.40F, 0.20F}, {0.24F, 0.22F}, {0.14F, 0.32F},
                          {0.12F, 0.45F}, {0.12F, 0.75F}, {0.14F, 1.0F}, {0.0F, 1.0F}}),
                2),
        1.0F);
    return outline;
}

const std::vector<ShapePoint>& g920Wings() {
    static const std::vector<ShapePoint> outline = placed(rounded(
        mirrored({{0.0F, -0.33F}, {0.10F, -0.34F}, {0.20F, -0.38F}, {0.30F, -0.41F}, {0.45F, -0.42F},
                  {0.58F, -0.40F}, {0.68F, -0.35F}, {0.73F, -0.27F}, {0.73F, -0.12F}, {0.67F, -0.05F},
                  {0.52F, -0.03F}, {0.38F, -0.03F}, {0.28F, 0.02F}, {0.0F, 0.04F}}),
        3));
    return outline;
}

}  // namespace

bool ShadowView::hasPicture(const IDeviceProfile& profile) {
    const std::string& name = profile.identity().shortName;
    return (name == "G25" && profile.buttonCount() == kG25Buttons) ||
           (name == "G27" && profile.buttonCount() == kG27Buttons) ||
           (name == "G920" && profile.buttonCount() == kG920Buttons) ||
           (name == "G923" && profile.buttonCount() == kG923Buttons);
}

ShadowView::ShadowView(ui::UiWindow& window, const IDeviceProfile& profile, const MapperConfig& mapper)
    : window_(window), profile_(profile), mapper_(mapper) {
    const std::string& name = profile.identity().shortName;
    if (name == "G27") picture_ = Picture::G27;
    if (name == "G920" || name == "G923") {
        picture_ = name == "G920" ? Picture::G920 : Picture::G923;
        gear1_ = kG920Gear1;
    }
}

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
    if (target >= gear1_ && target <= gear1_ + 6) return target == gear1_ + 6 ? "R" : std::to_string(target - gear1_ + 1);
    return std::string(profile_.buttonName(target));
}

std::string ShadowView::printed(const std::size_t target) const {
    if (!xbox()) return {};
    if (target <= 3 || target == kRsb || target == kLsb) return std::string(profile_.buttonName(target));   // A B X Y
    if (picture_ == Picture::G923 && target == kPlus) return "+";
    if (picture_ == Picture::G923 && target == kMinus) return "-";
    if (target >= gear1_ && target <= gear1_ + 6) return target == gear1_ + 6 ? "R" : std::to_string(target - gear1_ + 1);
    return {};   // View, Menu, Xbox: symbols; the paddles: nothing
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

Colour ShadowView::inkOn(const std::size_t target, const std::uint32_t pressed, const Colour idle) const {
    if (bit(pressed, target)) return style::kDark;
    return driven(target) ? idle : style::kFaint;
}

void ShadowView::printedOn(const float x, const float y, const std::size_t target, const Colour ink,
                           const Colour under) {
    if (target == kView) {   // two overlapping windows
        window_.fill(x - 5, y - 4, 6, 5, ink);
        window_.fill(x - 4, y - 3, 4, 3, under);
        window_.fill(x - 1, y - 1, 6, 5, ink);
        window_.fill(x, y, 4, 3, under);
    } else if (target == kMenu) {   // three lines
        for (const float k : {-3.0F, 0.0F, 3.0F}) window_.fill(x - 4, y + k - 1, 9, 1.6F, ink);
    } else if (target == kXbox) {   // the sphere with its X
        window_.fillEllipse(x, y, 6, 6, ink);
        for (const float d : {1.0F, -1.0F}) {
            constexpr float kW = 0.7F;
            polygon({{x - 4 - kW, y - d * (4 - kW)}, {x - 4 + kW, y - d * (4 + kW)}, {x + 4 + kW, y + d * (4 - kW)},
                     {x + 4 - kW, y + d * (4 + kW)}},
                    under);
        }
    } else {
        const std::string text = printed(target);
        if (!text.empty()) textCentred(x, y - 4, text, ink);
    }
}

void ShadowView::dpad(const float x, const float y, const float arm, const float half, const std::int8_t hat,
                      const Colour colour) {
    polygon({{x - half, y - half - arm}, {x + half, y - half - arm}, {x + half, y - half}, {x + half + arm, y - half},
             {x + half + arm, y + half}, {x + half, y + half}, {x + half, y + half + arm}, {x - half, y + half + arm},
             {x - half, y + half}, {x - half - arm, y + half}, {x - half - arm, y - half}, {x - half, y - half}},
            colour);
    // Hat 0..7 clockwise from up: the arms it presses (a diagonal lights two).
    if (hat == 7 || hat == 0 || hat == 1) window_.fill(x - half, y - half - arm, half * 2, arm, style::kActive);
    if (hat >= 1 && hat <= 3) window_.fill(x + half, y - half, arm, half * 2, style::kActive);
    if (hat >= 3 && hat <= 5) window_.fill(x - half, y + half, half * 2, arm, style::kActive);
    if (hat >= 5 && hat <= 7) window_.fill(x - half - arm, y - half, arm, half * 2, style::kActive);
}

void ShadowView::wheelShape(const std::vector<ShapePoint>& unit, const float cx, const float cy, const float r,
                            const Colour colour) {
    points_.clear();
    for (const ShapePoint& p : unit) points_.push_back({cx + p.x * r, cy + p.y * r});
    window_.fillPolygon(points_, colour);
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
    window_.text(62, Y(54),
                 xbox() ? "your labels beside, the " + profile_.identity().shortName + "'s own print on the buttons"
                        : std::string("your labels beside, the game's numbers on the buttons"),
                 style::kDim);
    style::headingBand(window_, listX, Y(54), window_.width() - listX);
    window_.text(listX + 6, Y(54), "PRESSED NOW", style::kHeading);
    window_.text(listX + 102, Y(54), "yours -> " + profile_.identity().shortName, style::kDim);

    drawWheel(state, report.buttons, report.hat);
    drawPedals(state);
    if (xbox()) {
        drawDrivingForceShifter(report.buttons);
    } else {
        drawShifter(report.buttons, report.hat);
    }
    drawList(state, report, listX);
}

void ShadowView::drawWheel(const WheelState& state, const std::uint32_t pressed, const std::int8_t hat) {
    const float cx = kWheelX;
    const float cy = Y(kWheelY);
    const float R = kWheelR;
    const auto at = [&](const float x, const float y) { return ShapePoint{cx + x * R, cy + y * R}; };

    // The paddles: half-moon blades behind the red buttons (the G27's: Wheel 1), nearly upright (the top leaning in a
    // little; the G920's upright), the round side out, with rounded tips. Drawn first: the spokes and buttons are in
    // front of them. A blade is its middle line (bowed outwards) with edges offset square to it by a half width that
    // swells from the tips' to the middle's; each tip is a half circle of exactly the tip's half width, turned along
    // the blade, so it runs on from the edges. The G27's and G920's are long and wide, the G25's smaller (as the real
    // one); the number on the top part (on the G27: above Wheel 2; the G920's carry none).
    std::array<ShapePoint, 2> paddleNumber{};
    for (const auto& [target, side] : {std::pair{kLPaddle, -1.0F}, std::pair{kRPaddle, 1.0F}}) {
        // The G25's (Logitech's front view): smaller, with slimmer tips, from above the side spoke to just below it.
        const bool g25 = picture_ == Picture::G25;
        const ShapePoint centre = at(side * (g25 ? 0.55F : 0.62F), g25 ? -0.07F : 0.05F);
        const float tipHalf = (g25 ? 0.05F : 0.09F) * R;
        const float midHalf = (g25 ? 0.11F : 0.10F) * R;
        const float length = (g25 ? 0.80F : 1.0F) * R - 2 * tipHalf;   // between the tips' centres
        const float numberAt = g25 ? 0.14F : 0.06F;                    // along the blade, from its top
        const float tilt = (xbox() ? 0.0F : 5.0F) * kPi / 180.0F;
        constexpr int kSteps = 32;
        constexpr int kCapSteps = 12;
        // A point of the blade's own frame (x outwards, y down, from `centre`) on the window.
        const auto place = [&](const float lx, const float ly) {
            const float xr = lx * std::cos(tilt) + ly * std::sin(tilt);
            const float yr = -lx * std::sin(tilt) + ly * std::cos(tilt);
            return ShapePoint{centre.x + side * xr, centre.y + yr};
        };
        const float bow = (g25 ? 0.10F : 0.125F) * R;
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

    drawFace(cx, cy, R);   // the spokes run under the rim

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

    if (xbox()) {
        drawG920Buttons(cx, cy, R, pressed, hat);
    }

    // The red wheel buttons (none on the G920), with their labels outside the rim, level with them, placed as in
    // Logitech's front views. The G25: one a side on the side spoke, round in a black ring. The G27: three red bars a
    // side on the spoke's black strip near the rim, Wheel 2 above Wheel 1 and Wheel 3 below. Both a little bigger than
    // the real ones, for their numbers.
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
        if (xbox()) break;
        if (picture_ == Picture::G27) {
            buttons[count++] = {left ? kLWheel2 : kRWheel2, side, 0.61F, -0.09F};
            buttons[count++] = {left ? kLWheel : kRWheel, side, 0.61F, 0.04F};
            buttons[count++] = {left ? kLWheel3 : kRWheel3, side, 0.61F, 0.17F};
        } else {
            buttons[count++] = {left ? kLWheel : kRWheel, side, 0.53F, -0.01F};
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        const WheelButton& b = buttons[i];
        const ShapePoint p = at(b.side * b.x, b.y);
        const Colour colour = controlColour(b.target, pressed, style::kShadowRed, style::kShadowRedOff);
        if (picture_ == Picture::G27) {
            // Red bars, wider than tall (sized for the number), on the spoke's black strip.
            roundedRect(p.x - 0.09F * R, p.y - 0.05F * R, 0.18F * R, 0.10F * R, 3, colour);
        } else {
            // The G25's: round, in a black ring.
            window_.fillEllipse(p.x, p.y, 0.095F * R, 0.095F * R, style::kShadowBlack);
            window_.fillEllipse(p.x, p.y, 0.075F * R, 0.075F * R, colour);
        }
        numberOn(p.x, p.y, b.target, bit(pressed, b.target));
        const std::string name = cut(label(b.target), 12);
        if (b.side < 0) {
            textLeftOf(cx - R - 24, p.y - 4, name, labelColour(b.target, pressed));
        } else {
            window_.text(cx + R + 30, p.y - 4, name, labelColour(b.target, pressed));
        }
    }
    // The paddles' numbers (in front of the spokes; the G920's have nothing printed) and their labels, above the
    // buttons'.
    if (!xbox()) {
        numberOn(paddleNumber[0].x, paddleNumber[0].y, kLPaddle, bit(pressed, kLPaddle));
        numberOn(paddleNumber[1].x, paddleNumber[1].y, kRPaddle, bit(pressed, kRPaddle));
    }
    const float paddleLabelY = cy - R * 0.42F;   // level with the paddles' numbers
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
    const bool g920 = xbox();   // the G920's pedals (the G923's alike)
    for (std::size_t i = 0; i < pedals.size(); ++i) {
        const Pedal& pedal = pedals[i];
        float px = x + kGap + static_cast<float>(i) * (kPedalW + kGap);
        float pw = kPedalW;
        float top = y + 36;
        float faceH = kH - 92;
        if (g920 && i == 2) {   // the G920's throttle: taller and narrower
            px += 8;
            pw -= 16;
            top -= 12;
            faceH += 12;
        }
        const float v = std::clamp(pedal.value, 0.0F, 1.0F);
        const bool used = pedal.config->sourceAxis >= 0;
        const float litFrom = top + faceH - faceH * v;
        window_.fill(px + pw / 2 - 6, top + faceH - 6, 12, 40, style::kShadowEdge);   // the arm
        if (g920) {
            // Squarer faces with short ribs across them (green where it is lit).
            roundedRect(px, top, pw, faceH, 10, used ? style::kSteel : style::kShadowOff);
            if (v > 0.0F) roundedRect(px, litFrom, pw, faceH * v, faceH * v > 20 ? 10 : 3, style::kActive);
            for (int r = 0; r < 7; ++r) {
                const float ry = top + 14 + static_cast<float>(r) * (faceH - 24) / 6;
                window_.fill(px + 12, ry, pw - 24, 3, v > 0.0F && ry > litFrom ? style::kActiveDeep : style::kShadowDeep);
            }
        } else {
            roundedRect(px, top, pw, faceH, 30, used ? style::kSteel : style::kShadowOff);   // the round-topped face
            if (v > 0.0F) roundedRect(px, litFrom, pw, faceH * v, faceH * v > 60 ? 30 : 6, style::kActive);
            // The face's rows of holes (green where it is lit).
            for (int r = 0; r < 5; ++r) {
                for (int c = 0; c < 3; ++c) {
                    const float hx = px + 20 + static_cast<float>(c) * 19;
                    const float hy = top + 26 + static_cast<float>(r) * 19;
                    window_.fillEllipse(hx, hy, 3.2F, 3.2F,
                                        v > 0.0F && hy > litFrom ? style::kActiveDeep : style::kShadowDeep);
                }
            }
        }
        const Colour colour = v > 0.0F ? style::kActive : (used ? style::kText : style::kFaint);
        textCentred(px + pw / 2, top - 18, cut(axisLabel(*pedal.config, pedal.name), 12), colour);
        char percent[8];
        std::snprintf(percent, sizeof(percent), "%.0f%%", static_cast<double>(v * 100.0F));
        textCentred(px + pw / 2, y + kH - 28, percent, v > 0.0F ? style::kActive : style::kDim);
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
    dpad(dx, dy, kArm, kHalf, hat, style::kShadowControl);
    const bool up = hat == 7 || hat == 0 || hat == 1;
    const bool right = hat >= 1 && hat <= 3;
    const bool down = hat >= 3 && hat <= 5;
    const bool left = hat >= 5 && hat <= 7;
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
    if (picture_ == Picture::G25) {
        window_.fillEllipse(x + 306, y + 30, 11, 11, style::kShadowEdge);
        window_.fillEllipse(x + 306, y + 30, 10, 10, style::kShadowDeep);
        window_.text(x + 302, y + 46, "H", style::kFaint);
    }
}

// --- the G920 -------------------------------------------------------------------------------------------------------

void ShadowView::drawFace(const float cx, const float cy, const float r) {
    if (xbox()) {
        drawG920Face(cx, cy, r);
        return;
    }
    if (picture_ == Picture::G27) {
        // Logitech's front view: the brushed spokes; a black strip at each side spoke's end for the three red
        // buttons; the hub a black disc ringed by six bolts; the ten rev lights above it (the game's output: never
        // lit here).
        wheelShape(g27Spokes(), cx, cy, r, style::kAluminium);
        for (const float side : {-1.0F, 1.0F}) {
            roundedRect(cx + side * 0.61F * r - 0.12F * r, cy - 0.17F * r, 0.24F * r, 0.42F * r, 5, style::kShadowBlack);
        }
        const float hubY = cy + 0.02F * r;
        window_.fillEllipse(cx, hubY, 0.27F * r + 1, 0.27F * r + 1, style::kShadowEdge);
        window_.fillEllipse(cx, hubY, 0.27F * r, 0.27F * r, style::kShadowBlack);
        for (int i = 0; i < 6; ++i) {   // silver bolts round the black hub
            const float a = (90.0F + static_cast<float>(i) * 60.0F) * kPi / 180.0F;
            window_.fillEllipse(cx + std::cos(a) * 0.20F * r, hubY - std::sin(a) * 0.20F * r, 0.03F * r, 0.03F * r,
                                style::kShadowKnobTop);
        }
        roundedRect(cx - 0.22F * r, cy - 0.29F * r, 0.44F * r, 0.08F * r, 3, style::kShadowBlack);
        for (int i = 0; i < 10; ++i) {
            const float lx = -0.18F + static_cast<float>(i) * 0.04F;
            window_.fillEllipse(cx + lx * r, cy - 0.25F * r, 0.014F * r, 0.014F * r, style::kShadowEdge);
        }
        return;
    }
    // The G25 (Logitech's front view): one brushed plate, no holes; the hub a grey disc with six black bolts round
    // the black centre cap, a little above the wheel's centre.
    wheelShape(g25Spokes(), cx, cy, r, style::kAluminium);
    const float hubY = cy - 0.06F * r;
    window_.fillEllipse(cx, hubY, 0.26F * r + 1, 0.26F * r + 1, style::kShadowEdge);
    window_.fillEllipse(cx, hubY, 0.26F * r, 0.26F * r, style::kShadowControl);
    for (int i = 0; i < 6; ++i) {
        const float a = (90.0F + static_cast<float>(i) * 60.0F) * kPi / 180.0F;
        window_.fillEllipse(cx + std::cos(a) * 0.19F * r, hubY - std::sin(a) * 0.19F * r, 0.035F * r, 0.035F * r,
                            style::kShadowBlack);
    }
    window_.fillEllipse(cx, hubY, 0.10F * r, 0.10F * r, style::kShadowBlack);
}

void ShadowView::drawG920Face(const float cx, const float cy, const float r) {
    // As the real one (Logitech's front view): the brushed plate and bottom spoke, the black wings over the plate's
    // top, and the hub over both: a dark ring of six bolts round the glossy cap with the G. No rev lights: the G920
    // has none. Everything runs under the rim.
    // The G923's (Logitech's Xbox front view): the same body, its plate dark brushed, ten rev lights over the hub.
    const bool g923 = picture_ == Picture::G923;
    wheelShape(g920Plate(), cx, cy, r, g923 ? style::kSteel : style::kAluminium);
    wheelShape(g920Wings(), cx, cy, r, style::kShadowConsole);
    if (g923) {
        for (int i = 0; i < 10; ++i) {   // the game's output: never lit here
            const float a = (-27.0F + static_cast<float>(i) * 6.0F) * kPi / 180.0F;
            window_.fillEllipse(cx + std::sin(a) * 0.355F * kFace * r, cy + (0.04F - 0.355F * std::cos(a)) * kFace * r,
                                0.017F * r, 0.017F * r, style::kShadowEdge);
        }
    }
    const float hubY = cy + 0.04F * kFace * r;
    window_.fillEllipse(cx, hubY, 0.30F * kFace * r + 1, 0.30F * kFace * r + 1, style::kShadowEdge);
    window_.fillEllipse(cx, hubY, 0.30F * kFace * r, 0.30F * kFace * r, style::kShadowDeep);
    for (int i = 0; i < 6; ++i) {
        const float a = (90.0F + static_cast<float>(i) * 60.0F) * kPi / 180.0F;
        window_.fillEllipse(cx + std::cos(a) * 0.245F * kFace * r, hubY - std::sin(a) * 0.245F * kFace * r,
                            0.024F * r, 0.024F * r, style::kShadowEdge);
    }
    window_.fillEllipse(cx, hubY, 0.185F * kFace * r + 1, 0.185F * kFace * r + 1, style::kShadowEdge);
    window_.fillEllipse(cx, hubY, 0.185F * kFace * r, 0.185F * kFace * r, style::kShadowKnob);
    window_.text(cx - 4, hubY - 4, "G", style::kFaint);
}

void ShadowView::drawG920Buttons(const float cx, const float cy, const float r, const std::uint32_t pressed,
                                 const std::int8_t hat) {
    // Places as on the real wheel (sized down with the face, kFace); sizes kept for their print.
    const auto at = [&](const float x, const float y) { return ShapePoint{cx + x * kFace * r, cy + y * kFace * r}; };
    const float leftX = cx - r - 24;    // labels outside the rim, right-aligned on the left
    const float rightX = cx + r + 30;

    // The D-pad on the left wing: a slim cross in a round dark bezel; "D-pad" beside it (its directions are the
    // hat's, named in the list).
    const ShapePoint d = at(-0.48F, -0.22F);
    window_.fillEllipse(d.x, d.y, 0.12F * r + 1, 0.12F * r + 1, style::kShadowEdge);
    window_.fillEllipse(d.x, d.y, 0.12F * r, 0.12F * r, style::kShadowBlack);
    dpad(d.x, d.y, 0.068F * r, 0.032F * r, hat, style::kShadowControl);
    textLeftOf(leftX, d.y - 4, "D-pad", hat >= 0 && hat < 8 ? style::kActive : style::kDim);

    // A B X Y on the right wing: black buttons, their letters in Xbox's colours; their labels in a row beside.
    struct Face {
        std::size_t target;
        float x;
        float y;
        Colour letter;
    };
    const ShapePoint f = at(0.48F, -0.22F);
    const std::array<Face, 4> faces = {{{3, 0, -0.10F, style::kLetterY}, {2, -0.10F, 0, style::kLetterX},
                                        {1, 0.10F, 0, style::kLetterB}, {0, 0, 0.10F, style::kLetterA}}};
    for (const Face& face : faces) {
        const float x = f.x + face.x * r;
        const float y = f.y + face.y * r;
        const Colour colour = controlColour(face.target, pressed, style::kShadowBlack);
        window_.fillEllipse(x, y, 0.05F * r + 1, 0.05F * r + 1, style::kShadowEdge);
        window_.fillEllipse(x, y, 0.05F * r, 0.05F * r, colour);
        printedOn(x, y, face.target, inkOn(face.target, pressed, face.letter), colour);
    }
    float x = rightX;
    for (std::size_t t = 0; t < 4; ++t) {   // A B X Y
        const std::string name = cut(label(t), 5);
        window_.text(x, f.y - 4, name, labelColour(t, pressed));
        x += static_cast<float>(name.size() + 1) * ui::kCharWidth;
    }

    // View and Menu beside the hub, LSB and RSB lower out on the plate: light keycaps with dark print, the top
    // corner nearer the hub cut off, as the real ones.
    struct Key {
        std::size_t target;
        float x;
        float y;
        float w;
        float h;
    };
    const bool g923 = picture_ == Picture::G923;   // black keys with light print, LSB / RSB further out
    const float keyY = g923 ? 0.09F : 0.07F;
    const float stickX = g923 ? 0.70F : 0.58F;
    const float stickY = g923 ? 0.19F : 0.24F;
    const std::array<Key, 4> keys = {{{kView, -0.44F, keyY, 0.115F, 0.12F}, {kMenu, 0.44F, keyY, 0.115F, 0.12F},
                                      {kLsb, -stickX, stickY, 0.25F, 0.11F}, {kRsb, stickX, stickY, 0.25F, 0.11F}}};
    for (const Key& key : keys) {
        const ShapePoint c = at(key.x, key.y);
        const float x0 = c.x - key.w * r / 2;
        const float x1 = c.x + key.w * r / 2;
        const float y0 = c.y - key.h * r / 2;
        const float y1 = c.y + key.h * r / 2;
        const float cutBy = key.h * r * 0.45F;
        std::array<ShapePoint, 5> corners{};
        if (key.x < 0) {   // left of the hub: its top right corner cut
            corners = {{{x0, y0}, {x1 - cutBy, y0}, {x1, y0 + cutBy}, {x1, y1}, {x0, y1}}};
        } else {
            corners = {{{x0 + cutBy, y0}, {x1, y0}, {x1, y1}, {x0, y1}, {x0, y0 + cutBy}}};
        }
        points_.clear();   // the corners rounded a little: each side's ends cut at a fifth
        for (std::size_t i = 0; i < corners.size(); ++i) {
            const ShapePoint a = corners[i];
            const ShapePoint b = corners[(i + 1) % corners.size()];
            points_.push_back({0.8F * a.x + 0.2F * b.x, 0.8F * a.y + 0.2F * b.y});
            points_.push_back({0.2F * a.x + 0.8F * b.x, 0.2F * a.y + 0.8F * b.y});
        }
        const Colour colour = controlColour(key.target, pressed, g923 ? style::kShadowBlack : style::kShadowKey);
        window_.fillPolygon(points_, colour);
        printedOn(c.x, c.y, key.target,
                  g923 ? inkOn(key.target, pressed)
                       : (driven(key.target) || bit(pressed, key.target) ? style::kDark : style::kFaint),
                  colour);
        const std::string name = cut(label(key.target), 12);
        if (key.x < 0) {
            textLeftOf(leftX, c.y - 4, name, labelColour(key.target, pressed));
        } else {
            window_.text(rightX, c.y - 4, name, labelColour(key.target, pressed));
        }
    }

    // The Xbox button on the bottom spoke, its little light above it; its label under it.
    const ShapePoint xb = at(0, 0.57F);
    window_.fillEllipse(xb.x, xb.y - 0.10F * r, 1.2F, 1.2F, style::kShadowLamp);
    const Colour colour = controlColour(kXbox, pressed, style::kShadowBlack);
    window_.fillEllipse(xb.x, xb.y, 0.065F * r + 1, 0.065F * r + 1, style::kShadowEdge);
    window_.fillEllipse(xb.x, xb.y, 0.065F * r, 0.065F * r, colour);
    printedOn(xb.x, xb.y, kXbox, inkOn(kXbox, pressed), colour);
    textCentred(xb.x, xb.y + 0.09F * r, cut(label(kXbox), 12), labelColour(kXbox, pressed));
    if (g923) drawG923Extras(at, r, leftX, rightX, pressed);
}

void ShadowView::drawG923Extras(const std::function<ShapePoint(float, float)>& at, const float r, const float leftX,
                                const float rightX, const std::uint32_t pressed) {
    const float cy = at(0, 0).y;
    // The Plus / Minus rocker, low on the left: a round button split across, + on top, - below, in a black housing;
    // the same size as the dial and level with it.
    constexpr float kLowY = 0.42F;
    constexpr float kHousing = 0.15F;
    const ShapePoint pm = at(-0.33F, kLowY);
    const float pmR = 0.10F * r;
    window_.fillEllipse(pm.x, pm.y, kHousing * r, kHousing * r, style::kShadowBlack);
    window_.fillEllipse(pm.x, pm.y, pmR + 1, pmR + 1, style::kShadowEdge);
    for (const auto& [target, sign] : {std::pair{kPlus, -1.0F}, std::pair{kMinus, 1.0F}}) {
        points_.clear();   // the half disc: the top half for Plus, the bottom for Minus
        for (int i = 0; i <= 16; ++i) {
            const float a = kPi * static_cast<float>(i) / 16;
            points_.push_back({pm.x + std::cos(a) * pmR, pm.y + sign * (std::sin(a) * pmR)});
        }
        const Colour colour = controlColour(target, pressed, style::kShadowKnob);
        window_.fillPolygon(points_, colour);
        textCentred(pm.x, pm.y + sign * 0.05F * r - 4, printed(target), inkOn(target, pressed));
    }
    window_.fill(pm.x - pmR, pm.y - 0.75F, pmR * 2, 1.5F, style::kShadowBlack);   // the split
    textLeftOf(leftX, cy + 0.30F * r - 4, cut(label(kPlus), 12), labelColour(kPlus, pressed));
    textLeftOf(leftX, cy + 0.41F * r - 4, cut(label(kMinus), 12), labelColour(kMinus, pressed));

    // The dial, low on the right: a toothed ring (its left half lights for Dial Left, its right half for Dial Right)
    // round the Enter button.
    const ShapePoint dl = at(0.33F, kLowY);
    window_.fillEllipse(dl.x, dl.y, kHousing * r, kHousing * r, style::kShadowBlack);
    for (int i = 0; i < 14; ++i) {
        const float a = (static_cast<float>(i) + 0.5F) * 2 * kPi / 14;
        const std::size_t target = std::sin(a) < 0 ? kDialLeft : kDialRight;
        window_.fillEllipse(dl.x + std::sin(a) * 0.123F * r, dl.y - std::cos(a) * 0.123F * r, 0.025F * r, 0.025F * r,
                            controlColour(target, pressed, style::kShadowControl));
    }
    window_.fillEllipse(dl.x, dl.y, 0.10F * r + 1, 0.10F * r + 1, style::kShadowEdge);
    window_.fillEllipse(dl.x, dl.y, 0.10F * r, 0.10F * r, style::kShadowBlack);
    const Colour enter = controlColour(kDialEnter, pressed, style::kShadowKnob);
    window_.fillEllipse(dl.x, dl.y, 0.075F * r, 0.075F * r, enter);
    const Colour ink = inkOn(kDialEnter, pressed);   // the Enter arrow: down the right, back to the left
    window_.fill(dl.x + 2, dl.y - 4, 1.6F, 5, ink);
    window_.fill(dl.x - 3, dl.y + 0.4F, 6.6F, 1.6F, ink);
    polygon({{dl.x - 5, dl.y + 1.2F}, {dl.x - 2, dl.y - 1.6F}, {dl.x - 2, dl.y + 4}}, ink);
    window_.text(rightX, cy + 0.30F * r - 4, cut(label(kDialEnter), 12), labelColour(kDialEnter, pressed));
    window_.text(rightX, cy + 0.41F * r - 4, cut(label(kDialRight), 12), labelColour(kDialRight, pressed));
    window_.text(rightX, cy + 0.52F * r - 4, cut(label(kDialLeft), 12), labelColour(kDialLeft, pressed));
}

void ShadowView::drawDrivingForceShifter(const std::uint32_t pressed) {
    // The Driving Force Shifter: a small box, the stick in a round boot, no buttons.
    const float x = kDfsX;
    const float y = Y(kDfsY);
    roundedRect(x - 1, y - 1, kDfsW + 2, kDfsH + 2, 23, style::kShadowEdge);
    roundedRect(x, y, kDfsW, kDfsH, 22, style::kShadow);
    const float bcx = x + kDfsW / 2;
    const float bcy = y + kDfsH / 2 + 4;
    constexpr float kBoot = 96;
    window_.fillEllipse(bcx, bcy, kBoot + 1, kBoot + 1, style::kShadowEdge);
    window_.fillEllipse(bcx, bcy, kBoot, kBoot, style::kShadowPanel);
    const std::array<float, 4> cols = {bcx - 54, bcx - 18, bcx + 18, bcx + 54};
    const float gateTop = bcy - 50;
    const float gateBottom = bcy + 50;
    window_.fill(cols[0] - 3.5F, bcy - 3.5F, cols[3] - cols[0] + 7, 7, style::kShadowDeep);
    for (std::size_t i = 0; i < cols.size(); ++i) {
        const float from = i == 3 ? bcy : gateTop;   // reverse: down only, right of 6
        window_.fill(cols[i] - 3.5F, from - 3.5F, 7, gateBottom - from + 7, style::kShadowDeep);
    }
    // Gears 1, 3, 5 up; 2, 4, 6 down; R down at the right: each slot with its gear printed; beside it only a rig
    // label (the slot already says which gear).
    const std::array<ShapePoint, 7> slots = {{{cols[0], gateTop}, {cols[0], gateBottom}, {cols[1], gateTop},
                                              {cols[1], gateBottom}, {cols[2], gateTop}, {cols[2], gateBottom},
                                              {cols[3], gateBottom}}};
    std::optional<std::size_t> engaged;
    for (std::size_t g = 0; g < slots.size(); ++g) {
        const std::size_t target = gear1_ + g;
        const ShapePoint slot = slots[g];
        if (bit(pressed, target) && !engaged) engaged = g;
        const Colour colour = controlColour(target, pressed, style::kShadowControl);
        window_.fillEllipse(slot.x, slot.y, 10, 10, colour);
        printedOn(slot.x, slot.y, target, inkOn(target, pressed), colour);
        const std::string rig = sourceLabel(mapper_.buttonDevice[target], mapper_.buttonMap[target], false);
        if (!rig.empty()) {
            textCentred(slot.x, slot.y < bcy ? slot.y - 24 : slot.y + 14, cut(rig, 4), labelColour(target, pressed));
        }
    }
    const ShapePoint knob = engaged ? slots[*engaged] : ShapePoint{(cols[1] + cols[2]) / 2, bcy};
    window_.fillEllipse(knob.x, knob.y, 16, 16, engaged ? style::kActive : style::kShadowEdge);
    window_.fillEllipse(knob.x, knob.y, 14, 14, style::kShadowKnob);
    if (engaged) {
        textCentred(knob.x, knob.y - 4, printed(gear1_ + *engaged), style::kActive);   // the engaged gear on the knob
    } else {
        window_.fillEllipse(knob.x - 4, knob.y - 5, 5, 5, style::kShadowKnobTop);
    }
    textCentred(bcx, y + kDfsH + 8, "Driving Force Shifter", style::kDim);
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
    const std::string onIt =
        xbox() ? "what the " + target + " has printed there."
        : order_ == ButtonOrder::Native && profile_.hasNativeButtonOrder() ? std::string("its number in the native order, from 0.")
                                                                           : std::string("its number in Logitech's driver, from 1.");
    window_.text(4, legendY,
                 "Beside a control: your rig's label (Rig tab), else the " + target + "'s own name. On it: " + onIt,
                 style::kDim);
    window_.text(4, legendY + 12, "Faint: nothing drives it. S or the List button: every rig axis and button.",
                 style::kDim);
}

}  // namespace wheelbri::service
