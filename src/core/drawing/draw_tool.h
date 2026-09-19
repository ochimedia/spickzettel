#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/drawing/canvas_state.h"
#include "platform/platform_types.h"

namespace sz::core {

// Translates raw mouse events into CanvasState stroke operations for the
// freehand pen, in whatever colour and width are currently set. Only the
// left mouse button draws.
//
// Also where freehand input is cleaned up before it becomes geometry -
// minimum spacing, smoothing, and the curve fitted through what survives
// (see stroke_smoothing.h, and the .cpp's own constants for why a raw
// sample stream makes a bad stroke). CanvasState deliberately stays dumb
// about all of it: it records what it is given, so a test - or a shape
// tool, whose corners must stay corners - can hand it exact points and get
// exactly those back.
class DrawTool {
public:
    DrawTool(uint32_t colorRGBA, float width);

    void OnMouseEvent(const platform::MouseEvent& event, CanvasState& canvas);

    void SetColor(uint32_t colorRGBA) { colorRGBA_ = colorRGBA; }
    void SetWidth(float width) { width_ = width; }

private:
    // See its own comment in the .cpp: hands the canvas the fitted curve for
    // every span whose window of control points is complete.
    void EmitReadySpans(CanvasState& canvas, bool final);

    uint32_t colorRGBA_;
    float width_;
    bool drawing_ = false;
    // The smoothed pen position, which lags the real pointer slightly. Only
    // meaningful while drawing_.
    StrokePoint smoothed_{};
    // The points the curve is fitted through - what is left of the input
    // after smoothing and the spacing filter - and how many of the spans
    // between them have been handed over already.
    std::vector<StrokePoint> controls_;
    size_t spansEmitted_ = 0;
    // Reused across spans so a stroke doesn't allocate per mouse-move.
    std::vector<StrokePoint> fittedScratch_;
};

}  // namespace sz::core
