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

// Appends the fitted curve for the spans of `controls` after the first
// `fitted`, and returns how many spans that now makes. Fitting the span from
// c[i-1] to c[i] needs the point either side of it for its tangents, so a
// span waits until the *next* control point exists - one control point of
// lag, a couple of pixels, which is why `final` exists: at the end of the
// stroke there is no next point, and the last span is fitted against a
// repeat of its own endpoint instead.
size_t FitSpans(const std::vector<StrokePoint>& controls, size_t fitted, bool final,
                std::vector<StrokePoint>& out) {
    while (fitted + 1 < controls.size()) {
        const size_t i = fitted + 1;
        const bool hasFollowing = (i + 1) < controls.size();
        if (!hasFollowing && !final) {
            break;
        }
        const StrokePoint& p1 = controls[i - 1];
        const StrokePoint& p2 = controls[i];
        const StrokePoint& p0 = (i >= 2) ? controls[i - 2] : p1;
        const StrokePoint& p3 = hasFollowing ? controls[i + 1] : p2;
        AppendFittedSpan(p0, p1, p2, p3, kCurveFlatnessPx, out);
        ++fitted;
    }
    return fitted;
}

}  // namespace

DrawTool::DrawTool(uint32_t colorRGBA, float width) : colorRGBA_(colorRGBA), width_(width) {}

// Hands CanvasState the fitted curve for every span whose window of control
// points is complete - see FitSpans.
void DrawTool::EmitReadySpans(CanvasState& canvas, bool final) {
    fittedScratch_.clear();
    spansEmitted_ = FitSpans(controls_, spansEmitted_, final, fittedScratch_);
    for (const StrokePoint& point : fittedScratch_) {
        canvas.ExtendStroke(point);
    }
}

// What the stroke would end with if the pen were lifted at `pen` now: the
// spans still waiting for their next control point, fitted as at the end,
// and on to the pen itself, as a release there would. Drawn after the
// stroke until the next move replaces it.
//
// Without it the ink trailed the pointer by a control point and the
// smoothing's lag, a few pixels, and caught up only once the next control
// point arrived. At a turn that is after the hand has turned: the stroke
// went on growing the old way while the pointer went the new one, which
// read as the pen overshooting what was meant. Measured on 2026-10-02 at a
// held V: 5px short while held, 3px of it drawn after turning back.
void DrawTool::UpdateTail(CanvasState& canvas, StrokePoint pen) {
    // From the control point before the first waiting span's start, which
    // that span's tangent needs.
    const size_t from = spansEmitted_ >= 1 ? spansEmitted_ - 1 : 0;
    tailControls_.assign(controls_.begin() + static_cast<std::ptrdiff_t>(from), controls_.end());
    // Only for something that is already a line, as at the release: before
    // the second control point a lift leaves the dot, and a tail to the pen
    // showed a stub that the lift then took back.
    if (controls_.size() >= 2 && DistanceSquared(pen, controls_.back()) >= kEndPointSnapPx * kEndPointSnapPx) {
        tailControls_.push_back(pen);
    }
    fittedScratch_.clear();
    FitSpans(tailControls_, spansEmitted_ - from, /*final=*/true, fittedScratch_);
    canvas.SetActiveStrokeTail(fittedScratch_);
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
                UpdateTail(canvas, point);
            }
            break;
        case platform::MouseEventKind::Up:
            if (drawing_) {
                // Only for something that is already a line. A press and
                // release in one spot - or a twitch too small to have earned
                // a second control point - stays a single point on purpose:
                // that is the dot, and snapping the end would turn it into a
                // stub with two ends instead.
                //
                // Or for a release far enough from the press to be one.
                // The pointer is read once a frame, and a quick stroke, or
                // any stroke during a hitch, can be pressed, moved and
                // released between two frames: no move reaches the pen, or
                // only a first one too short to earn a point. Far enough is
                // where a single move would have earned one through the
                // smoothing, which goes half the way: twice the spacing.
                const float distanceSquared = DistanceSquared(point, controls_.back());
                const float lineFromPressPx = 2.0f * kMinPointSpacingPx;
                if ((controls_.size() >= 2 && distanceSquared >= kEndPointSnapPx * kEndPointSnapPx) ||
                    (controls_.size() == 1 && distanceSquared >= lineFromPressPx * lineFromPressPx)) {
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
