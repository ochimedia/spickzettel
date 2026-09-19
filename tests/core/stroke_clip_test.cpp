#include "core/drawing/stroke_clip.h"

#include <gtest/gtest.h>

namespace sz::core {
namespace {

Stroke MakeStroke(std::vector<StrokePoint> points, uint32_t colorRGBA = 0xAABBCCDDu, float width = 7.5f) {
    Stroke s;
    s.points = std::move(points);
    s.colorRGBA = colorRGBA;
    s.width = width;
    return s;
}

// A single-point stroke is a dot (see CanvasState::EndStroke), so it is
// all or nothing: erased whole when the circle contains it.
TEST(StrokeClipTest, DotInsideTheCircleIsErasedWhole) {
    const Stroke dot = MakeStroke({StrokePoint{0, 0}});
    const std::optional<std::vector<Stroke>> clipped =
        ClipStrokeOutsideCircle(dot, StrokePoint{0, 0}, 100.0f);
    ASSERT_TRUE(clipped.has_value());
    EXPECT_TRUE(clipped->empty());
}

TEST(StrokeClipTest, DotOutsideTheCircleIsUntouched) {
    const Stroke dot = MakeStroke({StrokePoint{500, 500}});
    EXPECT_FALSE(ClipStrokeOutsideCircle(dot, StrokePoint{0, 0}, 100.0f).has_value());
}

TEST(StrokeClipTest, EmptyStrokeReturnsNullopt) {
    const Stroke nothing = MakeStroke({});
    EXPECT_FALSE(ClipStrokeOutsideCircle(nothing, StrokePoint{0, 0}, 100.0f).has_value());
}

TEST(StrokeClipTest, EntirelyOutsideReturnsNullopt) {
    const Stroke stroke = MakeStroke({StrokePoint{100, 100}, StrokePoint{110, 110}});
    EXPECT_FALSE(ClipStrokeOutsideCircle(stroke, StrokePoint{0, 0}, 5.0f).has_value());
}

TEST(StrokeClipTest, EntirelyOutsideMultiPointFreehandReturnsNullopt) {
    const Stroke stroke = MakeStroke({StrokePoint{100, 0}, StrokePoint{105, 5}, StrokePoint{110, 0}});
    EXPECT_FALSE(ClipStrokeOutsideCircle(stroke, StrokePoint{0, 0}, 5.0f).has_value());
}

TEST(StrokeClipTest, EntirelyInsideReturnsPresentEmptyVector) {
    const Stroke stroke = MakeStroke({StrokePoint{0, 0}, StrokePoint{2, 2}});
    const auto result = ClipStrokeOutsideCircle(stroke, StrokePoint{0, 0}, 10.0f);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
}

TEST(StrokeClipTest, EntirelyInsideMultiPointFreehandReturnsPresentEmptyVector) {
    const Stroke stroke = MakeStroke({StrokePoint{0, 0}, StrokePoint{1, 1}, StrokePoint{2, 0}, StrokePoint{1, -1}});
    const auto result = ClipStrokeOutsideCircle(stroke, StrokePoint{1, 0}, 10.0f);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
}

// One endpoint inside the eraser, one outside - the stroke shortens to the
// surviving half, cut cleanly at the circle boundary, rather than vanishing.
TEST(StrokeClipTest, OneEndpointInsideShortensToSurvivingHalf) {
    const Stroke stroke = MakeStroke({StrokePoint{0, 0}, StrokePoint{20, 0}});
    const auto result = ClipStrokeOutsideCircle(stroke, StrokePoint{0, 0}, 5.0f);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1u);
    const Stroke& fragment = (*result)[0];
    ASSERT_EQ(fragment.points.size(), 2u);
    EXPECT_NEAR(fragment.points[0].x, 5.0f, 1e-3f);   // circle boundary
    EXPECT_NEAR(fragment.points[0].y, 0.0f, 1e-3f);
    EXPECT_NEAR(fragment.points[1].x, 20.0f, 1e-3f);  // untouched original endpoint
    EXPECT_NEAR(fragment.points[1].y, 0.0f, 1e-3f);
}

// Both endpoints are outside the eraser circle, but the straight segment
// between them passes through it - a long, sparsely sampled segment, the
// exact shape of a line's or a rectangle's edge, which a per-point test
// never sees. It splits into two fragments, one each side of the gap.
TEST(StrokeClipTest, MidSegmentDipBothEndpointsOutsideSplitsIntoTwoFragments) {
    const Stroke stroke = MakeStroke({StrokePoint{-20, 0}, StrokePoint{20, 0}});
    const auto result = ClipStrokeOutsideCircle(stroke, StrokePoint{0, 0}, 5.0f);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 2u);

    const Stroke& first = (*result)[0];
    ASSERT_EQ(first.points.size(), 2u);
    EXPECT_NEAR(first.points[0].x, -20.0f, 1e-3f);
    EXPECT_NEAR(first.points[1].x, -5.0f, 1e-3f);

    const Stroke& second = (*result)[1];
    ASSERT_EQ(second.points.size(), 2u);
    EXPECT_NEAR(second.points[0].x, 5.0f, 1e-3f);
    EXPECT_NEAR(second.points[1].x, 20.0f, 1e-3f);
}

