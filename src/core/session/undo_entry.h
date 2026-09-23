#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "core/canvas/canvas.h"
#include "core/canvas/item.h"
#include "core/drawing/painted_image.h"
#include "core/drawing/stroke.h"

namespace sz::core::undo {

// What the session's history is made of - see Session::Undo. One struct per
// kind of change, each holding exactly what it needs and nothing of the
// others', so a field cannot be read by a kind that never sets it. Applying
// one is the session's (Session::Apply, one overload per kind); what each
// holds and which snippets it names is its own.
//
// Every one is pushed at the moment its change happens, and applying it
// either way leaves it holding what the opposite direction needs: the
// entry popped off one stack goes onto the other unchanged in kind. Some
// are their own inverse - a swap of their contents with the item's - and
// the rest are a mirror, one action and its opposite.
//
// Session-only, never persisted.

// Where a snippet is: everything a move, a resize or fullscreen changes, and
// nothing else - its rect, its fullscreen state, and the anchor its rect is
// recomputed from when the display changes (see Item::anchorRect). Kept
// whole rather than as the rect alone, so that an undo of a drag that took
// a snippet out of fullscreen puts it back in, and one made before a
// display change lands where it was.
struct Placement {
    ItemId itemId = 0;
    Rect rect;
    bool isFullscreen = false;
    bool isFullscreenStretch = false;
    Rect anchorRect;
    float anchorDisplayWidth = 0.0f;
    float anchorDisplayHeight = 0.0f;
};

// What a paste or a duplicate brought onto a canvas, one snippet each:
// moved there by a cut's paste from `fromCanvas`, where it stood at
// `fromIndex` in the stack - or, with fromCanvas 0, a copy made there.
// The index is the stack as it was when this one left, after the
// arrivals listed before it had gone: an ItemsArrived entry is undone
// last first for that reason.
struct Arrival {
    ItemId itemId = 0;
    CanvasId fromCanvas = 0;
    size_t fromIndex = 0;
};

// The painted half of a change: the tiles a brush gesture touched, as they
// were before (see PaintedImage::EndStroke), or the whole image a clear
// replaced. Tiles rather than the whole layer because a fullscreen layer is
// 8 MB and a stroke touches a few hundred kilobytes of it. Applying it puts
// them back and keeps what they replaced, so it is its own inverse.
struct Painted {
    ItemId itemId = 0;
    size_t layerIndex = 0;
    std::vector<PaintedTile> tiles;
    std::shared_ptr<PaintedImage> wholeImage;
    bool Empty() const { return tiles.empty() && !wholeImage; }
    size_t Bytes() const;
};

// A stroke was appended to the item's strokes. Undo takes that stroke back
// off, found from the back so it is still the last of two equal ones; redo
// pushes it on again, which is why the value is kept rather than only the
// id.
struct StrokeBaked {
    ItemId itemId = 0;
    Stroke stroke;
};

// One erase gesture - the circular eraser, the rectangular one, or "Clear
// drawing" - over an item that may carry vector strokes, painted pixels,
// or both. One entry for the whole gesture, whichever kinds of ink it
// touched: an eraser that took two undos to take back one drag read as a
// bug.
//
// The vector half is a list of replacements (see CanvasManager::EraseAt - a
// stroke only partly within the eraser is shortened or split rather than
// removed, and the fragments stand where it stood). Each names one
// original by its index in the list as it was before the gesture, carries
// that original exactly, and carries the fragments that stand in its
// place afterwards. Undo rebuilds the before-list from the after-list,
// redo the reverse, both by position and neither by value: an undo that
// matched strokes by value put the restored originals at the end, which
// changed the draw order and left the next undo of a stroke taking off a
// different stroke than the one it was for, and could not tell two equal
// strokes apart. Empty when the gesture clipped nothing; `painted` empty
// when it touched no pixels.
struct Erased {
    struct Replacement {
        size_t index = 0;
        Stroke original;
        std::vector<Stroke> fragments;
    };
    ItemId itemId = 0;
    size_t strokeCountBefore = 0;          // the list's length before the gesture
    std::vector<Replacement> replacements;  // ascending by index
    Painted painted;
};

// A snippet was deleted. It is still there, marked: undo clears the mark,
// redo makes it again.
struct ItemDeleted {
    ItemId itemId = 0;
};

// A text edit (see Session::BeginTextEdit) changed the note: one entry per
// edit, not per keystroke. Swapped with the item's text either way.
struct NoteTextChanged {
    ItemId itemId = 0;
    std::string previousText;
};

// A brush stroke painted onto a painted layer. An erase that touches pixels
// is an Erased entry instead, since it may have touched strokes too.
struct PaintedTilesChanged {
    Painted painted;
};

// A snippet was made, on the canvas the entry is filed under (see
// Session::CreateItem). ItemDeleted the other way round: undo marks it
// deleted, redo restores it.
struct ItemCreated {
    ItemId itemId = 0;
};

// Where each of these snippets was before a move, a resize or a fullscreen
// toggle (see Session::RecordPlacements). Swapped with each snippet's
// placement either way.
struct PlacementChanged {
    std::vector<Placement> placements;
};

// A paste or a duplicate brought these snippets onto `canvasId` (see
// Session::RecordArrivals); `duplicate` only says which, for the UI. Undo
// sends each back - a copy into its deletion mark, a moved one to its old
// canvas and place in the stack - and redo brings it again.
struct ItemsArrived {
    CanvasId canvasId = 0;
    std::vector<Arrival> arrivals;
    bool duplicate = false;
};

using Entry = std::variant<StrokeBaked, Erased, ItemDeleted, NoteTextChanged, PaintedTilesChanged, ItemCreated,
                           PlacementChanged, ItemsArrived>;

// What an entry holds, in bytes of points, pixels and text - what the
// stacks are capped by besides their length (see Session::PushCapped).
size_t Bytes(const Entry& entry);

// Takes `itemId` out of `entry`, for a snippet that has left the canvas the
// entry is filed under (see Session::ForgetHistoryOfItem). True when the
// entry is left naming nothing, and goes: an entry about several snippets
// keeps the others', whose change is still theirs to take back.
bool Forget(Entry& entry, ItemId itemId);

}  // namespace sz::core::undo
