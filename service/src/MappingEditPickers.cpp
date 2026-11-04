// MappingEditView: the popups that pick a control (the source picker, the secondaries' picker) and the rig's
// touches that feed them.

#include "MappingEditView.hpp"
#include "MappingEditViewParts.hpp"
#include "UiStyle.hpp"
#include "GameWatch.hpp"
#include "HoverText.hpp"

#include "Platform.hpp"
#include "ServiceLogging.hpp"
#include "Version.hpp"

#include "wheelbri/AutoMap.hpp"
#include "wheelbri/AxisTuning.hpp"
#include "wheelbri/CentringSpring.hpp"
#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/DeviceMatch.hpp"
#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/FileText.hpp"
#include "wheelbri/GameProfiles.hpp"
#include "wheelbri/GameRotation.hpp"
#include "wheelbri/GameSettings.hpp"
#include "wheelbri/GameTab.hpp"
#include "wheelbri/GameTuning.hpp"
#include "wheelbri/IniDocument.hpp"
#include "wheelbri/Logging.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <utility>

namespace wheelbri::service {

using namespace editview;

std::string MappingEditView::axisName(const SourceRef& source) const {
    const RigControl* const control = rigControl(source, true);
    const std::string name = control != nullptr ? controlName(*control) : std::string();
    return name.empty() ? "axis " + std::to_string(source.index) : name;
}

std::string MappingEditView::shortAlt(const ButtonRef& alt) const {
    if (alt.axis) return describeAlt(alt);
    const RigControl* const control = rigControl(alt.source, false);
    const std::string name = control != nullptr ? controlName(*control) : std::string();
    return name.empty() ? "button " + std::to_string(alt.source.index) : name;
}

std::string MappingEditView::describeAlt(const ButtonRef& alt) const {
    if (!alt.mapped()) return "(nothing)";
    if (!alt.axis) return describe(alt.source, false);
    // "Hand > 50%": the axis' name, and where it presses the button.
    return axisName(alt.source) + " > " + tuning::formatPercent(alt.threshold);
}

void MappingEditView::openPicker(const Slot kind, const std::size_t index) {
    mode_ = Mode::Pick;
    pickKind_ = kind;
    pickIndex_ = index;
    findText_.clear();
    listScroll_ = 0;
    touchFlashUntilMs_ = 0;
    touchNoteUntilMs_ = 0;
}

void MappingEditView::openAltPicker(const std::size_t index) {
    mode_ = Mode::PickAlt;
    pickKind_ = Slot::Button;
    pickIndex_ = index;
    altFocus_ = 0;
    findText_.clear();
    listScroll_ = 0;
    touchFlashUntilMs_ = 0;
    touchNoteUntilMs_ = 0;
}

void MappingEditView::openGameAltPicker(const std::size_t index) {
    const std::size_t picked = selectedGameIndex();
    if (picked >= games_.size() || index >= edit_.secondaries.size()) return;
    openAltPicker(index);
    pickKind_ = Slot::GameButton;
    altGameId_ = games_[picked].id;
    GameTuning& own = tuningFor(altGameId_);
    // None of its own yet: it starts from the Mapping tab's, as the row shows them.
    if (own.secondaries.count(static_cast<int>(index)) == 0) own.secondaries[static_cast<int>(index)] =
        edit_.secondaries[index];
}

void MappingEditView::closeAltPicker() {
    if (altForGame()) {
        const auto found = edit_.gameTunings.find(altGameId_);
        if (found != edit_.gameTunings.end()) {
            auto& lists = found->second.secondaries;
            const auto list = lists.find(static_cast<int>(pickIndex_));
            if (list != lists.end() && list->second == edit_.secondaries[pickIndex_]) lists.erase(list);
            dropEmptyTuning(altGameId_);
        }
    }
    mode_ = Mode::Idle;
}

std::vector<ButtonRef>& MappingEditView::altList() {
    if (pickKind_ == Slot::GameButton && !altGameId_.empty()) {
        return tuningFor(altGameId_).secondaries[static_cast<int>(pickIndex_)];
    }
    return edit_.secondaries[pickIndex_];
}

void MappingEditView::drawAltPicker() {
    hit(0, 0, canvasW(), contentH(), [] {});   // clicks outside the popup do nothing
    window_.fill(kPopupX, kPopupY, popupW(), popupH(), style::kBox);
    window_.outline(kPopupX, kPopupY, popupW(), popupH(), kHeading);
    const float left = kPopupX + 12;
    const float right = kPopupX + popupW() - 12;
    const std::size_t target = pickIndex_;
    const bool forGame = altForGame();
    const std::string targetName = profile_->identity().shortName + " " + std::string(profile_->buttonName(target));
    const auto game = std::find_if(games_.begin(), games_.end(), [this](const GameProfile& g) {
        return g.id == altGameId_;
    });
    const std::string inGame = forGame && game != games_.end() ? " in " + game->label : std::string();
    window_.text(left, kPopupY + 10, "What else presses " + targetName + inGame + "?", kHeading);
    button(kPopupX + popupW() - 100, kPopupY + 6, "Done", [this] { closeAltPicker(); }, false, true, 88);
    std::vector<ButtonRef>& list = altList();
    const std::size_t max = MapperConfig::kMaxSecondaries;

    // How to pick: press it on the rig (the app reads the devices meanwhile), or type to find it; and clear all.
    // While a session runs the app does not read the devices (the session has them): find it only.
    const std::string how = locked_ ? "Find the control:" : "Press a control on your rig, or find it:";
    window_.text(left, kPopupY + 32, how, kDim);
    const float findX = left + static_cast<float>(how.size() + 1) * ui::kCharWidth;
    const float findW = 24 * ui::kCharWidth + 12;
    window_.fill(findX, kPopupY + 28, findW, 16, kStripe);
    window_.outline(findX, kPopupY + 28, findW, 16, field_ == Field::None ? kHeading : style::kFieldEdge);
    window_.text(findX + 6, kPopupY + 32, findText_ + (field_ == Field::None ? "_" : ""), kText);
    tip(findX, kPopupY + 28, findW, 16,
        "Type a number or a name: only the controls holding it are listed. Esc clears it.");
    button(findX + findW + 12, kPopupY + 28, "(clear all)", [&list] { list.clear(); }, false, !list.empty(), 100);
    if (forGame) {
        // Back to the Mapping tab's: the game's list goes when the popup closes (closeAltPicker).
        const std::vector<ButtonRef>& mapped = edit_.secondaries[target];
        const float mappedX = findX + findW + 12 + 108;
        button(mappedX, kPopupY + 28, "Mapping tab's", [&list, &mapped] { list = mapped; }, list == mapped, true, 120);
        tip(mappedX, kPopupY + 28, 120, 16,
            "The Mapping tab's secondaries for it, followed again: " +
                (mapped.empty() ? std::string("none") : std::to_string(mapped.size())) + ".");
    }

    // The ones it has, as chips after "Has N of 8." (at most 8: one or two rows).
    const std::string has = "Has " + std::to_string(list.size()) + " of " + std::to_string(max) + ".";
    window_.text(left, kPopupY + 52, has, kDim);
    const int kColumns = popupColumns();
    const float kChipW = entryW(kColumns);
    const float chipsX = left + static_cast<float>(has.size() + 1) * ui::kCharWidth;
    const auto chipsPerRow = static_cast<std::size_t>(std::max(1.0F, std::floor((right - chipsX + 4) / (kChipW + 4))));
    if (altFocus_ >= list.size()) altFocus_ = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const float x = chipsX + static_cast<float>(i % chipsPerRow) * (kChipW + 4);
        const float y = kPopupY + 48 + static_cast<float>(i / chipsPerRow) * 18;
        window_.fill(x, y, kChipW, 16, kStripe);
        window_.outline(x, y, kChipW, 16, kHeading);
        window_.text(x + 4, y + 4, fit(shortAlt(list[i]), static_cast<std::size_t>((kChipW - 8) / ui::kCharWidth)),
                     kText);
        tip(x, y, kChipW, 16, list[i].axis ? describeAlt(list[i]) : describeFull(list[i].source, false));
    }
    const DeviceLabels* const wheel = wheelLabels();
    std::vector<const DeviceLabels*> order;
    if (wheel != nullptr) order.push_back(wheel);
    for (const DeviceLabels& device : devices_) {
        if (&device != wheel) order.push_back(&device);
    }
    // The list scrolls (the mouse wheel, onScroll): `vy` is where a row would be unscrolled, `y` where it is drawn.
    // Rows outside [listTop, bottom] are not drawn, and counted for the line at the bottom.
    const float listTop = kPopupY + 88;
    const float bottom = kPopupY + popupH() - 40;   // the last row ends above the line at the bottom
    float vy = listTop;
    std::size_t above = 0;
    std::size_t below = 0;
    const auto shown = [&](const float rowVy) {
        const float y = rowVy - listScroll_;
        if (y < listTop) return false;
        return y <= bottom;
    };
    const auto count = [&](const float rowVy) {
        if (rowVy - listScroll_ < listTop) {
            ++above;
        } else if (rowVy - listScroll_ > bottom) {
            ++below;
        }
    };
    if (devices_.empty()) {
        window_.text(left, listTop, "The rig has no devices yet: define them in the Rig tab.", kCaution);
    }
    // What the find box keeps: an entry whose text holds it (any case), or every entry of a device whose name does.
    std::string find = findText_;
    std::transform(find.begin(), find.end(), find.begin(), [](const unsigned char c) { return std::tolower(c); });
    const auto holds = [&find](std::string text) {
        if (find.empty()) return true;
        std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return std::tolower(c); });
        return text.find(find) != std::string::npos;
    };
    const ControlUse self{ControlUse::Kind::Secondary, target};
    const std::uint64_t now = nowMs();
    // A control's entry: pressed, it is added (an axis: its row's slider comes alive) or, when it is one already,
    // taken off. The control last touched on the rig flashes green.
    const auto entryAt = [&](const float x, const float entryY, const RigDevice& rigDevice, const bool isWheel,
                             const bool axis, const std::size_t index) {
        const SourceRef source{isWheel ? DeviceId{} : rigDevice.id, static_cast<int>(index)};
        // A game's: any control (as its main source), taken from nowhere else.
        EntryState state = forGame ? EntryState{} : entryState({source, axis}, self);
        if (forGame) {
            state.mine = std::any_of(list.begin(), list.end(), [&](const ButtonRef& b) {
                return b.axis == axis && b.source == source;
            });
        }
        if (!state.mine && !state.taken && list.size() >= max) {
            state.taken = true;   // full: nothing more is added
            state.tip = "At most " + std::to_string(max) + " secondary assignments per button";
        }
        const RigControl& control = axis ? rigDevice.axes[index] : rigDevice.buttons[index];
        const ButtonRef ref{source, axis, false, 0.5F};
        drawEntry(x, entryY, kChipW, (axis ? "axis " : "") + std::to_string(index) + " " + controlName(control), state,
                  [this, &list, ref, mine = state.mine] {
                      const auto same = [&ref](const ButtonRef& b) {
                          return b.axis == ref.axis && b.source == ref.source;
                      };
                      if (mine) {
                          list.erase(std::remove_if(list.begin(), list.end(), same), list.end());
                          altFocus_ = 0;
                          return;
                      }
                      list.push_back(ref);
                      if (ref.axis) altFocus_ = list.size() - 1;
                  });
        if (now < touchFlashUntilMs_ && touchFlash_ == source && touchFlashAxis_ == axis) {
            window_.outline(x - 1, entryY - 1, kChipW + 2, 18, kActive);
            window_.outline(x - 2, entryY - 2, kChipW + 4, 20, kActive);
        }
        return source;
    };
    const auto entryText = [this](const RigControl& control, const bool axis, const std::size_t index) {
        return (axis ? "axis " : "") + std::to_string(index) + " " + controlName(control);
    };
    // Each device's name line inside a section: its name as long as the popup allows.
    const auto deviceLine = [&](const DeviceLabels& device, const bool isWheel) {
        const std::string tail = "  " + deviceId(device.vendorId, device.productId) + "  " +
                                 (isWheel ? "(the wheel)" : deviceKindName(device.kind));
        const auto room = static_cast<std::size_t>((right - left - 8) / ui::kCharWidth);
        if (shown(vy)) {
            window_.text(left + 8, vy - listScroll_ + 2,
                         fit(deviceName(device), room > tail.size() + 8 ? room - tail.size() : 8) + tail, kText);
        }
        vy += 14;
    };
    const auto deviceHolds = [&](const DeviceLabels& device) { return !find.empty() && holds(deviceName(device)); };

    // AXES: every device's named axes, one a row: its entry, then the slider of the travel it presses past (0% as
    // soon as it moves, 100% only at its end), the number typed in, and which way round it reads. Live once the axis
    // is added; greyed before.
    // A section with nothing the find box keeps is left out, heading and all.
    const auto anyKept = [&](const bool axis) {
        for (const DeviceLabels* const entry : order) {
            const RigDevice* const rigDevice = rig_.find(idOf(*entry));
            if (rigDevice == nullptr) continue;
            const bool wholeDevice = deviceHolds(*entry);
            const std::span<const RigControl> controls =
                axis ? std::span<const RigControl>(rigDevice->axes) : std::span<const RigControl>(rigDevice->buttons);
            for (std::size_t i = 0; i < controls.size(); ++i) {
                if (controls[i].defined() && (wholeDevice || holds(entryText(controls[i], axis, i)))) return true;
            }
        }
        return false;
    };
    const bool anyAxes = anyKept(true);
    const bool anyButtons = anyKept(false);
    std::size_t matches = 0;
    if (anyAxes) {
        if (shown(vy)) style::heading(window_, left, vy - listScroll_ + 2, "AXES");
        vy += 16;
    }
    thresholdSliderX_ = left + kChipW + 12 + 24;
    thresholdSliderW_ = std::max(100.0F, popupW() - 24 - (kChipW + 12) - 24 - 38 - 56 - 8 - 88 - 8);
    for (const DeviceLabels* const entry : order) {
        const RigDevice* const rigDevice = rig_.find(idOf(*entry));
        if (rigDevice == nullptr) continue;
        const bool wholeDevice = deviceHolds(*entry);
        std::vector<std::size_t> axes;
        for (std::size_t a = 0; a < rigDevice->axes.size(); ++a) {
            if (rigDevice->axes[a].defined() && (wholeDevice || holds(entryText(rigDevice->axes[a], true, a)))) {
                axes.push_back(a);
            }
        }
        if (axes.empty()) continue;
        const bool isWheel = wheel != nullptr && idOf(*entry) == idOf(*wheel);
        deviceLine(*entry, isWheel);
        for (const std::size_t a : axes) {
            ++matches;
            if (!shown(vy)) {
                count(vy);
                vy += 18;
                continue;
            }
            const float y = vy - listScroll_;
            const SourceRef source = entryAt(left, y, *rigDevice, isWheel, true, a);
            std::size_t mine = list.size();
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (list[i].axis && list[i].source == source) mine = i;
            }
            const bool live = mine < list.size();
            const float sliderX = thresholdSliderX_;
            const float sliderW = thresholdSliderW_;
            const float threshold = live ? std::clamp(list[mine].threshold, 0.0F, 1.0F) : 0.0F;
            window_.text(sliderX - 24, y + 4, "0%", kDim);
            window_.fill(sliderX, y + 6, sliderW, 4, live ? kStripe : kPanel);
            if (live) {
                window_.fill(sliderX, y + 6, threshold * sliderW, 4, kHeading);
                window_.fill(sliderX + threshold * sliderW - 3, y + 1, 6, 14, kText);
                hit(sliderX - 4, y, sliderW + 8, 16, [this, mine] {
                    altFocus_ = mine;
                    setThresholdFromX(clickX_);   // the press sets it; holding the button drags it (onPointer)
                    thresholdDragging_ = true;
                });
                tip(sliderX - 4, y, sliderW + 8, 16,
                    "0%: pressed as soon as it moves; 100%: only at its end. Released 5% below (at rest near 0%).");
            } else {
                tip(sliderX - 4, y, sliderW + 8, 16, "Press the axis on the left to add it, then set where it presses");
            }
            const float valueX = sliderX + sliderW + 6;
            window_.text(valueX, y + 4, "100%", kDim);
            if (live) {
                drawField(valueX + 38, y, 56, Field::Threshold, tuning::formatPercent(list[mine].threshold), false,
                          mine);
            } else {
                window_.fill(valueX + 38, y, 56, 16, kPanel);
                window_.text(valueX + 44, y + 4, "--", kDim);
            }
            // Which way round it reads (a lever released at the top): here, not in the rig.
            const float invertX = valueX + 38 + 56 + 8;
            button(invertX, y, "Inverted", [&list, mine] {
                if (mine < list.size()) list[mine].invert = !list[mine].invert;
            }, live && list[mine].invert, live, 88);
            if (live) tip(invertX, y, 88, 16, "On: the axis rests at the top (+1) and is pressed towards the bottom.");
            vy += 18;
        }
        vy += 4;
    }

    // BUTTONS: every device's named buttons, a grid each.
    if (anyButtons) {
        if (anyAxes) vy += 4;
        if (shown(vy)) style::heading(window_, left, vy - listScroll_ + 2, "BUTTONS");
        vy += 16;
    }
    for (const DeviceLabels* const entry : order) {
        const RigDevice* const rigDevice = rig_.find(idOf(*entry));
        if (rigDevice == nullptr) continue;
        const bool wholeDevice = deviceHolds(*entry);
        std::vector<std::size_t> buttons;
        for (std::size_t b = 0; b < rigDevice->buttons.size(); ++b) {
            if (rigDevice->buttons[b].defined() && (wholeDevice || holds(entryText(rigDevice->buttons[b], false, b)))) {
                buttons.push_back(b);
            }
        }
        if (buttons.empty()) continue;
        const bool isWheel = wheel != nullptr && idOf(*entry) == idOf(*wheel);
        deviceLine(*entry, isWheel);
        const auto columns = static_cast<std::size_t>(kColumns);
        for (std::size_t n = 0; n < buttons.size(); ++n) {
            ++matches;
            const float rowVy = vy + static_cast<float>(n / columns) * 18;
            if (shown(rowVy)) {
                (void)entryAt(left + static_cast<float>(n % columns) * (kChipW + 4), rowVy - listScroll_, *rigDevice,
                              isWheel, false, buttons[n]);
            } else {
                count(rowVy);
            }
        }
        vy += static_cast<float>((buttons.size() + columns - 1) / columns) * 18 + 4;
    }
    if (!find.empty() && matches == 0) {
        window_.text(left, listTop + 18, "No control holds \"" + findText_ + "\".", kCaution);
    }
    // How far it can scroll: the list's height less the room it has.
    listScrollMax_ = std::max(0.0F, vy - (bottom + 18));
    listScroll_ = std::clamp(listScroll_, 0.0F, listScrollMax_);

    // The line at the bottom, on its own: a rig touch's note for a moment, else what is out of view, else the note.
    std::string line;
    ui::Colour colour = kDim;
    if (now < touchNoteUntilMs_) {
        line = touchNote_;
        colour = touchNoteOk_ ? kActive : kCaution;
    } else if (above > 0 || below > 0) {
        line = (below > 0 ? std::to_string(below) + " more below" : std::string()) +
               (above > 0 && below > 0 ? ", " : "") + (above > 0 ? std::to_string(above) + " above" : std::string()) +
               ": scroll with the mouse wheel. Greyed: used elsewhere (hover to see where).";
        colour = kCaution;
    } else {
        line = forGame ? "Any control, even one the Mapping tab uses. Axes: a pedal or a lever, pressed past the "
                         "threshold."
                       : "Greyed: used elsewhere (hover to see where). Axes: a pedal or a lever, pressed past the "
                         "threshold.";
    }
    window_.text(left, kPopupY + popupH() - 20, fit(line, static_cast<std::size_t>((right - left) / ui::kCharWidth)),
                 colour);
}

