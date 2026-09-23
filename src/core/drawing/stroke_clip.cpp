#include "core/drawing/stroke_clip.h"

#include <cmath>
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

}  // namespace

std::optional<std::vector<Stroke>> ClipStrokeOutsideCircle(const Stroke& stroke, StrokePoint center, float radius) {
    const float radiusSq = radius * radius;
    return ClipStrokeOutsideRegion(
        stroke, [&](const StrokePoint& p) { return IsInsideCircle(p, center, radiusSq); },
        [&](const StrokePoint& a, const StrokePoint& b) { return SegmentCircleCrossings(a, b, center, radius); });
}

std::optional<std::vector<Stroke>> ClipStrokeOutsideRect(const Stroke& stroke, float minX, float minY, float maxX,
                                                          float maxY) {
    return ClipStrokeOutsideRegion(
        stroke, [&](const StrokePoint& p) { return IsInsideRect(p, minX, minY, maxX, maxY); },
        [&](const StrokePoint& a, const StrokePoint& b) { return SegmentRectCrossings(a, b, minX, minY, maxX, maxY); });
}

}  // namespace sz::core
