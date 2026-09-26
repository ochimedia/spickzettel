#include "ui/overlay_app.h"
#include "ui/overlay_app_internal.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "core/canvas/item_geometry.h"

namespace sz::ui {

// ================= Textures, stroke rasters and overview previews =================
//
// What the UI draws pictures with: the textures of snippets' pictures, the
// bitmaps strokes are drawn into in the rasterized render mode, and the
// thumbnails the Overview shows - every one of them asked of the
// TextureCache as it is drawn.

namespace {

// A stroke raster is not allowed to be enormous no matter how the item is
// sized - 4096 on a side is well past a fullscreen capture and is where
// the memory (64 MB at RGBA8) stops being reasonable to hold per item. An
// item larger than this gets a *smaller resolution scale*, not a cropped
// bitmap - see FitResolutionScale for why the distinction is the
// difference between a stroke landing where it was drawn and a quarter of
// the way across the screen from it.
constexpr int kMaxRasterExtent = 4096;

}  // namespace

// ================= Pictures =================

uint64_t OverlayApp::PictureTexture(const Item& item) {
    const TextureKey key{TextureKey::Kind::Picture, item.id};
    // A capture's is there from the moment it was taken (see
    // Session::CaptureShotItem).
    if (const std::optional<uint64_t> made = Textures().Find(key)) {
        return *made;
    }
    if (!item.picture.stored || !Store()) {
        return 0;  // never had pixels of its own: the placeholder or the fill
    }
    const std::optional<persistence::DecodedImage> decoded = Store()->LoadImage(item.id);
    return Textures().Put(key, decoded.has_value()
                                   ? TexturePixels{decoded->pixelsRGBA.data(), decoded->width, decoded->height}
                                   : TexturePixels{});
}

void OverlayApp::KeepCurrentCanvasTextures() {
    const Canvas* canvas = Manager().CurrentOrNull();
    if (canvas == nullptr) {
        return;
    }
    for (const Item& item : canvas->items) {
        // Only what can be on screen: a deleted snippet on the current
        // canvas is as far from being drawn as one on another canvas.
        if (!Manager().IsDeleted(*canvas, item)) {
            PictureTexture(item);
            StrokeRasterTextureFor(item.id);
        }
    }
}

// ================= Overview bitmap previews =================

namespace {

// How many *full* images may be decoded in one frame. A fullscreen QOI is
// ~7ms, so a library of dozens would stall the panel on the frame it opens
// - which is the frame it can least afford to. Spread over frames instead:
// the thumbnails fill in over a fraction of a second and the panel stays
// responsive throughout.
//
// This is the fallback path. A picture is normally stored with a thumbnail
// (see LibraryStore::SaveImage), and those are read under a much larger
// budget below, because a 256px QOI decodes in well under a millisecond.
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
    picturePreviewLoadBudget_ = wanted ? kOverviewFullDecodesPerFrame : 0;
    picturePreviewThumbnailBudget_ = wanted ? kOverviewThumbnailLoadsPerFrame : 0;
}

PreviewTextureFn OverlayApp::PreviewTextureLookup() {
    if (!Cfg().overviewShowsBitmaps) {
        return {};
    }
    return [this](const Item& item) { return PicturePreviewTexture(item); };
}

std::optional<uint64_t> OverlayApp::PicturePreviewTexture(const Item& item) {
    if (!Store()) {
        return 0;
    }
    // The current canvas's pictures already have their full-size texture
    // loaded, and drawing that scaled into a 200px tile costs nothing extra
    // - no decode, no second texture. Which is also the canvas most likely
    // to be looked at in the overview.
    if (const std::optional<uint64_t> full = Textures().Find(TextureKey{TextureKey::Kind::Picture, item.id});
        full.has_value() && *full != 0) {
        return *full;
    }

    const TextureKey key{TextureKey::Kind::Thumbnail, item.id};
    if (const std::optional<uint64_t> made = Textures().Find(key)) {
        return *made;  // 0 if it failed, which stops it being retried
    }
    if (!item.picture.stored) {
        return 0;  // never had pixels of its own: the placeholder is the answer
    }

    // The thumbnail first, and on its own budget: this is the path nearly
    // every picture takes, and it is cheap enough that a whole folder's
    // worth lands on the first frame.
    if (picturePreviewThumbnailBudget_ > 0) {
        --picturePreviewThumbnailBudget_;
        if (const std::optional<persistence::DecodedImage> thumb = Store()->LoadThumbnail(item.id)) {
            return Textures().Put(key, TexturePixels{thumb->pixelsRGBA.data(), thumb->width, thumb->height});
        }
    } else {
        return std::nullopt;  // even the cheap path is spoken for this frame
    }

    // No thumbnail: it could not be made when the picture was stored.
    // Decode the real thing, under the small budget.
    if (picturePreviewLoadBudget_ <= 0) {
        // Its turn is next frame, or the one after. Distinct from the 0
        // above, and the caller draws the two differently: a stand-in for
        // an image that is arriving shortly is a wrong thumbnail that
        // corrects itself, which is what the placeholder gradient looked
        // like for the first few frames of every Overview visit.
        return std::nullopt;
    }
    --picturePreviewLoadBudget_;

    const std::optional<persistence::DecodedImage> decoded = Store()->LoadImage(item.id);
    if (!decoded.has_value()) {
        return Textures().Put(key, TexturePixels{});
    }
    const persistence::DecodedImage small =
        persistence::DownscaleToFit(*decoded, persistence::LibraryStore::kThumbnailMaxExtent);
    return Textures().Put(key, TexturePixels{small.pixelsRGBA.data(), small.width, small.height});
}

// ================= Rasterized vector strokes =================

void OverlayApp::BuildStrokeRaster(const Item& item, StrokeRaster& raster) {
    // Native-sized when that fits the cap, uniformly smaller when it
    // doesn't - and then every coordinate below goes through the same
    // scale, so the raster stays in register with the strokes whatever
    // size it came out at.
    const float scale = FitResolutionScale(item.nativeW, item.nativeH, 1.0f, kMaxRasterExtent);
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
    // whole list unchanged, and the same shape. Asked here rather than by
    // the caller - one comparison that decides both whether and how much
    // to draw.
    if (sameShape && prefixUnchanged && raster.builtFrom.size() == item.strokes.size()) {
        return;
    }
    size_t firstStroke = 0;
    if (sameShape && prefixUnchanged) {
        firstStroke = raster.builtFrom.size();
    } else {
        raster.pixels = StrokeBitmap(width, height);
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
        raster.pixels.BeginStroke(stroke.colorRGBA, stroke.width * 0.5f * scale);
        if (stroke.points.size() == 1) {
            const StrokePoint& p = stroke.points.front();
            raster.pixels.ExtendStroke(p.x * scale, p.y * scale, p.x * scale, p.y * scale);  // a dot
        }
        for (size_t s = 1; s < stroke.points.size(); ++s) {
            raster.pixels.ExtendStroke(stroke.points[s - 1].x * scale, stroke.points[s - 1].y * scale,
                                        stroke.points[s].x * scale, stroke.points[s].y * scale);
        }
        raster.pixels.EndStroke();
    }

    raster.nativeW = item.nativeW;
    raster.nativeH = item.nativeH;
    raster.builtFrom = item.strokes;
    // Its texture follows the next time it is drawn - uploaded whole rather
    // than by dirty rectangle: this runs once per finished stroke, not
    // several times a frame.
    ++raster.revision;
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

    for (const Item& item : canvas->items) {
        // An item with nothing on it must *lose* its raster, not keep the
        // one it had. Skipping it here is what left the last undone stroke
        // on screen with nothing left in the model to explain it.
        if (item.strokes.empty() || item.nativeW <= 0.0f || item.nativeH <= 0.0f) {
            strokeRasters_.erase(item.id);
            continue;
        }
        // The builder decides for itself whether there is anything to do -
        // nothing, the new strokes only, or everything from scratch - from
        // one comparison of what it built from against what is there now.
        BuildStrokeRaster(item, strokeRasters_[item.id]);
    }

    // Anything not on this canvas any more - switched away from, or
    // deleted - goes, and its texture with it, unasked for (see
    // TextureCache).
    for (auto it = strokeRasters_.begin(); it != strokeRasters_.end();) {
        const bool stillHere = std::any_of(canvas->items.begin(), canvas->items.end(),
                                            [&](const Item& item) { return item.id == it->first; });
        it = stillHere ? std::next(it) : strokeRasters_.erase(it);
    }
    strokeRasterGeneration_ = generation;
}

uint64_t OverlayApp::StrokeRasterTextureFor(ItemId itemId) {
    const auto it = strokeRasters_.find(itemId);
    if (it == strokeRasters_.end() || it->second.pixels.Empty()) {
        return 0;
    }
    const StrokeBitmap& pixels = it->second.pixels;
    return Textures().Get(TextureKey{TextureKey::Kind::StrokeRaster, itemId}, it->second.revision,
                          TexturePixels{pixels.PixelsRGBA().data(), pixels.Width(), pixels.Height()});
}

void OverlayApp::ReleaseStrokeRasters() {
    // Cleared, not left at the current generation: the next pass has to
    // rebuild from nothing, which is exactly what switching the mode back
    // on needs.
    strokeRasterGeneration_.reset();
    strokeRasters_.clear();
}

}  // namespace sz::ui
