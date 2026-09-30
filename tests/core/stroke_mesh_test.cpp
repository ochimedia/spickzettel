#include "core/drawing/stroke_mesh.h"

#include <cmath>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

namespace sz::core {
namespace {

constexpr float kFringe = 1.0f;
// How far a round pen's corner may reach past its outline - see
// kRoundCornerTolerancePx - and float noise on top.
constexpr float kCornerTolerance = 0.25f + 1e-3f;

float DistanceToSegment(float px, float py, const StrokePoint& a, const StrokePoint& b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float lengthSq = dx * dx + dy * dy;
    float t = 0.0f;
    if (lengthSq > 1e-9f) {
        t = ((px - a.x) * dx + (py - a.y) * dy) / lengthSq;
        t = std::max(0.0f, std::min(1.0f, t));
    }
    const float cx = a.x + dx * t;
    const float cy = a.y + dy * t;
    return std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
}

// How far the furthest vertex strays from the centerline it was built
// around - the direct measure of a spike.
float FurthestFromCenterline(const StrokeMesh& mesh, const std::vector<StrokePoint>& centerline) {
    float furthest = 0.0f;
    for (const StrokeVertex& v : mesh.vertices) {
        float nearest = std::numeric_limits<float>::max();
        for (size_t i = 0; i + 1 < centerline.size(); ++i) {
            nearest = std::min(nearest, DistanceToSegment(v.x, v.y, centerline[i], centerline[i + 1]));
        }
        if (centerline.size() == 1) {
            nearest = std::sqrt((v.x - centerline[0].x) * (v.x - centerline[0].x) +
                                 (v.y - centerline[0].y) * (v.y - centerline[0].y));
        }
        furthest = std::max(furthest, nearest);
    }
    return furthest;
}

bool AllFinite(const StrokeMesh& mesh) {
    for (const StrokeVertex& v : mesh.vertices) {
        if (!std::isfinite(v.x) || !std::isfinite(v.y)) {
            return false;
        }
    }
    return true;
}

// Total area of the triangles that are fully solid. Equal to the area of
// the shape only if no triangle is laid over another - which is the property
// a translucent stroke depends on, and the one that cannot be seen in a
// screenshot because a double-covered pixel just looks like a slightly
// darker pixel.
float SolidArea(const StrokeMesh& mesh) {
    float total = 0.0f;
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const StrokeVertex& a = mesh.vertices[mesh.indices[i]];
        const StrokeVertex& b = mesh.vertices[mesh.indices[i + 1]];
        const StrokeVertex& c = mesh.vertices[mesh.indices[i + 2]];
        if (a.coverage < 1.0f || b.coverage < 1.0f || c.coverage < 1.0f) {
            continue;
        }
        total += std::fabs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) * 0.5f;
    }
    return total;
}

TEST(StrokeMeshTest, EmptyInputProducesNoGeometry) {
    const StrokeMesh mesh = BuildStrokeMesh({}, 5.0f, kFringe, StrokeCorners::Round);
    EXPECT_TRUE(mesh.vertices.empty());
    EXPECT_TRUE(mesh.indices.empty());
}

// A dot is a disc, so every vertex sits at the pen's radius from the point -
// or one fringe further out, and nothing beyond that.
TEST(StrokeMeshTest, ASinglePointBecomesADisc) {
    const std::vector<StrokePoint> centerline = {StrokePoint{10, 10}};
    const StrokeMesh mesh = BuildStrokeMesh(centerline, 6.0f, kFringe, StrokeCorners::Round);

    ASSERT_FALSE(mesh.indices.empty());
    EXPECT_TRUE(AllFinite(mesh));
    for (const StrokeVertex& v : mesh.vertices) {
        const float r = std::sqrt((v.x - 10.0f) * (v.x - 10.0f) + (v.y - 10.0f) * (v.y - 10.0f));
        EXPECT_GE(r, 6.0f - 0.01f);
        EXPECT_LE(r, 6.0f + kFringe + 0.01f);
    }
}

