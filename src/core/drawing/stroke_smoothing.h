#pragma once

#include <vector>

#include "core/drawing/stroke.h"

namespace sz::core {

// Curve fitting for freehand input: turns the sparse control points a hand
// actually produces into the curve it meant.
//
// The spacing filter in DrawTool leaves points every couple of pixels, which
// is the right density to *store* but the wrong thing to *draw* - a polyline
// through them is a chain of short straight segments, and at a wide pen every
// one of those corners is a place for the outline to kink. Fitting a spline
// through the control points and sampling it instead gives the curve the hand
// described, at whatever density the curvature actually needs.
//
// Centripetal Catmull-Rom, not uniform: it passes exactly through every
// control point (so this never moves the ink away from where the hand was),
// and unlike the uniform parameterisation it cannot loop or cusp when the
// spacing between control points is uneven - which it always is, because a
// hand speeds up and slows down.
//
// This is deliberately an *input* stage, not a rendering one. Fitting here
// means the stored polyline is the curve: the eraser cuts what you see, hit
// testing agrees with the ink, and a shape tool - whose corners must stay
// corners - simply doesn't call it. Fitting at draw time would have meant
// the renderer smoothing a rectangle's corners with no way to know better.

// Appends the fitted curve from `p1` to `p2` to `out`, excluding `p1` (the
// caller already has it) and including `p2`. `p0` and `p3` are the
// neighbouring control points, which set the tangents at each end; at the
// start or end of a stroke, pass the endpoint itself for the missing
// neighbour and the curve simply relaxes to the segment's own direction.
//
// `flatnessPx` is how far the fitted curve may sit from the straight chord
// before it is worth another sample - the sampling is adaptive, so a
// near-straight span costs one segment however long it is, and only real
// curvature costs points.
void AppendFittedSpan(const StrokePoint& p0, const StrokePoint& p1, const StrokePoint& p2,
                      const StrokePoint& p3, float flatnessPx, std::vector<StrokePoint>& out);

}  // namespace sz::core
