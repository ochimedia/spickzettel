#include "core/drawing/stroke_mesh.h"

#include <algorithm>
#include <cmath>

namespace sz::core {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// How far a miter may stretch, as a multiple of the half width, before the
// join is drawn round instead. 2 is the turn at which the outer corner has
// travelled a full pen width past the centerline - about 120 degrees.
// Anything sharper looks better, and stays the width of the pen, as an arc.
// (ImGui's own polyline clamps only at 100x the half width.)
constexpr float kMiterLimit = 2.0f;

// How much of a round join or cap one arc segment may span. At a quarter of
// a radian, a cap on a 40px pen is round to well under half a pixel.
constexpr float kMaxArcStepRadians = 0.25f;

// Two points closer than this are the same point as far as the direction
// between them is concerned - normalising that difference would amplify
// float noise into an arbitrary normal.
constexpr float kCoincidentEpsilon = 1e-4f;

struct V2 {
    float x = 0.0f;
    float y = 0.0f;
};

V2 Sub(const V2& a, const V2& b) { return V2{a.x - b.x, a.y - b.y}; }
V2 Add(const V2& a, const V2& b) { return V2{a.x + b.x, a.y + b.y}; }
V2 Scale(const V2& v, float s) { return V2{v.x * s, v.y * s}; }
float Dot(const V2& a, const V2& b) { return a.x * b.x + a.y * b.y; }
float Cross(const V2& a, const V2& b) { return a.x * b.y - a.y * b.x; }
float Length(const V2& v) { return std::sqrt(v.x * v.x + v.y * v.y); }

V2 Normalized(const V2& v) {
    const float len = Length(v);
    if (len < kCoincidentEpsilon) {
        return V2{0.0f, 0.0f};
    }
    return V2{v.x / len, v.y / len};
}

// Left-hand normal of a direction of travel, in y-down screen space.
V2 LeftNormal(const V2& dir) { return V2{dir.y, -dir.x}; }

V2 Rotate(const V2& v, float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    return V2{v.x * c - v.y * s, v.x * s + v.y * c};
}

// One cross-section of the stroke: where its two edges are at this point
// along the path, and which way is "outward" from each, so the fringe knows
// where to go. A cap or a round join is just a run of these pivoting around
// the same center, which is why one strip covers every case.
struct Rib {
    V2 left;
    V2 right;
    V2 leftOut;   // unit, points away from the stroke on the left edge
    V2 rightOut;  // unit, ditto on the right
};

// Consecutive duplicates carry no direction and would poison every normal
// derived from them. A closed path keeps its repeated final point - that
// repeat is what tells the builder it is closed.
std::vector<V2> Deduplicate(const std::vector<StrokePoint>& points) {
    std::vector<V2> out;
    out.reserve(points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        const V2 v{points[i].x, points[i].y};
        const bool lastPoint = (i + 1 == points.size());
        const bool closesTheLoop = lastPoint && out.size() >= 3 &&
                                    Length(Sub(v, out.front())) < kCoincidentEpsilon;
        if (out.empty() || closesTheLoop || Length(Sub(v, out.back())) >= kCoincidentEpsilon) {
            out.push_back(v);
        }
    }
    return out;
}

// Arc of ribs around `center`, sweeping the outward direction of one edge
// from `fromDir` by `sweep` radians while the other edge stays pinned at
// `innerPoint` - so the strip between them fans out around the turn.
// Excludes the starting direction itself: whatever rib established it has
// already been emitted.
void AppendArcRibs(const V2& center, const V2& fromDir, float sweep, float radius, const V2& innerPoint,
                   const V2& innerOut, bool arcOnLeft, std::vector<Rib>& ribs) {
    const int steps = std::max(1, static_cast<int>(std::ceil(std::fabs(sweep) / kMaxArcStepRadians)));
    for (int i = 1; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const V2 dir = Rotate(fromDir, sweep * t);
        Rib rib;
        if (arcOnLeft) {
            rib.left = Add(center, Scale(dir, radius));
            rib.leftOut = dir;
            rib.right = innerPoint;
            rib.rightOut = innerOut;
        } else {
            rib.right = Add(center, Scale(dir, radius));
            rib.rightOut = dir;
            rib.left = innerPoint;
            rib.leftOut = innerOut;
        }
        ribs.push_back(rib);
    }
}

// The flat cross-section at `point`, square to a segment travelling `dir`.
Rib SquareRib(const V2& point, const V2& dir, float radius) {
    const V2 normal = LeftNormal(dir);
    Rib rib;
    rib.left = Add(point, Scale(normal, radius));
    rib.leftOut = normal;
    rib.right = Sub(point, Scale(normal, radius));
    rib.rightOut = Scale(normal, -1.0f);
    return rib;
}

// A half turn of ribs around one end of the stroke: both edges swing toward
// `outwardDir` until they meet at the point of the cap - the outline of a
// round pen where it was set down or lifted. Excludes the flat rib the cap
// starts from, which the caller emits.
void AppendCapRibs(const V2& center, const V2& outwardDir, float radius, std::vector<Rib>& ribs) {
    const V2 startLeft = LeftNormal(outwardDir);
    const int steps = std::max(1, static_cast<int>(std::ceil((kPi * 0.5f) / kMaxArcStepRadians)));
    for (int i = 1; i <= steps; ++i) {
        const float angle = (kPi * 0.5f) * (static_cast<float>(i) / static_cast<float>(steps));
        // Left edge rotates toward the outward direction, right edge mirrors
        // it, so both arrive at center + outwardDir * radius together.
        const V2 leftDir = Rotate(startLeft, angle);
        const V2 rightDir = Rotate(Scale(startLeft, -1.0f), -angle);
        Rib rib;
        rib.left = Add(center, Scale(leftDir, radius));
        rib.leftOut = leftDir;
        rib.right = Add(center, Scale(rightDir, radius));
        rib.rightOut = rightDir;
        ribs.push_back(rib);
    }
}

// A dot: the pen set down and lifted without travelling. Built as ribs
// sweeping a half turn, so it goes through the same strip-and-fringe code as
// everything else rather than needing a circle primitive of its own.
std::vector<Rib> BuildDiscRibs(const V2& center, float radius) {
    std::vector<Rib> ribs;
    const V2 start{1.0f, 0.0f};
    const int steps = std::max(2, static_cast<int>(std::ceil(kPi / kMaxArcStepRadians)));
    ribs.reserve(static_cast<size_t>(steps) + 1);
    for (int i = 0; i <= steps; ++i) {
        const float angle = kPi * (static_cast<float>(i) / static_cast<float>(steps));
        const V2 leftDir = Rotate(start, angle);
        const V2 rightDir = Rotate(start, -angle);
        Rib rib;
        rib.left = Add(center, Scale(leftDir, radius));
        rib.leftOut = leftDir;
        rib.right = Add(center, Scale(rightDir, radius));
        rib.rightOut = rightDir;
        ribs.push_back(rib);
    }
    return ribs;
}

// The ribs for one interior point, given the direction of the segments
// either side of it. A shallow turn takes a miter: one rib, whose two
// vertices both segments share, so nothing overlaps. A sharp one takes a
// round join - an arc on the outside of the turn, pivoting on the inside.
void AppendJointRibs(const V2& point, const V2& inDir, const V2& outDir, float halfWidth,
                     std::vector<Rib>& ribs) {
    const V2 inNormal = LeftNormal(inDir);
    const V2 outNormal = LeftNormal(outDir);
    const V2 mean = Normalized(Add(inNormal, outNormal));
    const bool reversal = (mean.x == 0.0f && mean.y == 0.0f);

    if (!reversal && Dot(mean, inNormal) >= 1.0f / kMiterLimit) {
        const float miter = halfWidth / Dot(mean, inNormal);
        Rib rib;
        rib.left = Add(point, Scale(mean, miter));
        rib.leftOut = mean;
        rib.right = Sub(point, Scale(mean, miter));
        rib.rightOut = Scale(mean, -1.0f);
        ribs.push_back(rib);
        return;
    }

    // Which edge is on the outside of the turn: walking east then south is a
    // turn to the right, and its outer edge is the left-hand one. A reversal
    // is a half turn whose direction the cross product can't resolve, so it
    // picks a side - either traces the same hairpin tip.
    const float turn = reversal ? kPi : std::atan2(Cross(inDir, outDir), Dot(inDir, outDir));
    const bool arcOnLeft = turn > 0.0f;
    const V2 fromDir = arcOnLeft ? inNormal : Scale(inNormal, -1.0f);
    // Pinned at the pen's own half width rather than at where the two inner
    // edges truly cross: past the miter limit that crossing is far up the
    // inside of the turn and folds the strip back over itself, which is a
    // worse artifact than a slightly pinched inner corner.
    const V2 innerDir = reversal ? Scale(fromDir, -1.0f) : (arcOnLeft ? Scale(mean, -1.0f) : mean);
    const V2 innerPoint = reversal ? point : Add(point, Scale(innerDir, halfWidth));

    // Square to the incoming segment, with the inner edge already pulled in
    // to the pivot the arc turns around...
    Rib entry = SquareRib(point, inDir, halfWidth);
    if (arcOnLeft) {
        entry.right = innerPoint;
        entry.rightOut = innerDir;
    } else {
        entry.left = innerPoint;
        entry.leftOut = innerDir;
    }
    ribs.push_back(entry);

    // ...then the arc itself, which lands square to the outgoing segment.
    AppendArcRibs(point, fromDir, turn, halfWidth, innerPoint, innerDir, arcOnLeft, ribs);
}

// Strips the ribs into triangles: a filled quad between each neighbouring
// pair, and a fringe quad along each outer edge fading to zero coverage.
// Neighbouring quads share their vertices, which is the whole point - no
// triangle is drawn over any other, so a translucent color lands exactly
// once everywhere along the stroke.
void RibsToMesh(const std::vector<Rib>& ribs, float fringePx, StrokeMesh& mesh) {
    // Cleared, not reassigned: `mesh` may be one the caller is rebuilding in
    // place, and its capacity is the thing worth keeping.
    mesh.vertices.clear();
    mesh.indices.clear();
    if (ribs.size() < 2) {
        return;
    }
    mesh.vertices.reserve(ribs.size() * 4);
    mesh.indices.reserve((ribs.size() - 1) * 18);

    for (const Rib& rib : ribs) {
        const V2 leftOuter = Add(rib.left, Scale(rib.leftOut, fringePx));
        const V2 rightOuter = Add(rib.right, Scale(rib.rightOut, fringePx));
        mesh.vertices.push_back(StrokeVertex{leftOuter.x, leftOuter.y, 0.0f});
        mesh.vertices.push_back(StrokeVertex{rib.left.x, rib.left.y, 1.0f});
        mesh.vertices.push_back(StrokeVertex{rib.right.x, rib.right.y, 1.0f});
        mesh.vertices.push_back(StrokeVertex{rightOuter.x, rightOuter.y, 0.0f});
    }

    const auto quad = [&mesh](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        mesh.indices.insert(mesh.indices.end(), {a, b, c, a, c, d});
    };
    for (uint32_t i = 0; i + 1 < static_cast<uint32_t>(ribs.size()); ++i) {
        const uint32_t v = i * 4;
        const uint32_t n = v + 4;
        quad(v + 0, v + 1, n + 1, n + 0);  // left fringe
        quad(v + 1, v + 2, n + 2, n + 1);  // body
        quad(v + 2, v + 3, n + 3, n + 2);  // right fringe
    }
}

}  // namespace

