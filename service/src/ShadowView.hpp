// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/DeviceLabels.hpp"
#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/MapperConfig.hpp"
#include "wheelbri/ShapeSpans.hpp"
#include "wheelbri/WheelState.hpp"

#include "UiStyle.hpp"
#include "UiWindow.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wheelbri::service {

// The Mapping tab's Shadow view while a session runs: the target drawn as flat shapes (its wheel, pedals and shifter,
// as they sit on the real one), every control lit while the game sees it pressed. On each button its number as the
// game numbers it (gameButtonNumber: from 1 in Logitech's driver order, from 0 in the native order), or, on the G920,
// what the real one has printed there (A, LSB, the View / Menu / Xbox symbols, the gears); beside it the rig's label
// for what drives it (Rig tab, [button_labels]), or the target's own name when that control has none; a control
// nothing drives is faint. On the right, what is pressed now ("yours -> target"). It shows what the game
// gets (the mapped report), not the rig's raw input: the List view (MappingView's live page) has that.
//
// Every target has a picture (hasPicture), drawn from Logitech's front views of the real wheel; MappingView keeps its
// List view for a profile without one.
class ShadowView {
public:
    // Whether `profile` has a picture here: the G25 (its 19 buttons), the G27 (its 23), the G920 (its 19) or the G923
    // (its 24; docs/target-<target>.md).
    [[nodiscard]] static bool hasPicture(const IDeviceProfile& profile);

    // Draws into `window` (not owned; it must outlive the view).
    ShadowView(ui::UiWindow& window, const IDeviceProfile& profile, const MapperConfig& mapper);

    void setButtonOrder(const ButtonOrder order) { order_ = order; }
    // The mapping changed while the session runs (LiveMapping.hpp): labels, faint controls and the steering arc follow.
    void setMapping(const MapperConfig& mapper) { mapper_ = mapper; }
    // The accessories the mapping reads (device 1, 2, 3), for their labels and names.
    void setAccessories(const std::array<DeviceLabels, MapperConfig::kMaxAccessories>& accessories) {
        accessories_ = accessories;
    }

    // The picture and the list, from `top` (where the Mapping tab's content starts) down to the tab bar.
    void draw(const WheelState& state, const TargetReport& report, float top);
    // Room kept at the bottom of the right column, above the shifter, for the live view's own block (its force
    // feedback): the list ends that much higher. After draw(): where that room is (left edge, top, width).
    void setColumnReserve(const float height) { columnReserve_ = height; }
    [[nodiscard]] float columnX() const;
    [[nodiscard]] float reserveTop() const;
    [[nodiscard]] float columnWidth() const { return window_.width() - columnX() - 4; }

    // What the picture writes for target button `target` (0-based, as named): on it (the game's number) and beside it
    // (the rig's label for its source, else the target's own name: gears just their number or R).
    [[nodiscard]] std::string number(std::size_t target) const;
    [[nodiscard]] std::string label(std::size_t target) const;
    // What the G920 has printed on target button `target` ("A", "LSB", a gear's "1".."R"); "" where it is a symbol
    // (View, Menu, Xbox), nothing (the paddles) or the target is not the G920.
    [[nodiscard]] std::string printed(std::size_t target) const;
    // Whether anything drives target button `target` (its main source or one of its secondaries).
    [[nodiscard]] bool driven(std::size_t target) const;

