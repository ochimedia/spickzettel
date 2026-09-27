#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/canvas/canvas_manager.h"
#include "core/drawing/canvas_state.h"
#include "core/drawing/stroke.h"
#include "core/gpu/texture_cache.h"
#include "core/persistence/library_store.h"
#include "core/session/history.h"
#include "platform/i_overlay_window.h"
#include "platform/platform_types.h"

namespace sz::core {

// What the app is working on, independent of how it is shown: the library,
// what is on disk and what is on the GPU. A UI is a view of this: it reads
// the model, and changes it only by asking the session - every change is
// one of the commands below, so that each one is kept in step with the
// history, the disk and the GPU in one place, and none can be made past
// them. The model is handed out read-only for that reason.
//
// Every command is written to the library as it is made, in one
// transaction (see Land): the file holds what the model holds at every
// moment but in the middle of a gesture. A command whose write fails is
// not made - the model is put back as it was, and LastWriteFailed says so.
//
// No ImGui here, and nothing about gestures, popovers or panels. The window
// it is attached to is the platform's, for screen captures, and for the
// textures the app draws with - which are the TextureCache's, not the
// model's: the model is content only.
class Session {
public:
    Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // ===== What it is attached to =====

    // For textures and captures. May stay null - a test that never shows a
    // window - in which case nothing is uploaded or captured, and everything
    // else still works.
    void AttachWindow(platform::IOverlayWindow* window) {
        window_ = window;
        textures_.AttachWindow(window);
    }
    // Every texture the app draws with - see TextureCache. A capture puts
    // its picture's here as it is taken, so that it is not read back from
    // the library it was just written to.
    TextureCache& Textures() { return textures_; }
    // Where the library is read from and written to. Left null, it is never
    // persisted, and everything else works the same.
    void SetLibraryStore(persistence::LibraryStore* store) { store_ = store; }
    persistence::LibraryStore* Store() const { return store_; }

    // Wholesale-replaces the library with a previously-saved one - see
    // CanvasManager::ImportSnapshot. Called once, right after startup.
    // Every snippet that comes in marked deleted is then deleted for good:
    // a deleted snippet only ever comes back by undo, and a history never
    // outlives the session it was made in, so nothing could reach it again.
    void ImportLibrary(CanvasManagerSnapshot snapshot);

    // The library, to read: what every gesture and the Overview look at.
    // Changed only through the commands below.
    const CanvasManager& Manager() const { return manager_; }

    // Whether the last command's write failed - so that command was not
    // made - with none landing since: for a UI to say so, and keep saying
    // so until one does. A command with nothing to write lands nothing.
    bool LastWriteFailed() const { return lastWriteFailed_; }
    // How many writes have failed since the session began: compared across
    // a call, whether anything it wrote failed - where LastWriteFailed says
    // only whether the last write did.
    uint64_t FailedWrites() const { return failedWrites_; }
    // Writes the whole library as it is: for one begun in memory - a first
    // run's, a folder and a canvas - before its first command, which writes
    // only what it changes and would find what holds it missing. True
    // when it landed, and always without a store.
    bool WriteWholeLibrary();

    // ===== Changes that are not undone =====
    //
    // Where things are kept and how they are looked at, rather than what
    // they hold: none of these goes on the history, and none changes
    // anything a step on it reads. Like every command, each first ends the
    // gesture open, if any (see EndOpenGesture).

    // See CanvasManager::SwitchToCanvas / SwitchToFolder. A switch also
    // drops whatever stroke was being drawn (see LiveLayer).
    void SwitchToCanvas(CanvasId id);
    void SwitchToFolder(FolderId id);
    // See CanvasManager::AddCanvas / AddFolder.
    CanvasId AddCanvas(std::string name);
    FolderId AddFolder(std::string name);
    // See CanvasManager's functions of the same names.
    void RenameFolder(FolderId id, std::string name);
    void RenameCanvas(CanvasId id, std::string name);
    void ReorderFolder(FolderId id, size_t newIndex);
    void ReorderCanvas(CanvasId id, size_t newIndex);
    void MoveCanvasToFolder(CanvasId canvasId, FolderId folderId);
    // Pins or unpins, minimizes or brings back, each snippet among `ids`
    // that there is (see Item::pinned, Item::minimized).
    void SetPinned(const std::vector<ItemId>& ids, bool pinned);
    void SetMinimized(const std::vector<ItemId>& ids, bool minimized);
    // The stacking order - see CanvasManager::BringItemsToFront and
    // MoveItemLayer.
    void BringItemsToFront(const std::vector<ItemId>& ids);
    void MoveItemLayer(ItemId id, int direction);
    // Keeps every snippet's rect right for the display - see
    // CanvasManager::SyncItemsToDisplaySize. Not a change of anything: a
    // rect it sets follows from the snippet's anchor.
    void SyncItemsToDisplaySize(float width, float height);

