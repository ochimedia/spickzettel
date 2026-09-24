#include "core/drawing/canvas_state.h"

#include <utility>

#include "core/drawing/stroke_clip.h"

namespace sz::core {

void CanvasState::BeginStroke(StrokePoint point, uint32_t colorRGBA, float width) {
    Stroke stroke;
    stroke.colorRGBA = colorRGBA;
    stroke.width = width;
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

void CanvasState::EraseNear(StrokePoint point, float radius) {
    std::vector<Stroke> result;
    result.reserve(strokes_.size());
    for (Stroke& stroke : strokes_) {
        std::optional<std::vector<Stroke>> clipped = ClipStrokeOutsideCircle(stroke, point, radius);
        if (!clipped.has_value()) {
            result.push_back(std::move(stroke));
            continue;
        }
        for (Stroke& fragment : *clipped) {
            result.push_back(std::move(fragment));
        }
    }
    strokes_ = std::move(result);
}

void CanvasState::EraseRectNear(float minX, float minY, float maxX, float maxY) {
    std::vector<Stroke> result;
    result.reserve(strokes_.size());
    for (Stroke& stroke : strokes_) {
        std::optional<std::vector<Stroke>> clipped = ClipStrokeOutsideRect(stroke, minX, minY, maxX, maxY);
        if (!clipped.has_value()) {
            result.push_back(std::move(stroke));
            continue;
        }
        for (Stroke& fragment : *clipped) {
            result.push_back(std::move(fragment));
        }
    }
    strokes_ = std::move(result);
}

}  // namespace sz::core
