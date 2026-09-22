#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/canvas/canvas.h"
#include "core/canvas/item.h"
#include "core/drawing/stroke.h"

namespace sz::core {

// A plain-data copy of everything CanvasManager owns - used only by
// core::persistence::LibraryStore to (de)serialize the on-disk library
// (see CanvasManager::ExportSnapshot/ImportSnapshot). Not a general
// mutation API; prefer CanvasManager's own named operations for that -
// this exists purely so persistence code has a flat, direct view of the
// state without reaching into CanvasManager's private members.
struct CanvasManagerSnapshot {
    std::vector<Folder> folders;
    std::vector<Canvas> canvases;  // items included; each Canvas's liveLayer is not persisted (see Canvas::liveLayer).
    FolderId currentFolderId = 0;
    CanvasId currentCanvasId = 0;
};

// The same four things, borrowed rather than copied - what a save reads.
// A snapshot copies every stroke point in the library, and a save runs
// every two seconds of quiet; the copy was the largest single cost of a
// save that otherwise writes only what changed. Valid only while the
// manager it came from is not mutated - see CanvasManager::View.
struct LibraryView {
    const std::vector<Folder>& folders;
    const std::vector<Canvas>& canvases;
    FolderId currentFolderId = 0;
    CanvasId currentCanvasId = 0;
};

// Owns the folders and canvases, which folder is browsed and which canvas
// is current, and every operation on the items they hold.
//
// There is no "always at least one canvas" invariant: an empty folder is a
// legal state the moment one is created, so an empty library is too.
// "Nothing at all" is an ordinary state throughout - CurrentOrNull()
// returns nullptr, every item operation no-ops, and AddCanvas mints a
// folder to put the canvas in if none is left.
//
// What the app calls deleting is a mark, made in place (see MarkDeleted): a
// deleted folder, canvas or snippet stays exactly where it was, stamped
// with when it went, and is hidden until it is restored or deleted for
// good. A deleted folder or canvas is restored or deleted for good from
// the Overview, with Show deleted on; a deleted snippet only ever comes
// back by undo, and is erased when the library is next opened (see
// Session::ImportLibrary). Restoring is clearing marks (see Restore). A
// thing counts as deleted when it or anything holding it is marked (see
// IsDeleted), so a canvas deleted with its snippets marks only the canvas,
// restoring it brings back exactly what went with it, and a snippet
// deleted before it keeps its own mark. The Delete* functions below are
// something else: the erasure that "Delete permanently" is, immediate and
// final.
//
// `currentFolderId_` (the folder the Overview is browsing, and where a new
// canvas lands) and `currentCanvasId_` (the canvas on screen) are
// deliberately decoupled: browsing a folder doesn't switch away from the
// canvas being edited. SwitchToCanvas re-syncs the browsed folder to the
// target's; SwitchToFolder only ever touches the browsed folder.
//
// Operations that target "an item" (DeleteItem, MoveItemLayer,
// ToggleFullscreen, MoveOrCopyItemToCanvas) look only at the current
// canvas, since they are invoked from controls drawn for it.
class CanvasManager {
public:
    // Starts with one folder containing one canvas, both current and both
    // named for the moment they were made (see TimestampName); a test can
    // hand the canvas a name of its own instead.
    explicit CanvasManager(std::string initialCanvasName = "");

    // Added to `currentFolderId_`, after the last canvas already in that
    // folder - newest last, the order a list you keep adding to is read
    // in. Canvases in other folders keep their relative order. If every
    // folder has been deleted, one is created first and becomes current, so
    // a canvas never has a dangling folderId. Also becomes the current
    // canvas if there wasn't one.
    CanvasId AddCanvas(std::string name);
    // Also switches `currentFolderId_` to `id`'s folder, so the Overview
    // ends up browsing wherever the newly-current canvas actually lives.
    // No-op for a deleted canvas, which is never the one looked at.
    void SwitchToCanvas(CanvasId id);
    // Erases `id` and every one of its items for good - immediate, and not
    // what a delete is (see the class comment). GPU textures aren't this
    // class's concern; the caller releases them first (see
    // CaptureTextureHandlesForCanvas). No-op only if `id` doesn't exist.
    //
    // If `id` was current, this stays inside the folder it lived in and
    // makes the canvas *before* it there current - or that folder's first,
    // if the deleted one was first. When the folder is left with nothing,
    // no canvas is current and `currentFolderId_` stays on the emptied
    // folder, so the Overview goes on browsing where the user was and "New
    // canvas" makes the next one right there. Nothing is created
    // automatically, and it never falls back to another folder's canvas:
    // deleting the canvas you were working on must not drop you somewhere
    // you may not have been all session.
    void DeleteCanvas(CanvasId id);
    // Moves `id` so it sits at `newIndex` among Canvases() entries that
    // share its folder (clamped to that subsequence's valid range) -
    // reordering is scoped within a folder, other folders' relative order
    // is untouched. No-op if `id` doesn't exist. Drives the Overview's
    // drag-to-reorder.
    void ReorderCanvas(CanvasId id, size_t newIndex);