// The spike test. ImGui's own polyline scales the miter by 1/cos^2 of half
// the turn and clamps only at 100x the half width, so this input throws a
// vertex hundreds of pixels off the line there. Nothing may sit further out
// than the round pen does.
TEST(StrokeMeshTest, AHairpinDoesNotSpike) {
    const float halfWidth = 10.0f;
    const std::vector<StrokePoint> centerline = {
        StrokePoint{0, 0},
        StrokePoint{100, 0},
        StrokePoint{2, 1},  // back almost the way it came: a near-reversal
    };
    const StrokeMesh mesh = BuildStrokeMesh(centerline, halfWidth, kFringe, StrokeCorners::Round);

    ASSERT_FALSE(mesh.indices.empty());
    EXPECT_TRUE(AllFinite(mesh));
    EXPECT_LT(FurthestFromCenterline(mesh, centerline), halfWidth + kCornerTolerance + kFringe);
}

// An exact 180-degree reversal has no miter direction at all - the two
// normals cancel - which is the case that divides by zero if it isn't
// handled.
TEST(StrokeMeshTest, AnExactReversalIsFiniteAndBounded) {
    const std::vector<StrokePoint> centerline = {
        StrokePoint{0, 0},
        StrokePoint{50, 0},
        StrokePoint{0, 0},
    };
    const StrokeMesh mesh = BuildStrokeMesh(centerline, 8.0f, kFringe, StrokeCorners::Round);

    ASSERT_FALSE(mesh.indices.empty());
    EXPECT_TRUE(AllFinite(mesh));
    EXPECT_LT(FurthestFromCenterline(mesh, centerline), 8.0f + kCornerTolerance + kFringe);
}

// Round caps: the stroke reaches a half width *past* each end point, which a
// flat cap does not, and that is the difference the pen's own shape implies.
TEST(StrokeMeshTest, RoundCapsReachPastBothEnds) {
    const float halfWidth = 7.0f;
    const std::vector<StrokePoint> centerline = {StrokePoint{20, 0}, StrokePoint{80, 0}};
    const StrokeMesh mesh = BuildStrokeMesh(centerline, halfWidth, kFringe, StrokeCorners::Round);

    float minX = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    for (const StrokeVertex& v : mesh.vertices) {
        minX = std::min(minX, v.x);
        maxX = std::max(maxX, v.x);
    }
    // Past the ends by about the radius (plus the fringe), not flush with
    // them, and not further than a round cap would go.
    EXPECT_LT(minX, 20.0f - halfWidth + 0.5f);
    EXPECT_GT(minX, 20.0f - halfWidth - kFringe - 0.01f);
    EXPECT_GT(maxX, 80.0f + halfWidth - 0.5f);
    EXPECT_LT(maxX, 80.0f + halfWidth + kFringe + 0.01f);
}

// The stroke is the width it says it is: on a straight run, the solid
// vertices sit exactly a half width either side of the line.
TEST(StrokeMeshTest, AStraightRunIsExactlyTheRequestedWidth) {
    const float halfWidth = 9.0f;
    const std::vector<StrokePoint> centerline = {StrokePoint{0, 50}, StrokePoint{100, 50}};
    const StrokeMesh mesh = BuildStrokeMesh(centerline, halfWidth, kFringe, StrokeCorners::Round);

    // A straight run needs no intermediate cross-sections - the body is one
    // quad from end to end - so the width is measured where those ends are:
    // the flat cross-sections the caps start from.
    int checked = 0;
    for (const StrokeVertex& v : mesh.vertices) {
        if (v.coverage < 1.0f) {
            continue;
        }
        if (std::fabs(v.x - 0.0f) > 0.01f && std::fabs(v.x - 100.0f) > 0.01f) {
            continue;
        }
        EXPECT_NEAR(std::fabs(v.y - 50.0f), halfWidth, 0.01f);
        ++checked;
    }
    EXPECT_GE(checked, 4);  // both edges at both ends
}

// Every vertex is either solid or fully transparent - the builder's contract
// with the renderer, which picks one of two colors rather than interpolating.
TEST(StrokeMeshTest, CoverageIsOnlyEverZeroOrOne) {
    const std::vector<StrokePoint> centerline = {StrokePoint{0, 0}, StrokePoint{30, 10}, StrokePoint{60, 0}};
    const StrokeMesh mesh = BuildStrokeMesh(centerline, 5.0f, kFringe, StrokeCorners::Round);

    ASSERT_FALSE(mesh.vertices.empty());
    for (const StrokeVertex& v : mesh.vertices) {
        EXPECT_TRUE(v.coverage == 0.0f || v.coverage == 1.0f);
    }
}

