#pragma once

#include <optional>
#include <vector>

#include "core/drawing/stroke.h"

namespace sz::core {

// Holds the drawing surface's content: a list of completed strokes plus at
// most one stroke currently being drawn. Platform-agnostic; has no
// knowledge of input devices, windows, or rendering APIs.
class CanvasState {
public:
    void BeginStroke(StrokePoint point, uint32_t colorRGBA, float width);
    void ExtendStroke(StrokePoint point);
    // Replaces the in-progress stroke's points wholesale (color/width from
    // BeginStroke are untouched) - for a tool that recomputes its whole
    // shape from scratch every mouse-move (a rectangle/line's live preview,
    // driven off a fixed start point plus the current one) rather than
    // appending one point per move like freehand does via ExtendStroke.
    // No-op if there's no active stroke.
    void SetActiveStrokePoints(std::vector<StrokePoint> points);
    void EndStroke();
    // Discards the in-progress stroke without committing it - for a shape
    // dragged too short to be meant - where EndStroke commits anything with
    // at least one point in it. No-op if there's no active stroke.
    void CancelActiveStroke();
    void Clear();

    // The eraser tool's real effect: clips every completed stroke against
    // the circle centered on `point` with the given `radius` (see
    // ClipStrokeOutsideCircle for the geometry), replacing a stroke that's
    // only partly within it with whatever fragment(s) survive outside it,
    // dropping one entirely consumed by it, and leaving an untouched one
    // exactly as it was.
    void EraseNear(StrokePoint point, float radius);
    // The rectangular-eraser equivalent of EraseNear - same contract,
    // clipped against the axis-aligned rectangle
    // [minX, maxX] x [minY, maxY] instead of a circle (see
    // ClipStrokeOutsideRect for the geometry).
    void EraseRectNear(float minX, float minY, float maxX, float maxY);

    const std::vector<Stroke>& Strokes() const { return strokes_; }
    const std::optional<Stroke>& ActiveStroke() const { return active_; }

private:
    std::vector<Stroke> strokes_;
    std::optional<Stroke> active_;
};

}  // namespace sz::core