    // nullptr when the library holds no canvases at all - a real,
    // reachable state (see the class comment), not an error. Deliberately
    // the only accessor: the reference-returning Current() this replaced
    // dereferenced a find_if result without checking it, so every caller
    // that forgot the empty case was undefined behavior rather than a
    // compile error. Every call site now has to say what it does with
    // nothing.
    Canvas* CurrentOrNull();
    const Canvas* CurrentOrNull() const;
    bool HasCurrentCanvas() const { return CurrentOrNull() != nullptr; }
    // 0 when there is no current canvas - never a real CanvasId (ids start
    // at 1; see nextId_). For callers that only want to key something by
    // canvas (OverlayApp's per-canvas undo stacks) rather than reach into
    // its contents.
    CanvasId CurrentCanvasId() const { return currentCanvasId_; }
    const std::vector<Canvas>& Canvases() const { return canvases_; }
    // Mutable access for the one caller that has to touch every canvas
    // rather than the current one: writing painted layers out to disk
    // records the filename it chose back onto each layer (see
    // OverlayApp::SavePaintedLayers). Deliberately does *not* bump the
    // generation counter - recording where pixels were saved is not a
    // change to the drawing, and treating it as one would make every save
    // dirty the library again and save forever.
    std::vector<Canvas>& CanvasesMutable() { return canvases_; }

    // Added at the end of Folders() (newest last, the order they were
    // made) and becomes
    // `currentFolderId_`. Starts out empty - a folder holding zero
    // canvases is a legal state of the model, and one a delete can leave
    // behind. It is not a state the *UI* makes on purpose: the Overview's
    // "New folder" follows this with a canvas of its own (see
    // OverlayApp::CreateCanvasInCurrentFolder).
    FolderId AddFolder(std::string name);
    // Browse-state only - does not affect `currentCanvasId_`. No-op if `id`
    // doesn't exist or is deleted.
    void SwitchToFolder(FolderId id);
    // Erases `id` *and every canvas within it, and every item on those* for
    // good - immediate, and not what a delete is. No-op only if `id`
    // doesn't exist: deleting the last folder, or one holding every
    // remaining canvas, is allowed and leaves the library empty. GPU
    // textures are the caller's to release first. If the current canvas
    // lived in the deleted folder, switches to another canvas (or to none);
    // otherwise, if `id` was merely the browsed folder, switches
    // `currentFolderId_` to the new front of Folders(), or to 0 if that was
    // the last folder.
    void DeleteFolder(FolderId id);
    // Moves `id` so it sits at `newIndex` in Folders() (clamped to the
    // valid range). No-op if `id` doesn't exist.
    void ReorderFolder(FolderId id, size_t newIndex);
    // Reassigns the canvas to a different folder, even if that empties its
    // current one - folders are allowed to hold zero canvases. No-op if
    // `canvasId` or `targetFolderId` doesn't exist, `targetFolderId` is
    // deleted (a canvas moved into it would be deleted with it), or it is
    // already the canvas's folder.
    void MoveCanvasToFolder(CanvasId canvasId, FolderId targetFolderId);
    // No-op (name unchanged) if `id` doesn't exist or `name` is empty.
    void RenameFolder(FolderId id, std::string name);
    // As RenameFolder.
    void RenameCanvas(CanvasId id, std::string name);

    const std::vector<Folder>& Folders() const { return folders_; }
    FolderId CurrentFolderId() const { return currentFolderId_; }
    // Every layer texture handle on the named canvas - what deleting it for
    // good would leak. Call *before* DeleteCanvas/DeleteFolder so the caller
    // can release each one first; this class has no platform dependency.
    // Empty if `id` doesn't exist.
    std::vector<uint64_t> CaptureTextureHandlesForCanvas(CanvasId id) const;