    // ===== Deleting and restoring =====
    //
    // A delete is a mark made in place, hidden until it is restored or
    // deleted for good - see CanvasManager's class comment.

    // Marks the folder or canvas `id` names deleted, now. A snippet is
    // deleted by DeleteItems, undoably, and is refused here: its mark is
    // the history's to change (see history::DeletionChanged). False, doing
    // nothing, if there is no such folder or canvas or it is already
    // marked.
    bool Delete(uint64_t id);
    // Clears the mark on the folder or canvas `id` and on whatever holds it
    // - see CanvasManager::Restore. False if nothing was deleted, and for a
    // snippet, as Delete.
    bool Restore(uint64_t id);
    // Erases `id` for good: out of the model, its history forgotten, and
    // the library saved at once. False, doing nothing, if there is no such
    // thing.
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
    // (AppConfig::purgeDeleted), run once the library is opened.
    size_t EraseDeletedBefore(int64_t cutoff);

    // ===== Undo =====

    // What a step of undo or redo took back or put back - for a UI to say
    // so. `undone` is true for an undo, false for a redo.
    using UndoWhat = history::What;
    struct UndoStep {
        UndoWhat what = UndoWhat::Stroke;
        bool undone = true;
        // A paste undone sends a moved snippet back to the canvas it came
        // from, and when that canvas is deleted it goes there all the same,
        // to be found when the canvas is restored: this is that canvas, for
        // a UI to say where the snippet went. 0 otherwise.
        CanvasId intoDeletedCanvas = 0;
    };
    // Takes back the current canvas's most recent step, or puts back the
    // one most recently taken back. History is kept per canvas, and every
    // step on it applies whenever it is reached - see history::History.
    // Nullopt when there is nothing to take back or put back.
    std::optional<UndoStep> Undo();
    std::optional<UndoStep> Redo();
    // Whether there is anything for Undo/Redo to do on the current canvas -
    // and when there is, it is done: nothing on a stack is ever refused.
    bool CanUndo() const;
    bool CanRedo() const;

    // The history itself, to read - for the tests.
    const history::History& History() const { return history_; }

    // ===== Edits that can be undone =====
    //
    // Everything that changes what a snippet holds, where it is or how it
    // looks goes through these, and onto the history as it happens.

    // Where a stroke is drawn while it is being drawn, in screen space:
    // scratch, not content, empty whenever no stroke is under way, and
    // dropped on a canvas switch so that none can follow the switch. The
    // UI draws into it; CommitLiveStroke, or a shape (see BeginShape),
    // moves what is in it into a snippet.
    CanvasState& LiveLayer() { return liveLayer_; }
    const CanvasState& LiveLayer() const { return liveLayer_; }
    // Moves the stroke just finished on the live layer into `itemId`, in
    // the item's own native space, and files it.
    void CommitLiveStroke(ItemId itemId);
    // Deletes a snippet on the current canvas, undoably - marked, and the
    // entry is what an undo restores. False if there is no such snippet
    // there, or it is deleted already.
    bool DeleteItem(ItemId itemId);
    // The same for several, as one undo step; those not on the current
    // canvas, or deleted already, are passed over. Returns how many were
    // deleted.
    size_t DeleteItems(const std::vector<ItemId>& itemIds);

    // ----- Where snippets are -----

    // A move or a resize by hand, as a gesture: Begin notes where the
    // snippets `ids` are, each Preview moves one of them there and then
    // (re-anchored, see CanvasManager::CommitItemLayout), and End files
    // the whole gesture as one entry - one undo for a whole drag, however
    // many events it took, and for every snippet it moved. Nothing is
    // filed when nothing changed: a click that selected and never moved.
    // Beginning another ends the one open.
    void BeginPlacement(const std::vector<ItemId>& ids);
    // Whether a placement is open holding exactly the snippets `ids`, in
    // that order - one a burst began, still going on.
    bool Placing(const std::vector<ItemId>& ids) const;
    // Whether a placement, or a style edit, is open at all.
    bool PlacementOpen() const { return placement_.has_value(); }
    bool StyleEditOpen() const { return styleEdit_.has_value(); }
    void PreviewRect(ItemId id, Rect rect);
    // Takes a fullscreen snippet out of fullscreen as part of the gesture -
    // what taking hold of one does. No-op for one that is not fullscreen.
    void PreviewLeaveFullscreen(ItemId id);
    // Files the gesture as one step. True if anything was filed.
    bool EndPlacement();
    // Ends it leaving no trace: every snippet it held back where the press
    // found it, nothing filed and nothing written - what Escape does to a
    // drag. The previews only ever changed the model in memory, so going
    // back to the checkpoint the gesture took is exact.
    void CancelPlacement();
    // A placement gesture in one call: each snippet to its rect, re-anchored.
    bool SetRects(const std::vector<std::pair<ItemId, Rect>>& rects);
    // See CanvasManager::ToggleFullscreen / ResetItemToNativeSize; one
    // entry each.
    void ToggleFullscreen(ItemId id, bool stretch);
    void ResetItemToNativeSize(ItemId id);

