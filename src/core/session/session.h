#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/canvas/canvas_manager.h"
#include "core/drawing/painted_image.h"
#include "core/drawing/stroke.h"
#include "core/persistence/library_store.h"
#include "platform/i_overlay_window.h"
#include "platform/platform_types.h"

namespace sz::core {

// What the app is working on, independent of how it is shown: the library,
// what is on disk and what is on the GPU. A UI is a view of this - it reads
// the model, edits it, and asks the session for everything that has to be
// kept in step with it: saving, loading pictures, deleting and restoring,
// capturing the screen.
//
// No ImGui here, and nothing about gestures, popovers or panels. The window
// it is attached to is the platform's, for the two things the session needs
// from it: textures, and screen captures.
class Session {
public:
    Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // ===== What it is attached to =====

    // For textures and captures. May stay null - a test that never shows a
    // window - in which case nothing is uploaded or captured, and everything
    // else still works.
    void AttachWindow(platform::IOverlayWindow* window) { window_ = window; }
    // Where the library is read from and written to. Left null, it is never
    // persisted, and everything else works the same.
    void SetLibraryStore(persistence::LibraryStore* store) { library_.store = store; }
    persistence::LibraryStore* Store() const { return library_.store; }

    // Wholesale-replaces the library with a previously-saved one - see
    // CanvasManager::ImportSnapshot. Called once, right after startup.
    void ImportLibrary(CanvasManagerSnapshot snapshot) { library_.manager.ImportSnapshot(std::move(snapshot)); }

    // The library: what every gesture, the Overview and the texture sync act
    // on.
    CanvasManager& Manager() { return library_.manager; }
    const CanvasManager& Manager() const { return library_.manager; }

    // ===== Deleting and restoring =====
    //
    // A delete is a mark made in place, hidden until it is restored or
    // deleted for good - see CanvasManager's class comment, and its
    // DeletedThings for the list both happen from.

    // Marks the folder, canvas or snippet `id` names deleted, now. What
    // every delete is but a snippet's own, which is DeleteItem, undoably.
    // False, doing nothing, if there is no such thing or it is already
    // marked.
    bool Delete(uint64_t id);
    // Clears the mark on `id` and on whatever holds it - see
    // CanvasManager::Restore. False if nothing was deleted.
    bool Restore(uint64_t id);
    // Erases `id` for good: out of the model, its textures released, its
    // history forgotten, and its directory deleted at once (see
    // LibraryStore::Remove). NotFound, doing nothing, if there is no such
    // thing. FilesRemain when the model has let go of it but its directory
    // could not be wholly removed - a picture in it held open by another
    // program, say - so that a UI can say so rather than report a delete
    // that left files behind; the store retries at every save.
    enum class Removal { NotFound, Removed, FilesRemain };
    Removal DeletePermanently(uint64_t id);
    // ===== Keeping the disk and the GPU in step =====

    // The debounced autosave - see kAutosaveQuietSeconds for the policy.
    // Call once per frame.
    void Tick(float deltaSeconds);
    // Writes the library right now if anything's changed since its last
    // save, bypassing the debounce - for a safe point where no frame will
    // come soon enough to catch it (hiding the overlay, exiting).
    void Flush();

    // Brings GPU shot textures in line with whichever canvas is current:
    // loads the ones it needs, frees every other canvas's (see
    // CanvasManager::SyncShotTexturesToCanvas for the exact rule and why
    // only the current canvas ever needs one). A no-op without a store or a
    // window.
    //
    // Deliberately *not* a one-time startup load of every canvas's images:
    // a 4K capture is ~33 MB of RGBA8, so a library of fifty of them would
    // pin ~1.6 GB of VRAM behind a game and pay for all of it as a stall on
    // the very first hotkey. The cost is one canvas's worth, paid on
    // switching to it.
    //
    // The ungated form, for a change that leaves the same canvas current -
    // something arriving on it, or leaving it.
    void SyncTexturesToCurrentCanvas();
    // The gated form: syncs only when the current canvas isn't the one
    // whose textures are resident. Cheap enough to call whenever it might
    // matter.
    //
    // Call it immediately before anything draws item content, not merely
    // once a frame. A canvas switch can happen *during* a frame - Alt+wheel
    // is handled from the frame itself, well after it began - and a switch
    // that lands after the sync leaves every shot on the new canvas with no
    // texture for the rest of that frame, which renders as the placeholder
    // gradient. That is what the gradient flash on Alt+wheel was: not a
    // slow load showing through, but the frame drawn between the switch and
    // the load.
    void EnsureTexturesForCurrentCanvas();