void MappingEditView::onScroll(const int notches) {
    if (mode_ != Mode::PickAlt && mode_ != Mode::Pick) return;
    listScroll_ = std::clamp(listScroll_ - static_cast<float>(notches) * 54.0F, 0.0F, listScrollMax_);
}

void MappingEditView::onRigTouch(const DeviceId& device, const bool axis, const int index) {
    if (!wantsRigTouches() || profile_ == nullptr || index < 0) return;
    const DeviceLabels* const wheel = wheelLabels();
    const bool isWheel = wheel != nullptr && idOf(*wheel) == device;
    const RigDevice* const rigDevice = rig_.find(device);
    const auto i = static_cast<std::size_t>(index);
    const std::uint64_t now = nowMs();
    const auto note = [&](std::string text, const bool ok) {
        touchNote_ = std::move(text);
        touchNoteOk_ = ok;
        touchNoteUntilMs_ = now + 3000;
    };
    // The source picker takes an axis for an axis, a button for the rest.
    if (mode_ == Mode::Pick && axis != (pickKind_ == Slot::Axis)) {
        note(std::string(axis ? "That was an axis" : "That was a button") + ": " + pickWhat() + " takes " +
                 (axis ? "a button." : "an axis."),
             false);
        return;
    }
    const bool named =
        rigDevice != nullptr && (axis ? i < rigDevice->axes.size() && rigDevice->axes[i].defined()
                                      : i < rigDevice->buttons.size() && rigDevice->buttons[i].defined());
    if (!named) {
        note(std::string(axis ? "That axis" : "That button") + " has no name in the Rig tab yet: name it there first.",
             false);
        return;
    }
    const SourceRef source{isWheel ? DeviceId{} : device, index};
    const RigControl& control = axis ? rigDevice->axes[i] : rigDevice->buttons[i];
    const std::string name = (axis ? "axis " : "") + std::to_string(index) + " " + controlName(control);
    touchFlash_ = source;
    touchFlashAxis_ = axis;
    touchFlashUntilMs_ = now + 1500;
    if (mode_ == Mode::Pick) {
        // Picked and closed, as a click on it; one used elsewhere only flashes (a click moves it: never a press).
        const EntryState state =
            pickKind_ == Slot::GameButton ? EntryState{} : entryState({source, axis}, pickUse());
        if (state.taken && !state.mine) {
            note(name + ": used elsewhere (" + state.tip + "): click it to move it here.", false);
            return;
        }
        setPicked(source);
        mode_ = Mode::Idle;
        return;
    }
    std::vector<ButtonRef>& list = altList();
    EntryState state = altForGame() ? EntryState{}
                                    : entryState({source, axis}, ControlUse{ControlUse::Kind::Secondary, pickIndex_});
    if (altForGame()) {
        state.mine = std::any_of(list.begin(), list.end(), [&](const ButtonRef& b) {
            return b.axis == axis && b.source == source;
        });
    }
    if (state.mine) {
        note(name + ": already here (click it to take it off).", true);
    } else if (state.taken) {
        note(name + ": used elsewhere (" + state.tip + ").", false);
    } else if (list.size() >= MapperConfig::kMaxSecondaries) {
        note("At most " + std::to_string(MapperConfig::kMaxSecondaries) + " secondary assignments per button.", false);
    } else {
        list.push_back(ButtonRef{source, axis, false, 0.5F});
        if (axis) altFocus_ = list.size() - 1;
        note("Added " + name + ".", true);
    }
}

