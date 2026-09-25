#pragma once

#include <cstdint>
#include <string>

namespace sz::core {

// An item's picture: pixels loaded from a file in the snippet's own
// directory, or - when there is no file, or the capture failed - nothing
// but the fill it stands in with.
struct Layer {
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

    // Compares textureHandle too: a copy that kept the original's handle is
    // not equal to it, and that is the bug this would otherwise hide.
    bool operator==(const Layer&) const = default;
};

}  // namespace sz::core
