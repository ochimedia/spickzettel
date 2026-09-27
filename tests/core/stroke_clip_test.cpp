#include "core/drawing/stroke_clip.h"

#include <gtest/gtest.h>

#include <cmath>
#include <utility>

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
// existing vertex (see SegmentRectCrossings), so only the segments either
// side of it can split the run there. Without that the walk bridges the
// erased middle into one fragment spanning the whole polyline, which looks
// exactly like not having erased anything.
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

// A line that only touches the eraser - tangent to its circle - meets its
// boundary once and never goes in. Nothing is erased, and above all
// nothing is doubled: taking the touch for a way in once handed back the
// first half of the segment twice, which a translucent stroke showed as a
// darker stretch and every further touch added to.
TEST(StrokeClipTest, ATangentTouchLeavesTheStrokeAsItWas) {
    const Stroke across = MakeStroke({StrokePoint{-10, 1}, StrokePoint{10, 1}});
    EXPECT_FALSE(ClipStrokeOutsideCircle(across, StrokePoint{0, 0}, 1.0f).has_value());
    const Stroke back = MakeStroke({StrokePoint{10, 1}, StrokePoint{-10, 1}});
    EXPECT_FALSE(ClipStrokeOutsideCircle(back, StrokePoint{0, 0}, 1.0f).has_value());
    const Stroke polyline = MakeStroke({StrokePoint{-10, 1}, StrokePoint{10, 1}, StrokePoint{10, 20}});
    EXPECT_FALSE(ClipStrokeOutsideCircle(polyline, StrokePoint{0, 0}, 1.0f).has_value());
}

// The same through a corner of the rectangle eraser, where the way in and
// the way out are one point.
TEST(StrokeClipTest, ALineThroughARectanglesCornerOnlyLeavesTheStrokeAsItWas) {
    const Stroke corner = MakeStroke({StrokePoint{0, 10}, StrokePoint{10, 0}});
    EXPECT_FALSE(ClipStrokeOutsideRect(corner, -5, -5, 5, 5).has_value());
    const Stroke back = MakeStroke({StrokePoint{10, 0}, StrokePoint{0, 10}});
    EXPECT_FALSE(ClipStrokeOutsideRect(back, -5, -5, 5, 5).has_value());
}

// A real bite taken twice over the same place takes nothing the second
// time: what is left lies outside the eraser, touching it at most.
TEST(StrokeClipTest, ErasingTheSamePlaceAgainChangesNothing) {
    const Stroke stroke = MakeStroke({StrokePoint{-20, 0}, StrokePoint{20, 0}, StrokePoint{20, 20}});
    const auto first = ClipStrokeOutsideCircle(stroke, StrokePoint{0, 0}, 5.0f);
    ASSERT_TRUE(first.has_value());
    ASSERT_EQ(first->size(), 2u);
    for (const Stroke& fragment : *first) {
        EXPECT_FALSE(ClipStrokeOutsideCircle(fragment, StrokePoint{0, 0}, 5.0f).has_value());
    }
}

// ===== The capsule: the eraser's pass between two positions =====

// The two cut points of a stroke split in two, as (end of the first
// fragment, start of the second) along `coordinate`.
std::pair<float, float> Gap(const std::optional<std::vector<Stroke>>& clipped, float StrokePoint::* coordinate) {
    EXPECT_TRUE(clipped.has_value());
    if (!clipped.has_value() || clipped->size() != 2u) {
        ADD_FAILURE() << "not split in two";
        return {0.0f, 0.0f};
    }
    return {(*clipped)[0].points.back().*coordinate, (*clipped)[1].points.front().*coordinate};
}

// Through the band between the ends, across it: cut where it enters and
// leaves, a radius either side of the pass.
TEST(StrokeClipTest, CapsuleCutsALineCrossingItsBand) {
    const Stroke stroke = MakeStroke({StrokePoint{10, -20}, StrokePoint{10, 20}});
    const auto [end, start] = Gap(ClipStrokeOutsideCapsule(stroke, {0, 0}, {20, 0}, 5.0f), &StrokePoint::y);
    EXPECT_NEAR(end, -5.0f, 1e-3f);
    EXPECT_NEAR(start, 5.0f, 1e-3f);
}

