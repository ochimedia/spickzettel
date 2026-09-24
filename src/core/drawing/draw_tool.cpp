#include "core/drawing/draw_tool.h"

#include <cmath>

#include "core/drawing/stroke_smoothing.h"

namespace sz::core {

namespace {

// Raw mouse positions are not what a stroke wants, and the gap between the
// two is what made a slow, careful line look worse the wider the pen got.
//
// Two things go wrong at once. A mouse reports far faster than a hand can
// move, so consecutive samples of a slow line can be a fraction of a pixel
// apart - and the *direction* between two points that close is quantization
// noise, not the direction the hand is going. And a polyline through those
// samples has a corner at every one of them, which at a wide pen is a corner
// in the outline too.
//
// So: discard a sample that hasn't traveled far enough to mean anything,
// low-pass what is left, and fit a curve through the points that survive.
// All three belong here rather than in CanvasState, which is a container of
// what was drawn, not a judge of what the input meant - and the shape tools,
// which compute their whole point list from two corners, must not be touched
// by any of it. A rectangle's corners are corners.

// How far the pointer must travel before the stroke gains another control
// point. Below this the direction is noise; above it the fitted curve has
// plenty to work with.
constexpr float kMinPointSpacingPx = 2.0f;

// Exponential smoothing on the incoming position: each sample moves the pen
// this fraction of the way toward it. 1.0 would be no smoothing at all; the
// lower it goes the steadier the line and the further the ink trails the
// cursor. Half settles the jitter while keeping the lag to about one sample.
constexpr float kSmoothingFactor = 0.5f;

// How far the fitted curve may sit from the straight chord between two
// control points before it is worth another sample. Sub-pixel, so the curve
// is smooth at any pen width, and adaptive, so a straight span costs one
// segment however long it is.
constexpr float kCurveFlatnessPx = 0.35f;

// How far the release point must be from the last control point for the
// stroke to end exactly there. Smoothing leaves the last control trailing
// the cursor, which on a deliberate line is visible as falling short.
constexpr float kEndPointSnapPx = 1.0f;

float DistanceSquared(const StrokePoint& a, const StrokePoint& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

}  // namespace

DrawTool::DrawTool(uint32_t colorRGBA, float width) : colorRGBA_(colorRGBA), width_(width) {}

// Hands CanvasState the fitted curve for every span whose window of control
// points is complete. Fitting the span from c[i-1] to c[i] needs the point
// either side of it for its tangents, so a span waits until the *next*
// control point exists - one control point of lag, a couple of pixels, which
// is why `final` exists: at the end of the stroke there is no next point, and
// the last span is fitted against a repeat of its own endpoint instead.
void DrawTool::EmitReadySpans(CanvasState& canvas, bool final) {
    while (spansEmitted_ + 1 < controls_.size()) {
        const size_t i = spansEmitted_ + 1;
        const bool hasFollowing = (i + 1) < controls_.size();
        if (!hasFollowing && !final) {
            return;
        }
        const StrokePoint& p1 = controls_[i - 1];
        const StrokePoint& p2 = controls_[i];
        const StrokePoint& p0 = (i >= 2) ? controls_[i - 2] : p1;
        const StrokePoint& p3 = hasFollowing ? controls_[i + 1] : p2;

        fittedScratch_.clear();
        AppendFittedSpan(p0, p1, p2, p3, kCurveFlatnessPx, fittedScratch_);
        for (const StrokePoint& point : fittedScratch_) {
            canvas.ExtendStroke(point);
        }
        ++spansEmitted_;
    }
}

void DrawTool::OnMouseEvent(const platform::MouseEvent& event, CanvasState& canvas) {
    if (event.button != platform::MouseButton::Left) {
        return;
    }

    const StrokePoint point{event.position.x, event.position.y};

    switch (event.kind) {
        case platform::MouseEventKind::Down:
            drawing_ = true;
            smoothed_ = point;
            controls_.clear();
            controls_.push_back(point);
            spansEmitted_ = 0;
            canvas.BeginStroke(point, colorRGBA_, width_);
            break;
        case platform::MouseEventKind::Move:
            if (drawing_) {
                smoothed_ = StrokePoint{smoothed_.x + (point.x - smoothed_.x) * kSmoothingFactor,
                                        smoothed_.y + (point.y - smoothed_.y) * kSmoothingFactor};
                // Measured against the last control point, not the previous
                // sample: a hand creeping forward a third of a pixel at a
                // time still earns a point once it has covered the distance,
                // rather than never.
                if (DistanceSquared(smoothed_, controls_.back()) >=
                    kMinPointSpacingPx * kMinPointSpacingPx) {
                    controls_.push_back(smoothed_);
                    EmitReadySpans(canvas, /*final=*/false);
                }
            }
            break;
        case platform::MouseEventKind::Up:
            if (drawing_) {
                // Only for something that is already a line. A press and
                // release in one spot - or a twitch too small to have earned
                // a second control point - stays a single point on purpose:
                // that is the dot, and snapping the end would turn it into a
                // stub with two ends instead.
                if (controls_.size() >= 2 &&
                    DistanceSquared(point, controls_.back()) >= kEndPointSnapPx * kEndPointSnapPx) {
                    controls_.push_back(point);
                }
                EmitReadySpans(canvas, /*final=*/true);
                canvas.EndStroke();
                drawing_ = false;
                controls_.clear();
                spansEmitted_ = 0;
            }
            break;
    }
}

}  // namespace sz::core