    // Also sets Item::nativeW/H and the initial anchor
    // (anchorRect/anchorDisplayWidth/Height, see their own doc comments)
    // to `rect` and the display size most recently passed to
    // SyncItemsToDisplaySize (0 if that hasn't run yet, in which case the
    // anchor starts out "not yet anchored" and gets adopted on the next
    // call). 0, making nothing, with no current canvas.
    ItemId CreateItem(bool hasBackground, Rect rect, std::string name);
    // Erases `id` for good from the current canvas - immediate, and not
    // what a delete is. Texture cleanup is the caller's job. No-op if `id`
    // isn't on the current canvas. The app's snippet delete is not this: it
    // marks the snippet deleted, where an undo can find it (see
    // Session::DeleteItem).
    void DeleteItem(ItemId id);
    // The cross-canvas counterpart to DeleteItem, for Erase, which takes a
    // snippet out of whichever canvas holds it. No-op if `canvasId` or `id`
    // no longer exists.
    void DeleteItemFromCanvas(CanvasId canvasId, ItemId id);

    // Moves the item past the nearest snippet at `direction` (+1 = toward
    // the top of the stack / painted last, -1 = toward the bottom) that it
    // actually overlaps - landing directly in front of, or behind, that
    // one snippet.
    //
    // Past the nearest *overlapping* one rather than swapping with the
    // immediate neighbour, which may be nowhere near it: a canvas holds
    // snippets all over the screen, and stepping over one that shares no
    // pixels with this one changes the order without changing anything
    // anybody can see. "Forward" means "in front of the thing covering
    // me", which is the only reason to press the button.
    //
    // Snippets that aren't on screen - deleted, minimized - are not passed
    // and not counted: they cover nothing. No-op if `id` isn't on the
    // current canvas, is itself not on screen, or nothing in that
    // direction overlaps it (see CanMoveItemLayer, which is the same
    // question asked ahead of time).
    void MoveItemLayer(ItemId id, int direction);
    // Whether MoveItemLayer would move anything - for the buttons that
    // call it, which are disabled rather than silently doing nothing.
    bool CanMoveItemLayer(ItemId id, int direction) const;

    // Moves the item to the very top of the stack (painted and hit-tested
    // last). A no-op, without MarkChanged(), if `id` isn't on the current
    // canvas or is already at the top: the UI calls this at the start of
    // every drag, and the common case - the frontmost item dragged again -
    // must not dirty the library for no change.
    void BringItemToFront(ItemId id);
    // BringItemToFront for several at once: the ones among `ids` on the
    // current canvas go to the top as a block, in the order they already
    // had among themselves, and everything else keeps its order below
    // them. What a multi-selection taken hold of does - raising only the
    // snippet under the pointer would pull it out of the group. The same
    // no-op, without MarkChanged(), when they are the top of the stack
    // already.
    void BringItemsToFront(const std::vector<ItemId>& ids);

    // Toggles fullscreen. Entering fits the item's own aspect ratio into the
    // viewport, centered, unless `stretch` asks for an exact fill; either
    // way `rect` is set directly and the anchor is left untouched -
    // fullscreen is a temporary override, not a placement. Exiting
    // recomputes the restore rect from that anchor against the *current*
    // viewport (see RescaleRectForDisplaySize), so a display change while
    // fullscreen lands the item at the current screen's equivalent place.
    void ToggleFullscreen(ItemId id, float viewportW, float viewportH, bool stretch = false);

    // Moves (or, if `copy`, deep-copies) the item from the current canvas
    // to another. No-op if `id` isn't on the current canvas, the target
    // doesn't exist, or the target is the current canvas. Returns the
    // copy's new id for a copy (0 on a no-op) and always 0 for a move,
    // since the caller already knows `id`. A copy's picture file is the
    // caller's to give it (see Session::ClonePicturesForCopy); this class
    // has no file access.
    ItemId MoveOrCopyItemToCanvas(ItemId id, CanvasId targetCanvasId, bool copy);
    // The general form of that one and of DuplicateItem below, and the
    // body both of them are: copies or moves `id` - found on whichever
    // canvas holds it, not only the current one - onto `targetCanvasId`,
    // which may be the canvas it is already on. Returns the resulting
    // snippet's id: a new one for a copy (which owns its own strokes,
    // painted pixels and, once the caller has given it one, image file -
    // see DetachLayersForCopy - and starts unmarked, whatever its source's
    // mark: a copy is a new thing), the same one for a move. 0 if there is
    // no such snippet, no such canvas, or the canvas is deleted - nothing
    // is placed where it cannot be seen.
    //
    // The two narrower ones stay because their guards are part of what
    // they mean - the picker's move refuses a target that is already
    // current, Duplicate is a copy that can only land here - and because
    // this one is the clipboard's, where the snippet being pasted may
    // have been copied on a canvas nobody is looking at any more.
    ItemId PlaceItemOnCanvas(ItemId id, CanvasId targetCanvasId, bool copy);
    // Which canvas holds `id`, deleted or not - nullopt if nothing does.
    // What a paste asks about the snippet it is about to move, whose
    // history is filed under the canvas it is leaving.
    std::optional<CanvasId> CanvasHoldingItem(ItemId id) const;
    // Deep-copies the item onto the same canvas, at the same rect (the
    // caller offsets it). Returns the new id, or 0 if `id` isn't on the
    // current canvas.
    ItemId DuplicateItem(ItemId id);

