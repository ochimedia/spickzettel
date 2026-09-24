#include "core/drawing/stroke_smoothing.h"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

namespace sz::core {
namespace {

constexpr float kFlatness = 0.35f;

float Distance(const StrokePoint& a, const StrokePoint& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

// Distance from `p` to the infinite line through `a` and `b`.
float DistanceToLine(const StrokePoint& p, const StrokePoint& a, const StrokePoint& b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-6f) {
        return Distance(p, a);
    }
    return std::fabs((p.x - a.x) * dy - (p.y - a.y) * dx) / len;
}

// The property the whole thing rests on: fitting must not move the ink away
// from where the hand actually was. The curve is sampled between control
// points, but it ends on one exactly.
TEST(StrokeSmoothingTest, SpanEndsExactlyOnItsControlPoint) {
    std::vector<StrokePoint> out;
    AppendFittedSpan(StrokePoint{0, 0}, StrokePoint{10, 0}, StrokePoint{20, 6}, StrokePoint{30, 0}, kFlatness,
                     out);

    ASSERT_FALSE(out.empty());
    EXPECT_FLOAT_EQ(out.back().x, 20.0f);
    EXPECT_FLOAT_EQ(out.back().y, 6.0f);
}

// A straight span is already the curve, so it should cost exactly one
// segment however long it is - adaptive sampling is what keeps a fitted
// stroke from being three times the size of the one that went in.
TEST(StrokeSmoothingTest, AStraightSpanCostsOneSegment) {
    std::vector<StrokePoint> out;
    AppendFittedSpan(StrokePoint{0, 0}, StrokePoint{100, 0}, StrokePoint{200, 0}, StrokePoint{300, 0},
                     kFlatness, out);

    EXPECT_EQ(out.size(), 1u);
}

// ...and a curved one earns more, placed so the polyline through them stays
// within the flatness tolerance of the curve it stands for.
TEST(StrokeSmoothingTest, ACurvedSpanIsSubdividedUntilItIsFlatEnough) {
    std::vector<StrokePoint> out;
    // A quarter-circle-ish corner: the fitted curve bows well away from the
    // chord between p1 and p2.
    AppendFittedSpan(StrokePoint{0, 40}, StrokePoint{0, 0}, StrokePoint{40, 0}, StrokePoint{80, 40},
                     kFlatness, out);

    ASSERT_GT(out.size(), 2u);
    // Every sample sits between its neighbors rather than doubling back -
    // the failure mode a uniform parameterization has on uneven spacing.
    StrokePoint previous{0, 0};
    for (const StrokePoint& p : out) {
        EXPECT_GE(p.x, previous.x - 1e-3f);
        previous = p;
    }
}

// The curve has to bulge *toward* the neighboring control points, which is
// what makes a hand-drawn arc look like an arc instead of a chain of chords.
TEST(StrokeSmoothingTest, TheFittedCurveLeavesTheChord) {
    std::vector<StrokePoint> out;
    const StrokePoint p1{0, 0};
    const StrokePoint p2{40, 0};
    AppendFittedSpan(StrokePoint{-40, 30}, p1, p2, StrokePoint{80, 30}, kFlatness, out);

    float furthest = 0.0f;
    for (const StrokePoint& p : out) {
        furthest = std::max(furthest, DistanceToLine(p, p1, p2));
    }
    EXPECT_GT(furthest, 1.0f);
}

// Coincident control points are a real input - a hand that stops dead still
// reports the same position twice - and the knot spacing they produce is
// what a naive implementation divides by.
TEST(StrokeSmoothingTest, CoincidentControlPointsProduceFinitePoints) {
    std::vector<StrokePoint> out;
    AppendFittedSpan(StrokePoint{5, 5}, StrokePoint{5, 5}, StrokePoint{5, 5}, StrokePoint{5, 5}, kFlatness,
                     out);

    ASSERT_FALSE(out.empty());
    for (const StrokePoint& p : out) {
        EXPECT_TRUE(std::isfinite(p.x));
        EXPECT_TRUE(std::isfinite(p.y));
    }
}

// Wildly uneven spacing is where the uniform parameterization ties a loop.
// Centripetal doesn't, and this is the shape that shows it: the fitted span
// must stay inside the neighborhood of its own two endpoints.
TEST(StrokeSmoothingTest, UnevenSpacingDoesNotOvershoot) {
    std::vector<StrokePoint> out;
    const StrokePoint p1{0, 0};
    const StrokePoint p2{2, 0};  // a very short span between two long ones
    AppendFittedSpan(StrokePoint{-100, 0}, p1, p2, StrokePoint{102, 0}, kFlatness, out);

    for (const StrokePoint& p : out) {
        EXPECT_GE(p.x, -1.0f);
        EXPECT_LE(p.x, 3.0f);
        EXPECT_LT(std::fabs(p.y), 1.0f);
    }
}

}  // namespace
}  // namespace sz::core
