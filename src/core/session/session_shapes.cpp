#include "core/session/session.h"

#include <cmath>
#include <vector>

namespace sz::core {

// ================= Shapes =================

namespace {

// How far from where it began a shape has to be dragged to be meant.
// Anything shorter is a stray click and leaves nothing - see EndShape.
constexpr float kMinShapeLengthPx = 24.0f;

// A straight segment, or a rectangle's outline closed back on its first
// corner. A rectangle with no height or no width is the line it looks
// like: as an outline it went out and back over itself, closed, with a
// zero-length side the joins pinched on - and ink laid twice, darker along
// its whole length where it was translucent.
std::vector<StrokePoint> ShapePoints(Session::Shape shape, float startX, float startY, float endX, float endY) {
    if (shape == Session::Shape::Rectangle && startX != endX && startY != endY) {
        return {
            StrokePoint{startX, startY},
            StrokePoint{endX, startY},
            StrokePoint{endX, endY},
            StrokePoint{startX, endY},
            StrokePoint{startX, startY},
        };
    }
    return {StrokePoint{startX, startY}, StrokePoint{endX, endY}};
}
}  // namespace

void Session::BeginShape(ItemId itemId, Shape shape, float screenX, float screenY, uint32_t colorRGBA,
                         float widthScreenPx) {
    CancelShape();
    Canvas* canvas = Manager().CurrentOrNull();
    if (canvas == nullptr || Manager().FindItemAnywhere(itemId) == nullptr) {
        return;
    }
    shapeItemId_ = itemId;
    shape_ = shape;
    shapeStartX_ = shapeLastX_ = screenX;
    shapeStartY_ = shapeLastY_ = screenY;
    canvas->liveLayer.BeginStroke(StrokePoint{screenX, screenY}, colorRGBA, widthScreenPx);
}

void Session::UpdateShape(float screenX, float screenY) {
    if (!shapeItemId_.has_value()) {
        return;
    }
    shapeLastX_ = screenX;
    shapeLastY_ = screenY;
    // A shape's whole point list is recomputed from its fixed corner on
    // every move, where a freehand stroke appends - so the live stroke is
    // replaced rather than extended.
    Canvas* canvas = Manager().CurrentOrNull();
    if (canvas != nullptr && canvas->liveLayer.ActiveStroke().has_value()) {
        canvas->liveLayer.SetActiveStrokePoints(ShapePoints(shape_, shapeStartX_, shapeStartY_, screenX, screenY));
    }
}

void Session::SetShape(Shape shape) {
    if (!shapeItemId_.has_value() || shape == shape_) {
        return;
    }
    shape_ = shape;
    UpdateShape(shapeLastX_, shapeLastY_);
}

void Session::EndShape(float screenX, float screenY) {
    if (!shapeItemId_.has_value()) {
        return;
    }
    const ItemId itemId = *shapeItemId_;
    shapeItemId_.reset();
    Canvas* canvas = Manager().CurrentOrNull();
    if (canvas == nullptr) {
        return;
    }
    CanvasState& live = canvas->liveLayer;
    const float dx = screenX - shapeStartX_;
    const float dy = screenY - shapeStartY_;
    if (!live.ActiveStroke().has_value() || Manager().FindItemAnywhere(itemId) == nullptr ||
        std::sqrt(dx * dx + dy * dy) < kMinShapeLengthPx) {
        live.CancelActiveStroke();
        return;
    }
    live.SetActiveStrokePoints(ShapePoints(shape_, shapeStartX_, shapeStartY_, screenX, screenY));
    live.EndStroke();
    CommitLiveStroke(itemId);
}

void Session::CancelShape() {
    if (!shapeItemId_.has_value()) {
        return;
    }
    shapeItemId_.reset();
    if (Canvas* canvas = Manager().CurrentOrNull()) {
        canvas->liveLayer.CancelActiveStroke();
    }
}

}  // namespace sz::core
