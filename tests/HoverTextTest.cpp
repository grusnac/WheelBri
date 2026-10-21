// Hover text (service/src/HoverText.cpp): which area's text shows, how it wraps, and where its box goes.

#include "HoverText.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace wheelbri::service {
namespace {

class BoxWindow final : public ui::UiWindow {
public:
    ui::WindowInput pump() override { return {}; }
    [[nodiscard]] bool visible() const override { return true; }
    void begin(ui::Colour) override {}
    void fill(const float x, const float y, const float w, const float h, ui::Colour) override {
        boxes.push_back({x, y, w, h});
    }
    void outline(float, float, float, float, ui::Colour) override {}
    void text(float, float, const std::string_view value, ui::Colour) override { texts.emplace_back(value); }
    void present() override {}
    bool openFolder(const std::filesystem::path&, std::string&) override { return false; }
    bool pickFolder(const std::string&, std::filesystem::path&, std::string&) override { return false; }
    [[nodiscard]] std::string describe() const override { return "box"; }

    struct Box {
        float x, y, w, h;
    };
    std::vector<Box> boxes;
    std::vector<std::string> texts;
};

// The last area added under the mouse wins; edges: the right and bottom ones are outside; nothing once the mouse
// left the window; areas before `first` are skipped.
TEST(HoverText, TheLastAreaUnderTheMouseWins) {
    HoverText hover;
    hover.add(0, 0, 100, 20, "wide");
    hover.add(10, 0, 20, 20, "narrow");
    hover.add(0, 0, 5, 5, "");   // no text: not kept
    EXPECT_EQ(hover.size(), 2U);
    hover.setHover(15, 5, true);
    ASSERT_NE(hover.under(), nullptr);
    EXPECT_EQ(*hover.under(), "narrow");
    EXPECT_EQ(*hover.under(0), "narrow");
    hover.setHover(30, 5, true);   // the narrow one's right edge: outside it
    EXPECT_EQ(*hover.under(), "wide");
    hover.setHover(15, 20, true);  // the bottom edge
    EXPECT_EQ(hover.under(), nullptr);
    hover.setHover(15, 5, false);
    EXPECT_EQ(hover.under(), nullptr);
    hover.setHover(50, 5, true);
    EXPECT_EQ(hover.under(2), nullptr);
    hover.clear();
    EXPECT_EQ(hover.under(), nullptr);
}

// Wrapped at spaces to the width; a word longer than a line is cut; at most kMaxLines, the last ending in "..".
TEST(HoverText, LinesWrapAtSpaces) {
    EXPECT_EQ(HoverText::lines("one two three", 7), (std::vector<std::string>{"one two", "three"}));
    EXPECT_EQ(HoverText::lines("abcdefghij", 4), (std::vector<std::string>{"abcd", "efgh", "ij"}));
    EXPECT_TRUE(HoverText::lines("", 10).empty());
    std::string many;
    for (int i = 0; i < 20; ++i) many += "word ";
    const std::vector<std::string> cut = HoverText::lines(many, 10);
    ASSERT_EQ(cut.size(), HoverText::kMaxLines);
    EXPECT_EQ(cut.back().substr(cut.back().size() - 2), "..");
    for (const std::string& line : cut) EXPECT_LE(line.size(), 10U);
}

// The box goes below and right of the pointer, inside the canvas at the right edge and above the pointer near the
// bottom; one text call per line.
TEST(HoverText, TheBoxStaysInsideTheCanvas) {
    BoxWindow window;
    HoverText::drawBox(window, 100, 100, "short", 960, 700);
    ASSERT_EQ(window.boxes.size(), 1U);
    EXPECT_FLOAT_EQ(window.boxes[0].x, 112);
    EXPECT_FLOAT_EQ(window.boxes[0].y, 118);
    EXPECT_EQ(window.texts, (std::vector<std::string>{"short"}));

    window.boxes.clear();
    window.texts.clear();
    const std::string longer(150, 'x');
    HoverText::drawBox(window, 950, 690, longer, 960, 700);
    ASSERT_EQ(window.boxes.size(), 1U);
    EXPECT_LE(window.boxes[0].x + window.boxes[0].w, 958);
    EXPECT_LT(window.boxes[0].y + window.boxes[0].h, 690);   // above the pointer
    EXPECT_EQ(window.texts.size(), 3U);                      // 64 + 64 + 22
}


// Under the mouse: inside an area's width but above or below it is not in it; draw() draws the box of the one under the
// mouse and nothing when there is none.
TEST(HoverText, DrawShowsOnlyWhatIsUnderTheMouse) {
    HoverText hover;
    hover.add(10, 10, 100, 20, "here");
    BoxWindow window;
    hover.setHover(50, 5, true);   // above it
    hover.draw(window, 960, 700);
    EXPECT_TRUE(window.texts.empty());
    hover.setHover(50, 35, true);   // below it
    hover.draw(window, 960, 700);
    EXPECT_TRUE(window.texts.empty());
    hover.setHover(50, 15, true);
    hover.draw(window, 960, 700);
    EXPECT_EQ(window.texts, (std::vector<std::string>{"here"}));
    hover.clear();
    EXPECT_EQ(hover.size(), 0U);
}

// Nothing to show (no text, or a canvas too narrow for a line): no box.
TEST(HoverText, NoBoxForNothing) {
    BoxWindow window;
    HoverText::drawBox(window, 10, 10, "", 960, 700);
    EXPECT_TRUE(window.boxes.empty());
    EXPECT_TRUE(HoverText::lines("abc", 2).empty());
}

// Exactly six lines: the last is whole, no ".."; a text that starts with a space is cut at the width (no line of
// nothing); a word longer than a line is cut in it.
TEST(HoverText, LineEdges) {
    const std::vector<std::string> six = HoverText::lines("aaa bbb ccc ddd eee fff", 3);
    ASSERT_EQ(six.size(), 6U);
    EXPECT_EQ(six.back(), "fff");
    const std::vector<std::string> spaced = HoverText::lines(" abcdefgh", 4);
    ASSERT_FALSE(spaced.empty());
    EXPECT_EQ(spaced.front(), " abc");
    EXPECT_EQ(HoverText::lines("abcdefgh", 4), (std::vector<std::string>{"abcd", "efgh"}));
}

}  // namespace
}  // namespace wheelbri::service
