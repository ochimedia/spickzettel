#include "core/drawing/stroke_clip.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace sz::core {

namespace {

StrokePoint Lerp(const StrokePoint& a, const StrokePoint& b, float t) {
    return StrokePoint{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

// Shared walk: given a per-point inside-the-region test and a per-segment
// boundary finder (at most 2 values, ascending t in (0, 1) - true of both a
// circle and a convex polygon like a rectangle, since a straight line meets
// a convex region's boundary at most twice), walks `stroke`'s polyline and
// returns the runs of it that lie *outside* the region, split wherever it
// goes in - the actual "cut a bite out of the ink" logic, region-agnostic.
// `IsInside`/`Crossings` are the only region-specific pieces; the circle and
// the rectangle wire their own geometry into this once, so the two can
// never disagree on how a crossing splits a stroke.
//
// The boundary points cut each segment into pieces, and each piece is
// classified by the point halfway along it, rather than each boundary point
// being taken as a change from outside to inside or back. A line that only
// touches the region - tangent to the circle, through a rectangle's corner
// - meets its boundary once without going in: the finder reports that
// point once, and a walk that toggled at it believed itself inside for the
// rest of the segment, then rebuilt the segment from its start when its
// end turned out to be outside, and handed back the first half twice. A
// piece is inside or it is not, whatever the boundary did at its ends, so
// a touch leaves the stroke as it was. It also covers a vertex exactly on
// the boundary, which the finder deliberately does not report (t = 0 or 1
// would make a zero-length fragment): the pieces either side of it say
// what happened there.
template <typename IsInsideFn, typename CrossingsFn>
std::optional<std::vector<Stroke>> ClipStrokeOutsideRegion(const Stroke& stroke, IsInsideFn isInside,
                                                            CrossingsFn crossings) {
    const std::vector<StrokePoint>& pts = stroke.points;
    if (pts.empty()) {
        return std::nullopt;
    }
    // A dot (see CanvasState::EndStroke) has no segments to walk and no
    // fragments to leave behind: it is either taken whole or not touched.
    if (pts.size() == 1) {
        return isInside(pts[0]) ? std::optional<std::vector<Stroke>>(std::vector<Stroke>{}) : std::nullopt;
    }

    bool changed = false;
    std::vector<Stroke> fragments;
    std::vector<StrokePoint> currentRun;

    const auto finalizeRun = [&]() {
        if (currentRun.size() >= 2) {
            Stroke fragment;
            fragment.points = currentRun;
            fragment.colorRGBA = stroke.colorRGBA;
            fragment.width = stroke.width;
            fragment.corners = stroke.corners;
            fragments.push_back(std::move(fragment));
        }
        currentRun.clear();
    };

    for (size_t i = 1; i < pts.size(); ++i) {
        const StrokePoint& p0 = pts[i - 1];
        const StrokePoint& p1 = pts[i];
        std::vector<float> cuts{0.0f};
        for (const float t : crossings(p0, p1)) {
            cuts.push_back(t);
        }
        cuts.push_back(1.0f);

        for (size_t piece = 0; piece + 1 < cuts.size(); ++piece) {
            const float from = cuts[piece];
            if (isInside(Lerp(p0, p1, (from + cuts[piece + 1]) * 0.5f))) {
                changed = true;
                finalizeRun();
                continue;
            }
            // Outside, and on to the end of every outside piece after it -
            // a boundary point between two outside pieces is a touch, not a
            // vertex of the stroke.
            size_t last = piece;
            while (last + 2 < cuts.size() &&
                   !isInside(Lerp(p0, p1, (cuts[last + 1] + cuts[last + 2]) * 0.5f))) {
                ++last;
            }
            if (currentRun.empty()) {
                currentRun.push_back(Lerp(p0, p1, from));
            }
            currentRun.push_back(last + 2 == cuts.size() ? p1 : Lerp(p0, p1, cuts[last + 1]));
            piece = last;
        }
    }
    finalizeRun();

    if (!changed) {
        return std::nullopt;
    }
    return fragments;
}

bool IsInsideCircle(const StrokePoint& p, StrokePoint center, float radiusSq) {
    const float dx = p.x - center.x;
    const float dy = p.y - center.y;
    return dx * dx + dy * dy <= radiusSq;
}

// t-values in (0, 1), ascending, where segment a->b crosses the circle's
// boundary - at most two, since a straight line can cross a circle's
// perimeter at most twice. Empty if the segment (extended infinitely)
// misses the circle entirely, or a/b coincide (a zero-length segment
// can't cross anything). Points essentially on the boundary already (within
// kEpsilon of t=0 or t=1) are treated as not crossing, to avoid emitting
// a zero-length fragment right at an existing polyline vertex.
std::vector<float> SegmentCircleCrossings(const StrokePoint& a, const StrokePoint& b, StrokePoint center,
                                           float radius) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float fx = a.x - center.x;
    const float fy = a.y - center.y;

    const float coeffA = dx * dx + dy * dy;
    if (coeffA <= 1e-12f) {
        return {};
    }
    const float coeffB = 2.0f * (fx * dx + fy * dy);
    const float coeffC = fx * fx + fy * fy - radius * radius;

    const float discriminant = coeffB * coeffB - 4.0f * coeffA * coeffC;
    if (discriminant < 0.0f) {
        return {};
    }
    const float sqrtDisc = std::sqrt(discriminant);
    const float t1 = (-coeffB - sqrtDisc) / (2.0f * coeffA);
    const float t2 = (-coeffB + sqrtDisc) / (2.0f * coeffA);

    constexpr float kEpsilon = 1e-6f;
    std::vector<float> result;
    if (t1 > kEpsilon && t1 < 1.0f - kEpsilon) {
        result.push_back(t1);
    }
    if (t2 > kEpsilon && t2 < 1.0f - kEpsilon && (result.empty() || t2 - result.back() > kEpsilon)) {
        result.push_back(t2);
    }
    return result;
}

bool IsInsideRect(const StrokePoint& p, float minX, float minY, float maxX, float maxY) {
    return p.x >= minX && p.x <= maxX && p.y >= minY && p.y <= maxY;
}

// t-values in (0, 1), ascending, where segment a->b crosses the
// rectangle's boundary - at most two (a rectangle is convex, same
// reasoning as the circle case). Standard Liang-Barsky parametric clip
// against the 4 half-planes (x>=minX, x<=maxX, y>=minY, y<=maxY): the
// portion of the segment inside the rectangle is exactly t in [t0, t1]
// once every half-plane has narrowed that interval, so a genuine partial
// crossing shows up as t0 and/or t1 landing strictly inside (0, 1) - a
// segment that's entirely inside or entirely outside the rectangle never
// produces an interior t0/t1 at all, matching SegmentCircleCrossings'
// own "no crossings" behavior for those cases (the outer per-point
// isInside test in ClipStrokeOutsideRegion is what actually decides those
// cases, not this function).
std::vector<float> SegmentRectCrossings(const StrokePoint& a, const StrokePoint& b, float minX, float minY,
                                         float maxX, float maxY) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float p[4] = {-dx, dx, -dy, dy};
    const float q[4] = {a.x - minX, maxX - a.x, a.y - minY, maxY - a.y};

    float t0 = 0.0f;
    float t1 = 1.0f;
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0.0f) {
            if (q[i] < 0.0f) {
                return {};  // parallel to this boundary and entirely outside it
            }
            continue;
        }
        const float r = q[i] / p[i];
        if (p[i] < 0.0f) {
            t0 = std::max(t0, r);
        } else {
            t1 = std::min(t1, r);
        }
    }
    if (t0 > t1) {
        return {};
    }

    constexpr float kEpsilon = 1e-6f;
    std::vector<float> result;
    if (t0 > kEpsilon && t0 < 1.0f - kEpsilon) {
        result.push_back(t0);
    }
    if (t1 > kEpsilon && t1 < 1.0f - kEpsilon && (result.empty() || t1 - result.back() > kEpsilon)) {
        result.push_back(t1);
    }
    return result;
}

