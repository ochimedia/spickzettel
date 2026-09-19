#include "core/session/session.h"

#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include "core/canvas/item_geometry.h"

namespace sz::core {

// ================= Painting =================
//
// The pixel half of the drawing tools: where a pen's marks land when
// AppConfig::paintPixelsInsteadOfStrokes is on, and what the erasers take
// pixels off. The brush itself lives in core/drawing/painted_image.h;
// everything here is about which layer to paint into, what the coordinates
// are, keeping the GPU's copy of the pixels current, and filing the result
// on the undo stack.

namespace {

// How much of a layer's own pixel grid one unit of the item's native space
// buys, when a painted layer is first created. 1 keeps a layer exactly the
// item's native size, which is the honest default: painted pixels don't
// re-render when an item is scaled up, and quietly spending four times the
// memory to postpone that is the user's call, not ours.
constexpr float kDefaultPaintResolutionScale = 1.0f;

// How far from where it began a shape has to be dragged to be meant.
// Anything shorter is a stray click and leaves nothing - see EndShape.
constexpr float kMinShapeLengthPx = 24.0f;

// A painted layer is not allowed to be enormous no matter how the item is
// sized - 4096 on a side is well past a fullscreen capture and is where
// the memory (64 MB at RGBA8) stops being reasonable to hold per item. An
// item larger than this gets a *smaller resolution scale*, not a cropped
// bitmap - see FitResolutionScale for why the distinction is the
// difference between a stroke landing under the pen and a quarter of the
// way across the screen from it.
constexpr int kMaxPaintedExtent = 4096;

}  // namespace

Layer* Session::FindPaintedLayer(Item& item, size_t* outIndex) {
    // Whatever state it is in - with pixels loaded or without. Callers that
    // need pixels ask Layer::HasPaintedPixels; asking it here instead is
    // what made EnsurePaintedLayer miss a pixel-less layer and add another.
    for (size_t i = 0; i < item.layers.size(); ++i) {
        if (item.layers[i].kind == LayerKind::Painted) {
            if (outIndex) {
                *outIndex = i;
            }
            return &item.layers[i];
        }
    }
    return nullptr;
}

Layer* Session::EnsurePaintedLayer(Item& item) {
    Layer* existing = FindPaintedLayer(item);
    if (existing && existing->HasPaintedPixels() && existing->textureHandle != 0) {
        return existing;
    }
    if (!window_ || item.nativeW <= 0.0f || item.nativeH <= 0.0f) {
        return nullptr;
    }

    if (existing) {
        // The item already has its painted layer, just not in a paintable
        // state. Without pixels: its file was never written (a save that
        // failed before the app closed) or has gone missing, so the record
        // loaded with nothing to load - it is still *the* painted layer of
        // this item, so it gets fresh pixels rather than a second layer
        // pushed beside it, which would leave a dead layer in its record
        // for good. Without a texture: a
        // copy whose pixels came with it but whose texture didn't - give it
        // one here rather than painting into something invisible.
        if (!existing->HasPaintedPixels()) {
            // Its own scale where that still fits the cap - see the fresh
            // layer below for why the scale is what gets capped.
            const float requested =
                existing->resolutionScale > 0.0f ? existing->resolutionScale : kDefaultPaintResolutionScale;
            const float scale = FitResolutionScale(item.nativeW, item.nativeH, requested, kMaxPaintedExtent);
            existing->resolutionScale = scale;
            existing->painted = std::make_shared<PaintedImage>(ScaledPixelExtent(item.nativeW, scale),
                                                                ScaledPixelExtent(item.nativeH, scale));
        }
        if (existing->textureHandle == 0) {
            existing->textureHandle = window_->CreateTextureFromPixels(
                existing->painted->PixelsRGBA().data(), existing->painted->Width(), existing->painted->Height());
            if (existing->textureHandle == 0) {
                return nullptr;  // same rule as below: better to paint nowhere than to paint blind
            }
        }
        return existing;
    }

    // The scale is what gets capped, not the bitmap: ScreenToPaintedPixels
    // multiplies by the layer's own resolutionScale, so a layer that came
    // out smaller than native stays aligned with the pen for free.
    const float scale = FitResolutionScale(item.nativeW, item.nativeH, kDefaultPaintResolutionScale, kMaxPaintedExtent);
    const int width = ScaledPixelExtent(item.nativeW, scale);
    const int height = ScaledPixelExtent(item.nativeH, scale);

    Layer painted;
    painted.kind = LayerKind::Painted;
    // Fully opaque as a *layer*: transparency belongs to the pixels, which
    // start out entirely clear, so a fresh painted layer adds nothing to
    // what is underneath until something is drawn on it. Starting the
    // layer itself dimmed would silently wash out every mark.
    painted.opacity = 1.0f;
    painted.resolutionScale = scale;
    painted.painted = std::make_shared<PaintedImage>(width, height);
    painted.textureHandle =
        window_->CreateTextureFromPixels(painted.painted->PixelsRGBA().data(), width, height);
    if (painted.textureHandle == 0) {
        return nullptr;  // nothing would be visible; better to paint nowhere than to paint blind
    }
    // On top of everything already there - a screenshot underneath, and
    // whatever else has been painted before it.
    item.layers.push_back(std::move(painted));
    Manager().MarkChanged();
    return &item.layers.back();
}

void Session::ScreenToPaintedPixels(const Item& item, const Layer& layer, float screenX, float screenY, float& outX,
                                    float& outY) const {
    // Screen -> the item's own native space, through the same transform a
    // stroke is baked with (see ScreenToNative), then -> the layer's pixel
    // grid.
    const NativePoint native = ScreenToNative(item, screenX, screenY);
    outX = native.x * layer.resolutionScale;
    outY = native.y * layer.resolutionScale;
}

void Session::UploadPaintedRegion(Layer& layer, const PixelRect& region) {
    if (region.Empty() || !layer.painted) {
        return;
    }
    // The one place every path that changes painted pixels passes through,
    // which is why the dirty flag is set here rather than at each of them.
    // Missing one would mean losing those pixels the next time their canvas
    // stops being current.
    layer.paintedDirty = true;
    if (!window_ || layer.textureHandle == 0) {
        return;
    }
    window_->UpdateTextureRegion(layer.textureHandle, layer.painted->PixelsRGBA().data(), layer.painted->Width(),
                                  region.x, region.y, region.w, region.h);
}

void Session::BeginPaintStroke(Item& item, float screenX, float screenY, bool erase, uint32_t colorRGBA,
                               float widthScreenPx) {
    // Whatever was in progress is over the moment a new gesture starts -
    // and goes on the stack as its own entry, since it was its own gesture.
    PushPaintedTilesUndo(EndPaintStroke());
    // A pen creates the layer it needs; an eraser only ever acts on one
    // that is already there with pixels in it - erasing on a snippet
    // nothing was painted on must not leave a blank layer behind, so it
    // starts no session and the Extends that follow do nothing.
    Layer* layer = erase ? FindPaintedLayer(item) : EnsurePaintedLayer(item);
    if (!layer || !layer->HasPaintedPixels()) {
        return;
    }
    // The brush radius travels the same road the coordinates do - the one
    // screen-to-native transform, then the layer's own grid - so a pen set
    // to 20px covers 20 screen pixels whatever the item is scaled to.
    const float pixelsPerScreenPx = ScreenToNative(item, screenX, screenY).scale * layer->resolutionScale;
    const float radius = widthScreenPx * 0.5f * pixelsPerScreenPx;

    layer->painted->BeginStroke(erase ? 0xFFFFFFFFu : colorRGBA, radius,
                                 erase ? PaintedImage::BrushMode::Erase : PaintedImage::BrushMode::Paint);
    paintStrokeItemId_ = item.id;
    paintStrokeLayerIndex_ = static_cast<size_t>(layer - item.layers.data());
    paintStrokeTouched_ = false;

    float x = 0.0f;
    float y = 0.0f;
    ScreenToPaintedPixels(item, *layer, screenX, screenY, x, y);
    // A dab at the down point, so a click leaves a dot rather than nothing
    // - the same rule the vector pen follows (see CanvasState::EndStroke).
    const PixelRect dirty = layer->painted->ExtendStroke(x, y, x, y);
    if (!dirty.Empty()) {
        paintStrokeTouched_ = true;
        UploadPaintedRegion(*layer, dirty);
    }
    // Kept in *screen* space, not layer pixels: a layer's own grid is one
    // conversion away and different per layer, and the gesture is defined
    // in the space the hand moves in.
    paintLastScreenX_ = screenX;
    paintLastScreenY_ = screenY;
}

void Session::ExtendPaintStroke(float screenX, float screenY) {
    if (!paintStrokeItemId_.has_value()) {
        return;
    }
    Item* item = Manager().FindItemAnywhere(*paintStrokeItemId_);
    if (!item || paintStrokeLayerIndex_ >= item->layers.size()) {
        return;
    }
    Layer& layer = item->layers[paintStrokeLayerIndex_];
    if (!layer.painted) {
        return;
    }
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    ScreenToPaintedPixels(*item, layer, paintLastScreenX_, paintLastScreenY_, x0, y0);
    ScreenToPaintedPixels(*item, layer, screenX, screenY, x1, y1);
    const PixelRect dirty = layer.painted->ExtendStroke(x0, y0, x1, y1);
    paintLastScreenX_ = screenX;
    paintLastScreenY_ = screenY;
    if (dirty.Empty()) {
        return;
    }
    paintStrokeTouched_ = true;
    UploadPaintedRegion(layer, dirty);
}

Session::PaintedUndo Session::EndPaintStroke() {
    PaintedUndo result;
    if (!paintStrokeItemId_.has_value()) {
        return result;
    }
    result.itemId = *paintStrokeItemId_;
    result.layerIndex = paintStrokeLayerIndex_;
    const bool touched = paintStrokeTouched_;
    paintStrokeItemId_.reset();
    paintStrokeTouched_ = false;

    Item* item = Manager().FindItemAnywhere(result.itemId);
    if (!item || result.layerIndex >= item->layers.size() || !item->layers[result.layerIndex].painted) {
        return result;
    }
    std::vector<PaintedTile> before = item->layers[result.layerIndex].painted->EndStroke();
    if (!touched) {
        return result;  // a gesture that marked nothing leaves no undo entry
    }
    result.tiles = std::move(before);
    if (!result.Empty()) {
        Manager().MarkChanged();
    }
    return result;
}

void Session::PushPaintedTilesUndo(PaintedUndo painted) {
    if (painted.Empty()) {
        return;
    }
    UndoEntry entry;
    entry.kind = UndoEntry::Kind::PaintedTilesChanged;
    entry.itemId = painted.itemId;
    entry.layerIndex = painted.layerIndex;
    entry.paintedTiles = std::move(painted.tiles);
    entry.paintedBefore = std::move(painted.wholeImage);
    PushUndo(std::move(entry));
}

Session::PaintedUndo Session::ErasePaintedLayersInRect(Item& item, float minX, float minY, float maxX, float maxY) {
    PaintedUndo result;
    size_t index = 0;
    Layer* found = FindPaintedLayer(item, &index);
    if (!found || !found->HasPaintedPixels()) {
        return result;
    }
    Layer& layer = *found;

    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    ScreenToPaintedPixels(item, layer, minX, minY, x0, y0);
    ScreenToPaintedPixels(item, layer, maxX, maxY, x1, y1);

    // A whole gesture in one call - the rect eraser only acts at the end,
    // against its final rectangle - so the session opens and closes here.
    // The radius is meaningless for a rectangle; the value is a placeholder
    // that ExtendRect never reads.
    PushPaintedTilesUndo(EndPaintStroke());
    layer.painted->BeginStroke(0xFFFFFFFFu, 1.0f, PaintedImage::BrushMode::Erase);
    const PixelRect dirty = layer.painted->ExtendRect(x0, y0, x1, y1);
    std::vector<PaintedTile> before = layer.painted->EndStroke();
    if (dirty.Empty() || before.empty()) {
        return result;
    }
    UploadPaintedRegion(layer, dirty);
    Manager().MarkChanged();

    result.itemId = item.id;
    result.layerIndex = index;
    result.tiles = std::move(before);
    return result;
}

Session::PaintedUndo Session::ClearPaintedLayers(Item& item) {
    PaintedUndo result;
    size_t index = 0;
    Layer* found = FindPaintedLayer(item, &index);
    if (!found || !found->HasPaintedPixels()) {
        return result;
    }
    Layer& layer = *found;

    // Swapped for a fresh transparent image of the same size rather than
    // erased tile by tile: clearing touches every tile there is, and the
    // pixels already live behind a shared_ptr, so handing the old image
    // back for the undo entry costs a pointer.
    PushPaintedTilesUndo(EndPaintStroke());
    result.itemId = item.id;
    result.layerIndex = index;
    result.wholeImage = layer.painted;

    layer.painted = std::make_shared<PaintedImage>(layer.painted->Width(), layer.painted->Height());
    UploadPaintedRegion(layer, PixelRect{0, 0, layer.painted->Width(), layer.painted->Height()});
    Manager().MarkChanged();
    return result;
}

// ----- The brush, as a gesture -----

void Session::BeginPaint(ItemId itemId, float screenX, float screenY, uint32_t colorRGBA, float widthScreenPx) {
    Item* item = Manager().FindItemAnywhere(itemId);
    if (!item) {
        return;
    }
    BeginPaintStroke(*item, screenX, screenY, /*erase=*/false, colorRGBA, widthScreenPx);
}

void Session::ExtendPaint(float screenX, float screenY) { ExtendPaintStroke(screenX, screenY); }

void Session::EndPaint() { PushPaintedTilesUndo(EndPaintStroke()); }

// ----- Shapes, as a gesture -----

namespace {
// A straight segment, or a rectangle's outline closed back on its first
// corner.
std::vector<StrokePoint> ShapePoints(Session::Shape shape, float startX, float startY, float endX, float endY) {
    if (shape == Session::Shape::Rectangle) {
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
                         float widthScreenPx, bool paintPixels) {
    CancelShape();
    Canvas* canvas = Manager().CurrentOrNull();
    if (canvas == nullptr || Manager().FindItemAnywhere(itemId) == nullptr) {
        return;
    }
    shapeItemId_ = itemId;
    shape_ = shape;
    shapeStartX_ = shapeLastX_ = screenX;
    shapeStartY_ = shapeLastY_ = screenY;
    shapeColorRGBA_ = colorRGBA;
    shapeWidth_ = widthScreenPx;
    shapePaintsPixels_ = paintPixels;
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
    const std::vector<StrokePoint> points = ShapePoints(shape_, shapeStartX_, shapeStartY_, screenX, screenY);
    if (shapePaintsPixels_) {
        // Ink like any other in bitmap mode: the finished shape painted in.
        // Corners come out round rather than mitred, since the brush knows
        // one shape - the same way the rasterized stroke renderer draws them.
        live.CancelActiveStroke();
        BeginPaint(itemId, points.front().x, points.front().y, shapeColorRGBA_, shapeWidth_);
        for (size_t i = 1; i < points.size(); ++i) {
            ExtendPaint(points[i].x, points[i].y);
        }
        EndPaint();
        return;
    }
    live.SetActiveStrokePoints(points);
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