    // Clips every stroke of item `id` (on the current canvas, else a no-op)
    // within `radiusScreenPx` of the screen-space point (screenX, screenY)
    // against that circle (see ClipStrokeOutsideCircle), in the item's
    // native space - the radius is scaled the same way BakeStrokeToNative
    // scales stroke width. A stroke only partly within the circle is
    // shortened/split rather than removed outright - see
    // CanvasState::EraseNear's own doc comment, which this shares the same
    // clipping behavior with.
    //
    // Returns what became of each stroke, index for index against the list
    // as it was when the call began: kStrokeUntouched, or the number of
    // fragments (0 for wholly erased) that now stand in its place, in its
    // place - the list keeps its order, with each clipped stroke replaced
    // by its fragments where it was. That is what lets the session compose
    // a whole eraser drag into one exact undo entry (see
    // Session::NoteEraseOutcome). Empty if the item was not found.
    std::vector<size_t> EraseAt(ItemId id, float screenX, float screenY, float radiusScreenPx);
    static constexpr size_t kStrokeUntouched = static_cast<size_t>(-1);

    // The rectangular-eraser equivalent of EraseAt - the same "clip, don't
    // delete whole strokes" contract (see ClipStrokeOutsideRect), against
    // the screen-space rectangle [minX, maxX] x [minY, maxY] instead of a
    // circle. The rect's own two corners are each transformed into the
    // item's native space
    // independently via ScreenToNative (unlike EraseAt's single averaged
    // radius scale, an axis-aligned rect's corners transform exactly
    // under its per-axis scale, with no approximation needed).
    std::vector<size_t> EraseRectAt(ItemId id, float minX, float minY, float maxX, float maxY);

    // Finds an item by id across *every* canvas, unlike the item operations
    // above. Returns nullptr if `id` doesn't exist anywhere.
    Item* FindItemAnywhere(ItemId id);

    // Transforms a stroke from screen space into `item`'s native coordinate
    // space, from how its current rect compares to its fixed native size.
    // Pure, so it is testable on its own.
    static Stroke BakeStrokeToNative(const Item& item, const Stroke& screenSpaceStroke);

    // A counter bumped by every method here that actually mutates
    // folders, canvases or items (no-op paths don't), plus MarkChanged()
    // for the content mutations made directly on a reference this class
    // handed out (a drag, an opacity slider, a baked stroke). Never
    // persisted and never reset by ImportSnapshot, since a load isn't a
    // user change. The autosave compares it frame to frame, and the mesh
    // caches gate their invalidation checks on it.
    uint64_t Generation() const { return generation_; }
    // Call after mutating a Canvas or Item through a reference, whenever
    // that mutation isn't already one of this class's own methods.
    void MarkChanged() { ++generation_; }

    // Whether the current canvas has a snippet that stays on screen with
    // the overlay put away: pinned, and neither minimized nor deleted. See
    // Item::pinned.
    bool CurrentCanvasHasPinnedItems() const;

    // A flat, direct-copy view of everything this class owns, for
    // core::persistence::LibraryStore - see CanvasManagerSnapshot's own
    // doc comment.
    CanvasManagerSnapshot ExportSnapshot() const;
    // The library as it is, for reading - see LibraryView. What a save
    // takes; a snapshot is for a caller that needs its own copy to alter.
    LibraryView View() const { return LibraryView{folders_, canvases_, currentFolderId_, currentCanvasId_}; }
    // Wholesale-replaces this manager's state with `snapshot` - once, right
    // after startup. Moves the current canvas off a deleted one (see
    // SettleOffDeleted), but leaves Generation() untouched: loading isn't a
    // user change.
    void ImportSnapshot(CanvasManagerSnapshot snapshot);