    // ----- How snippets look -----

    // A style change as it is being made - a slider dragged, a color
    // picked, the opacity wheel spun over several snippets: each takes its
    // style at once, and the edit ends with EndStyleEdit, or with the next
    // command of any other kind. It goes on for as long as it is about the
    // same snippets; a preview about others ends it and begins another.
    void PreviewStyles(const std::vector<std::pair<ItemId, ItemStyle>>& styles);
    void PreviewStyle(ItemId id, const ItemStyle& style) { PreviewStyles({{id, style}}); }
    // Files the edit as one step, with every snippet it held when any of
    // them changed. True if anything was filed.
    bool EndStyleEdit();
    // The edit undone as if never made: the snippets' styles as they were
    // before, nothing filed - see CancelPlacement.
    void CancelStyleEdit();
    // Styles for several snippets at once, as one step: an edit begun and
    // ended, and the same answer.
    bool SetStyles(const std::vector<std::pair<ItemId, ItemStyle>>& styles);

    // ----- Making, copying and moving snippets -----

    // Makes a snippet on the current canvas as `prototype` says - see
    // CanvasManager::CreateItem(Item) - and, for a screenshot, captures
    // what is under it into its picture (see CaptureShotItem). Undone it
    // is marked deleted, where a capture taken by mistake can still be
    // found, and redone it is restored. Made with `undoable` false, it is
    // not on the history at all: the app's own welcome notes. 0, having
    // made nothing, without a canvas.
    ItemId CreateItem(Item prototype, bool undoable = true);
    ItemId CreateItem(bool hasBackground, Rect rect, std::string name);

    // What a paste, a duplicate or a send left where it went.
    struct Placed {
        // The snippets now there: the copies, or those moved.
        std::vector<ItemId> items;
        // A copy whose source's picture could not be read: the copy lacks
        // it, and a UI says so rather than showing a copy that looks whole.
        bool pictureLost = false;
    };
    // The clipboard's paste onto the current canvas: copies of `ids`, or,
    // for a cut, the snippets themselves, moved here on top. One deleted
    // since it was copied is passed over; a cut one already here stays
    // where it is. A copy of a snippet on this canvas is offset from it,
    // so that the two can be told apart; one from elsewhere keeps its
    // place. One entry for the whole paste: undone, the copies are marked
    // deleted, and what was moved goes back where it stood on the canvas
    // it came from.
    Placed Paste(const std::vector<ItemId>& ids, bool cut);
    // A copy of each of `ids` on the current canvas, offset from it - Copy
    // and Paste in one step, one entry.
    Placed Duplicate(const std::vector<ItemId>& ids);
    // Moves `ids` off the current canvas to `target`, or copies them there
    // - the Overview's picker, and the new canvas the selection is taken
    // to. None to the current canvas or a deleted one.
    Placed SendItemsTo(const std::vector<ItemId>& ids, CanvasId target, bool copy);

    // Every stroke on a snippet, cleared as one undoable step. False if there was nothing to clear.
    bool ClearDrawing(ItemId itemId);
    // A text edit of a snippet's note, as a gesture: Begin remembers what
    // the text was, each Preview sets the note to what has been typed so
    // far, and End - given the final text, or taking the note as it stands
    // - files one step for the whole edit if it changed. Like every
    // gesture, it is ended by the next command of any other kind, which is
    // why the text is the note's as it is typed: whatever ended it keeps
    // what was typed.
    void BeginTextEdit(ItemId itemId);
    void PreviewText(std::string text);
    void EndTextEdit(std::optional<std::string> text = std::nullopt);
    // The snippet whose note is being edited, while one is - for a UI to
    // see that its editor was ended from elsewhere.
    std::optional<ItemId> TextEditItem() const { return textEditItemId_; }