// A rectangle's polyline (its 5 corner points, straight edges implied)
// erased squarely in the middle of one edge, nowhere near a vertex, takes a
// clean circular bite out of that edge, splitting the outline into two open
// fragments.
TEST(StrokeClipTest, RectangleEdgeMidSegmentBiteDoesNotRequireTouchingAVertex) {
    const Stroke rectangle = MakeStroke({
        StrokePoint{0, 0},
        StrokePoint{100, 0},
        StrokePoint{100, 50},
        StrokePoint{0, 50},
        StrokePoint{0, 0},
    });
    // Centered on the midpoint of the top edge (50, 0), far from every
    // corner (nearest corner is 50 units away; radius is only 10).
    const auto result = ClipStrokeOutsideCircle(rectangle, StrokePoint{50, 0}, 10.0f);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 2u);

    const Stroke& bittenEdgeStart = (*result)[0];
    ASSERT_EQ(bittenEdgeStart.points.size(), 2u);
    EXPECT_NEAR(bittenEdgeStart.points[0].x, 0.0f, 1e-3f);
    EXPECT_NEAR(bittenEdgeStart.points[1].x, 40.0f, 1e-3f);

    // The rest of the rectangle's outline survives as one continuous
    // fragment, unaffected past the bite.
    const Stroke& rest = (*result)[1];
    ASSERT_EQ(rest.points.size(), 5u);
    EXPECT_NEAR(rest.points[0].x, 60.0f, 1e-3f);
    EXPECT_NEAR(rest.points[1].x, 100.0f, 1e-3f);
    EXPECT_NEAR(rest.points[1].y, 0.0f, 1e-3f);
    EXPECT_NEAR(rest.points[2].x, 100.0f, 1e-3f);
    EXPECT_NEAR(rest.points[2].y, 50.0f, 1e-3f);
    EXPECT_NEAR(rest.points[4].x, 0.0f, 1e-3f);
    EXPECT_NEAR(rest.points[4].y, 0.0f, 1e-3f);
}

TEST(StrokeClipTest, DuplicateConsecutivePointsDoNotCrashAndAreHandledGracefully) {
    const Stroke stroke = MakeStroke({StrokePoint{0, 0}, StrokePoint{0, 0}, StrokePoint{20, 0}});
    const auto result = ClipStrokeOutsideCircle(stroke, StrokePoint{0, 0}, 5.0f);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1u);
    ASSERT_EQ((*result)[0].points.size(), 2u);
    EXPECT_NEAR((*result)[0].points[0].x, 5.0f, 1e-3f);
    EXPECT_NEAR((*result)[0].points[1].x, 20.0f, 1e-3f);
}

TEST(StrokeClipTest, FragmentsPreserveColorAndWidth) {
    const Stroke stroke = MakeStroke({StrokePoint{-20, 0}, StrokePoint{20, 0}}, 0x11223344u, 12.5f);
    const auto result = ClipStrokeOutsideCircle(stroke, StrokePoint{0, 0}, 5.0f);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 2u);
    for (const Stroke& fragment : *result) {
        EXPECT_EQ(fragment.colorRGBA, 0x11223344u);
        EXPECT_FLOAT_EQ(fragment.width, 12.5f);
    }
}

// ================= ClipStrokeOutsideRect (rectangular eraser) =================
// Same contract, same shared walk (see ClipStrokeOutsideRegion in
// stroke_clip.cpp) as the circle tests above - these mirror that
// coverage against a rectangle instead.

// Same all-or-nothing rule as the circle above - the two regions must
// never disagree about what erasing means.
TEST(StrokeClipTest, RectDotInsideIsErasedWholeAndOutsideIsUntouched) {
    const Stroke inside = MakeStroke({StrokePoint{0, 0}});
    const std::optional<std::vector<Stroke>> clipped = ClipStrokeOutsideRect(inside, -5, -5, 5, 5);
    ASSERT_TRUE(clipped.has_value());
    EXPECT_TRUE(clipped->empty());

    const Stroke outside = MakeStroke({StrokePoint{50, 50}});
    EXPECT_FALSE(ClipStrokeOutsideRect(outside, -5, -5, 5, 5).has_value());
}

TEST(StrokeClipTest, RectEntirelyOutsideReturnsNullopt) {
    const Stroke stroke = MakeStroke({StrokePoint{100, 100}, StrokePoint{110, 110}});
    EXPECT_FALSE(ClipStrokeOutsideRect(stroke, -5, -5, 5, 5).has_value());
}

TEST(StrokeClipTest, RectEntirelyInsideReturnsPresentEmptyVector) {
    const Stroke stroke = MakeStroke({StrokePoint{-2, -2}, StrokePoint{2, 2}});
    const auto result = ClipStrokeOutsideRect(stroke, -5, -5, 5, 5);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
}