    // Where the switch between the Shadow and List views is (MappingView draws it in its header), shared so the
    // header and the clicks agree.
    static constexpr float kSwitchX = 48.0F;
    static constexpr float kSwitchY = 14.0F;
    static constexpr float kSwitchH = 12.0F;
    static constexpr float kSwitchShadowW = 64.0F;
    static constexpr float kSwitchListW = 48.0F;
    static constexpr float kSwitchGap = 4.0F;
    static constexpr float kSwitchEnd = kSwitchX + kSwitchShadowW + kSwitchGap + kSwitchListW;

private:
    // Drawing helpers on the picture's own coordinates (the design's, moved down by offY_).
    [[nodiscard]] float Y(const float y) const { return y + offY_; }
    void polygon(std::initializer_list<ShapePoint> points, ui::Colour colour);
    void roundedRect(float x, float y, float w, float h, float r, ui::Colour colour);
    void roundedRectOutline(float x, float y, float w, float h, float r);   // into points_
    // The part of that rounded rect from fromY down, its outline the rect's own (a pedal's lit part follows its face).
    void roundedRectBelow(float x, float y, float w, float h, float r, float fromY, ui::Colour colour);
    void annularArc(float cx, float cy, float outer, float inner, float fromDeg, float toDeg, ui::Colour colour);
    void numberOn(float x, float y, std::size_t target, bool pressed);
    // The G920's print on a button in `ink`: its letters, or View's, Menu's and Xbox's symbols drawn small; `under`
    // is the button's own colour (the symbols' cut-outs).
    void printedOn(float x, float y, std::size_t target, ui::Colour ink, ui::Colour under);
    // The print's colour: dark on a lit button, `idle` on a driven one, faint on one nothing drives.
    [[nodiscard]] ui::Colour inkOn(std::size_t target, std::uint32_t pressed, ui::Colour idle = style::kText) const;
    // A D-pad (a cross) centred at x, y: arms `arm` long and 2 * `half` wide, those `hat` presses lit.
    void dpad(float x, float y, float arm, float half, std::int8_t hat, ui::Colour colour);
    // A closed outline in the wheel's own units (R = 1, from its centre) placed on the wheel at cx, cy.
    void wheelShape(const std::vector<ShapePoint>& unit, float cx, float cy, float r, ui::Colour colour);
    void textLeftOf(float rightX, float y, const std::string& value, ui::Colour colour);
    void textCentred(float x, float y, const std::string& value, ui::Colour colour);

    void drawWheel(const WheelState& state, std::uint32_t pressed, std::int8_t hat);
    void drawFace(float cx, float cy, float r);       // the spokes and hub, under the rim
    void drawG920Face(float cx, float cy, float r);
    void drawG920Buttons(float cx, float cy, float r, std::uint32_t pressed, std::int8_t hat);
    // The G923's own: the Plus / Minus rocker and the dial round its Enter button (`at` places on the face).
    void drawG923Extras(const std::function<ShapePoint(float, float)>& at, float r, float leftX, float rightX,
                        std::uint32_t pressed);
    void drawPedals(const WheelState& state);
    void drawShifter(std::uint32_t pressed, std::int8_t hat);
    void drawDrivingForceShifter(std::uint32_t pressed);   // the G920's: the gate alone
    void drawList(const WheelState& state, const TargetReport& report, float listX);

    // Colours by state: lit, a control at rest (`idle`), one nothing drives.
    [[nodiscard]] ui::Colour controlColour(std::size_t target, std::uint32_t pressed, ui::Colour idle,
                                           ui::Colour off = style::kShadowOff) const;
    [[nodiscard]] ui::Colour labelColour(std::size_t target, std::uint32_t pressed) const;

    // The rig's name for a source: its label, else "Rig 37" / "<accessory> 4"; `axis` for an axis.
    [[nodiscard]] std::string sourceName(int device, int index, bool axis) const;
    // Just the label ("" when it has none).
    [[nodiscard]] std::string sourceLabel(int device, int index, bool axis) const;
    [[nodiscard]] std::string axisLabel(const PedalConfig& pedal, const char* fallback) const;
    [[nodiscard]] std::string hatLabel(std::size_t direction) const;   // 0 up, 1 right, 2 down, 3 left
    [[nodiscard]] float steeringDegrees(const WheelState& state) const;

    ui::UiWindow& window_;
    const IDeviceProfile& profile_;
    MapperConfig mapper_;
    float columnReserve_ = 0.0F;
    std::array<DeviceLabels, MapperConfig::kMaxAccessories> accessories_{};
    ButtonOrder order_ = ButtonOrder::Driver;
    enum class Picture { G25, G27, G920, G923 };
    // The Xbox wheels: the G920 and the G923 share a body, their buttons' own print, the Driving Force Shifter.
    [[nodiscard]] bool xbox() const { return picture_ == Picture::G920 || picture_ == Picture::G923; }
    Picture picture_ = Picture::G25;
    std::size_t gear1_ = 8;        // Gear 1's target button (Gear 2..6 and R follow): the G920's 12
    float offY_ = 0.0F;
    std::vector<ShapePoint> points_;   // a shape's corners (kept: drawing allocates nothing once grown)
    std::vector<ShapePoint> cutPoints_;   // a shape cut short (roundedRectBelow), kept the same way
};

}  // namespace wheelbri::service