    // The eraser, as a gesture: clips the item's strokes under a circle
    // `widthScreenPx` across, and files the whole gesture as one entry when
    // it ends.
    void BeginErase(ItemId itemId, float screenX, float screenY, float widthScreenPx);
    void ExtendErase(float screenX, float screenY, float widthScreenPx);
    void EndErase();
    // The erase undone as if never begun - see CancelPlacement.
    void CancelErase();
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
    // Drops the shape in progress without leaving anything - what EndShape
    // does with a drag too short to be meant, and what Escape does to one.
    void CancelShape();
    bool IsDrawingShape() const { return shapeItemId_.has_value(); }

    // ===== Capturing the screen =====

    // Grabs the whole display as it looks right now and holds it, so that
    // an overlay can show a still picture instead of the live application
    // and captures can be cropped out of it (see CaptureShotItem). Replaces
    // whatever was frozen before.
    void FreezeScreen(const platform::DisplayInfo& display);
    // Drops that image, and its texture with it. Call on hiding the
    // overlay: it's a full screen's worth, and there's no reason to hold
    // one while nothing is shown.
    void ReleaseFrozenScreen();
    // The frozen screen's texture, for this frame (see TextureCache): 0
    // while nothing is frozen - which is also how "the capture failed" is
    // represented - or while it cannot be uploaded.
    uint64_t FrozenScreenTexture();
private:
    // What the tests reach the model through, to set up a library without
    // going command by command - see tests/support/session_test_access.h.
    friend struct SessionTestAccess;
    CanvasManager& Model() { return manager_; }
    const CanvasManager& Model() const { return manager_; }

    // ----- Writing -----

    // What a command could change, before it runs - see
    // CanvasManager::TakeCheckpoint. `items` are the snippets whose content
    // it may change; the rest of the library it may only reorder, add to or
    // take from.
    using Checkpoint = CanvasManager::Checkpoint;
    Checkpoint Before(const std::vector<ItemId>& items) const { return Model().TakeCheckpoint(items); }
    // Writes everything that changed since `before`, with the pictures the
    // command made (see captured_), in one transaction. When the write
    // fails, the model is put back as `before` has it - the command is not
    // made - and LastWriteFailed says so. True when it landed, and always
    // without a store.
    bool Land(const Checkpoint& before);
    // Land, then file `step` on `canvas`'s history, and let the histories of
    // the snippets the command moved follow them (see migrations_). False
    // when the write failed, and then nothing is filed.
    bool Commit(const Checkpoint& before, CanvasId canvas, history::Step step);
    // Erases what `ids` name for good - folders, canvases or snippets - as
    // one command: out of the model and the library, their textures
    // released and their history forgotten once it has landed. How many
    // went; none when the write failed.
    size_t EraseForGood(const std::vector<uint64_t>& ids);

    // Captures what is under `item` into its picture - cropped out of the
    // frozen screen while one is held, live otherwise - and puts its
    // texture in the cache. The pixels are written with the command that
    // made the snippet (see captured_).
    void CaptureShotItem(Item& item);
    // Gives the copy `copyId` a picture of its own when `sourceId` has one,
    // copied as stored, with the command that made the copy. False when
    // the source's picture cannot be found in the library, so the copy
    // lacks it.
    bool ClonePicturesForCopy(ItemId sourceId, ItemId copyId);
    // Ends whatever gesture is open - a text edit (as it stands), a
    // placement, a style edit, an erase or a shape - and files it, so that
    // the command about to run comes after it on the history. Every command
    // calls this first; a gesture's own calls continue it instead. One
    // gesture at a time is what keeps a step from being filed in the middle
    // of another's changes, where undoing it would undo part of those.
    //
    // False when the gesture's write failed, so it was not made - and then
    // the command does nothing more: it would act on a library the hand
    // did not leave it in, an undo taking back a second thing where the
    // failed one had already gone back, and its own write would land and
    // clear LastWriteFailed before the failure was ever seen.
    [[nodiscard]] bool EndOpenGesture();
    // Where each of these snippets is now. Ids that name nothing are left
    // out.
    using Placements = std::vector<std::pair<ItemId, history::Placement>>;
    Placements PlacementsOf(const std::vector<ItemId>& ids) const;
    // Commits the change from `before` to how those snippets are placed now
    // as one step - see EndPlacement.
    bool CommitPlacements(const Checkpoint& checkpoint, const Placements& before);
    // A cut's paste or a send of one snippet: moves `itemId` from the
    // canvas holding it onto `target`, on top; its history follows once the
    // command lands (see migrations_). The change for the step that files
    // it, or nullopt, moving nothing, for no such snippet, one deleted, or
    // one already there.
    std::optional<history::Change> MoveItemTo(ItemId itemId, CanvasId target);
    // Moves a copy just made off its source, far enough to see that there
    // are two, and re-anchors it there.
    void OffsetCopy(ItemId copyId);
    // Applies the step on top of one of the current canvas's stacks.
    std::optional<UndoStep> StepHistory(bool undo);
    std::optional<platform::CaptureResult> CropFrozenScreen(const Rect& rect) const;