    // ===== Undo =====

    // What a step of undo or redo took back or put back - for a UI to say
    // so. `undone` is true for an undo, false for a redo.
    enum class UndoWhat { Stroke, Erase, Delete, TextEdit, Painting, Create };
    struct UndoStep {
        UndoWhat what = UndoWhat::Stroke;
        bool undone = true;
    };
    // Takes back the current canvas's most recent edit, or puts back the
    // one most recently taken back. History is kept *per canvas* - see
    // undoStacks_.
    // Best-effort by design: an entry naming an item that has since gone is
    // dropped rather than moved to the opposite stack, and nullopt says
    // nothing happened.
    std::optional<UndoStep> Undo();
    std::optional<UndoStep> Redo();
    // Whether there is anything for Undo/Redo to do on the current canvas.
    // Deliberately just the emptiness of each stack, not a deeper "would
    // this entry still apply" test - predicting that would mean duplicating
    // the whole dispatch just to decide a button's tint.
    bool CanUndo() const;
    bool CanRedo() const;
    // Forgets every entry naming `itemId`, on `canvasId` only - for an item
    // moved to a different canvas, where its history would otherwise stay
    // filed under the canvas it left, and an undo there would edit an item
    // that now lives somewhere else.
    void ForgetHistoryOfItem(CanvasId canvasId, ItemId itemId);

    // ===== Edits that can be undone =====
    //
    // Everything that changes a snippet's ink or text goes through these,
    // so that it goes on the history as it happens. Moves, resizes, renames
    // and reorders are not tracked; they are edits of the model directly.

    // Moves the stroke just finished on the current canvas's live layer
    // into `itemId`, in the item's own native space, and files it.
    void CommitLiveStroke(ItemId itemId);
    // Deletes a snippet on the current canvas, undoably - marked, and the
    // entry is what an undo restores. False if there is no such snippet
    // there, or it is deleted already.
    bool DeleteItem(ItemId itemId);
    // Makes a snippet on the current canvas, undoably - see
    // CanvasManager::CreateItem for what it starts as. Undone it is marked
    // deleted, where a capture taken by mistake can still be found, and
    // redone it is restored. 0, having made nothing, without a canvas.
    ItemId CreateItem(bool hasBackground, Rect rect, std::string name);
    // Removes a snippet nothing has been put into - no strokes, no painted
    // pixels, no text, no picture of its own - as if it had never been
    // made: erased rather than marked, and off the history. For a snippet a
    // click made that turned out not to be meant. False, doing nothing, for
    // a snippet with anything in it or no snippet by that id.
    bool DiscardIfUntouched(ItemId itemId);
    // Everything drawn on a snippet - its strokes and its painted pixels -
    // cleared as one undoable step. False if there was nothing to clear.
    bool ClearDrawing(ItemId itemId);
    // A text edit of a snippet's note: Begin remembers what the text was,
    // End commits the new text and files one entry for the whole edit if it
    // changed. End with nullopt abandons it. Beginning another ends the one
    // open, uncommitted. While one is open, an undo that would change that
    // item's text does nothing - its own commit would overwrite it anyway.
    void BeginTextEdit(ItemId itemId);
    void EndTextEdit(std::optional<std::string> text);

    // The brush, as a gesture in screen space: paints into the item's
    // painted layer (made on first use), and files one entry when it ends.
    void BeginPaint(ItemId itemId, float screenX, float screenY, uint32_t colorRGBA, float widthScreenPx);
    void ExtendPaint(float screenX, float screenY);
    void EndPaint();

    // The eraser, as a gesture: clips the item's strokes and erases its
    // painted pixels under a circle `widthScreenPx` across, and files the
    // whole gesture - both halves - as one entry when it ends.
    void BeginErase(ItemId itemId, float screenX, float screenY, float widthScreenPx);
    void ExtendErase(float screenX, float screenY, float widthScreenPx);
    void EndErase();
    // The rectangular eraser: one whole gesture in one call.
    void EraseRect(ItemId itemId, float minX, float minY, float maxX, float maxY);

