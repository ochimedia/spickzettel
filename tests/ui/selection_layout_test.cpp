// The selection bar's layout, from the rects alone - see
// ui/selection_layout.h.
#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <vector>

#include "ui/selection_layout.h"
#include "ui/ui_scale.h"

namespace sz::ui {
namespace {

using core::ChromeButton;

class SelectionLayoutTest : public ::testing::TestWithParam<float> {
protected:
    void SetUp() override { SetUiScale(GetParam()); }
    void TearDown() override { SetUiScale(1.0f); }
};

// The divider stands in the middle of the gap between the two groups: as
// much of the gap either side of it, and its edges on whole pixels, so it
// is drawn sharp. Where the scale puts the buttons themselves between
// pixels, it is as near the middle as a sharp line can be.
TEST_P(SelectionLayoutTest, TheDividerIsCenteredBetweenTheGroups) {
    const std::vector<ChromeButton> buttons = {ChromeButton::Pen,  ChromeButton::Eraser,   ChromeButton::Text,
                                               ChromeButton::Color, ChromeButton::Pin,     ChromeButton::More,
                                               ChromeButton::Minimize, ChromeButton::Maximize, ChromeButton::Close};
    const BarLayout bar = LayoutBar(core::Rect{301.0f, 400.0f, 500.0f, 300.0f}, 1920.0f, 1080.0f, buttons);
    const float width = PxWhole(1.0f);
    const std::optional<float> divider = BarDividerX(bar, buttons, width);
    ASSERT_TRUE(divider.has_value());

    const float left = *divider - width * 0.5f;
    const float right = *divider + width * 0.5f;
    EXPECT_EQ(left, std::round(left)) << "a sharp line";
    const float before = left - BarButtonRect(bar, buttons, ChromeButton::Color).max.x;
    const float after = BarButtonRect(bar, buttons, ChromeButton::Pin).min.x - right;
    const bool wholePixels = Px(1.0f) == std::round(Px(1.0f)) && Px(kBarPad) == std::round(Px(kBarPad));
    if (wholePixels && std::fmod(Px(kBarGroupGap) - width, 2.0f) == 0.0f) {
        EXPECT_EQ(before, after) << "as much gap on either side";
    } else {
        EXPECT_LE(std::abs(before - after), 1.0f) << "within a pixel of it";
    }
    EXPECT_GT(before, Px(kBarButtonGap)) << "a wider gap than between two buttons of one group";
}

INSTANTIATE_TEST_SUITE_P(Scales, SelectionLayoutTest, ::testing::Values(1.0f, 1.25f, 1.5f, 2.0f));

// The resize band is 8 px outside the snippet; its corners reach 16 px
// along the edges, and the snippet itself is not in it.
TEST(ResizeBand, IsOutsideTheSnippetWithCornersReachingAlongTheEdges) {
    SetUiScale(1.0f);
    const core::Rect r{100.0f, 100.0f, 200.0f, 100.0f};
    const auto at = [&](float x, float y) { return ResizeBandAt(r, x, y, 1920.0f, 1080.0f); };
    EXPECT_EQ(at(96.0f, 96.0f), ResizeHandle::NW);
    EXPECT_EQ(at(110.0f, 96.0f), ResizeHandle::NW) << "along the top edge, within 16 px";
    EXPECT_EQ(at(120.0f, 96.0f), ResizeHandle::N);
    EXPECT_EQ(at(200.0f, 207.0f), ResizeHandle::S);
    EXPECT_EQ(at(303.0f, 150.0f), ResizeHandle::E);
    EXPECT_EQ(at(303.0f, 195.0f), ResizeHandle::SE);
    EXPECT_FALSE(at(150.0f, 150.0f).has_value()) << "the snippet itself";
    EXPECT_FALSE(at(102.0f, 102.0f).has_value()) << "even just inside its corner";
    EXPECT_FALSE(at(200.0f, 91.0f).has_value()) << "beyond the band";
}

// On a small snippet the corners reach a quarter of the side at most, so
// some of each edge stays the edge's.
TEST(ResizeBand, OnASmallSnippetEachEdgeKeepsSomeOfItsOwn) {
    SetUiScale(1.0f);
    const core::Rect r{100.0f, 100.0f, 16.0f, 16.0f};
    const auto at = [&](float x, float y) { return ResizeBandAt(r, x, y, 1920.0f, 1080.0f); };
    EXPECT_EQ(at(101.0f, 96.0f), ResizeHandle::NW);
    EXPECT_EQ(at(108.0f, 96.0f), ResizeHandle::N);
    EXPECT_EQ(at(114.0f, 96.0f), ResizeHandle::NE);
    EXPECT_EQ(at(96.0f, 108.0f), ResizeHandle::W);
}

// Against the screen's edge the band is inside the snippet on that side,
// and on that side only - from where the snippet's visible edge is.
TEST(ResizeBand, AgainstTheScreensEdgeItIsInside) {
    SetUiScale(1.0f);
    const core::Rect atLeft{-50.0f, 100.0f, 200.0f, 100.0f};
    EXPECT_EQ(ResizeBandAt(atLeft, 5.0f, 150.0f, 1920.0f, 1080.0f), ResizeHandle::W);
    EXPECT_FALSE(ResizeBandAt(atLeft, 10.0f, 150.0f, 1920.0f, 1080.0f).has_value());
    EXPECT_FALSE(ResizeBandAt(atLeft, 146.0f, 150.0f, 1920.0f, 1080.0f).has_value()) << "the other side is outside";
    const core::Rect atBottom{100.0f, 980.0f, 200.0f, 100.0f};
    EXPECT_EQ(ResizeBandAt(atBottom, 200.0f, 1075.0f, 1920.0f, 1080.0f), ResizeHandle::S);
}

// The band is at the interface's scale.
TEST(ResizeBand, ScalesWithTheInterface) {
    SetUiScale(2.0f);
    const core::Rect r{100.0f, 100.0f, 200.0f, 100.0f};
    EXPECT_EQ(ResizeBandAt(r, 200.0f, 85.0f, 1920.0f, 1080.0f), ResizeHandle::N);
    EXPECT_FALSE(ResizeBandAt(r, 200.0f, 83.0f, 1920.0f, 1080.0f).has_value());
    SetUiScale(1.0f);
}

// One group alone has no divider.
TEST(SelectionLayout, OneGroupHasNoDivider) {
    SetUiScale(1.0f);
    const std::vector<ChromeButton> buttons = {ChromeButton::Pin, ChromeButton::Close};
    const BarLayout bar = LayoutBar(core::Rect{300.0f, 400.0f, 500.0f, 300.0f}, 1920.0f, 1080.0f, buttons);
    EXPECT_FALSE(BarDividerX(bar, buttons, 1.0f).has_value());
}

}  // namespace
}  // namespace sz::ui
