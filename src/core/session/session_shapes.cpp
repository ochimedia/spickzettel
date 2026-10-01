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

void Session::BeginShape(const std::vector<ItemId>& itemIds, Shape shape, float screenX, float screenY,
                         uint32_t colorRGBA, float widthScreenPx) {
    if (!EndOpenGesture()) {
        return;
    }
    if (Model().CurrentOrNull() == nullptr || itemIds.empty() || Model().FindItemAnywhere(itemIds.front()) == nullptr) {
        return;
    }
    shapeItems_ = itemIds;
    shape_ = shape;
    shapeStartX_ = shapeLastX_ = screenX;
    shapeStartY_ = shapeLastY_ = screenY;
    // A rectangle's corners stay square (see StrokeCorners). A line has none,
    // and may become a rectangle before it is let go (see SetShape).
    liveLayer_.BeginStroke(StrokePoint{screenX, screenY}, colorRGBA, widthScreenPx, StrokeCorners::Sharp);
}

void Session::UpdateShape(float screenX, float screenY) {
    if (shapeItems_.empty()) {
        return;
    }
    shapeLastX_ = screenX;
    shapeLastY_ = screenY;
    // A shape's whole point list is recomputed from its fixed corner on
    // every move, where a freehand stroke appends - so the live stroke is
    // replaced rather than extended.
    if (liveLayer_.ActiveStroke().has_value()) {
        liveLayer_.SetActiveStrokePoints(ShapePoints(shape_, shapeStartX_, shapeStartY_, screenX, screenY));
    }
}

void Session::SetShape(Shape shape) {
    if (shapeItems_.empty() || shape == shape_) {
        return;
    }
    shape_ = shape;
    UpdateShape(shapeLastX_, shapeLastY_);
}

void Session::EndShape(float screenX, float screenY) {
    if (shapeItems_.empty()) {
        return;
    }
    const std::vector<ItemId> itemIds = std::move(shapeItems_);
    shapeItems_.clear();
    CanvasState& live = liveLayer_;
    const float dx = screenX - shapeStartX_;
    const float dy = screenY - shapeStartY_;
    if (!live.ActiveStroke().has_value() || Model().FindItemAnywhere(itemIds.front()) == nullptr ||
        std::sqrt(dx * dx + dy * dy) < kMinShapeLengthPx) {
        live.CancelActiveStroke();
        return;
    }
    live.SetActiveStrokePoints(ShapePoints(shape_, shapeStartX_, shapeStartY_, screenX, screenY));
    live.EndStroke();
    CommitLiveStroke(itemIds);
}

void Session::CancelShape() {
    if (shapeItems_.empty()) {
        return;
    }
    shapeItems_.clear();
    liveLayer_.CancelActiveStroke();
}

}  // namespace sz::core