    // A straight-edged mark: a line, or a rectangle's outline.
    enum class Shape { Line, Rectangle };
    // A shape, as a gesture in screen space from a fixed corner: previewed
    // on the current canvas's live layer while it is dragged - a vector
    // stroke in either mode, cheap to replace wholesale as the shape changes
    // - and on End baked into the item as a stroke, or painted into its
    // pixels with `paintPixels`, as one undo entry. SetShape changes which
    // shape it is mid-drag; Cancel drops it, leaving nothing.
    void BeginShape(ItemId itemId, Shape shape, float screenX, float screenY, uint32_t colorRGBA,
                    float widthScreenPx, bool paintPixels);
    void UpdateShape(float screenX, float screenY);
    void SetShape(Shape shape);
    void EndShape(float screenX, float screenY);
    bool IsDrawingShape() const { return shapeItemId_.has_value(); }

    // The painted layer of `item`, of which there is one - see
    // EnsurePaintedLayer, which reuses rather than stacks. Null if it has
    // none. Whatever state it is in: a caller that needs pixels asks
    // Layer::HasPaintedPixels.
    static Layer* FindPaintedLayer(Item& item, size_t* outIndex = nullptr);

    // ===== Capturing the screen =====

    // Grabs the whole display as it looks right now and holds it, so that
    // an overlay can show a still picture instead of the live application
    // and captures can be cropped out of it (see CaptureShotItem). Replaces
    // whatever was frozen before.
    void FreezeScreen(const platform::DisplayInfo& display);
    // Drops that image. Call on hiding the overlay: it's a full-screen
    // texture, and there's no reason to hold one while nothing is shown.
    void ReleaseFrozenScreen();
    // 0 while nothing is frozen - which is also how "the capture failed"
    // is represented.
    uint64_t FrozenScreenTexture() const { return frozenScreenTexture_; }
    // Captures what is under `item` into its picture layer - cropped out of
    // the frozen screen while one is held, live otherwise - uploads it, and
    // writes it to disk at once rather than waiting for the autosave (see
    // LibraryStore::SaveImage).
    void CaptureShotItem(Item& item);
    // Gives the copy `copyId` a picture file and texture of its own, read
    // from `sourceId`'s - a copy must never share either with its source.
    void CloneShotImageForCopy(ItemId sourceId, ItemId copyId);

private:
    // A library, as the session holds one: the records, where they are
    // written, and the debounced autosave between the two.
    struct LibraryInstance {
        CanvasManager manager;
        persistence::LibraryStore* store = nullptr;
        // lastSavedGeneration starts at 0, matching
        // CanvasManager::Generation()'s own starting value, so a freshly
        // loaded/started library (nothing changed yet) correctly reads as
        // "nothing pending" from the very first frame.
        uint64_t lastObservedGeneration = 0;
        uint64_t lastSavedGeneration = 0;
        float secondsSinceLastChange = 0.0f;
        float secondsSinceFirstUnsavedChange = 0.0f;
        // After a failed save: how long until the next attempt, and how
        // long the wait after *that* would be, doubling per failure up to
        // a ceiling (see kAutosaveRetryMaxSeconds). Zero when the last save
        // succeeded. The quiet-period timers above are the content's;
        // these are the disk's, and a failure resets neither of the former
        // - which is why they cannot double as a retry delay.
        float saveRetryCountdownSeconds = 0.0f;
        float saveRetryBackoffSeconds = 0.0f;
    };

    // Writes every painted layer's pixels out as QOI and records the
    // filename on the layer. Called just before the library metadata is
    // written, so the two always agree about what is on disk. Returns false
    // if any layer's pixels could not be written; that layer stays dirty,
    // and the save this is part of is not acknowledged.
    bool SavePaintedLayers(LibraryInstance& instance);
    void UpdateAutosave(LibraryInstance& instance, float deltaSeconds);
    // The actual write. True means everything the current generation covers
    // is on disk: every painted layer's pixels and every record. False means
    // it is not, nothing is acknowledged, and a retry is scheduled on its
    // own backoff.
    bool SaveLibraryNow(LibraryInstance& instance);
    void FlushIfDirty(LibraryInstance& instance);
    std::optional<platform::CaptureResult> CropFrozenScreen(const Rect& rect) const;

    // ----- Undo -----