// Past the pass's end, where only the round cap reaches.
TEST(StrokeClipTest, CapsuleCutsALineThroughItsRoundEnd) {
    const Stroke stroke = MakeStroke({StrokePoint{23, -20}, StrokePoint{23, 20}});
    const auto [end, start] = Gap(ClipStrokeOutsideCapsule(stroke, {0, 0}, {20, 0}, 5.0f), &StrokePoint::y);
    EXPECT_NEAR(end, -4.0f, 1e-3f);
    EXPECT_NEAR(start, 4.0f, 1e-3f);
}

// Along the pass, off its middle: in through one cap, along the band and
// out through the other - one cut each side.
TEST(StrokeClipTest, CapsuleCutsALineRunningAlongIt) {
    const Stroke stroke = MakeStroke({StrokePoint{-20, 3}, StrokePoint{40, 3}});
    const auto [end, start] = Gap(ClipStrokeOutsideCapsule(stroke, {0, 0}, {20, 0}, 5.0f), &StrokePoint::x);
    EXPECT_NEAR(end, -4.0f, 1e-3f);
    EXPECT_NEAR(start, 24.0f, 1e-3f);
}

// A slanted pass across a level line: cut where the line is within the
// radius of the pass - here |2x| / sqrt(5) <= 2.
TEST(StrokeClipTest, CapsuleCutsALineASlantedPassCrosses) {
    const Stroke stroke = MakeStroke({StrokePoint{-50, 0}, StrokePoint{50, 0}});
    const auto [end, start] = Gap(ClipStrokeOutsideCapsule(stroke, {-10, -20}, {10, 20}, 2.0f), &StrokePoint::x);
    EXPECT_NEAR(end, -std::sqrt(5.0f), 1e-3f);
    EXPECT_NEAR(start, std::sqrt(5.0f), 1e-3f);
}

// Beside the pass, farther than the radius, and beyond its ends: untouched.
TEST(StrokeClipTest, CapsuleLeavesWhatItDidNotReach) {
    const auto untouched = [](std::vector<StrokePoint> points) {
        return !ClipStrokeOutsideCapsule(MakeStroke(std::move(points)), {0, 0}, {20, 0}, 5.0f).has_value();
    };
    EXPECT_TRUE(untouched({StrokePoint{-20, 6}, StrokePoint{40, 6}}));
    EXPECT_TRUE(untouched({StrokePoint{26, -20}, StrokePoint{26, 20}}));
    EXPECT_TRUE(untouched({StrokePoint{30, 0}}));
}

// A whole stroke inside the pass goes; a dot on it goes whole.
TEST(StrokeClipTest, CapsuleTakesAllThatLiesInside) {
    const auto line =
        ClipStrokeOutsideCapsule(MakeStroke({StrokePoint{2, 1}, StrokePoint{18, -1}}), {0, 0}, {20, 0}, 5.0f);
    ASSERT_TRUE(line.has_value());
    EXPECT_TRUE(line->empty());
    const auto dot = ClipStrokeOutsideCapsule(MakeStroke({StrokePoint{10, 4}}), {0, 0}, {20, 0}, 5.0f);
    ASSERT_TRUE(dot.has_value());
    EXPECT_TRUE(dot->empty());
}

// No pass at all - the two positions one - is the circle.
TEST(StrokeClipTest, CapsuleOfOnePointIsTheCircle) {
    const Stroke stroke = MakeStroke({StrokePoint{-20, 0}, StrokePoint{20, 0}});
    EXPECT_EQ(ClipStrokeOutsideCapsule(stroke, {0, 0}, {0, 0}, 5.0f), ClipStrokeOutsideCircle(stroke, {0, 0}, 5.0f));
}

}  // namespace
}  // namespace sz::core
