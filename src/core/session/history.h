#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "core/canvas/canvas.h"
#include "core/canvas/canvas_manager.h"
#include "core/canvas/item.h"
#include "core/drawing/stroke.h"

namespace sz::core::history {

// The session's undo history - see Session::Undo, and docs/ARCHITECTURE.md
// ("Undo is per canvas") for the rules that keep every step on it
// applicable. Session-only, never persisted.
//
// A step is what one undo takes back: a list of changes, each about exactly
// one snippet. A change holds what the opposite direction needs, and
// applying it either way leaves it holding what the other one needs again,
// so a step goes from one stack to the other unchanged in kind. Most kinds
// are their own inverse - a swap of what the change holds with what the
// snippet has - and the rest are mirrors: a stroke taken off or put back,
// an erase rebuilt one way or the other, a move there or back.

// What a step was, for a UI to say.
enum class What { Stroke, Erase, Delete, TextEdit, Create, Placement, Style, Paste, Duplicate, Move, CopyTo };

// Where a snippet is: everything a move, a resize or fullscreen changes, and
// nothing else - its rect, its fullscreen state, and the anchor its rect is
// recomputed from when the display changes (see Item::anchorRect). Kept
// whole rather than as the rect alone, so that an undo of a drag that took
// a snippet out of fullscreen puts it back in, and one made before a
// display change lands where it was.
struct Placement {
    Rect rect;
    bool isFullscreen = false;
    bool isFullscreenStretch = false;
    Rect anchorRect;
    float anchorDisplayWidth = 0.0f;
    float anchorDisplayHeight = 0.0f;

    bool operator==(const Placement&) const = default;
    static Placement Of(const Item& item);
    void ApplyTo(Item& item) const;
};

// A stroke was appended to the snippet's strokes. Undo takes it back off
// the end, redo pushes it on again - which is why the value is kept.
struct StrokeAdded {
    Stroke stroke;
};

// One erase gesture - the circular eraser, the rectangular one, or "Clear
// drawing" - over a snippet's strokes, as a list of replacements (see
// CanvasManager::EraseAt: a stroke only partly under the eraser is
// shortened or split, and the fragments stand where it stood). Each names
// one original by its index in the list as it was before the gesture,
// carries it exactly, and carries the fragments that stand in its place
// afterwards. Undo rebuilds the before-list from the after-list, redo the
// reverse, both by position and neither by value: an undo that matched
// strokes by value put the restored originals at the end, which changed the
// draw order, and could not tell two equal strokes apart.
struct StrokesErased {
    struct Replacement {
        size_t index = 0;
        Stroke original;
        std::vector<Stroke> fragments;
    };
    size_t strokeCountBefore = 0;           // the list's length before the gesture
    std::vector<Replacement> replacements;  // ascending by index
};

// The snippet's note text, its placement, its style or its deletion mark,
// as it is on the other side of the change: swapped with the snippet's
// own either way. The deletion mark is what a new snippet, a copy and a
// delete are - undone, a new snippet is marked deleted, where a capture
// taken by mistake can still be found, and a deleted one is not.
struct TextChanged {
    std::string text;
};
struct PlacementChanged {
    Placement placement;
};
struct StyleChanged {
    ItemStyle style;
};
struct DeletionChanged {
    int64_t deletedAt = 0;
};

// The snippet came from canvas `from`, where it stood at `fromIndex` in the
// stack, to canvas `to`, where it stands at `toIndex` - on top, as it
// arrives. Undone it goes back to where it stood - into `from` even while
// that canvas is deleted, where restoring the canvas finds it - and redone
// it comes back to where it stood on `to`. Each index is taken as the
// snippet leaves, so that however the stacks have been reordered since,
// an undo and a redo put it back exactly where the other found it.
struct Moved {
    CanvasId from = 0;
    size_t fromIndex = 0;
    CanvasId to = 0;
    size_t toIndex = static_cast<size_t>(-1);
};

using ChangeKind = std::variant<StrokeAdded, StrokesErased, TextChanged, PlacementChanged, StyleChanged,
                                DeletionChanged, Moved>;

struct Change {
    ItemId item = 0;
    ChangeKind kind;
};

struct Step {
    // When it was done, or last redone: the order the steps on one stack
    // are in, and what a snippet's changes are merged in by when they
    // follow it to another canvas (see History::Migrate). Several steps can
    // share one - the parts of a step split between the canvases its
    // snippets went to.
    uint64_t seq = 0;
    What what = What::Stroke;
    // At most one per snippet. Applied first to last when redone, last to
    // first when undone.
    std::vector<Change> changes;
};

// What a step holds, in bytes of points and text - what the stacks are
// capped by besides their length.
size_t Bytes(const Step& step);

// Whether `change` can be applied the way `undo` says: its snippet there,
// on the canvas the change expects it on, and its strokes the list the
// change describes. The history keeps this true of every step on it (see
// History); it is asked anyway, of every change of a step before any of
// them is applied, so that a step is never applied halfway.
bool CanApply(const CanvasManager& manager, const Change& change, bool undo);
// Applies it, leaving it holding what the opposite direction needs.
void Apply(CanvasManager& manager, Change& change, bool undo);

// The history of every canvas: an undo and a redo stack each. What is on a
// canvas's stacks is only ever about snippets on that canvas, so no undo
// can reach one that is not on screen; the rules below keep it that way,
// and keep every step on every stack applicable, whatever happens in the
// library in between:
//
//  - A snippet's undo changes are always on the undo stack of the canvas
//    holding it. When it moves - pasted, sent, or by an undo or redo of
//    either - they move with it (Migrate), merged in by `seq`, and a step
//    about several snippets is split between their canvases.
//  - A new change to a snippet drops its changes from every redo stack
//    (and, as usual, the new step's canvas's redo stack goes whole): the
//    future they were for is gone.
//  - An undo or redo that changes a snippet drops its changes from the redo
//    stacks of every other canvas, which were for a state it has just
//    left.
//  - A snippet deleted for good takes every change about it with it, and a
//    canvas deleted for good takes its stacks and every move from or to it.
//
// Every one of those is done eagerly, at the moment it becomes true, so
// that a step is never refused when it is reached.
class History {
public:
    bool CanUndo(CanvasId canvas) const;
    bool CanRedo(CanvasId canvas) const;
    // Counts the changes to a history that move its top - a step filed,
    // undone or redone - so that a caller can tell whether anything came
    // between two of its own edits. A snippet's history moving or being
    // forgotten counts for nothing, and so does the oldest falling off the
    // end.
    uint64_t Revision() const { return revision_; }

