#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/canvas/canvas_manager.h"
#include "core/drawing/stroke.h"
#include "core/persistence/library_store.h"
#include "core/session/undo_entry.h"
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
    // Every snippet that comes in marked deleted is then deleted for good:
    // a deleted snippet only ever comes back by undo, and a history never
    // outlives the session it was made in, so nothing could reach it again.
    void ImportLibrary(CanvasManagerSnapshot snapshot);

    // The library: what every gesture, the Overview and the texture sync act
    // on.
    CanvasManager& Manager() { return library_.manager; }
    const CanvasManager& Manager() const { return library_.manager; }

    // ===== Deleting and restoring =====
    //
    // A delete is a mark made in place, hidden until it is restored or
    // deleted for good - see CanvasManager's class comment.

    // Marks the folder, canvas or snippet `id` names deleted, now. What
    // every delete is but a snippet's own, which is DeleteItem, undoably.
    // False, doing nothing, if there is no such thing or it is already
    // marked.
    bool Delete(uint64_t id);
    // Clears the mark on `id` and on whatever holds it - see
    // CanvasManager::Restore. False if nothing was deleted.
    bool Restore(uint64_t id);
    // Erases `id` for good: out of the model, its textures released, its
    // history forgotten, and the library saved at once. False, doing
    // nothing, if there is no such thing.
    bool DeletePermanently(uint64_t id);
    // DeletePermanently for every canvas in `folderId` that carries a mark
    // of its own (see CanvasManager::MarkedCanvasesIn) - what a folder's
    // "Delete permanently" is while the folder itself is not deleted: what
    // is deleted in it goes, and the folder and the rest stay. False if
    // there was none.
    bool DeleteMarkedCanvasesPermanently(FolderId folderId);
    // Deletes for good every folder and canvas deleted before `cutoff`
    // (seconds since the epoch) - see CanvasManager::MarkedBefore for which
    // those are - and returns how many went. The retention period
    // (AppConfig::purgeDeleted), run once the library is opened. Without
    // DeletePermanently's texture sync, like ImportLibrary's own erasing and
    // for the same reason; and nothing deleted has a texture to give back.
    size_t EraseDeletedBefore(int64_t cutoff);
    // ===== Keeping the disk and the GPU in step =====

    // The debounced autosave - see kAutosaveQuietSeconds for the policy.
    // Call once per frame.
    void Tick(float deltaSeconds);
    // Writes the library right now if anything's changed since its last
    // save, bypassing the debounce - for a safe point where no frame will
    // come soon enough to catch it (hiding the overlay, exiting). True
    // when everything is on disk afterwards - nothing was pending, or the
    // save landed whole. False means something is not, and stays owed:
    // the caller decides what to tell the user, the next Tick or Flush
    // tries again.
    bool Flush();
    // Whether anything is owed to the disk: a change since the last save
    // that landed, or a capture whose picture could not be written yet
    // (see CaptureShotItem). What the autosave and Flush act on, and what a
    // UI can show.
    bool HasUnsavedChanges() const;
    // Whether the most recent save attempt failed and is waiting to be
    // retried - for a UI to say so. Cleared by the save that lands.
    bool LastSaveFailed() const { return library_.saveRetryBackoffSeconds > 0.0f; }
    // Writes what is in memory to a new library at `file`, pictures
    // included - from this session where it holds the pixels (a capture
    // whose write has not landed), and from the real library otherwise, so
    // that the copy opens on its own. A note beside it (`file` plus ".txt")
    // says where it came from and whether it is whole. For the moment the
    // app has to go - exit, the OS ending the session - and the library it
    // was working in cannot be written: the alternative is losing the
    // changes silently. True if the copy is whole; false if a picture could
    // not be copied, or the copy itself not written.
    bool WriteRecoveryCopy(const std::filesystem::path& file);

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

    // Every texture this handed out is lost - the GPU device was replaced
    // (see IOverlayWindow::TextureGeneration). Each is let go of, and made
    // again from what it showed: the current canvas's pictures on the next
    // EnsureTexturesForCurrentCanvas, from the library, a picture
    // not written yet from the pixels kept for it, and the frozen screen
    // from the pixels kept for cropping.
    void ReplaceLostTextures();

    // ===== Undo =====

    // What a step of undo or redo took back or put back - for a UI to say
    // so. `undone` is true for an undo, false for a redo.
    enum class UndoWhat { Stroke, Erase, Delete, TextEdit, Create, Placement, Paste, Duplicate };
    struct UndoStep {
        UndoWhat what = UndoWhat::Stroke;
        bool undone = true;
        // The step could not be taken, and was dropped so that the next
        // one can be: a paste whose snippets were moved here and have
        // nowhere to go back to, or no longer anything to come back from
        // - see RecordArrivals. Nothing changed, and a UI says why.
        bool refused = false;
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
    // Counts the changes to the history that move its top - an entry
    // filed, undone or redone - so that a caller can tell whether anything
    // came between two of its own edits (see
    // OverlayApp::RecordPlacementBurst). A merge into the top entry is that
    // entry, and counts for nothing; so does forgetting a snippet's or a
    // canvas's entries, or the oldest falling off the end.
    uint64_t HistoryRevision() const { return historyRevision_; }
    // Forgets every entry naming `itemId`, on `canvasId` only - for an item
    // moved to a different canvas, where its history would otherwise stay
    // filed under the canvas it left, and an undo there would edit an item
    // that now lives somewhere else.
    void ForgetHistoryOfItem(CanvasId canvasId, ItemId itemId);

    // ===== Edits that can be undone =====
    //
    // Everything that changes a snippet's ink or text goes through these,
    // so that it goes on the history as it happens. A snippet's placement -
    // moved, resized, fullscreen - is changed in the model directly and
    // recorded afterwards, once per gesture (see RecordPlacements).
    // Renames and reorders are not tracked.

    // Moves the stroke just finished on the current canvas's live layer
    // into `itemId`, in the item's own native space, and files it.
    void CommitLiveStroke(ItemId itemId);
    // Deletes a snippet on the current canvas, undoably - marked, and the
    // entry is what an undo restores. False if there is no such snippet
    // there, or it is deleted already.
    bool DeleteItem(ItemId itemId);
    // The same for several, as one undo step; those not on the current
    // canvas, or deleted already, are passed over. Returns how many were
    // deleted.
    size_t DeleteItems(const std::vector<ItemId>& itemIds);

    // Where a snippet is - see undo::Placement.
    using Placement = undo::Placement;
    // The placements of these snippets as they are now, for a later
    // RecordPlacements. Ids that name nothing are left out.
    std::vector<Placement> PlacementsOf(const std::vector<ItemId>& ids) const;
    // Files the change from `before` (taken with PlacementsOf) to how those
    // snippets are placed now as one entry - one undo for a whole drag,
    // however many events it took, and for every snippet it moved. Nothing
    // is filed when nothing changed: a click that selected and never moved.
    // With `merge`, a change that continues the last one - the same
    // snippets, and nothing else filed since - is folded into it instead,
    // keeping that entry's `before`: a burst of wheel notches or arrow-key
    // nudges is taken back in one step, to where it started. The caller
    // decides what counts as a burst. True if anything was filed or merged.
    bool RecordPlacements(std::vector<Placement> before, bool merge = false);
    // Makes a snippet on the current canvas, undoably - see
    // CanvasManager::CreateItem for what it starts as. Undone it is marked
    // deleted, where a capture taken by mistake can still be found, and
    // redone it is restored. 0, having made nothing, without a canvas.
    ItemId CreateItem(bool hasBackground, Rect rect, std::string name);
    // What a paste or a duplicate brought onto the current canvas, one
    // snippet each - see undo::Arrival.
    using Arrival = undo::Arrival;
    // A cut's paste of one snippet: moves `itemId` from the canvas holding
    // it onto the current one, on top, and leaves its history behind (see
    // ForgetHistoryOfItem). Nullopt, moving nothing, for no such snippet,
    // one deleted, or one already here.
    std::optional<Arrival> MoveItemHere(ItemId itemId);
    // Files everything one paste or duplicate brought as one entry. Undone,
    // the copies are marked deleted, as a new snippet undone is, and what
    // was moved goes back to where it stood on the canvas it came from.
    // When that canvas is gone or deleted, nothing moves: undone, the
    // snippets would land somewhere nobody can see them, and with a canvas
    // deleted for good nowhere at all - so they stay, and the step is
    // refused (see UndoStep::refused). Redone, the copies are restored and
    // what was moved comes back on top, or is refused the same way if it
    // has been deleted since. Nothing is filed for no arrivals.
    void RecordArrivals(std::vector<Arrival> arrivals, bool duplicate);
    // Removes a snippet nothing has been put into - no strokes, no text, no
    // picture of its own - as if it had never been
    // made: erased rather than marked, and off the history. For a snippet a
    // click made that turned out not to be meant. False, doing nothing, for
    // a snippet with anything in it or no snippet by that id.
    bool DiscardIfUntouched(ItemId itemId);
    // Whether nothing has been put into the snippet yet - the question
    // DiscardIfUntouched asks, on its own. False for no snippet by that id.
    bool IsUntouched(ItemId itemId) const;
    // Every stroke on a snippet, cleared as one undoable step. False if there was nothing to clear.
    bool ClearDrawing(ItemId itemId);
    // A text edit of a snippet's note: Begin remembers what the text was,
    // End commits the new text and files one entry for the whole edit if it
    // changed. End with nullopt abandons it. Beginning another ends the one
    // open, uncommitted. While one is open, an undo that would change that
    // item's text does nothing - its own commit would overwrite it anyway.
    void BeginTextEdit(ItemId itemId);
    void EndTextEdit(std::optional<std::string> text);

    // The eraser, as a gesture: clips the item's strokes under a circle
    // `widthScreenPx` across, and files the whole gesture as one entry when
    // it ends.
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
    // - and on End baked into the item as a stroke, as one undo entry.
    // SetShape changes which
    // shape it is mid-drag; Cancel drops it, leaving nothing.
    void BeginShape(ItemId itemId, Shape shape, float screenX, float screenY, uint32_t colorRGBA,
                    float widthScreenPx);
    void UpdateShape(float screenX, float screenY);
    void SetShape(Shape shape);
    void EndShape(float screenX, float screenY);
    bool IsDrawingShape() const { return shapeItemId_.has_value(); }

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
    // Gives the copy `copyId` a picture of its own when `sourceId` has one
    // - a copy must never share a file or a texture with its source. The
    // pixels come from this session when the source's capture is still
    // waiting to be written (see PendingPicture) and from disk otherwise,
    // and are written under the copy's own name at once (a write that
    // fails waits the same way). False when the source names a picture
    // that could not be read, so the copy lacks it: a UI says so rather
    // than showing a copy that looks whole.
    bool ClonePicturesForCopy(ItemId sourceId, ItemId copyId);

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

    // A capture's pixels whose write failed at capture time, kept so the
    // write can be tried again - see CaptureShotItem. A screenshot is the
    // one thing in the library that cannot be remade, so its pixels are
    // not let go of until they are on disk.
    struct PendingPicture {
        std::vector<uint8_t> pixelsRGBA;
        int width = 0;
        int height = 0;
    };
    // Tries again to write every pending picture, recording the filename
    // on its item's picture layer as it lands. Called before the records
    // are written, and part of the same all-or-nothing answer. A picture
    // whose item has since gone for good is dropped.
    bool SavePendingPictures(LibraryInstance& instance);
    void UpdateAutosave(LibraryInstance& instance, float deltaSeconds);
    // The actual write. True means everything the current generation covers
    // is on disk: every pending picture and every record. False means
    // it is not, nothing is acknowledged, and a retry is scheduled on its
    // own backoff.
    bool SaveLibraryNow(LibraryInstance& instance);
    bool FlushIfDirty(LibraryInstance& instance);
    std::optional<platform::CaptureResult> CropFrozenScreen(const Rect& rect) const;

    // ----- Undo -----

    // Appends `entry` to the current canvas's history, evicting past the
    // caps, and clears that canvas's redo stack: a new action makes whatever
    // was on it unreachable by any sequence of undos.
    void PushUndo(undo::Entry entry);
    // The one push every stack goes through - PushUndo's, and the two
    // hand-overs between undo and redo - so the caps live in one place: at
    // most kUndoStackCap entries, and at most kUndoStackCapBytes of points
    // and text between them. Evicts from the oldest end until both hold,
    // always keeping the entry just pushed.
    static void PushCapped(std::deque<undo::Entry>& stack, undo::Entry entry);
    // Pops the current canvas's top entry off one stack, applies it, and -
    // if it took effect - pushes it onto the other.
    std::optional<UndoStep> StepHistory(bool undo);
    // Applies an entry one way or the other and says what it was - nullopt
    // when the item or canvas it names is already gone. One overload per
    // kind of entry (see undo::Entry), so that a kind added without one
    // does not compile.
    std::optional<UndoWhat> Apply(undo::StrokeBaked& entry, bool undo);
    std::optional<UndoWhat> Apply(undo::Erased& entry, bool undo);
    std::optional<UndoWhat> Apply(undo::ItemDeleted& entry, bool undo);
    std::optional<UndoWhat> Apply(undo::NoteTextChanged& entry, bool undo);
    std::optional<UndoWhat> Apply(undo::ItemCreated& entry, bool undo);
    std::optional<UndoWhat> Apply(undo::PlacementChanged& entry, bool undo);
    std::optional<UndoWhat> Apply(undo::ItemsArrived& entry, bool undo);
    // The vector half of an Erased entry, one function per direction - see
    // undo::Erased. False, changing nothing, when the item's strokes are
    // not the list the entry describes.
    static bool RestoreStrokesBeforeErase(Item& item, const undo::Erased& entry);
    static bool ReapplyErase(Item& item, const undo::Erased& entry);
    // Whether every snippet an ItemsArrived entry moved can go the way
    // `undo` says: back to a canvas that is still there and not deleted,
    // or here again from one, itself not deleted. A copy always can.
    bool ArrivalsCanMove(const undo::ItemsArrived& entry, bool undo) const;
    // Forgets everything recorded for a canvas - called when the canvas is
    // deleted for good, which is safe precisely because there is nothing
    // left there that could ever want these. A canvas merely deleted keeps
    // its history for when it is restored.
    void DropHistoryOfCanvas(CanvasId canvasId);
    // DeletePermanently without the texture sync and the save that follow
    // it: the thing, its textures and its history. What ImportLibrary
    // erases with, before there is a device to sync against.
    bool Erase(uint64_t id);
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
    // standing for it. No-op if the gesture changed nothing.
    void PushEraseGestureUndoEntry(ItemId itemId);

    platform::IOverlayWindow* window_ = nullptr;
    // A library as the session holds one - see LibraryInstance. The shape
    // is kept for a second one (an archive, say) should the app ever look
    // at two.
    LibraryInstance library_;
    // See PendingPicture.
    std::unordered_map<ItemId, PendingPicture> pendingPictures_;
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
    std::unordered_map<CanvasId, std::deque<undo::Entry>> undoStacks_;
    // What Undo has taken back, most recent last - same keying and caps.
    std::unordered_map<CanvasId, std::deque<undo::Entry>> redoStacks_;
    // See HistoryRevision.
    uint64_t historyRevision_ = 0;
    // A copy of the erased item's whole stroke list, taken as an eraser
    // gesture begins, and beside it where every stroke currently in the
    // list came from: eraseOrigins_ is parallel to the item's strokes and
    // holds, for each, the index in the snapshot of the original it is or
    // stands for; eraseReplaced_ is parallel to the snapshot and marks the
    // originals some call has clipped. Together they say, at the gesture's
    // end, exactly which fragments stand for which original - however many
    // times a fragment was clipped again by a later call in the same drag -
    // which is what one entry for the *whole* gesture needs, and what a
    // before/after diff by value could not say (see undo::Erased).
    std::vector<Stroke> eraseGestureStartSnapshot_;
    std::vector<size_t> eraseOrigins_;
    std::vector<bool> eraseReplaced_;
    // The item the circular eraser gesture in progress is erasing, if any.
    std::optional<ItemId> eraseItemId_;
    // The text edit in progress, and the note as it was when it began.
    std::optional<ItemId> textEditItemId_;
    std::string textEditOriginal_;
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

    // The frozen screen, while one is held - see FreezeScreen. The pixels
    // are kept for CaptureShotItem to crop out of.
    uint64_t frozenScreenTexture_ = 0;
    int frozenScreenWidth_ = 0;
    int frozenScreenHeight_ = 0;
    std::vector<uint8_t> frozenScreenPixels_;
};

}  // namespace sz::core