ControlUse MappingEditView::pickUse() const {
    return {pickKind_ == Slot::Axis  ? ControlUse::Kind::Axis
            : pickKind_ == Slot::Hat ? ControlUse::Kind::Hat
                                     : ControlUse::Kind::Button,
            pickIndex_};
}

std::string MappingEditView::pickWhat() const {
    if (pickKind_ == Slot::Axis) return kAxisNames[pickIndex_];
    if (pickKind_ == Slot::Hat) return std::string("D-pad ") + kHatNames[pickIndex_];
    std::string what = "button " + std::to_string(gameNumber(pickIndex_)) + " " +
                       std::string(profile_ != nullptr ? profile_->buttonName(pickIndex_) : std::string_view());
    if (pickKind_ == Slot::GameButton && selectedGameIndex() < games_.size()) {
        what += " in " + games_[selectedGameIndex()].label;
    }
    return what;
}

void MappingEditView::moveHere(const SourceRef& source) {
    if (profile_ == nullptr) return;
    const bool axis = pickKind_ == Slot::Axis;
    const ControlUse self = pickUse();
    AutoInputs& autos = edit_.autoInputs;
    for (const ControlUse& use : usesOf(edit_, *profile_, {source, axis})) {
        if (use == self) continue;
        const std::size_t i = use.index;
        switch (use.kind) {
            case ControlUse::Kind::Axis:
                edit_.axes[i] = {};
                autos.axes[i] = false;   // taken away on purpose: the Rig does not give it back
                break;
            case ControlUse::Kind::Hat:
                edit_.hat[i] = {};
                autos.hat[i] = false;
                break;
            case ControlUse::Kind::Button:
                edit_.buttons[i] = {};
                if (i < autos.buttons.size()) autos.buttons[i] = false;
                break;
            case ControlUse::Kind::Secondary: {
                std::vector<ButtonRef>& list = edit_.secondaries[i];
                list.erase(std::remove_if(list.begin(), list.end(),
                                          [&](const ButtonRef& b) { return b.axis == axis && b.source == source; }),
                           list.end());
                break;
            }
        }
    }
    setPicked(source);
}