// Whether `p` lies within `radius` of the segment a->b (radiusSq is its
// square): the distance to the nearest point of the segment.
bool IsInsideCapsule(const StrokePoint& p, StrokePoint a, StrokePoint b, float radiusSq) {
    const float abx = b.x - a.x;
    const float aby = b.y - a.y;
    const float lengthSq = abx * abx + aby * aby;
    float t = 0.0f;
    if (lengthSq > 0.0f) {
        t = std::clamp(((p.x - a.x) * abx + (p.y - a.y) * aby) / lengthSq, 0.0f, 1.0f);
    }
    return IsInsideCircle(p, StrokePoint{a.x + abx * t, a.y + aby * t}, radiusSq);
}

// Narrows [lo, hi] to where c + k*t >= 0 - one half-plane of a
// Liang-Barsky clip, on the line's unbounded parameter.
void KeepWhereNotNegative(float& lo, float& hi, float c, float k) {
    if (k == 0.0f) {
        if (c < 0.0f) {
            lo = 1.0f;
            hi = 0.0f;  // parallel to this boundary and outside it: nothing
        }
        return;
    }
    const float r = -c / k;
    if (k > 0.0f) {
        lo = std::max(lo, r);
    } else {
        hi = std::min(hi, r);
    }
}

// t-values in (0, 1), ascending, where segment p0->p1 crosses the boundary
// of the capsule around a->b - at most two, the capsule being convex. The
// capsule is the band along a->b and a circle at each end; each meets the
// segment's line in an interval, and since their union is convex, the
// line is inside it over the one interval from the least of their starts
// to the greatest of their ends. Reported as the circle's and the
// rectangle's are, for ClipStrokeOutsideRegion to classify.
std::vector<float> SegmentCapsuleCrossings(const StrokePoint& p0, const StrokePoint& p1, StrokePoint a, StrokePoint b,
                                            float radius) {
    const float dx = p1.x - p0.x;
    const float dy = p1.y - p0.y;
    const float coeffA = dx * dx + dy * dy;
    if (coeffA <= 1e-12f) {
        return {};
    }
    constexpr float kUnbounded = std::numeric_limits<float>::infinity();
    float from = kUnbounded;
    float to = -kUnbounded;
    const auto take = [&](float lo, float hi) {
        if (lo <= hi) {
            from = std::min(from, lo);
            to = std::max(to, hi);
        }
    };

    // The circle at each end: where the line is within `radius` of it.
    for (const StrokePoint& center : {a, b}) {
        const float fx = p0.x - center.x;
        const float fy = p0.y - center.y;
        const float coeffB = 2.0f * (fx * dx + fy * dy);
        const float coeffC = fx * fx + fy * fy - radius * radius;
        const float discriminant = coeffB * coeffB - 4.0f * coeffA * coeffC;
        if (discriminant >= 0.0f) {
            const float sqrtDisc = std::sqrt(discriminant);
            take((-coeffB - sqrtDisc) / (2.0f * coeffA), (-coeffB + sqrtDisc) / (2.0f * coeffA));
        }
    }

    // The band: along a->b between its ends, and within `radius` across.
    const float abx = b.x - a.x;
    const float aby = b.y - a.y;
    const float length = std::sqrt(abx * abx + aby * aby);
    if (length > 1e-6f) {
        const float ux = abx / length;
        const float uy = aby / length;
        const float along0 = (p0.x - a.x) * ux + (p0.y - a.y) * uy;
        const float alongK = dx * ux + dy * uy;
        const float across0 = (p0.y - a.y) * ux - (p0.x - a.x) * uy;
        const float acrossK = dy * ux - dx * uy;
        float lo = -kUnbounded;
        float hi = kUnbounded;
        KeepWhereNotNegative(lo, hi, along0, alongK);
        KeepWhereNotNegative(lo, hi, length - along0, -alongK);
        KeepWhereNotNegative(lo, hi, radius + across0, acrossK);
        KeepWhereNotNegative(lo, hi, radius - across0, -acrossK);
        take(lo, hi);
    }

    constexpr float kEpsilon = 1e-6f;
    std::vector<float> result;
    if (from > kEpsilon && from < 1.0f - kEpsilon) {
        result.push_back(from);
    }
    if (to > kEpsilon && to < 1.0f - kEpsilon && (result.empty() || to - result.back() > kEpsilon)) {
        result.push_back(to);
    }
    return result;
}

}  // namespace