    // A single reversible action, pushed at the moment it happens and
    // reversed by Undo, which in turn pushes the very same entry onto the
    // redo stack for Redo to reapply. Deliberately narrow in scope - covers
    // exactly the frequent, low-stakes case (a quick way to take back the
    // last drawing change or an accidental snippet delete) rather than a
    // full command-pattern undo for every mutation. Session-only, never
    // persisted.
    //
    // Every field an undo needs to reverse an action is also everything a
    // redo needs to reapply it - Undo moves the popped entry onto the redo
    // stack unchanged for every kind but the swapping ones, which exchange
    // their own contents with the item's as they apply, so the entry that
    // lands on the opposite stack holds exactly what that direction needs.
    struct UndoEntry {
        enum class Kind {
            // itemId + strokes[0]: a stroke was appended to item.strokes -
            // undo pops it back off; redo pushes strokes[0] back on, which
            // is why this is populated at push time even though undo itself
            // only needs itemId.
            StrokeBaked,
            // One erase gesture - the circular eraser, the rectangular one,
            // or "Clear drawing" - over an item that may carry vector
            // strokes, painted pixels, or both. One entry for the whole
            // gesture, whichever kinds of ink it touched: an eraser that
            // took two undos to take back one drag, because the strokes and
            // the pixels went on the stack separately, read as a bug.
            //
            // The vector half is itemId + strokeCountBefore + replacements
            // (see CanvasManager::EraseAt - a stroke only partly within the
            // eraser is shortened/split rather than removed outright, and
            // the fragments stand where it stood). Each replacement names
            // one original by its index in the list as it was before the
            // gesture, carries that original exactly, and carries the
            // fragments that stand in its place afterwards. Undo rebuilds
            // the before-list from the after-list, redo the reverse, both
            // by position and neither by value: an undo that matched
            // strokes by value put the restored originals at the end,
            // which changed the draw order and left the next undo of a
            // stroke taking off a different stroke than the one it was
            // for, and could not tell two equal strokes apart. Empty when
            // the gesture clipped nothing.
            //
            // The painted half is layerIndex + paintedTiles (the tiles the
            // brush touched, as they were before - see PaintedImage::
            // EndStroke) or layerIndex + paintedBefore (the whole image, for
            // a clear). Undo puts them back and keeps what they replaced,
            // which is the redo state - so this half is its own inverse and
            // the entry swaps its own contents each way (SwapPaintedUndoState).
            // Empty when the gesture touched no pixels.
            Erased,
            // canvasId + deletedItemId: this snippet on canvasId was
            // deleted. The snippet itself is still there, marked - undo
            // clears the mark, redo makes it again.
            ItemDeleted,
            // itemId + previousNoteText: a text edit (see BeginTextEdit/
            // EndTextEdit) actually changed the note - undo swaps
            // previousNoteText back in. One entry per edit, not per
            // keystroke.
            NoteTextChanged,
            // itemId + layerIndex + paintedTiles: a brush stroke painted
            // onto a painted layer (an erase that touches pixels is an
            // Erased entry above, since it may have touched strokes too).
            // `paintedTiles` holds the pixels of every tile it touched
            // *before* it ran - see PaintedImage::EndStroke, which
            // produces exactly this. Undo puts them back and keeps what it
            // replaced, which is the redo state, so one entry serves both
            // directions by swapping its own contents. Tiles rather than
            // the whole layer because a fullscreen layer is 8 MB and a
            // stroke touches a few hundred kilobytes of it.
            PaintedTilesChanged,
            // itemId: this snippet was made, on the canvas the entry is
            // filed under (see CreateItem). ItemDeleted the other way
            // round: undo marks it deleted, redo restores it.
            ItemCreated,
        };
        // One original an erase gesture touched: where it was, what it
        // was, and what stands in its place - see Kind::Erased.
        struct StrokeReplacement {
            size_t index = 0;
            Stroke original;
            std::vector<Stroke> fragments;
        };
        Kind kind = Kind::StrokeBaked;
        ItemId itemId = 0;
        CanvasId canvasId = 0;
        std::vector<Stroke> strokes;                  // StrokeBaked: the one stroke, strokes[0]
        std::vector<StrokeReplacement> replacements;  // Erased only, ascending by index
        size_t strokeCountBefore = 0;                 // Erased only: the list's length before the gesture
        ItemId deletedItemId = 0;                     // ItemDeleted only
        std::string previousNoteText;        // NoteTextChanged only
        size_t layerIndex = 0;                        // Erased / PaintedTilesChanged: which of the item's layers
        std::vector<PaintedTile> paintedTiles;        // Erased (a brush erase) / PaintedTilesChanged
        std::shared_ptr<PaintedImage> paintedBefore;  // Erased only, for a whole-layer clear
    };
    // The painted half of an edit, before it becomes an entry: the tiles a
    // brush gesture touched, or the whole image a clear replaced.
    struct PaintedUndo {
        ItemId itemId = 0;
        size_t layerIndex = 0;
        std::vector<PaintedTile> tiles;
        std::shared_ptr<PaintedImage> wholeImage;
        bool Empty() const { return tiles.empty() && !wholeImage; }
    };

