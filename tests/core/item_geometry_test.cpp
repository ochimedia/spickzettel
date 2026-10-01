#include "core/canvas/item_geometry.h"

#include <cmath>

#include <gtest/gtest.h>

namespace sz::core {
namespace {

// Shrinks `rect` from its bottom-right corner by repeatedly applying a
// large inward drag, the way holding the mouse down and dragging well
// past the floor does - each call is one frame's delta (OverlayApp resets
// ImGui's drag delta every frame after applying it).
// `steps` is deliberately more than enough to bottom out on either axis
// from a 4K-ish starting size (60 x 40px = 2400px of shrink) - the point
// is where it *stops*, not how long it takes to get there.
Rect ShrinkFromBottomRight(Rect rect, bool lockAspect, int steps = 60) {
    for (int i = 0; i < steps; ++i) {
        ApplyResizeHandleDelta(rect, /*movesLeft=*/false, /*movesRight=*/true, /*movesTop=*/false,
                                /*movesBottom=*/true, /*dx=*/-40.0f, /*dy=*/-40.0f, lockAspect);
    }
    return rect;
}

// Two independent per-axis floors (16x16, a fixed square) applied after
// the aspect-ratio math reshape anything that isn't square: a 16:9 snippet
// hits the height floor at 28x16 and then squashes the rest of the way to
// 16x16. The floor has to be one floor on the item's own shape.
TEST(ItemGeometryTest, AspectLockedShrinkKeepsTheRatioAllTheWayToTheFloor) {
    const Rect wide{100.0f, 100.0f, 1920.0f, 1080.0f};
    const float ratio = wide.w / wide.h;

    const Rect shrunk = ShrinkFromBottomRight(wide, /*lockAspect=*/true);

    EXPECT_NEAR(shrunk.w / shrunk.h, ratio, 0.01f);
    // ...and it does stop: at the point where the *first* of the two
    // floors is reached, not past it.
    EXPECT_GE(shrunk.w, kItemMinWidth);
    EXPECT_GE(shrunk.h, kItemMinHeight);
    EXPECT_FLOAT_EQ(shrunk.h, kItemMinHeight);  // 16:9 is wider than square, so height is the binding floor
}

TEST(ItemGeometryTest, AspectLockedShrinkKeepsTheRatioForATallItemToo) {
    const Rect tall{100.0f, 100.0f, 200.0f, 1200.0f};
    const float ratio = tall.w / tall.h;

    const Rect shrunk = ShrinkFromBottomRight(tall, /*lockAspect=*/true);

    EXPECT_NEAR(shrunk.w / shrunk.h, ratio, 0.01f);
    EXPECT_GE(shrunk.w, kItemMinWidth);
    EXPECT_GE(shrunk.h, kItemMinHeight);
    EXPECT_FLOAT_EQ(shrunk.w, kItemMinWidth);  // taller than square, so width is the binding floor
}

// The escape hatch is meant to reshape, so there the two plain per-axis
// floors are exactly right - an unlocked drag can still squash anything
// down to 16x16.
TEST(ItemGeometryTest, FreeResizeStillBottomsOutAtThePlainPerAxisFloor) {
    const Rect wide{100.0f, 100.0f, 1920.0f, 1080.0f};

    const Rect shrunk = ShrinkFromBottomRight(wide, /*lockAspect=*/false);

    EXPECT_FLOAT_EQ(shrunk.w, kItemMinWidth);
    EXPECT_FLOAT_EQ(shrunk.h, kItemMinHeight);
}

// The size has to follow the pointer without jumping. A corner drag whose
// two axes disagree about which way they are going used to flip between
// "the width drives" and "the height drives" the moment one overtook the
// other, and the two answers are nothing like each other: a 400x300
// snippet went from 440x330 to 360x270 on a ten-thousandth of a pixel.
// Swept across that crossover in both of the directions that cross the
// diagonal, the step per pixel of pointer movement has to stay small.
TEST(ItemGeometryTest, AnAspectLockedCornerDragNeverJumps) {
    const Rect start{100.0f, 100.0f, 400.0f, 300.0f};
    // One axis held still while the other is swept past it, so the sweep
    // crosses the line where the two changed places rather than running
    // along it. Both signs: the crossing is at dy = -30 for a 40px pull
    // to the right, and at dy = +30 for one to the left.
    for (const float dx : {40.0f, -40.0f}) {
        float previous = -1.0f;
        for (int step = -320; step <= 320; ++step) {
            const float dy = static_cast<float>(step) * 0.25f;
            Rect rect = start;
            ApplyResizeHandleDelta(rect, /*movesLeft=*/false, /*movesRight=*/true, /*movesTop=*/false,
                                    /*movesBottom=*/true, dx, dy, /*lockAspect=*/true);
            EXPECT_NEAR(rect.w / rect.h, start.w / start.h, 0.01f) << "the shape is kept throughout";
            if (previous >= 0.0f) {
                EXPECT_LT(std::fabs(rect.w - previous), 1.0f)
                    << "a quarter-pixel of pointer movement, at dx " << dx << " dy " << dy;
            }
            previous = rect.w;
        }
    }
}

// Along the diagonal the projection has to agree with the obvious answer:
// dragging the corner out by a tenth of the snippet makes it a tenth
// bigger, which is what the old rule did on this path too.
TEST(ItemGeometryTest, AnAspectLockedCornerFollowsTheDiagonalExactly) {
    Rect rect{100.0f, 100.0f, 400.0f, 300.0f};
    ApplyResizeHandleDelta(rect, /*movesLeft=*/false, /*movesRight=*/true, /*movesTop=*/false,
                            /*movesBottom=*/true, /*dx=*/40.0f, /*dy=*/30.0f, /*lockAspect=*/true);
    EXPECT_NEAR(rect.w, 440.0f, 0.01f);
    EXPECT_NEAR(rect.h, 330.0f, 0.01f);
}

// An edge handle drives one axis and derives the other; it must land on
// the same shape-preserving floor as a corner does.
TEST(ItemGeometryTest, AspectLockedEdgeDragKeepsTheRatioToTheFloor) {
    Rect wide{100.0f, 100.0f, 1920.0f, 1080.0f};
    const float ratio = wide.w / wide.h;
    for (int i = 0; i < 40; ++i) {
        ApplyResizeHandleDelta(wide, /*movesLeft=*/false, /*movesRight=*/true, /*movesTop=*/false,
                                /*movesBottom=*/false, /*dx=*/-60.0f, /*dy=*/0.0f, /*lockAspect=*/true);
    }
    EXPECT_NEAR(wide.w / wide.h, ratio, 0.01f);
    EXPECT_GE(wide.h, kItemMinHeight);
}

TEST(ItemGeometryTest, MinimumSizeForAspectRatioClearsBothFloorsWithoutReshaping) {
    // Exactly the floor's own shape - neither axis needs growing.
    const MinItemSize square = MinimumSizeForAspectRatio(kItemMinWidth / kItemMinHeight);
    EXPECT_FLOAT_EQ(square.w, kItemMinWidth);
    EXPECT_FLOAT_EQ(square.h, kItemMinHeight);

    // Wider than the floor: height binds, width has to come up with it.
    const MinItemSize wide = MinimumSizeForAspectRatio(16.0f / 9.0f);
    EXPECT_FLOAT_EQ(wide.h, kItemMinHeight);
    EXPECT_NEAR(wide.w / wide.h, 16.0f / 9.0f, 0.001f);
    EXPECT_GT(wide.w, kItemMinWidth);

    // Taller than the floor: width binds instead.
    const MinItemSize tall = MinimumSizeForAspectRatio(1.0f / 6.0f);
    EXPECT_FLOAT_EQ(tall.w, kItemMinWidth);
    EXPECT_NEAR(tall.w / tall.h, 1.0f / 6.0f, 0.001f);
    EXPECT_GT(tall.h, kItemMinHeight);

    // A degenerate ratio has no shape to preserve - fall back to the
    // plain floor rather than dividing by it.
    const MinItemSize degenerate = MinimumSizeForAspectRatio(0.0f);
    EXPECT_FLOAT_EQ(degenerate.w, kItemMinWidth);
    EXPECT_FLOAT_EQ(degenerate.h, kItemMinHeight);
}

TEST(ItemGeometryTest, GrowRectToMinimumSizeLeavesAnAlreadyBigEnoughRectAlone) {
    const Rect big{10.0f, 20.0f, 800.0f, 450.0f};
    const Rect floored = GrowRectToMinimumSize(big);
    EXPECT_EQ(floored, big);
}

TEST(ItemGeometryTest, GrowRectToMinimumSizeKeepsAspectRatio) {
    // The shape a 4K screenshot ends up as after a big enough downscale -
    // RescaleRectForDisplaySize keeps its ratio, and the floor applied
    // afterwards must not immediately undo that.
    const Rect squashed{0.0f, 0.0f, 60.0f, 33.75f};  // 16:9, below both floors
    const Rect floored = GrowRectToMinimumSize(squashed);

    EXPECT_NEAR(floored.w / floored.h, 16.0f / 9.0f, 0.001f);
    EXPECT_GE(floored.w, kItemMinWidth);
    EXPECT_GE(floored.h, kItemMinHeight);
    EXPECT_FLOAT_EQ(floored.x, squashed.x);  // grows from its top-left, doesn't move
    EXPECT_FLOAT_EQ(floored.y, squashed.y);
}

TEST(ItemGeometryTest, GrowRectToMinimumSizeFallsBackToThePlainFloorForADegenerateRect) {
    const Rect empty{5.0f, 5.0f, 0.0f, 0.0f};
    const Rect floored = GrowRectToMinimumSize(empty);
    EXPECT_FLOAT_EQ(floored.w, kItemMinWidth);
    EXPECT_FLOAT_EQ(floored.h, kItemMinHeight);
}

TEST(ItemGeometryTest, ScreenToNativeIsATranslationWhenTheItemIsShownAtNativeSize) {
    Item item;
    item.rect = Rect{100.0f, 50.0f, 400.0f, 300.0f};
    item.nativeW = 400.0f;
    item.nativeH = 300.0f;
    const NativePoint p = ScreenToNative(item, 130.0f, 80.0f);
    EXPECT_FLOAT_EQ(p.x, 30.0f);
    EXPECT_FLOAT_EQ(p.y, 30.0f);
    EXPECT_FLOAT_EQ(p.scale, 1.0f);
}

TEST(ItemGeometryTest, RectsOverlapOnlyWhereTheyShareArea) {
    const Rect square{0.0f, 0.0f, 100.0f, 100.0f};
    EXPECT_TRUE(RectsOverlap(square, Rect{50.0f, 50.0f, 100.0f, 100.0f}));
    EXPECT_TRUE(RectsOverlap(square, Rect{10.0f, 10.0f, 10.0f, 10.0f})) << "wholly inside counts";
    EXPECT_FALSE(RectsOverlap(square, Rect{200.0f, 0.0f, 50.0f, 50.0f}));
    // Edge to edge, and corner to corner: nothing of either is hidden by
    // the other, and a box drawn along the seam has caught neither.
    EXPECT_FALSE(RectsOverlap(square, Rect{100.0f, 0.0f, 50.0f, 50.0f}));
    EXPECT_FALSE(RectsOverlap(square, Rect{100.0f, 100.0f, 50.0f, 50.0f}));
    EXPECT_FALSE(RectsOverlap(square, Rect{50.0f, 50.0f, 0.0f, 0.0f})) << "a rect with no area touches nothing";
}

TEST(ItemGeometryTest, ScreenToNativeScalesEachAxisAndTakesTheirGeometricMeanForLengths) {
    // Shown at half its native width and a quarter of its native height.
    Item item;
    item.rect = Rect{0.0f, 0.0f, 200.0f, 75.0f};
    item.nativeW = 400.0f;
    item.nativeH = 300.0f;
    const NativePoint p = ScreenToNative(item, 100.0f, 30.0f);
    EXPECT_FLOAT_EQ(p.x, 200.0f);
    EXPECT_FLOAT_EQ(p.y, 120.0f);
    // sqrt(2 * 4): what a width or radius is multiplied by - and back out,
    // sqrt(1/2 * 1/4), which undoes it exactly (see LengthScale).
    EXPECT_FLOAT_EQ(p.scale, std::sqrt(8.0f));
    EXPECT_FLOAT_EQ(p.scale * LengthScale(0.5f, 0.25f), 1.0f);
}

TEST(ItemGeometryTest, ScreenToNativeTreatsAZeroSizedRectAsUnscaled) {
    Item item;
    item.rect = Rect{10.0f, 10.0f, 0.0f, 0.0f};
    item.nativeW = 400.0f;
    item.nativeH = 300.0f;
    const NativePoint p = ScreenToNative(item, 15.0f, 12.0f);
    EXPECT_FLOAT_EQ(p.x, 5.0f);
    EXPECT_FLOAT_EQ(p.y, 2.0f);
    EXPECT_FLOAT_EQ(p.scale, 1.0f);
}

}  // namespace
}  // namespace sz::core
