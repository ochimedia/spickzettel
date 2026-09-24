#include "core/drawing/stroke_smoothing.h"

#include <algorithm>
#include <cmath>

namespace sz::core {

namespace {

// Centripetal parameterization: knot spacing is the square root of the
// distance between control points (alpha = 0.5). Uniform spacing (alpha = 0)
// is simpler and is what most quick implementations use, but it overshoots
// and can tie a small loop wherever two control points sit much closer
// together than their neighbors - which is exactly what a hand slowing into
// a corner produces.
constexpr float kAlpha = 0.5f;

// The most samples one span is allowed to cost. Reached only by a span with
// curvature so tight that the fit is a hairpin, where more samples stop
// helping anyway.
constexpr int kMaxSamplesPerSpan = 16;

float Distance(const StrokePoint& a, const StrokePoint& b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    return std::sqrt(dx * dx + dy * dy);
}

StrokePoint Blend(const StrokePoint& a, const StrokePoint& b, float wa, float wb) {
    return StrokePoint{a.x * wa + b.x * wb, a.y * wa + b.y * wb};
}

// One evaluation of the Barry-Goldman pyramid: three linear blends between
// neighboring control points, then two between those, then one between
// those - which is the Catmull-Rom spline written in a form that takes the
// knot values directly, so a non-uniform parameterization needs no special
// casing. Every denominator is a knot difference, and coincident control
// points make one of them zero, hence the guards.
StrokePoint EvaluateSpline(const StrokePoint& p0, const StrokePoint& p1, const StrokePoint& p2,
                            const StrokePoint& p3, float t0, float t1, float t2, float t3, float t) {
    const auto lerpKnots = [](const StrokePoint& a, const StrokePoint& b, float ka, float kb, float at) {
        const float span = kb - ka;
        if (std::fabs(span) < 1e-6f) {
            return b;
        }
        return Blend(a, b, (kb - at) / span, (at - ka) / span);
    };
    const StrokePoint a1 = lerpKnots(p0, p1, t0, t1, t);
    const StrokePoint a2 = lerpKnots(p1, p2, t1, t2, t);
    const StrokePoint a3 = lerpKnots(p2, p3, t2, t3, t);
    const StrokePoint b1 = lerpKnots(a1, a2, t0, t2, t);
    const StrokePoint b2 = lerpKnots(a2, a3, t1, t3, t);
    return lerpKnots(b1, b2, t1, t2, t);
}

}  // namespace

void AppendFittedSpan(const StrokePoint& p0, const StrokePoint& p1, const StrokePoint& p2,
                      const StrokePoint& p3, float flatnessPx, std::vector<StrokePoint>& out) {
    // Knots, spaced by the centripetal rule. A zero-length gap would collapse
    // the pyramid above, so each knot is nudged to stay strictly increasing;
    // the effect on the curve is nil and the alternative is a division by
    // zero the guards would silently turn into a corner.
    const float t0 = 0.0f;
    const float t1 = t0 + std::max(std::pow(Distance(p0, p1), kAlpha), 1e-4f);
    const float t2 = t1 + std::max(std::pow(Distance(p1, p2), kAlpha), 1e-4f);
    const float t3 = t2 + std::max(std::pow(Distance(p2, p3), kAlpha), 1e-4f);

    // How far the curve strays from the chord, measured at the midpoint. For
    // a cubic the error of an n-segment approximation falls with n^2, so the
    // sample count is the square root of how many times too large that
    // deviation currently is. A span that is already straight measures ~0 and
    // costs a single segment no matter how long it is.
    const StrokePoint mid = EvaluateSpline(p0, p1, p2, p3, t0, t1, t2, t3, (t1 + t2) * 0.5f);
    const StrokePoint chordMid = Blend(p1, p2, 0.5f, 0.5f);
    const float deviation = Distance(mid, chordMid);
    const float safeFlatness = std::max(flatnessPx, 1e-3f);
    const int samples = std::clamp(static_cast<int>(std::ceil(std::sqrt(deviation / safeFlatness))), 1,
                                    kMaxSamplesPerSpan);

    for (int i = 1; i <= samples; ++i) {
        // The last sample is the control point itself rather than an
        // evaluation at t2 - same value in exact arithmetic, but this way the
        // fitted curve provably passes through every point the hand made,
        // with no floating-point drift to argue about.
        if (i == samples) {
            out.push_back(p2);
            break;
        }
        const float t = t1 + (t2 - t1) * (static_cast<float>(i) / static_cast<float>(samples));
        out.push_back(EvaluateSpline(p0, p1, p2, p3, t0, t1, t2, t3, t));
    }
}

}  // namespace sz::core