void MappingEditView::drawPicker() {
    hit(0, 0, canvasW(), contentH(), [] {});   // clicks outside the popup do nothing
    window_.fill(kPopupX, kPopupY, popupW(), popupH(), style::kBox);
    window_.outline(kPopupX, kPopupY, popupW(), popupH(), kHeading);
    const bool axis = pickKind_ == Slot::Axis;
    const bool forGame = pickKind_ == Slot::GameButton;
    const float left = kPopupX + 12;
    const float right = kPopupX + popupW() - 12;
    window_.text(left, kPopupY + 10, "What drives " + pickWhat() + "?", kHeading);
    const SourceRef current = pickedSource();
    const ControlUse self = pickUse();
    button(kPopupX + popupW() - 100, kPopupY + 6, "Cancel", [this] { mode_ = Mode::Idle; }, false, true, 88);
    button(left, kPopupY + 28, "(nothing)", [this] {
        setPicked({});
        mode_ = Mode::Idle;
    }, !current.mapped(), true, 120);

    // How to pick: press it on the rig (the app reads the devices meanwhile), or type to find it.
    const std::string how = locked_ ? std::string(axis ? "Find the axis:" : "Find the button:")
                            : axis  ? "Move an axis on your rig, or find it:"
                                    : "Press a button on your rig, or find it:";
    const float howX = left + 132;
    window_.text(howX, kPopupY + 32, how, kDim);
    const float findX = howX + static_cast<float>(how.size() + 1) * ui::kCharWidth;
    const float findW = 24 * ui::kCharWidth + 12;
    window_.fill(findX, kPopupY + 28, findW, 16, kStripe);
    window_.outline(findX, kPopupY + 28, findW, 16, kHeading);
    window_.text(findX + 6, kPopupY + 32, findText_ + "_", kText);
    tip(findX, kPopupY + 28, findW, 16,
        "Type a number or a name: only the controls holding it are listed. Esc clears it.");
    if (forGame) {
        // Back to following the Mapping tab's main source (the row and its secondaries stay).
        const SourceRef mapped = edit_.buttons[pickIndex_];
        const float mappedX = findX + findW + 12;
        const GameTuning* const own =
            selectedGameIndex() < games_.size() ? tuningOf(games_[selectedGameIndex()].id) : nullptr;
        const bool follows = own == nullptr || own->buttons.count(static_cast<int>(pickIndex_)) == 0;
        button(mappedX, kPopupY + 28, "Mapping tab's", [this, mapped] {
            setPicked(mapped);
            mode_ = Mode::Idle;
        }, follows, true, 120);
        tip(mappedX, kPopupY + 28, 120, 16,
            "The Mapping tab's, followed again: " + (mapped.mapped() ? describeFull(mapped, false) : "nothing") + ".");
    }

    const DeviceLabels* const wheel = wheelLabels();
    // The wheel first, then the accessories.
    std::vector<const DeviceLabels*> order;
    if (wheel != nullptr) order.push_back(wheel);
    for (const DeviceLabels& device : devices_) {
        if (&device != wheel) order.push_back(&device);
    }
    const int kColumns = popupColumns();   // more columns in a wider window
    const auto columns = static_cast<std::size_t>(kColumns);
    const float kEntryW = entryW(kColumns);
    // The list scrolls (the mouse wheel): `vy` is where a row would be unscrolled; rows outside [listTop, bottom] are
    // not drawn, and counted for the line at the bottom.
    const float listTop = kPopupY + 54;
    const float bottom = kPopupY + popupH() - 40;
    float vy = listTop;
    std::size_t above = 0;
    std::size_t below = 0;
    const auto shown = [&](const float rowVy) {
        const float y = rowVy - listScroll_;
        return y >= listTop && y <= bottom;
    };
    const auto count = [&](const float rowVy) {
        if (rowVy - listScroll_ < listTop) {
            ++above;
        } else if (rowVy - listScroll_ > bottom) {
            ++below;
        }
    };
    if (devices_.empty()) {
        window_.text(left, listTop, "The rig has no devices yet: define them in the Rig tab.", kCaution);
    }
    std::string find = findText_;
    std::transform(find.begin(), find.end(), find.begin(), [](const unsigned char c) { return std::tolower(c); });
    const auto holds = [&find](std::string text) {
        if (find.empty()) return true;
        std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return std::tolower(c); });
        return text.find(find) != std::string::npos;
    };
    const std::uint64_t now = nowMs();
    std::size_t matches = 0;
    for (const DeviceLabels* const entry : order) {
        const DeviceLabels& device = *entry;
        const bool isWheel = wheel != nullptr && idOf(device) == idOf(*wheel);
        const RigDevice* const rigDevice = rig_.find(idOf(device));
        const bool wholeDevice = !find.empty() && holds(deviceName(device));
        // The device's named controls of the kind picked, those the find box keeps.
        std::vector<std::size_t> kept;
        bool anyDefined = false;
        if (rigDevice != nullptr) {
            const std::size_t n = axis ? rigDevice->axes.size() : rigDevice->buttons.size();
            for (std::size_t index = 0; index < n; ++index) {
                const RigControl& control = axis ? rigDevice->axes[index] : rigDevice->buttons[index];
                if (!control.defined()) continue;
                anyDefined = true;
                if (wholeDevice || holds(std::to_string(index) + " " + controlName(control))) kept.push_back(index);
            }
        }
        if (kept.empty() && (!find.empty() || anyDefined)) continue;   // nothing of it to list
        const std::string tail = "  " + deviceId(device.vendorId, device.productId) + "  " +
                                 (isWheel ? "(the wheel)" : deviceKindName(device.kind));
        const auto room = static_cast<std::size_t>((right - left) / ui::kCharWidth);
        if (shown(vy)) {
            window_.text(left, vy - listScroll_,
                         fit(deviceName(device), room > tail.size() + 8 ? room - tail.size() : 8) + tail, kText);
        }
        vy += 14;
        if (kept.empty()) {
            if (shown(vy)) {
                window_.text(left, vy - listScroll_,
                             axis ? "no axis named: name them in the Rig tab"
                                  : "no button named: name them in the Rig tab",
                             kDim);
            }
            vy += 14 + 8;
            continue;
        }
        for (std::size_t n = 0; n < kept.size(); ++n) {
            ++matches;
            const std::size_t index = kept[n];
            const float rowVy = vy + static_cast<float>(n / columns) * 18;
            if (!shown(rowVy)) {
                count(rowVy);
                continue;
            }
            const RigControl& control = axis ? rigDevice->axes[index] : rigDevice->buttons[index];
            const SourceRef source{isWheel ? DeviceId{} : idOf(device), static_cast<int>(index)};
            const float x = left + static_cast<float>(n % columns) * (kEntryW + 4);
            const float y = rowVy - listScroll_;
            // For one game a control may be used again (it swaps for that game): nothing greyed then. Used elsewhere:
            // greyed, and a click moves it here (taken from the other use).
            EntryState state = forGame ? EntryState{} : entryState({source, axis}, self);
            state.mine = forGame ? source == current : state.mine || source == current;
            const bool moves = state.taken && !state.mine;
            if (moves) state.tip += ". Click to move it here.";
            drawEntry(x, y, kEntryW, std::to_string(index) + " " + controlName(control), state,
                      [this, source, moves] {
                          if (moves) {
                              moveHere(source);
                          } else {
                              setPicked(source);   // which way round stays the row's Invert
                          }
                          mode_ = Mode::Idle;
                      },
                      true);
            if (now < touchFlashUntilMs_ && touchFlash_ == source && touchFlashAxis_ == axis) {
                window_.outline(x - 1, y - 1, kEntryW + 2, 18, kActive);
                window_.outline(x - 2, y - 2, kEntryW + 4, 20, kActive);
            }
        }
        vy += static_cast<float>((kept.size() + columns - 1) / columns) * 18 + 8;
    }
    if (!find.empty() && matches == 0) {
        window_.text(left, listTop, "No control holds \"" + findText_ + "\".", kCaution);
    }
    listScrollMax_ = std::max(0.0F, vy - (bottom + 18));
    listScroll_ = std::clamp(listScroll_, 0.0F, listScrollMax_);

    // The line at the bottom: a rig touch's note for a moment, else what is out of view, else what greyed means.
    std::string line;
    ui::Colour colour = kDim;
    const std::string greyed = forGame ? "For this game only: any control, also one the Mapping tab uses elsewhere."
                                       : "Greyed: used elsewhere (hover to see where); a click moves it here.";
    if (now < touchNoteUntilMs_) {
        line = touchNote_;
        colour = touchNoteOk_ ? kActive : kCaution;
    } else if (above > 0 || below > 0) {
        line = (below > 0 ? std::to_string(below) + " more below" : std::string()) +
               (above > 0 && below > 0 ? ", " : "") + (above > 0 ? std::to_string(above) + " above" : std::string()) +
               ": scroll with the mouse wheel. " + greyed;
        colour = kCaution;
    } else {
        line = greyed;
    }
    window_.text(left, kPopupY + popupH() - 20, fit(line, static_cast<std::size_t>((right - left) / ui::kCharWidth)),
                 colour);
}

}  // namespace wheelbri::service
