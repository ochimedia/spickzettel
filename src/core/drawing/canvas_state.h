#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "core/drawing/stroke.h"

namespace sz::core {

// A list of completed strokes plus at most one stroke currently being
// drawn: the session's live layer, where a stroke or a shape is drawn
// before it is committed to its snippet (see Session::LiveLayer).
// Platform-agnostic; has no knowledge of input devices, windows, or
// rendering APIs.
class CanvasState {
public:
    void BeginStroke(StrokePoint point, uint32_t colorRGBA, float width,
                     StrokeCorners corners = StrokeCorners::Round);
    void ExtendStroke(StrokePoint point);
    // A provisional end for the in-progress stroke: drawn after its points,
    // and dropped by the next ExtendStroke, SetActiveStrokeTail or
    // EndStroke, so it is never part of what is committed. For a tool whose
    // points come in later than the pointer moves - see DrawTool. No-op if
    // there's no active stroke.
    void SetActiveStrokeTail(const std::vector<StrokePoint>& tail);
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

    const std::vector<Stroke>& Strokes() const { return strokes_; }
    const std::optional<Stroke>& ActiveStroke() const { return active_; }

private:
    std::vector<Stroke> strokes_;
    std::optional<Stroke> active_;
    // How many of active_'s points are its own; the rest are the tail.
    size_t activeOwnPoints_ = 0;
};

}  // namespace sz::core