    // Keeps every item's `rect` correct for the display it is shown on
    // right now. Called every frame with the display size, so a resolution
    // change is caught on the next frame with no platform callback.
    //
    // `rect` is recomputed fresh from each item's fixed anchor (see
    // Item::anchorRect) via RescaleRectForDisplaySize on every call, never
    // chained onto whatever `rect` already holds. That is what makes a
    // display change reversible: the uniform size factor is asymmetric
    // between shrinking and growing, so rescaling in place ratchets items
    // smaller every cycle and never back up. An anchor with
    // anchorDisplayWidth/Height == 0 is adopted as-is the first time this
    // runs. Every touched rect is floored (GrowRectToMinimumSize, which
    // keeps the ratio) and clamped to the viewport - the same guarantees an
    // interactive resize enforces. A fullscreen item's `rect` is set from
    // the viewport directly, stretched or fitted as it was entered, with
    // its anchor left alone underneath.
    //
    // A non-positive size is a no-op. Otherwise also records the size for
    // CreateItem/CommitItemLayout/ResetItemToNativeSize.
    void SyncItemsToDisplaySize(float currentW, float currentH);

    // Re-anchors the item to wherever a move, resize or nudge just left
    // `rect`, against the current display size. Without this the next
    // display change would silently revert a deliberate resize. No-op if
    // `id` isn't on the current canvas or before SyncItemsToDisplaySize has
    // run. Doesn't bump Generation(); callers do when they mutate `rect`.
    void CommitItemLayout(ItemId id);

    // Resets the item to its native size (a capture's pixel dimensions, a
    // drawing's creation size), re-centered where it sits, and re-anchors
    // it there. Exits fullscreen first, since a fixed size and a
    // viewport-filling override contradict. No-op if `id` isn't on the
    // current canvas or has no native size.
    void ResetItemToNativeSize(ItemId id);

    // Makes `canvasId` the *only* canvas holding live textures, which is
    // the whole GPU-memory budget this app needs: nothing but the current
    // canvas is ever drawn from a real texture, so every other canvas's
    // textures are pure cost.
    //
    // On `canvasId`, for every item there that isn't deleted: any layer
    // with a persisted image or painted pixels but no live texture gets one
    // from `loadLayer` (0 meaning the load failed, leaving the placeholder
    // like a failed capture). On every other canvas, and for a deleted item
    // on this one: any *resident* layer - holding a texture, or painted
    // pixels, or both - is handed to `releaseLayer` and its handle cleared.
    // Residency is asked directly rather than read off the handle: a
    // painted layer with pixels but no texture is just as resident, and
    // only the callback knows whether its pixels are safe to drop.
    //
    // A release only ever happens for a layer with a file to reload from
    // or pixels in memory; a texture with nothing behind it (a capture
    // whose save failed) is kept rather than freed into blankness.
    // Idempotent and cheap when nothing changed. Doesn't touch Generation():
    // a GPU handle isn't content.
    void SyncShotTexturesToCanvas(CanvasId canvasId, const std::function<uint64_t(const Item&, Layer&)>& loadLayer,
                                   const std::function<void(Layer&)>& releaseLayer);

private:
    // Where `id` sits in `canvas`'s stack, and which snippet a z-order
    // step in `direction` from there has to pass - the nearest one that
    // overlaps it and is on screen. Both nullopt when there is no such
    // thing, which is what makes MoveItemLayer and CanMoveItemLayer one
    // answer asked twice.
    std::optional<size_t> IndexOfItemOnCanvas(const Canvas& canvas, ItemId id) const;
    std::optional<size_t> NearestOverlappingItem(const Canvas& canvas, size_t index, int direction) const;

    Item* FindInCurrent(ItemId id);

    // Makes some remaining canvas current again after the previous current
    // canvas stopped being a valid choice (its canvas or folder was
    // deleted) - prefers one in `preferredFolderId` if given and it has
    // one, otherwise any remaining canvas. Leaves `currentCanvasId_` at 0
    // ("no current canvas") if there are none left at all, which is a
    // legal state - see the class comment. Also disarms.
    void SwitchToNextAvailableCanvas(std::optional<FolderId> preferredFolderId);

