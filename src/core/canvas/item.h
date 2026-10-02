#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/canvas/picture.h"
#include "core/drawing/stroke.h"

namespace sz::core {

using ItemId = uint64_t;

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    bool operator==(const Rect&) const = default;
};

// An item - a *snippet*, in the interface's words - placed on a Canvas:
// freehand strokes over a picture (see picture.h), which is a captured
// screenshot or a plain fill. There is no hard screenshot/drawing type
// split: `hasBackground` records how an item was made, and everything
// visible about it is the picture's own opacity.
//
// `strokes` are stored in the item's own fixed native coordinate space
// (nativeW/nativeH, set at creation), independent of `rect`'s current
// on-screen size, so a stroke drawn at one size still looks right after
// the item is resized - see CanvasManager::BakeStrokeToNative.
struct Item {
    ItemId id = 0;
    std::string name;
    // See Folder::createdAt/deletedAt.
    int64_t createdAt = 0;
    int64_t deletedAt = 0;

    Rect rect;             // current on-screen position and size
    float nativeW = 0.0f;  // fixed at creation: the coordinate space of `strokes`
    float nativeH = 0.0f;

    std::vector<Stroke> strokes;  // native space

    // Opacity of this item's own strokes, independent of the picture's.
    float foregroundOpacity = 1.0f;  // 0..1
    // Whether a resize from the band keeps the item's shape (Shift does the
    // other). A property of its own, set from the defaults when the item
    // is made and changed in its popover: it used to follow from whether
    // the item had text, so that typing a caption into a screenshot
    // quietly changed what resizing it did.
    bool keepAspect = true;
    bool isFullscreen = false;
    // Whether entering fullscreen stretched `rect` to exactly fill the
    // viewport rather than fitting the item's own aspect ratio into it,
    // letterboxed or pillarboxed. Meaningless while !isFullscreen. Needed
    // because `rect` alone cannot tell a stretch-filled item from one whose
    // aspect ratio happens to match the viewport's, and the live display
    // sync has to keep a fit-mode item fitted when the display changes.
    bool isFullscreenStretch = false;

    // The item's last deliberate, non-fullscreen placement, and the display
    // size it was placed against. `rect` is recomputed from this pair by
    // CanvasManager::SyncItemsToDisplaySize whenever the display size
    // differs, and CommitItemLayout re-anchors after every deliberate move
    // or resize. Always computed from this fixed pair, never chained onto
    // whatever `rect` holds, which is what makes display changes reversible
    // without drift. Untouched while fullscreen, where it records the place
    // to restore to. anchorDisplayWidth/Height == 0 means "not yet
    // anchored": the sync adopts the current rect the first time it runs.
    Rect anchorRect;
    float anchorDisplayWidth = 0.0f;
    float anchorDisplayHeight = 0.0f;

    // Hidden from the canvas and shown as a chip in the dock instead.
    // Nothing else about the item changes; restoring puts it back exactly
    // where it was. Independent of isFullscreen.
    bool minimized = false;

    // Stays on screen when the overlay is put away: while the current
    // canvas has a pinned snippet that is not minimized, "hidden" is the
    // pinned view instead - click-through, showing those snippets and
    // nothing else - and unpinning is how they go.
    bool pinned = false;

    // Underneath `strokes` and `noteText`. Every item has one, even one
    // that is fully transparent and draws nothing, which is exactly what a
    // plain drawing starts as.
    Picture picture;

    // Whether this item was made as a screenshot rather than a drawing. Set
    // once at creation and never changed; it does not gate whether a
    // background is drawn (the picture's opacity does). It decides the
    // picture's starting opacity and placeholder, the item's "Screenshot N" /
    // "Drawing N" name, and whether an empty item counts as untouched: a
    // screenshot is content even when its capture failed. The name is also
    // the on-disk key.
    bool hasBackground = false;

    // A caption on top of whatever else the item has. Empty means none;
    // there is deliberately no companion flag and no separate note kind -
    // a note is a drawing with text typed into it. Edited in place and kept
    // re-editable indefinitely, unlike a shape, which is baked into
    // `strokes` once.
    std::string noteText;

    // How `noteText` is drawn, per item rather than app-wide: a caption over
    // a dark screenshot and one over a pale drawing want different answers,
    // and both can sit on one canvas. 0xRRGGBBAA, and unlike a picture's tint
    // the alpha *is* used - text has no separate opacity, so fading a
    // caption means fading its color.
    uint32_t noteTextColorRGBA = 0xFFFFFFFFu;  // opaque white
    // Cap height in px at the item's *current* size - deliberately not
    // scaled with the item the way strokes are, because a caption that
    // shrinks to illegibility when its item is made small is worse than one
    // that keeps its size and wraps sooner. 17 matches the UI font. Clamped
    // to kNoteTextSizeMin..Max wherever it is edited or loaded.
    float noteTextSizePx = 17.0f;

    bool operator==(const Item&) const = default;
};

// How a snippet is drawn, apart from where: the opacities, the colors and
// the caption's size, and whether a resize keeps its shape - everything its
// popover and the opacity wheel change, and nothing else.
struct ItemStyle {
    float foregroundOpacity = 1.0f;
    float pictureOpacity = 1.0f;
    uint32_t pictureTintRGBA = 0xFFFFFFFFu;
    uint32_t noteTextColorRGBA = 0xFFFFFFFFu;
    float noteTextSizePx = 17.0f;
    bool keepAspect = true;

    bool operator==(const ItemStyle&) const = default;

    static ItemStyle Of(const Item& item) {
        return ItemStyle{item.foregroundOpacity, item.picture.opacity,   item.picture.tintColorRGBA,
                         item.noteTextColorRGBA, item.noteTextSizePx, item.keepAspect};
    }
    void ApplyTo(Item& item) const {
        item.foregroundOpacity = foregroundOpacity;
        item.picture.opacity = pictureOpacity;
        item.picture.tintColorRGBA = pictureTintRGBA;
        item.noteTextColorRGBA = noteTextColorRGBA;
        item.noteTextSizePx = noteTextSizePx;
        item.keepAspect = keepAspect;
    }
};

// The band noteTextSizePx is held to: below 8 the UI font stops being
// readable; above 96 a single line no longer fits across a typical item.
inline constexpr float kNoteTextSizeMin = 8.0f;
inline constexpr float kNoteTextSizeMax = 96.0f;

}  // namespace sz::core
