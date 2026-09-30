#include "core/drawing/canvas_state.h"

#include <utility>

namespace sz::core {

void CanvasState::BeginStroke(StrokePoint point, uint32_t colorRGBA, float width, StrokeCorners corners) {
    Stroke stroke;
    stroke.colorRGBA = colorRGBA;
    stroke.width = width;
    stroke.corners = corners;
    stroke.points.push_back(point);
    active_ = std::move(stroke);
}

void CanvasState::ExtendStroke(StrokePoint point) {
    if (!active_.has_value()) {
        return;
    }
    active_->points.push_back(point);
}

void CanvasState::SetActiveStrokePoints(std::vector<StrokePoint> points) {
    if (!active_.has_value()) {
        return;
    }
    active_->points = std::move(points);
}

void CanvasState::CancelActiveStroke() { active_.reset(); }

void CanvasState::EndStroke() {
    if (!active_.has_value()) {
        return;
    }
    // One point is a legal stroke: it is a dot, the mark a round pen makes
    // when set down and lifted without traveling, rendered as a disc of
    // the stroke's own width. Empty is dropped: SetActiveStrokePoints can
    // leave a stroke with no points, and that is nothing rather than a mark.
    if (!active_->points.empty()) {
        strokes_.push_back(std::move(*active_));
    }
    active_.reset();
}

void CanvasState::Clear() {
    strokes_.clear();
    active_.reset();
}

}  // namespace sz::core