// Indices must address vertices that exist, in whole triangles - the mesh
// goes straight into a draw list's index buffer.
TEST(StrokeMeshTest, IndicesAreWholeTrianglesInRange) {
    const std::vector<StrokePoint> centerline = {StrokePoint{0, 0}, StrokePoint{20, 20}, StrokePoint{40, 0},
                                                  StrokePoint{60, 30}};
    const StrokeMesh mesh = BuildStrokeMesh(centerline, 4.0f, kFringe, StrokeCorners::Round);

    ASSERT_FALSE(mesh.indices.empty());
    EXPECT_EQ(mesh.indices.size() % 3, 0u);
    for (const uint32_t index : mesh.indices) {
        EXPECT_LT(index, mesh.vertices.size());
    }
}

// A closed path - what the rectangle tool produces, its last point repeating
// its first - is joined at the seam rather than capped, so nothing sticks
// out past the corner.
TEST(StrokeMeshTest, AClosedPathIsJoinedAtTheSeamNotCapped) {
    const float halfWidth = 5.0f;
    const std::vector<StrokePoint> rectangle = {
        StrokePoint{0, 0}, StrokePoint{100, 0}, StrokePoint{100, 60}, StrokePoint{0, 60}, StrokePoint{0, 0},
    };
    const StrokeMesh mesh = BuildStrokeMesh(rectangle, halfWidth, kFringe, StrokeCorners::Sharp);

    ASSERT_FALSE(mesh.indices.empty());
    EXPECT_TRUE(AllFinite(mesh));
    // A round cap at the seam would reach a half width beyond the corner,
    // diagonally. A mitered corner reaches the corner's own offset and no
    // further.
    for (const StrokeVertex& v : mesh.vertices) {
        EXPECT_GE(v.x, -halfWidth - kFringe - 0.01f);
        EXPECT_LE(v.x, 100.0f + halfWidth + kFringe + 0.01f);
        EXPECT_GE(v.y, -halfWidth - kFringe - 0.01f);
        EXPECT_LE(v.y, 60.0f + halfWidth + kFringe + 0.01f);
    }
}

// A rectangle's corner is well inside the miter limit, so it stays a
// corner: the outer vertex reaches the full diagonal offset rather than
// being rounded off.
TEST(StrokeMeshTest, ARectanglesCornerKeepsItsMiter) {
    const float halfWidth = 6.0f;
    const std::vector<StrokePoint> centerline = {StrokePoint{0, 0}, StrokePoint{50, 0}, StrokePoint{50, 50}};
    const StrokeMesh mesh = BuildStrokeMesh(centerline, halfWidth, kFringe, StrokeCorners::Sharp);

    // The outer corner of a 90-degree turn sits at half width * sqrt(2) from
    // the corner point, diagonally out.
    const float wanted = halfWidth * std::sqrt(2.0f);
    float best = std::numeric_limits<float>::max();
    for (const StrokeVertex& v : mesh.vertices) {
        if (v.coverage < 1.0f) {
            continue;
        }
        const float d = std::sqrt((v.x - 50.0f) * (v.x - 50.0f) + (v.y - 0.0f) * (v.y - 0.0f));
        best = std::min(best, std::fabs(d - wanted));
    }
    EXPECT_LT(best, 0.01f);
}

// The corner a round pen leaves is round, at any turn and any width: no
// vertex reaches further past the pen's outline than the tolerance. Under
// the rectangle's miter limit a 40px pen grew a point 20px long at a turn
// of 120 degrees - the spikes seen on quick flicks of a wide pen.
TEST(StrokeMeshTest, ARoundPenLeavesNoPointAtAnyTurn) {
    for (const float halfWidth : {1.0f, 4.0f, 10.0f, 20.0f, 40.0f}) {
        for (int degrees = 5; degrees < 180; degrees += 5) {
            const float turn = static_cast<float>(degrees) * 3.14159265f / 180.0f;
            const std::vector<StrokePoint> centerline = {
                StrokePoint{0, 0}, StrokePoint{200, 0},
                StrokePoint{200 + 200 * std::cos(turn), 200 * std::sin(turn)}};
            const StrokeMesh mesh = BuildStrokeMesh(centerline, halfWidth, kFringe, StrokeCorners::Round);
            ASSERT_TRUE(AllFinite(mesh));
            float furthestSolid = 0.0f;
            for (const StrokeVertex& v : mesh.vertices) {
                if (v.coverage == 1.0f) {
                    furthestSolid = std::max(furthestSolid, FurthestFromCenterline(StrokeMesh{{v}, {}}, centerline));
                }
            }
            EXPECT_LE(furthestSolid, halfWidth + kCornerTolerance) << halfWidth << " at " << degrees;
            EXPECT_LE(FurthestFromCenterline(mesh, centerline), halfWidth + kCornerTolerance + kFringe)
                << halfWidth << " at " << degrees;
        }
    }
}

