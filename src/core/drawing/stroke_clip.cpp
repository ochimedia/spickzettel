#include "core/drawing/stroke_clip.h"

#include <cmath>
#include <utility>

namespace sz::core {

namespace {

StrokePoint Lerp(const StrokePoint& a, const StrokePoint& b, float t) {
    return StrokePoint{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

// Shared walk: given a per-point inside-the-region test and a per-segment
// boundary-crossing finder (at most 2 crossings, ascending t in (0, 1) -
// true of both a circle and a convex polygon like a rectangle, since a
// straight line can enter/exit a convex region at most once each), walks
// `stroke`'s polyline and returns the runs of points that lie *outside*
// the region, split at every crossing - the actual "cut a bite out of the
// ink" logic, region-agnostic. `IsInside`/`Crossings` are the only
// region-specific pieces; the circle and the rectangle wire their own
// geometry into this once, so the two can never disagree on how a
// crossing splits a stroke.
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

    bool stateInside = isInside(pts[0]);
    if (stateInside) {
        changed = true;
    } else {
        currentRun.push_back(pts[0]);
    }

    for (size_t i = 1; i < pts.size(); ++i) {
        const StrokePoint& p0 = pts[i - 1];
        const StrokePoint& p1 = pts[i];
        const bool p1Inside = isInside(p1);

        const std::vector<float> crossingTs = crossings(p0, p1);
        if (!crossingTs.empty()) {
            changed = true;
        }
        for (const float t : crossingTs) {
            const StrokePoint crossPoint = Lerp(p0, p1, t);
            if (stateInside) {
                // Re-entering the outside partway along this segment -
                // start a fresh run right here.
                currentRun.clear();
                currentRun.push_back(crossPoint);
            } else {
                // Leaving the outside partway along this segment - close
                // the run off right here.
                currentRun.push_back(crossPoint);
                finalizeRun();
            }
            stateInside = !stateInside;
        }

        // Normally, having walked every crossing above, `stateInside`
        // already agrees with `p1Inside` (computed directly from p1's own
        // position) - a convex region's entry/exit exactly matches point
        // classification. The one case they can disagree: one of this
        // segment's own endpoints lies exactly on the region's boundary
        // (isInside's own comparisons are inclusive), so the true crossing
        // t is 0 or 1 - which the crossings() finder deliberately excludes
        // (see its own comment) to avoid a zero-length fragment at an
        // existing vertex. Which endpoint depends on the transition
        // direction: entering (outside -> inside) can only reach this
        // branch with no interior crossing if p1 itself is the boundary
        // point (p0 was genuinely outside, established by the previous
        // iteration), so close the run there. Exiting (inside -> outside)
        // is the mirror case: p0 was already inside, so an empty crossing
        // list here means p0 itself is exactly the boundary (p1 is
        // genuinely outside) - start the new run at p0, then keep p1 too.
        if (stateInside != p1Inside) {
            changed = true;
            if (stateInside) {
                currentRun.clear();
                currentRun.push_back(p0);
                currentRun.push_back(p1);
            } else {
                currentRun.push_back(p1);
                finalizeRun();
            }
            stateInside = p1Inside;
        } else if (p1Inside) {
            changed = true;
        } else {
            currentRun.push_back(p1);
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
