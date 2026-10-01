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

// One group alone has no divider.
TEST(SelectionLayout, OneGroupHasNoDivider) {
    SetUiScale(1.0f);
    const std::vector<ChromeButton> buttons = {ChromeButton::Pin, ChromeButton::Close};
    const BarLayout bar = LayoutBar(core::Rect{300.0f, 400.0f, 500.0f, 300.0f}, 1920.0f, 1080.0f, buttons);
    EXPECT_FALSE(BarDividerX(bar, buttons, 1.0f).has_value());
}

}  // namespace
}  // namespace sz::ui