// A turn of a few degrees - most of a fitted curve's - keeps its miter, one
// rib like a straight run's, so a smooth stroke costs no more than it did.
TEST(StrokeMeshTest, ANearlyStraightJoinStaysOneRib) {
    const float halfWidth = 6.0f;
    const float turn = 10.0f * 3.14159265f / 180.0f;
    const std::vector<StrokePoint> straight = {StrokePoint{0, 0}, StrokePoint{50, 0}, StrokePoint{100, 0}};
    const std::vector<StrokePoint> bent = {StrokePoint{0, 0}, StrokePoint{50, 0},
                                           StrokePoint{50 + 50 * std::cos(turn), 50 * std::sin(turn)}};
    EXPECT_EQ(BuildStrokeMesh(bent, halfWidth, kFringe, StrokeCorners::Round).vertices.size(),
              BuildStrokeMesh(straight, halfWidth, kFringe, StrokeCorners::Round).vertices.size());
}

// The translucency property, measured rather than eyeballed: the triangles
// cover the stroke's own area exactly once. A renderer that overlaps its
// geometry - as offsetting each segment separately does - paints those
// places twice, which at less than full opacity is a visibly darker patch.
// Area is the test that catches it: overlapping triangles add up to more
// than the shape.
//
// A capsule: a rectangle of length x width, plus the two half-discs of the
// caps, which together make one full circle.
TEST(StrokeMeshTest, TheTrianglesCoverTheStrokeExactlyOnce) {
    const float halfWidth = 8.0f;
    const float length = 120.0f;
    const std::vector<StrokePoint> centerline = {StrokePoint{0, 0}, StrokePoint{length, 0}};
    const StrokeMesh mesh = BuildStrokeMesh(centerline, halfWidth, kFringe, StrokeCorners::Round);

    const float capsule = length * 2.0f * halfWidth + 3.14159265f * halfWidth * halfWidth;
    // Within a percent: the caps are arcs of straight segments, so they fall
    // a hair short of a true half-disc.
    EXPECT_NEAR(SolidArea(mesh), capsule, capsule * 0.01f);
}

// Same measurement over a shape with turns in it, including one sharp enough
// to take a round join. The area of a stroked path is not something to
// compute in closed form, so this compares against the same stroke drawn
// straight: bending a path cannot *add* ink, and overlapping geometry is the
// only way the total could come out higher.
TEST(StrokeMeshTest, TurningThePathDoesNotPaintAnythingTwice) {
    const float halfWidth = 8.0f;
    const std::vector<StrokePoint> bent = {
        StrokePoint{0, 0}, StrokePoint{60, 0}, StrokePoint{100, 40}, StrokePoint{140, 0},
    };
    float pathLength = 0.0f;
    for (size_t i = 0; i + 1 < bent.size(); ++i) {
        const float dx = bent[i + 1].x - bent[i].x;
        const float dy = bent[i + 1].y - bent[i].y;
        pathLength += std::sqrt(dx * dx + dy * dy);
    }
    const StrokeMesh mesh = BuildStrokeMesh(bent, halfWidth, kFringe, StrokeCorners::Round);

    const float straightEquivalent = pathLength * 2.0f * halfWidth + 3.14159265f * halfWidth * halfWidth;
    // No more than the straight run's ink (turns pinch the inside, so a
    // little less is right), and not so much less that the stroke has holes.
    EXPECT_LE(SolidArea(mesh), straightEquivalent * 1.001f);
    EXPECT_GT(SolidArea(mesh), straightEquivalent * 0.9f);
}

// Duplicate consecutive points come out of a hand that stopped moving, and
// carry no direction to build a normal from.
TEST(StrokeMeshTest, DuplicatePointsDoNotProduceGarbage) {
    const std::vector<StrokePoint> centerline = {StrokePoint{0, 0}, StrokePoint{0, 0}, StrokePoint{30, 0},
                                                  StrokePoint{30, 0}, StrokePoint{60, 0}};
    const StrokeMesh mesh = BuildStrokeMesh(centerline, 5.0f, kFringe, StrokeCorners::Round);

    ASSERT_FALSE(mesh.indices.empty());
    EXPECT_TRUE(AllFinite(mesh));
}

}  // namespace
}  // namespace sz::core