    // Files `step`, just done on `canvas`, on top of its undo stack.
    void Record(CanvasId canvas, Step step);

    // The step an undo or a redo on `canvas` would apply; null for none.
    const Step* NextUndo(CanvasId canvas) const;
    const Step* NextRedo(CanvasId canvas) const;
    // Takes it off its stack, to be applied and handed back to Undone or
    // Redone - or to Drop, if it could not be.
    Step TakeUndo(CanvasId canvas);
    Step TakeRedo(CanvasId canvas);
    void Undone(CanvasId canvas, Step step);
    void Redone(CanvasId canvas, Step step);
    // Puts a step taken with TakeUndo (`undo`) or TakeRedo back on top of
    // the stack it came from, as it was - one whose write failed, so that
    // it was not applied after all.
    void PutBack(CanvasId canvas, Step step, bool undo);

    // `item` is on `canvas` now: its undo changes on any other canvas's
    // stack follow it there.
    void Migrate(ItemId item, CanvasId canvas);
    // `item` is gone for good: every change about it goes.
    void ForgetItem(ItemId item);
    // `canvas` is gone for good: its stacks go, and every move from or to
    // it. What was on it is forgotten item by item.
    void ForgetCanvas(CanvasId canvas);

    // Every step on `canvas`'s stacks, bottom first - for the tests.
    const std::deque<Step>* UndoStack(CanvasId canvas) const;
    const std::deque<Step>* RedoStack(CanvasId canvas) const;
    // Every canvas with anything on its stacks - for the tests.
    std::vector<CanvasId> Canvases() const;

    // How much one stack may hold: plenty of headroom for "take back what I
    // just did" without growing unbounded across a long session. The oldest
    // goes first, which leaves every step above it applicable.
    static constexpr size_t kStackCap = 50;
    static constexpr size_t kStackCapBytes = size_t{128} << 20;

private:
    struct Stacks {
        std::deque<Step> undo;
        std::deque<Step> redo;
    };
    // Pushes onto `stack`, then Cap.
    static void PushCapped(std::deque<Step>& stack, Step step);
    // Evicts from the bottom of `stack` past the caps, always keeping its
    // top.
    static void Cap(std::deque<Step>& stack);
    // Takes every change about `item` out of `stack`, dropping steps left
    // empty.
    static void Remove(std::deque<Step>& stack, ItemId item);
    // Drops `item`'s changes from every redo stack but `except`'s.
    void DropRedoOf(ItemId item, CanvasId except);
    // Lets go of the stacks of every canvas left with nothing on them.
    void DropEmptyCanvases();

    std::unordered_map<CanvasId, Stacks> stacks_;
    uint64_t nextSeq_ = 1;
    uint64_t revision_ = 0;
};

}  // namespace sz::core::history
