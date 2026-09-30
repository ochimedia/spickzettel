#pragma once

#include <optional>
#include <vector>

#include "core/drawing/stroke.h"

namespace sz::core {

// The eraser's hit-and-cut logic: what is left of `stroke` after removing
// everything within `radius` of `center`, by clipping its polyline against
// the circle rather than testing whether any single point is close enough.
// A point test misses a segment that passes through the circle with both
// ends outside it - which is what a line's or a rectangle's edge looks
// like - and deletes far more of a freehand stroke than the eraser
// covered. A straight segment can enter and leave the circle once each, so
// one stroke can split into any number of surviving fragments: erasing the
// middle of a squiggle leaves two shorter strokes, erasing partway along a
// rectangle's edge shortens it right there.
//
// Returns std::nullopt if the circle doesn't touch `stroke` at all (no
// point inside it, no segment passing through it) - the caller's cue to
// leave the stroke alone rather than replace it with an identical copy.
// Otherwise returns the surviving fragments, each keeping stroke's own
// colorRGBA/width/corners, in original point order; a fragment that would end up
// with fewer than 2 points is dropped, so an empty (but present) vector
// means the whole stroke was erased. Cutting a line down to a single
// surviving sample leaves nothing rather than a dot: a dot is something
// you place (see CanvasState::EndStroke), not a crumb the eraser leaves
// behind.
//
// A stroke that *is* a single point - a dot - is all or nothing: erased
// whole if the region contains it, untouched otherwise. It has no segment
// for the clip walk to find a crossing on, and before that was handled it
// was the one mark the eraser could not remove.
std::optional<std::vector<Stroke>> ClipStrokeOutsideCircle(const Stroke& stroke, StrokePoint center, float radius);

// The same, against everything within `radius` of the segment from `from`
// to `to` - the circle swept along it, a capsule: where the eraser passed
// between two of its positions. Movement comes once a frame, and a quick
// hand moves the eraser farther than its width between two; clipped
// against a circle at each, a line crossed in between was never touched.
// A capsule is convex, as the circle and the rectangle are, so the same
// walk does it. `from` and `to` the same point is the circle.
std::optional<std::vector<Stroke>> ClipStrokeOutsideCapsule(const Stroke& stroke, StrokePoint from, StrokePoint to,
                                                            float radius);

// The rectangular-eraser equivalent of ClipStrokeOutsideCircle - same
// contract (nullopt if the region never touches the stroke; otherwise the
// surviving fragments, a present empty vector meaning the whole stroke
// was erased), clipped against an axis-aligned rectangle
// [minX, maxX] x [minY, maxY] instead of a circle. Plain floats rather
// than core::Rect: core/drawing is a dependency of core/canvas, not the
// other way around.
std::optional<std::vector<Stroke>> ClipStrokeOutsideRect(const Stroke& stroke, float minX, float minY, float maxX,
                                                          float maxY);

}  // namespace sz::core
