#include "ui/overlay_app.h"
#include "ui/overlay_app_internal.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "core/canvas/item_geometry.h"

namespace sz::ui {

// ================= Stroke rasters and overview previews =================
//
// GPU-side caches the UI keeps for drawing: the bitmaps strokes are drawn
// into in the rasterized render mode, and the thumbnail textures the
// Overview shows. Painting itself - putting pixels into a snippet's painted
// layer - is the session's (see core/session/session_paint.cpp).

namespace {

// A painted layer is not allowed to be enormous no matter how the item is
// sized - 4096 on a side is well past a fullscreen capture and is where
// the memory (64 MB at RGBA8) stops being reasonable to hold per item. An
// item larger than this gets a *smaller resolution scale*, not a cropped
// bitmap - see FitResolutionScale for why the distinction is the
// difference between a stroke landing under the pen and a quarter of the
// way across the screen from it.
constexpr int kMaxPaintedExtent = 4096;

}  // namespace

// ================= Overview bitmap previews =================

namespace {

// How many *full* images may be decoded in one frame. A fullscreen QOI is
// ~7ms, so a library of dozens would stall the panel on the frame it opens
// - which is the frame it can least afford to. Spread over frames instead:
// the thumbnails fill in over a fraction of a second and the panel stays
// responsive throughout.
//
// This is the fallback path. An image written by this version has a
// thumbnail beside it (see LibraryStore::SaveThumbnail), and those are read
// under a much larger budget below, because a 256px QOI decodes in well
// under a millisecond.
constexpr int kOverviewFullDecodesPerFrame = 2;

// The same, for thumbnails. High enough that a normal library fills in on
// the first frame - which is the whole point of writing them - and bounded
// anyway, because "how many canvases are in this folder" has no upper
// limit and a first frame that decodes two thousand of anything is a
// stall whatever each one costs.
constexpr int kOverviewThumbnailLoadsPerFrame = 48;

}  // namespace

void OverlayApp::BeginOverviewPreviewFrame() {
    const bool wanted = Cfg().overviewShowsBitmaps;
    layerPreviewLoadBudget_ = wanted ? kOverviewFullDecodesPerFrame : 0;
    layerPreviewThumbnailBudget_ = wanted ? kOverviewThumbnailLoadsPerFrame : 0;
}

OverlayApp::PreviewTextureFn OverlayApp::PreviewTextureLookup() {
    if (!Cfg().overviewShowsBitmaps) {
        return {};
    }
    return [this](const Item& item, size_t index) { return LayerPreviewTexture(item, index); };
}

std::optional<uint64_t> OverlayApp::LayerPreviewTexture(const Item& item, size_t layerIndex) {
    if (layerIndex >= item.layers.size() || !window_ || !Store()) {
        return 0;
    }
    const Layer& layer = item.layers[layerIndex];
    // The current canvas's layers already have their full-size texture
    // loaded, and drawing that scaled into a 200px tile costs nothing extra
    // - no decode, no second texture. Which is also the canvas most likely
    // to be looked at in the overview.
    if (layer.textureHandle != 0) {
        return layer.textureHandle;
    }

    const LayerKey key{item.id, layerIndex};
    const auto existing = layerPreviews_.find(key);
    if (existing != layerPreviews_.end()) {
        return existing->second.textureHandle;  // 0 if it failed, which stops it being retried
    }
    if (layer.imageFile.empty()) {
        return 0;  // never had pixels of its own: the placeholder is the answer
    }

    // The sidecar first, and on its own budget: this is the path an image
    // written by this version takes, and it is cheap enough that a whole
    // folder's worth lands on the first frame.
    if (layerPreviewThumbnailBudget_ > 0) {
        --layerPreviewThumbnailBudget_;
        if (const std::optional<persistence::DecodedImage> thumb =
                Store()->LoadThumbnail(item.id, layer.imageFile)) {
            LayerPreview preview;
            preview.textureHandle =
                window_->CreateTextureFromPixels(thumb->pixelsRGBA.data(), thumb->width, thumb->height);
            layerPreviews_.emplace(key, preview);
            return preview.textureHandle;
        }
    } else {
        return std::nullopt;  // even the cheap path is spoken for this frame
    }

    // No sidecar: its thumbnail was never written or didn't survive. Decode the real thing under the small
    // budget, and write the sidecar on the way out so this is the last time
    // this image costs that.
    if (layerPreviewLoadBudget_ <= 0) {
        // Its turn is next frame, or the one after. Distinct from the 0
        // above, and the caller draws the two differently: a stand-in for
        // an image that is arriving shortly is a wrong thumbnail that
        // corrects itself, which is what the placeholder gradient looked
        // like for the first few frames of every Overview visit.
        return std::nullopt;
    }
    --layerPreviewLoadBudget_;

    LayerPreview preview;
    if (const std::optional<persistence::DecodedImage> decoded = Store()->LoadImage(item.id, layer.imageFile)) {
        const persistence::DecodedImage small =
            persistence::DownscaleToFit(*decoded, persistence::LibraryStore::kThumbnailMaxExtent);
        preview.textureHandle =
            window_->CreateTextureFromPixels(small.pixelsRGBA.data(), small.width, small.height);
        Store()->SaveThumbnail(item.id, layer.imageFile, small);
    }
    layerPreviews_.emplace(key, preview);
    return preview.textureHandle;
}

void OverlayApp::ReleaseLayerPreviews() {
    if (window_) {
        for (auto& [key, preview] : layerPreviews_) {
            (void)key;
            if (preview.textureHandle != 0) {
                window_->ReleaseTexture(preview.textureHandle);
            }
        }
    }
    layerPreviews_.clear();
}

// ================= Rasterized vector strokes =================

void OverlayApp::BuildStrokeRaster(const Item& item, StrokeRaster& raster) {
    // Native-sized when that fits the cap, uniformly smaller when it
    // doesn't - and then every coordinate below goes through the same
    // scale, so the raster stays in register with the strokes whatever
    // size it came out at.
    const float scale = FitResolutionScale(item.nativeW, item.nativeH, 1.0f, kMaxPaintedExtent);
    const int width = ScaledPixelExtent(item.nativeW, scale);
    const int height = ScaledPixelExtent(item.nativeH, scale);

    // Only new strokes to draw? Put them on top of what is already there.
    // Finishing a stroke is by far the most common reason to get here, and
    // redrawing every earlier stroke each time would make an item cost more
    // with every mark ever made on it.
    //
    // "Only new strokes" has to mean the ones already drawn are untouched,
    // which a count cannot say: undo, redo and the eraser all rewrite the
    // list, and the eraser can rewrite the middle of it without changing
    // its length. So the strokes this raster was built from are compared
    // against the ones now there - element by element, and each of those
    // rejects on point count before it looks at a single point.
    const bool sameShape = raster.nativeW == item.nativeW && raster.nativeH == item.nativeH &&
                           !raster.pixels.Empty();
    const bool prefixUnchanged =
        raster.builtFrom.size() <= item.strokes.size() &&
        std::equal(raster.builtFrom.begin(), raster.builtFrom.end(), item.strokes.begin());
    // The same comparison answers "is there anything to do at all": the
    // whole list unchanged, the same shape, and a texture to show it with.
    // Asked here rather than by the caller - one comparison that decides
    // both whether and how much to draw.
    const bool upToDate = sameShape && prefixUnchanged && raster.builtFrom.size() == item.strokes.size() &&
                          raster.textureHandle != 0;
    if (upToDate) {
        return;
    }
    size_t firstStroke = 0;
    if (sameShape && prefixUnchanged) {
        // Possibly nothing new to draw at all - the one way here with an
        // unchanged list is a texture that failed to come into being, and
        // the upload below is the retry.
        firstStroke = raster.builtFrom.size();
    } else {
        raster.pixels = PaintedImage(width, height);
        if (raster.textureHandle != 0 && window_) {
            window_->ReleaseTexture(raster.textureHandle);
            raster.textureHandle = 0;
        }
    }

    // Strokes are in the item's own native space; the image is that space
    // times `scale` (1 for anything under the cap). Each stroke is one brush
    // session: coverage accumulated across all of its segments and the
    // color laid down once, which is exactly what stops a stroke that
    // crosses itself darkening at the crossing.
    for (size_t i = firstStroke; i < item.strokes.size(); ++i) {
        const Stroke& stroke = item.strokes[i];
        if (stroke.points.empty()) {
            continue;
        }
        raster.pixels.BeginStroke(stroke.colorRGBA, stroke.width * 0.5f * scale, PaintedImage::BrushMode::Paint);
        if (stroke.points.size() == 1) {
            const StrokePoint& p = stroke.points.front();
            raster.pixels.ExtendStroke(p.x * scale, p.y * scale, p.x * scale, p.y * scale);  // a dot
        }
        for (size_t s = 1; s < stroke.points.size(); ++s) {
            raster.pixels.ExtendStroke(stroke.points[s - 1].x * scale, stroke.points[s - 1].y * scale,
                                        stroke.points[s].x * scale, stroke.points[s].y * scale);
        }
        raster.pixels.EndStroke();  // the tiles it returns are nobody's undo entry: this is a cache
    }

    raster.nativeW = item.nativeW;
    raster.nativeH = item.nativeH;
    raster.builtFrom = item.strokes;
    if (!window_ || raster.pixels.Empty()) {
        return;
    }
    // Uploaded whole rather than by dirty rectangle: unlike a brush, this
    // runs once per finished stroke, not several times a frame.
    if (raster.textureHandle == 0) {
        raster.textureHandle = window_->CreateTextureFromPixels(raster.pixels.PixelsRGBA().data(),
                                                                 raster.pixels.Width(), raster.pixels.Height());
    } else {
        window_->UpdateTextureRegion(raster.textureHandle, raster.pixels.PixelsRGBA().data(),
                                      raster.pixels.Width(), 0, 0, raster.pixels.Width(),
                                      raster.pixels.Height());
    }
}

void OverlayApp::RefreshStrokeRasters() {
    if (Cfg().strokeRenderMode != StrokeRenderMode::Rasterized) {
        // Includes the moment the mode is switched away: the textures and
        // the megabytes behind them go with it.
        ReleaseStrokeRasters();
        return;
    }
    const Canvas* canvas = Manager().CurrentOrNull();
    if (!canvas || !window_) {
        ReleaseStrokeRasters();
        return;
    }

    // The gate. While the generation hasn't moved, nothing on any canvas
    // has changed, so there is nothing for the comparisons below to find -
    // which is what makes comparing whole stroke lists affordable at all.
    const uint64_t generation = Manager().Generation();
    if (strokeRasterGeneration_.has_value() && *strokeRasterGeneration_ == generation) {
        return;
    }

    bool everythingBuilt = true;
    for (const Item& item : canvas->items) {
        // An item with nothing on it must *lose* its raster, not keep the
        // one it had. Skipping it here is what left the last undone stroke
        // on screen with nothing left in the model to explain it.
        if (item.strokes.empty() || item.nativeW <= 0.0f || item.nativeH <= 0.0f) {
            const auto stale = strokeRasters_.find(item.id);
            if (stale != strokeRasters_.end()) {
                if (stale->second.textureHandle != 0) {
                    window_->ReleaseTexture(stale->second.textureHandle);
                }
                strokeRasters_.erase(stale);
            }
            continue;
        }
        // The builder decides for itself whether there is anything to do -
        // nothing, the new strokes only, or everything from scratch - from
        // one comparison of what it built from against what is there now.
        StrokeRaster& raster = strokeRasters_[item.id];
        BuildStrokeRaster(item, raster);
        // A texture that couldn't be created is worth trying again rather
        // than leaving the item drawn tessellated forever, so the gate
        // below stays open until one exists.
        everythingBuilt = everythingBuilt && raster.textureHandle != 0;
    }

    // Anything not on this canvas any more - switched away from, or
    // deleted - gives its texture back. Same discipline as the layer
    // textures next door, and for the same reason.
    for (auto it = strokeRasters_.begin(); it != strokeRasters_.end();) {
        const bool stillHere = std::any_of(canvas->items.begin(), canvas->items.end(),
                                            [&](const Item& item) { return item.id == it->first; });
        if (stillHere) {
            ++it;
            continue;
        }
        if (it->second.textureHandle != 0) {
            window_->ReleaseTexture(it->second.textureHandle);
        }
        it = strokeRasters_.erase(it);
    }

    if (everythingBuilt) {
        strokeRasterGeneration_ = generation;
    }
}

uint64_t OverlayApp::StrokeRasterTextureFor(ItemId itemId) const {
    const auto it = strokeRasters_.find(itemId);
    return it == strokeRasters_.end() ? 0 : it->second.textureHandle;
}

void OverlayApp::ReleaseStrokeRasters() {
    // Cleared, not left at the current generation: the next pass has to
    // rebuild from nothing, which is exactly what switching the mode back
    // on needs.
    strokeRasterGeneration_.reset();
    if (window_) {
        for (auto& [itemId, raster] : strokeRasters_) {
            (void)itemId;
            if (raster.textureHandle != 0) {
                window_->ReleaseTexture(raster.textureHandle);
            }
        }
    }
    strokeRasters_.clear();
}

}  // namespace sz::ui
