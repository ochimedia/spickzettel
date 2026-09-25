#pragma once

#include <cstdint>

namespace sz::core {

// A snippet's picture, underneath its strokes and its caption: pixels
// stored in the library with the snippet, or - when there are none, or the
// capture failed - nothing but the fill it stands in with.
struct Picture {
    // 0..1, the picture's own alpha, independent of the snippet's strokes.
    // A new snippet's picture starts at the opacity Settings > Defaults
    // gives its kind - a capture opaque and a drawing see-through, unless
    // changed there; a slider from then on.
    float opacity = 0.0f;

    // 0xRRGGBBAA; the alpha byte is unused, `opacity` is the alpha drawn.
    // Multiplied into the pixels when there are some (white leaves a
    // capture untouched), and used as a plain fill when there are not.
    uint32_t tintColorRGBA = 0xFFFFFFFFu;

    // With no pixels to show: true draws the placeholder gradient seeded by
    // `placeholderHue` ("a capture belongs here and isn't loaded"), false
    // draws a flat `tintColorRGBA` fill. Set at creation from whether the
    // snippet is a screenshot.
    bool showsPlaceholder = false;
    float placeholderHue = 0.0f;

    // Whether the library holds pixels for it, keyed by the snippet (see
    // LibraryStore::SaveImage). Not itself saved: the library says it.
    bool stored = false;

    // Opaque platform texture handle, 0 for none. Loaded and released by
    // the session as canvases become and stop being current; never
    // persisted. Single-ownership: two pictures sharing one handle would
    // double-release, so a copy resets it and loads its own.
    uint64_t textureHandle = 0;

    // Compares textureHandle too: a copy that kept the original's handle is
    // not equal to it, and that is the bug this would otherwise hide.
    bool operator==(const Picture&) const = default;
};

}  // namespace sz::core