    std::vector<Folder> folders_;
    std::vector<Canvas> canvases_;
    FolderId currentFolderId_ = 0;
    CanvasId currentCanvasId_ = 0;
    // Ids are drawn at random and checked, not counted - see util/uid.h.
    uint64_t NewId() const;

public:
    // Whether anything in this library already holds `id` - folder, canvas
    // or item alike, deleted or not. One space for all three: it costs
    // nothing, and it means a directory found in the wrong place can never
    // be mistaken for a different kind of thing that happens to share its
    // number.
    bool IsIdTaken(uint64_t id) const;

    // Null if nothing by that id.
    const Folder* FindFolder(FolderId id) const;
    const Canvas* FindCanvas(CanvasId id) const;

    // ===== Deleted things =====
    //
    // See the class comment: a delete is a mark made in place, and a
    // restore clears it.

    // Marks the folder, canvas or snippet `id` names as deleted at `when`
    // (seconds since the epoch). It stays where it is, hidden, and what was
    // current moves off it (see SettleOffDeleted). False, doing nothing, if
    // nothing has that id or it is marked already.
    bool MarkDeleted(uint64_t id, int64_t when);
    // Brings `id` back, and whatever has to come back for it to be seen:
    //  - a folder: its own mark and those of every canvas in it, so that
    //    restoring a folder brings back all of it, whether it was deleted
    //    whole or only some canvases in it were;
    //  - a canvas: its own mark, and if its folder is deleted, the
    //    folder's - which would bring back every canvas that went with the
    //    folder, so those are marked instead, each with the folder's stamp:
    //    the one canvas comes back, and the rest stay deleted as and when
    //    they were;
    //  - a snippet: its own mark, and its canvas as above if that counts
    //    as deleted.
    // False if `id` names nothing, or nothing that counted as deleted.
    bool Restore(uint64_t id);
    // Erases `id` for good, whatever kind of thing it is - DeleteFolder,
    // DeleteCanvas or DeleteItemFromCanvas. False if nothing has that id.
    bool Erase(uint64_t id);

    // Whether a thing counts as deleted: its own mark, or the mark of
    // anything holding it.
    bool IsDeleted(const Folder& folder) const { return folder.deletedAt != 0; }
    bool IsDeleted(const Canvas& canvas) const;
    bool IsDeleted(const Canvas& canvas, const Item& item) const { return item.deletedAt != 0 || IsDeleted(canvas); }
    // The same for a snippet found by id, wherever it is. False for no such
    // snippet.
    bool IsItemDeleted(ItemId id) const;

    // Whether the folder is deleted or holds a canvas that is - what the
    // Overview marks out with Show deleted on, and what a folder's Restore
    // has something to do for.
    bool HoldsDeleted(const Folder& folder) const;
    // The canvases in `folderId` carrying a mark of their own, in
    // Canvases() order. Not those deleted only because the folder is.
    std::vector<CanvasId> MarkedCanvasesIn(FolderId folderId) const;
    // How many folders and canvases carry a mark of their own - what there
    // is to restore from the Overview.
    size_t DeletedFolderAndCanvasCount() const;
    // Every snippet carrying a mark of its own, wherever it is.
    std::vector<ItemId> MarkedSnippets() const;
    // The folders and canvases whose own mark is older than `cutoff`
    // (seconds since the epoch), folders first - what a retention period
    // deletes for good. A canvas in a folder on the list is left off it:
    // it goes with the folder. One that went only with its folder has no
    // mark of its own and is never listed alone; one marked on its own
    // before its folder went can be, and going first leaves the folder as
    // it would be had it been deleted for good by hand.
    std::vector<uint64_t> MarkedBefore(int64_t cutoff) const;

private:
    // Where the mark of the folder, canvas or snippet `id` names is kept.
    int64_t* DeletedStampOf(uint64_t id);
    // Moves the current canvas and the browsed folder off anything deleted:
    // the current canvas to the live one before it in its folder (or the
    // first after it, or none - the same fallback DeleteCanvas makes), or
    // the first live canvas anywhere if its folder is deleted too; the
    // browsed folder to the current canvas's.
    void SettleOffDeleted();

    uint64_t generation_ = 0;
    // The most recent display size passed to SyncItemsToDisplaySize; 0
    // until it has run at least once.
    float currentDisplayWidth_ = 0.0f;
    float currentDisplayHeight_ = 0.0f;
};

}  // namespace sz::core