    // Starts following `itemId`'s strokes through an erase gesture: keeps
    // the list as it is now, and notes that every stroke is still its own
    // original - see eraseGestureStartSnapshot_.
    void SnapshotStrokesForErase(ItemId itemId);
    // Folds one erase call's outcome (see CanvasManager::EraseAt) into what
    // is being followed: each stroke the call clipped is marked replaced,
    // and its fragments are noted as standing for the same original it did.
    void NoteEraseOutcome(const std::vector<size_t>& outcome);
    // Files one step for the whole gesture, built from what was followed:
    // every original marked replaced, with the fragments now standing for
    // it. No-op if the gesture changed nothing.
    void RecordEraseGesture(ItemId itemId);

    platform::IOverlayWindow* window_ = nullptr;
    CanvasManager manager_;
    persistence::LibraryStore* store_ = nullptr;
    // See LastWriteFailed and FailedWrites.
    bool lastWriteFailed_ = false;
    uint64_t failedWrites_ = 0;
    // What the command in progress has for Land to write besides rows: the
    // pixels of the screenshots it captured, and the pictures its copies
    // take from their sources. Emptied by every Land, landed or not.
    struct Captured {
        ItemId item = 0;
        std::vector<uint8_t> pixelsRGBA;
        int width = 0;
        int height = 0;
    };
    std::vector<Captured> captured_;
    std::vector<std::pair<ItemId, ItemId>> pictureCopies_;
    // The snippets the command in progress moved between canvases, and
    // where to: their histories follow them once it has landed, and not
    // before - a command whose write fails has moved nothing.
    std::vector<std::pair<ItemId, CanvasId>> migrations_;
    // See Textures.
    TextureCache textures_;

    // The undo history, per canvas - see history::History.
    history::History history_;
    // A copy of the erased item's whole stroke list, taken as an eraser
    // gesture begins, and beside it where every stroke currently in the
    // list came from: eraseOrigins_ is parallel to the item's strokes and
    // holds, for each, the index in the snapshot of the original it is or
    // stands for; eraseReplaced_ is parallel to the snapshot and marks the
    // originals some call has clipped. Together they say, at the gesture's
    // end, exactly which fragments stand for which original - however many
    // times a fragment was clipped again by a later call in the same drag -
    // which is what one entry for the *whole* gesture needs, and what a
    // before/after diff by value could not say (see history::StrokesErased).
    std::vector<Stroke> eraseGestureStartSnapshot_;
    std::vector<size_t> eraseOrigins_;
    std::vector<bool> eraseReplaced_;
    // The item the circular eraser gesture in progress is erasing, if any,
    // and the library as it was when the gesture began.
    std::optional<ItemId> eraseItemId_;
    Checkpoint eraseCheckpoint_;
    // The placement gesture in progress: where its snippets were when it
    // began - see BeginPlacement - and the library as it was then.
    struct PlacementGesture {
        Placements before;
        Checkpoint checkpoint;
    };
    std::optional<PlacementGesture> placement_;
    // The style edit in progress: which snippets, their styles when the
    // edit began - see PreviewStyles - and the library as it was then.
    struct StyleEdit {
        std::vector<std::pair<ItemId, ItemStyle>> before;
        Checkpoint checkpoint;
    };
    std::optional<StyleEdit> styleEdit_;
    // See LiveLayer.
    CanvasState liveLayer_;
    // The text edit in progress, the note as it was when it began, and the
    // library as it was then.
    std::optional<ItemId> textEditItemId_;
    std::string textEditOriginal_;
    Checkpoint textEditCheckpoint_;
    // The shape in progress, if any - see BeginShape. Where it began and
    // where the pointer last was, so SetShape can redraw it without a move.
    std::optional<ItemId> shapeItemId_;
    Shape shape_ = Shape::Line;
    float shapeStartX_ = 0.0f;
    float shapeStartY_ = 0.0f;
    float shapeLastX_ = 0.0f;
    float shapeLastY_ = 0.0f;

    // The frozen screen, while one is held - see FreezeScreen: what its
    // texture is made from, and what CaptureShotItem crops out of.
    int frozenScreenWidth_ = 0;
    int frozenScreenHeight_ = 0;
    std::vector<uint8_t> frozenScreenPixels_;
};

}  // namespace sz::core
