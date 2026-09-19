#pragma once

#include <cstdint>
#include <vector>

#include "core/drawing/stroke.h"

namespace sz::core {

// One vertex of a tessellated stroke, in whatever space the points handed to
// BuildStrokeMesh were in. `coverage` is 1 inside the stroke and 0 on its
// outermost edge - the anti-aliasing fringe, which the caller turns into
// alpha. Nothing here knows about a renderer, a color, or a draw list.
struct StrokeVertex {
    float x = 0.0f;
    float y = 0.0f;
    float coverage = 1.0f;
};

// Triangles, as an indexed mesh. Indices are triplets into `vertices`.
struct StrokeMesh {
    std::vector<StrokeVertex> vertices;
    std::vector<uint32_t> indices;
};

// Turns a stroke's centerline into the shape a round pen would actually
// leave: a body of the given width with round caps at both ends, and joins
// that stay the width of the pen however sharp the turn.
//
// Why this exists rather than a polyline call. ImGui builds a thick line by
// offsetting each point along the average of its two adjacent segment
// normals and rescaling by 1/cos^2 of half the turn (IM_FIXNORMAL2F) - a
// miter with no limit worth the name, clamped only at 100x the half width.
// A near-reversal therefore throws the join vertex most of a hundred widths
// out the side of the line - a spike on every tremble of a wide pen. It
// also has no cap but a flat one, and no round join at all.
//
// So the geometry is built here instead:
//   - a miter join while the turn is shallow, which is nearly always after
//     the input has been fitted (see stroke_smoothing.h) - one shared pair
//     of vertices per centerline point, so consecutive segments share an
//     edge rather than overlapping;
//   - a round join past that limit, an arc on the outside of the turn, which
//     is what keeps a hairpin the width of the pen instead of a spike;
//   - a round cap at each end, the mark the pen's own shape implies;
//   - one point becomes a disc, which is the dot.
//
// The result is a single connected mesh with no overlapping triangles along
// its length. That is what makes a *translucent* stroke look like one
// stroke: every overlap is a place where the color gets applied twice. A
// stroke that genuinely crosses over itself still darkens where it crosses
// - that needs the stroke composited as a layer rather than drawn as
// triangles, which is what StrokeRenderMode::Rasterized is for.
//
// `halfWidth` is half the pen width. `fringePx` is how wide the
// anti-aliasing edge should be, in the same space - one pixel, normally.
// A closed centerline (first point equal to last, as a rectangle tool's is)
// is recognised and joined at the seam instead of capped.
StrokeMesh BuildStrokeMesh(const std::vector<StrokePoint>& centerline, float halfWidth, float fringePx);

// The same, into a mesh the caller already owns. `out` is cleared but keeps
// whatever capacity it had, so a mesh rebuilt in place - which is what
// StrokeMeshCache does every time a stroke it holds is edited - reuses its
// two buffers instead of freeing and reallocating them. The value-returning
// overload above is this one into a fresh mesh.
void BuildStrokeMesh(const std::vector<StrokePoint>& centerline, float halfWidth, float fringePx, StrokeMesh& out);

}  // namespace sz::core