    // Appends `entry` to the current canvas's history, evicting past the
    // caps, and clears that canvas's redo stack: a new action makes whatever
    // was on it unreachable by any sequence of undos.
    void PushUndo(UndoEntry entry);
    // The one push every stack goes through - PushUndo's, and the two
    // hand-overs between undo and redo - so the caps live in one place: at
    // most kUndoStackCap entries, and at most kUndoStackCapBytes of pixels
    // and points between them. Evicts from the oldest end until both hold,
    // always keeping the entry just pushed.
    static void PushCapped(std::deque<UndoEntry>& stack, UndoEntry entry);
    static size_t UndoEntryBytes(const UndoEntry& entry);
    // Pops the current canvas's top entry off one stack, applies it, and -
    // if it took effect - pushes it onto the other.
    std::optional<UndoStep> StepHistory(bool undo);
    // Applies `entry` one way or the other and says what it was - nullopt
    // when the item or canvas it names is already gone.
    std::optional<UndoWhat> ApplyUndoEntry(UndoEntry& entry, bool undo);
    // The vector half of an Erased entry, one function per direction - see
    // UndoEntry::Kind::Erased. False, changing nothing, when the item's
    // strokes are not the list the entry describes.
    static bool RestoreStrokesBeforeErase(Item& item, const UndoEntry& entry);
    static bool ReapplyErase(Item& item, const UndoEntry& entry);
    // The painted half of an entry, applied in either direction: puts back
    // the tiles (or the whole image) the entry holds and keeps what they
    // replaced, so the entry is its own inverse afterwards.
    bool SwapPaintedUndoState(UndoEntry& entry);
    // Forgets everything recorded for a canvas - called when the canvas is
    // deleted for good, which is safe precisely because there is nothing
    // left there that could ever want these. A canvas merely deleted keeps
    // its history for when it is restored.
    void DropHistoryOfCanvas(CanvasId canvasId);
    // Starts following `itemId`'s strokes through an erase gesture: keeps
    // the list as it is now, and notes that every stroke is still its own
    // original - see eraseGestureStartSnapshot_.
    void SnapshotStrokesForErase(ItemId itemId);
    // Folds one erase call's outcome (see CanvasManager::EraseAt) into what
    // is being followed: each stroke the call clipped is marked replaced,
    // and its fragments are noted as standing for the same original it did.
    void NoteEraseOutcome(const std::vector<size_t>& outcome);
    // Pushes one Erased entry for the whole gesture, built from what was
    // followed: every original marked replaced, with the fragments now
    // standing for it, *and* the painted pixels the same gesture took away.
    // No-op if neither half changed anything.
    void PushEraseGestureUndoEntry(ItemId itemId, PaintedUndo painted);

    // ----- Painting -----

    // The painted layer `item` paints into, in a paintable state: created
    // on first use - sized from the item's native size times the resolution
    // multiplier, so an item that is never painted on costs nothing - or,
    // when the item already has one that lost its pixels or never got a
    // texture, that same layer given what it lacks rather than a second one
    // beside it. Null without a window to make a texture with, or for a
    // degenerate size.
    Layer* EnsurePaintedLayer(Item& item);
    // Screen space to a painted layer's own pixels: through the item's
    // native space (the same transform strokes are baked with), then scaled
    // by the layer's own resolution.
    void ScreenToPaintedPixels(const Item& item, const Layer& layer, float screenX, float screenY, float& outX,
                               float& outY) const;
    // Pushes the dirty rectangle a brush just wrote to the layer's texture,
    // and marks the layer's pixels as not yet on disk.
    void UploadPaintedRegion(Layer& layer, const PixelRect& region);
    // The three halves of a brush gesture, for the pen and the eraser alike.
    // `erase` picks the blend and decides one more thing: a pen creates the
    // painted layer it needs, an eraser only acts on one that is already
    // there with pixels in it, so an erase over a snippet nothing was
    // painted on starts no stroke and leaves no blank layer behind.
    void BeginPaintStroke(Item& item, float screenX, float screenY, bool erase, uint32_t colorRGBA,
                          float widthScreenPx);
    void ExtendPaintStroke(float screenX, float screenY);
    // Ends the stroke and hands back what undo needs - empty for a stroke
    // that marked nothing.
    PaintedUndo EndPaintStroke();
    void PushPaintedTilesUndo(PaintedUndo painted);
    // The rectangular eraser's pixel half, opening and closing its own
    // brush stroke.
    PaintedUndo ErasePaintedLayersInRect(Item& item, float minX, float minY, float maxX, float maxY);
    // Empties the painted layer on `item`, returning the old image whole.
    PaintedUndo ClearPaintedLayers(Item& item);