std::optional<std::vector<Stroke>> ClipStrokeOutsideCircle(const Stroke& stroke, StrokePoint center, float radius) {
    const float radiusSq = radius * radius;
    return ClipStrokeOutsideRegion(
        stroke, [&](const StrokePoint& p) { return IsInsideCircle(p, center, radiusSq); },
        [&](const StrokePoint& a, const StrokePoint& b) { return SegmentCircleCrossings(a, b, center, radius); });
}

std::optional<std::vector<Stroke>> ClipStrokeOutsideCapsule(const Stroke& stroke, StrokePoint from, StrokePoint to,
                                                            float radius) {
    if (from.x == to.x && from.y == to.y) {
        return ClipStrokeOutsideCircle(stroke, from, radius);
    }
    const float radiusSq = radius * radius;
    return ClipStrokeOutsideRegion(
        stroke, [&](const StrokePoint& p) { return IsInsideCapsule(p, from, to, radiusSq); },
        [&](const StrokePoint& a, const StrokePoint& b) { return SegmentCapsuleCrossings(a, b, from, to, radius); });
}

std::optional<std::vector<Stroke>> ClipStrokeOutsideRect(const Stroke& stroke, float minX, float minY, float maxX,
                                                          float maxY) {
    return ClipStrokeOutsideRegion(
        stroke, [&](const StrokePoint& p) { return IsInsideRect(p, minX, minY, maxX, maxY); },
        [&](const StrokePoint& a, const StrokePoint& b) { return SegmentRectCrossings(a, b, minX, minY, maxX, maxY); });
}

}  // namespace sz::core