StrokeMesh BuildStrokeMesh(const std::vector<StrokePoint>& centerline, float halfWidth, float fringePx) {
    StrokeMesh mesh;
    BuildStrokeMesh(centerline, halfWidth, fringePx, mesh);
    return mesh;
}

void BuildStrokeMesh(const std::vector<StrokePoint>& centerline, float halfWidth, float fringePx, StrokeMesh& out) {
    const std::vector<V2> pts = Deduplicate(centerline);
    const float radius = std::max(halfWidth, 0.01f);
    if (pts.empty()) {
        out.vertices.clear();
        out.indices.clear();
        return;
    }
    if (pts.size() == 1) {
        RibsToMesh(BuildDiscRibs(pts[0], radius), fringePx, out);
        return;
    }

    // A path whose last point repeats its first (the rectangle tool's does)
    // has no ends to cap - it has one more join instead, at the seam.
    const bool closed = pts.size() >= 4 && Length(Sub(pts.front(), pts.back())) < kCoincidentEpsilon;
    const size_t count = closed ? pts.size() - 1 : pts.size();

    // Direction of the segment leaving each point. For an open path the last
    // entry wraps around to the first point and is never read.
    std::vector<V2> dirs;
    dirs.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        dirs.push_back(Normalized(Sub(pts[(i + 1) % pts.size()], pts[i])));
    }

    std::vector<Rib> ribs;
    ribs.reserve(count * 2 + 16);

    if (closed) {
        // The seam is a join like any other; walking from it and repeating
        // its ribs at the end is what closes the strip on itself.
        AppendJointRibs(pts[0], dirs[count - 1], dirs[0], radius, ribs);
        const size_t seamRibCount = ribs.size();
        for (size_t i = 1; i < count; ++i) {
            AppendJointRibs(pts[i], dirs[i - 1], dirs[i], radius, ribs);
        }
        for (size_t i = 0; i < seamRibCount; ++i) {
            ribs.push_back(ribs[i]);
        }
        RibsToMesh(ribs, fringePx, out);
        return;
    }

    // Start cap, built from the far end and then reversed into path order.
    // Reversing swaps which edge is which, hence the second swap.
    {
        std::vector<Rib> startCap;
        AppendCapRibs(pts.front(), Scale(dirs.front(), -1.0f), radius, startCap);
        std::reverse(startCap.begin(), startCap.end());
        for (Rib& rib : startCap) {
            std::swap(rib.left, rib.right);
            std::swap(rib.leftOut, rib.rightOut);
        }
        ribs.insert(ribs.end(), startCap.begin(), startCap.end());
        ribs.push_back(SquareRib(pts.front(), dirs.front(), radius));
    }

    for (size_t i = 1; i + 1 < count; ++i) {
        AppendJointRibs(pts[i], dirs[i - 1], dirs[i], radius, ribs);
    }

    // ...and the end cap, which starts from its own flat cross-section.
    ribs.push_back(SquareRib(pts[count - 1], dirs[count - 2], radius));
    AppendCapRibs(pts[count - 1], dirs[count - 2], radius, ribs);

    RibsToMesh(ribs, fringePx, out);
}

}  // namespace sz::core