    platform::IOverlayWindow* window_ = nullptr;
    // A library as the session holds one - see LibraryInstance. The shape
    // is kept for a second one (an archive, say) should the app ever look
    // at two.
    LibraryInstance library_;
    // Which canvas's shot textures are currently resident on the GPU, or
    // nullopt before the first sync has run. Compared against the current
    // canvas by EnsureTexturesForCurrentCanvas; a mismatch is what triggers
    // the sync.
    std::optional<CanvasId> shotTextureCanvasId_;

    // Undo/redo history, kept *per canvas* rather than as one global stack.
    // One global stack meant an undo could reach a canvas that isn't even
    // on screen: draw on A, switch to B, press undo - and a stroke vanishes
    // from A, which you aren't looking at. With the split, no undo can
    // touch a canvas that isn't on screen: every entry resolves an item that
    // was on the canvas the entry is filed under, and the one way an entry
    // could outlive that - its item being moved to another canvas - is
    // closed by ForgetHistoryOfItem. A canvas's entries are dropped outright
    // when it is deleted for good (see DropHistoryOfCanvas).
    std::unordered_map<CanvasId, std::deque<UndoEntry>> undoStacks_;
    // What Undo has taken back, most recent last - same keying and caps.
    std::unordered_map<CanvasId, std::deque<UndoEntry>> redoStacks_;
    // A copy of the erased item's whole stroke list, taken as an eraser
    // gesture begins, and beside it where every stroke currently in the
    // list came from: eraseOrigins_ is parallel to the item's strokes and
    // holds, for each, the index in the snapshot of the original it is or
    // stands for; eraseReplaced_ is parallel to the snapshot and marks the
    // originals some call has clipped. Together they say, at the gesture's
    // end, exactly which fragments stand for which original - however many
    // times a fragment was clipped again by a later call in the same drag -
    // which is what one entry for the *whole* gesture needs, and what a
    // before/after diff by value could not say (see UndoEntry::Kind::Erased).
    std::vector<Stroke> eraseGestureStartSnapshot_;
    std::vector<size_t> eraseOrigins_;
    std::vector<bool> eraseReplaced_;
    // The item the circular eraser gesture in progress is erasing, if any.
    std::optional<ItemId> eraseItemId_;
    // The text edit in progress, and the note as it was when it began.
    std::optional<ItemId> textEditItemId_;
    std::string textEditOriginal_;
    // The brush stroke in progress, if any - which item and which of its
    // layers, so the tiles it saves go on the stack against the right one.
    // `paintStrokeTouched_` stays false for a stroke that never marked
    // anything, which is what stops a stray click pushing an empty entry.
    std::optional<ItemId> paintStrokeItemId_;
    size_t paintStrokeLayerIndex_ = 0;
    bool paintStrokeTouched_ = false;
    // Where the stroke last was, in screen space - a layer's own pixel grid
    // is one conversion away and differs per layer, while the gesture is
    // defined in the space the hand moves in.
    float paintLastScreenX_ = 0.0f;
    float paintLastScreenY_ = 0.0f;

    // Drops the shape in progress without leaving anything - what EndShape
    // does with a drag too short to be meant.
    void CancelShape();
    // The shape in progress, if any - see BeginShape. Where it began and
    // where the pointer last was, so SetShape can redraw it without a move.
    std::optional<ItemId> shapeItemId_;
    Shape shape_ = Shape::Line;
    float shapeStartX_ = 0.0f;
    float shapeStartY_ = 0.0f;
    float shapeLastX_ = 0.0f;
    float shapeLastY_ = 0.0f;
    uint32_t shapeColorRGBA_ = 0;
    float shapeWidth_ = 0.0f;
    bool shapePaintsPixels_ = false;

    // The frozen screen, while one is held - see FreezeScreen. The pixels
    // are kept for CaptureShotItem to crop out of.
    uint64_t frozenScreenTexture_ = 0;
    int frozenScreenWidth_ = 0;
    int frozenScreenHeight_ = 0;
    std::vector<uint8_t> frozenScreenPixels_;
};

}  // namespace sz::core