// One endpoint inside the eraser rect, one outside - shortens to just the
// surviving (outside) half, cut cleanly at the rect boundary.
TEST(StrokeClipTest, RectOneEndpointInsideShortensToSurvivingHalf) {
    const Stroke stroke = MakeStroke({StrokePoint{0, 0}, StrokePoint{20, 0}});
    const auto result = ClipStrokeOutsideRect(stroke, -5, -5, 5, 5);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1u);
    const Stroke& fragment = (*result)[0];
    ASSERT_EQ(fragment.points.size(), 2u);
    EXPECT_NEAR(fragment.points[0].x, 5.0f, 1e-3f);   // rect's right edge
    EXPECT_NEAR(fragment.points[0].y, 0.0f, 1e-3f);
    EXPECT_NEAR(fragment.points[1].x, 20.0f, 1e-3f);  // untouched original endpoint
    EXPECT_NEAR(fragment.points[1].y, 0.0f, 1e-3f);
}

// Both endpoints outside the eraser rect, but the segment passes straight
// through it - splits into two fragments, one on each side of the erased
// gap (the same case a naive per-point/per-vertex test would miss
// entirely for a long, sparsely-sampled segment).
TEST(StrokeClipTest, RectMidSegmentDipBothEndpointsOutsideSplitsIntoTwoFragments) {
    const Stroke stroke = MakeStroke({StrokePoint{-20, 0}, StrokePoint{20, 0}});
    const auto result = ClipStrokeOutsideRect(stroke, -5, -5, 5, 5);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 2u);

    const Stroke& first = (*result)[0];
    ASSERT_EQ(first.points.size(), 2u);
    EXPECT_NEAR(first.points[0].x, -20.0f, 1e-3f);
    EXPECT_NEAR(first.points[1].x, -5.0f, 1e-3f);

    const Stroke& second = (*result)[1];
    ASSERT_EQ(second.points.size(), 2u);
    EXPECT_NEAR(second.points[0].x, 5.0f, 1e-3f);
    EXPECT_NEAR(second.points[1].x, 20.0f, 1e-3f);
}

// Same shape as the mid-segment-dip case above, but crossing the rect's
// top/bottom edges instead of its left/right ones - makes sure the y-axis
// half-planes are exercised too, not just x.
TEST(StrokeClipTest, RectMidSegmentDipThroughTopAndBottomEdges) {
    const Stroke stroke = MakeStroke({StrokePoint{0, -20}, StrokePoint{0, 20}});
    const auto result = ClipStrokeOutsideRect(stroke, -5, -5, 5, 5);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 2u);

    const Stroke& first = (*result)[0];
    ASSERT_EQ(first.points.size(), 2u);
    EXPECT_NEAR(first.points[0].y, -20.0f, 1e-3f);
    EXPECT_NEAR(first.points[1].y, -5.0f, 1e-3f);

    const Stroke& second = (*result)[1];
    ASSERT_EQ(second.points.size(), 2u);
    EXPECT_NEAR(second.points[0].y, 5.0f, 1e-3f);
    EXPECT_NEAR(second.points[1].y, 20.0f, 1e-3f);
}

// A multi-point polyline with two sample points exactly on the rect's
// boundary: the crossing finder deliberately reports no crossing at an
// existing vertex (see SegmentRectCrossings), so only the state comparison
// against the pointwise test can split the run there. Without it the walk
// bridges the erased middle into one fragment spanning the whole polyline,
// which looks exactly like not having erased anything.
TEST(StrokeClipTest, RectMultiPointPolylineWithVerticesExactlyOnBoundarySplitsCorrectly) {
    const Stroke stroke = MakeStroke({
        StrokePoint{-15, 0},
        StrokePoint{-10, 0},
        StrokePoint{-5, 0},  // exactly on the rect's left edge
        StrokePoint{0, 0},
        StrokePoint{5, 0},  // exactly on the rect's right edge
        StrokePoint{10, 0},
        StrokePoint{15, 0},
    });
    const auto result = ClipStrokeOutsideRect(stroke, -5, -5, 5, 5);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 2u);

    const Stroke& first = (*result)[0];
    ASSERT_EQ(first.points.size(), 3u);
    EXPECT_NEAR(first.points[0].x, -15.0f, 1e-3f);
    EXPECT_NEAR(first.points[1].x, -10.0f, 1e-3f);
    EXPECT_NEAR(first.points[2].x, -5.0f, 1e-3f);

    const Stroke& second = (*result)[1];
    ASSERT_EQ(second.points.size(), 3u);
    EXPECT_NEAR(second.points[0].x, 5.0f, 1e-3f);
    EXPECT_NEAR(second.points[1].x, 10.0f, 1e-3f);
    EXPECT_NEAR(second.points[2].x, 15.0f, 1e-3f);
}

TEST(StrokeClipTest, RectFragmentsPreserveColorAndWidth) {
    const Stroke stroke = MakeStroke({StrokePoint{-20, 0}, StrokePoint{20, 0}}, 0x11223344u, 12.5f);
    const auto result = ClipStrokeOutsideRect(stroke, -5, -5, 5, 5);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 2u);
    for (const Stroke& fragment : *result) {
        EXPECT_EQ(fragment.colorRGBA, 0x11223344u);
        EXPECT_FLOAT_EQ(fragment.width, 12.5f);
    }
}

}  // namespace
}  // namespace sz::core
