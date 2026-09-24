#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/drawing/painted_image.h"

namespace sz::core {

// What a layer's pixels come from.
enum class LayerKind {
    // Pixels loaded from a file in the snippet's own directory, or - when
    // there is no file, or the capture failed - nothing but the fill this
    // layer stands in with.
    Image,
    // Pixels this app paints into, held in memory as well as on disk (see
    // `painted`) because a brush composites against what is already there.
    // Pixel-authoritative: these pixels *are* the drawing, so erasing takes
    // away exactly the shape of the brush, and scaling the item resamples
    // them rather than redrawing at the new size.
    Painted,
};

// One layer of an item's picture, composited bottom-first. An item has at
// least one - its Image layer - and a Painted one on top once something
// has been painted on it.
//
// The rule that keeps the two kinds honest: a layer is either
// *vector-authoritative* (its pixels are a cache, rebuildable at any
// resolution from the strokes that own them, never persisted as truth) or
// *pixel-authoritative* (its pixels are the document, persisted and undone
// as pixels). A pixel-level operation on the first kind flattens it into
// the second, the way a rasterize step does in any vector editor; there is
// no state in which both are true. Both kinds here are the second kind;
// Item::strokes are the first.
struct Layer {
    LayerKind kind = LayerKind::Image;

    // 0..1, this layer's own alpha, independent of every other layer's and
    // of the item's strokes. A new item's picture starts at the opacity
    // Settings > Defaults gives its kind - a capture opaque and a drawing
    // see-through, unless changed there; a slider from then on.
    float opacity = 0.0f;

    // 0xRRGGBBAA; the alpha byte is unused, `opacity` is the alpha drawn.
    // Multiplied into this layer's pixels when it has some (white leaves a
    // capture untouched), and used as a plain fill when it doesn't.
    uint32_t tintColorRGBA = 0xFFFFFFFFu;

    // With no pixels to show: true draws the placeholder gradient seeded by
    // `placeholderHue` ("a capture belongs here and isn't loaded"), false
    // draws a flat `tintColorRGBA` fill. Set at creation from whether the
    // item is a screenshot.
    bool showsPlaceholder = false;
    float placeholderHue = 0.0f;

    // Filename of this layer's pixels, resolved through the owning snippet
    // - the file lives in the snippet's own directory, wherever that
    // currently is (see LibraryStore::FindImage). Empty when this layer has
    // no pixels on disk.
    std::string imageFile;

    // Opaque platform texture handle, 0 for none. Loaded and released by
    // the session as canvases become and stop being current; never
    // persisted. Single-ownership: two layers sharing one handle would
    // double-release, so a copy resets it and reloads from `imageFile`.
    uint64_t textureHandle = 0;

    // Painted layers only: the pixels themselves, in memory because a
    // brush composites against what is already there and a texture is
    // write-only from this side. Null for an Image layer.
    //
    // A shared_ptr, not a value: an undo entry or a deleted item's snapshot
    // holds a whole Item by value, and a painted layer is megabytes.
    // Sharing is safe there because those are snapshots of something going
    // away or being re-saved; the one case that must *not* share is
    // duplicating a live item, which deep-copies (see DetachLayersForCopy).
    std::shared_ptr<PaintedImage> painted;

    // Painted layers only: the pixels have changed since they were last
    // written to `imageFile`, so they are the only copy there is. Not
    // persisted. The pixels are dropped when their canvas stops being
    // current, and dropping something never saved loses it; this is what
    // the release path checks first.
    bool paintedDirty = false;

    // Painted layers only: layer pixels per unit of the item's native
    // space. 1 is exactly the item's native size; 2 is twice the detail at
    // four times the memory. Fixed when the layer is created.
    float resolutionScale = 1.0f;

    // A painted layer that actually has pixels in memory - the one question
    // the sync, the save, the erasers and the brush all ask. A Painted layer
    // without them is a real state: its file was never written, or is gone,
    // or its canvas isn't current and the pixels were dropped.
    bool HasPaintedPixels() const { return kind == LayerKind::Painted && painted && !painted->Empty(); }

    // Compares textureHandle and the pixel buffer's identity too: a copy
    // that kept the original's handle is not equal to it, and that is the
    // bug this would otherwise hide.
    bool operator==(const Layer&) const = default;
};

}  // namespace sz::core
